/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <boost/test/unit_test.hpp>

#include <netclass.h>
#include <stroke_params.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/any.pb.h>


BOOST_AUTO_TEST_SUITE( NetclassTests )


BOOST_AUTO_TEST_CASE( NameEqualsMatchesGetName )
{
    // The allocation-free comparison must preserve GetName() equality for every valid name
    auto check = []( const NETCLASS& aNetclass, const wxString& aName, bool aExpected )
    {
        BOOST_CHECK_EQUAL( aNetclass.NameEquals( aName ), aExpected );
        BOOST_CHECK_EQUAL( aNetclass.NameEquals( aName ), aNetclass.GetName() == aName );
    };

    NETCLASS first( wxS( "Power" ) );
    NETCLASS second( wxS( "Ground" ) );
    NETCLASS third( wxS( "Signal" ) );
    NETCLASS empty( wxEmptyString );
    NETCLASS comma( wxS( "A,B" ) );
    NETCLASS alias( wxS( "Alias" ) );
    NETCLASS composite( wxEmptyString );

    check( first, wxS( "Power" ), true );
    check( first, wxS( "Powe" ), false );
    check( first, wxS( "PowerX" ), false );
    check( empty, wxEmptyString, true );
    check( empty, wxS( "Power" ), false );

    alias.SetConstituentNetclasses( { &first } );
    check( alias, wxS( "Alias" ), true );
    check( alias, wxS( "Power" ), false );

    composite.SetConstituentNetclasses( { &first, &second } );
    check( composite, wxS( "Power,Ground" ), true );
    check( composite, wxS( "Power;Ground" ), false );

    composite.SetConstituentNetclasses( { &comma, &third } );
    check( composite, wxS( "A,B,Signal" ), true );
    check( composite, wxS( "A,B;Signal" ), false );

    composite.SetConstituentNetclasses( { &empty, &first } );
    check( composite, wxS( ",Power" ), true );
    check( composite, wxS( "Power" ), false );
}


BOOST_AUTO_TEST_CASE( SchematicLineStyleRoundTripsThroughApi )
{
    for( int style = static_cast<int>( LINE_STYLE::DEFAULT );
         style <= static_cast<int>( LINE_STYLE::DASHDOTDOT ); ++style )
    {
        NETCLASS original( wxS( "Signal" ) );
        original.SetLineStyle( style );
        original.SetuViaDiameter( 300000 );
        original.SetuViaDrill( 100000 );
        google::protobuf::Any packed;
        original.Serialize( packed );
        kiapi::common::project::NetClass proto;
        BOOST_REQUIRE( packed.UnpackTo( &proto ) );
        BOOST_REQUIRE( proto.schematic().has_line_style() );
        NETCLASS restored( wxS( "Restored" ) );
        BOOST_REQUIRE( restored.Deserialize( packed ) );
        BOOST_REQUIRE( restored.HasLineStyle() );
        BOOST_CHECK_EQUAL( restored.GetLineStyle(), style );
        BOOST_CHECK_EQUAL( restored.GetuViaDiameter(), 300000 );
        BOOST_CHECK_EQUAL( restored.GetuViaDrill(), 100000 );
    }
}

BOOST_AUTO_TEST_SUITE_END()
