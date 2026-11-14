/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <cmath>
#include <string>

#include <boost/test/unit_test.hpp>
#include <magic_enum.hpp>

#include <api/api_handler_common.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <transline_calculations/coupled_stripline.h>
#include <transline_calculations/microstrip.h>
#include <transline_calculations/units.h>

using namespace kiapi::common::commands;
using TCP = TRANSLINE_PARAMETERS;


namespace
{

API_RESULT calculate( API_HANDLER_COMMON& aHandler, const CalculateTransmissionLine& aCommand )
{
    kiapi::common::ApiRequest request;
    BOOST_REQUIRE( request.mutable_message()->PackFrom( aCommand ) );
    return aHandler.Handle( request );
}


void addParam( CalculateTransmissionLine& aCommand, TransmissionLineParameter aParam, double aValue )
{
    TransmissionLineValue* value = aCommand.add_parameters();
    value->set_parameter( aParam );
    value->set_value( aValue );
}


double resultOf( const TransmissionLineResponse& aResponse, TransmissionLineParameter aParam )
{
    for( const TransmissionLineResult& result : aResponse.results() )
    {
        if( result.parameter() == aParam )
            return result.value();
    }

    BOOST_FAIL( "parameter missing from the response: " + TransmissionLineParameter_Name( aParam ) );
    return 0.0;
}

} // namespace


BOOST_AUTO_TEST_SUITE( ApiHandlerCommon )


BOOST_AUTO_TEST_CASE( TransmissionLineParametersMirrorCalculator )
{
    for( TCP param : magic_enum::enum_values<TCP>() )
    {
        if( param == TCP::UNKNOWN_ID || param == TCP::EXTRAS_COUNT )
            continue;

        auto proto = static_cast<TransmissionLineParameter>( static_cast<int>( param ) + 1 );

        BOOST_REQUIRE( TransmissionLineParameter_IsValid( proto ) );
        BOOST_CHECK_EQUAL( TransmissionLineParameter_Name( proto ),
                           "TLP_" + std::string( magic_enum::enum_name( param ) ) );
    }
}


BOOST_AUTO_TEST_CASE( CalculateMicrostripMatchesCalculator )
{
    const double eps = 4.3, h = 0.2e-3, t = 35e-6, w = 0.35e-3, tand = 0.02, freq = 1e9;
    const double sigma = 1.0 / 1.72e-8;

    MICROSTRIP direct;
    direct.SetParameter( TCP::SIGMA, sigma );
    direct.SetParameter( TCP::EPSILON_EFF, 1.0 );
    direct.SetParameter( TCP::SKIN_DEPTH,
                         1.0 / std::sqrt( M_PI * freq * TRANSLINE_CALCULATIONS::MU0 * sigma ) );
    direct.SetParameter( TCP::EPSILONR, eps );
    direct.SetParameter( TCP::H_T, 1e20 );
    direct.SetParameter( TCP::H, h );
    direct.SetParameter( TCP::T, t );
    direct.SetParameter( TCP::PHYS_WIDTH, w );
    direct.SetParameter( TCP::FREQUENCY, freq );
    direct.SetParameter( TCP::TAND, tand );
    direct.SetParameter( TCP::PHYS_LEN, 1.0 );
    direct.SetParameter( TCP::MUR, 1 );
    direct.SetParameter( TCP::MURC, 1 );
    direct.SetParameter( TCP::ANG_L, 1 );
    direct.Analyse();

    API_HANDLER_COMMON handler;
    CalculateTransmissionLine cmd;
    cmd.set_model( TLM_MICROSTRIP );
    addParam( cmd, TLP_EPSILONR, eps );
    addParam( cmd, TLP_H, h );
    addParam( cmd, TLP_T, t );
    addParam( cmd, TLP_PHYS_WIDTH, w );
    addParam( cmd, TLP_TAND, tand );
    addParam( cmd, TLP_FREQUENCY, freq );

    API_RESULT result = calculate( handler, cmd );
    BOOST_REQUIRE( result.has_value() );

    TransmissionLineResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK( response.converged() );

    const double z0 = resultOf( response, TLP_Z0 );
    BOOST_CHECK_CLOSE( z0, direct.GetAnalysisResults()[TCP::Z0].first, 1e-9 );
    BOOST_CHECK_CLOSE( resultOf( response, TLP_UNIT_PROP_DELAY ),
                       direct.GetAnalysisResults()[TCP::UNIT_PROP_DELAY].first, 1e-9 );
    BOOST_CHECK( z0 > 40.0 && z0 < 60.0 );
}


BOOST_AUTO_TEST_CASE( SynthesizeCoupledStriplineWidth )
{
    const double h = 0.2e-3 + 18e-6 + 0.2e-3, gap = 0.15e-3;

    API_HANDLER_COMMON handler;
    CalculateTransmissionLine cmd;
    cmd.set_model( TLM_COUPLED_STRIPLINE );
    cmd.set_mode( TLMODE_SYNTHESIZE );
    cmd.set_synthesis( TLS_FIX_SPACING );
    addParam( cmd, TLP_EPSILONR, 4.2 );
    addParam( cmd, TLP_H, h );
    addParam( cmd, TLP_T, 18e-6 );
    addParam( cmd, TLP_PHYS_S, gap );
    addParam( cmd, TLP_PHYS_WIDTH, 0.1e-3 );
    addParam( cmd, TLP_Z_DIFF, 100.0 );
    addParam( cmd, TLP_Z0_E, 50.0 );
    addParam( cmd, TLP_Z0_O, 50.0 );

    API_RESULT result = calculate( handler, cmd );
    BOOST_REQUIRE( result.has_value() );

    TransmissionLineResponse synth;
    BOOST_REQUIRE( result->message().UnpackTo( &synth ) );
    BOOST_REQUIRE( synth.converged() );

    const double width = resultOf( synth, TLP_PHYS_WIDTH );
    BOOST_CHECK( width > 0.03e-3 && width < 0.5e-3 );
    BOOST_CHECK_CLOSE( resultOf( synth, TLP_PHYS_S ), gap, 1e-6 );

    // Analysing the synthesized geometry gives back the target
    CalculateTransmissionLine check;
    check.set_model( TLM_COUPLED_STRIPLINE );
    addParam( check, TLP_EPSILONR, 4.2 );
    addParam( check, TLP_H, h );
    addParam( check, TLP_T, 18e-6 );
    addParam( check, TLP_PHYS_S, gap );
    addParam( check, TLP_PHYS_WIDTH, width );

    result = calculate( handler, check );
    BOOST_REQUIRE( result.has_value() );

    TransmissionLineResponse analysed;
    BOOST_REQUIRE( result->message().UnpackTo( &analysed ) );
    BOOST_CHECK_CLOSE( resultOf( analysed, TLP_Z_DIFF ), 100.0, 0.5 );
}


BOOST_AUTO_TEST_CASE( CalculateTransmissionLineRejectsForeignParameter )
{
    API_HANDLER_COMMON handler;
    CalculateTransmissionLine cmd;
    cmd.set_model( TLM_STRIPLINE );
    addParam( cmd, TLP_H_T, 1.0 );

    API_RESULT result = calculate( handler, cmd );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
    BOOST_CHECK( result.error().error_message().find( "TLP_H_T" ) != std::string::npos );

    cmd.set_model( TLM_UNKNOWN );
    result = calculate( handler, cmd );
    BOOST_REQUIRE( !result.has_value() );
}


BOOST_AUTO_TEST_SUITE_END()
