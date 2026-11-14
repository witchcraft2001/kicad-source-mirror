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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <api/api_handler_fp_libraries.h>
#include <api/api_utils.h>
#include <api/common/commands/library_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <footprint_library_adapter.h>
#include <libraries/library_manager.h>
#include <pgm_base.h>
#include <project_pcb.h>
#include <settings/settings_manager.h>

#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/textfile.h>

#include <filesystem>
#include <fstream>
#include <iterator>


namespace
{

using kiapi::common::ApiStatusCode;
using namespace kiapi::common::commands;

/// A project directory whose footprint table lists qa/data/libraries/Resistor_SMD.pretty
struct API_FP_LIBRARIES_FIXTURE
{
    API_FP_LIBRARIES_FIXTURE()
    {
        std::error_code ec;
        m_dir = std::filesystem::temp_directory_path( ec )
                / std::filesystem::path( wxString::Format( wxS( "kicad_qa_apifp_%p" ), (void*) this ).ToStdString() );
        std::filesystem::create_directories( m_dir / "Resistor_SMD.pretty", ec );

        wxFileName source( wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ), wxEmptyString );
        source.AppendDir( wxS( "libraries" ) );

        wxFileName pro( source.GetPath(), wxS( "test_project.kicad_pro" ) );
        pro.AppendDir( wxS( "test_project" ) );
        BOOST_REQUIRE( wxCopyFile( pro.GetFullPath(), wxString( ( m_dir / "fp_project.kicad_pro" ).string() ) ) );

        wxString pretty = wxFileName( source.GetPath(), wxEmptyString ).GetPath() + wxFileName::GetPathSeparator()
                          + wxS( "Resistor_SMD.pretty" );
        wxDir dir( pretty );
        wxString name;

        for( bool more = dir.GetFirst( &name, wxS( "*.kicad_mod" ) ); more; more = dir.GetNext( &name ) )
        {
            BOOST_REQUIRE( wxCopyFile( pretty + wxFileName::GetPathSeparator() + name,
                                       wxString( ( m_dir / "Resistor_SMD.pretty" / name.ToStdString() ).string() ) ) );
        }

        wxTextFile table( wxString( ( m_dir / "fp-lib-table" ).string() ) );
        table.Create();
        table.AddLine( wxS( "(fp_lib_table" ) );
        table.AddLine( wxS( "  (version 7)" ) );
        table.AddLine( wxS( "  (lib (name \"Resistor_SMD\")(type \"KiCad\")(uri \"${KIPRJMOD}/Resistor_SMD.pretty\")"
                            "(options \"\")(descr \"Resistors\"))" ) );
        table.AddLine( wxS( ")" ) );
        table.Write();
        table.Close();

        SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
        mgr.LoadProject( wxString( ( m_dir / "fp_project.kicad_pro" ).string() ) );
        m_project = &mgr.Prj();

        Pgm().GetLibraryManager().LoadProjectTables( m_project->GetProjectDirectory() );
        m_adapter = PROJECT_PCB::FootprintLibAdapter( m_project );
    }

    ~API_FP_LIBRARIES_FIXTURE()
    {
        Pgm().GetSettingsManager().UnloadProject( m_project, false );

        std::error_code ec;
        std::filesystem::remove_all( m_dir, ec );
    }

    std::filesystem::path       m_dir;
    PROJECT*                    m_project = nullptr;
    FOOTPRINT_LIBRARY_ADAPTER*  m_adapter = nullptr;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerFpLibraries, API_FP_LIBRARIES_FIXTURE )


BOOST_AUTO_TEST_CASE( SearchFindsFootprintsBySizeAndCountsPads )
{
    BOOST_REQUIRE( m_adapter );
    m_adapter->AsyncLoad();
    m_adapter->BlockUntilLoaded();

    API_HANDLER_FP_LIBRARIES handler;

    SearchLibraries command;
    command.set_type( kiapi::common::types::LibraryType::LT_FOOTPRINT );
    command.set_query( "0603" );
    command.add_libraries( "Resistor_SMD" );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), ApiStatusCode::AS_OK );

    SearchLibrariesResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.items_size(), response.results_size() );
    BOOST_REQUIRE_GE( response.items_size(), 1 );

    bool foundPlain = false;

    for( int i = 0; i < response.items_size(); ++i )
    {
        BOOST_CHECK_EQUAL( response.items( i ).library_nickname(), "Resistor_SMD" );
        BOOST_CHECK( response.items( i ).entry_name().find( "0603" ) != std::string::npos );
        BOOST_CHECK_EQUAL( response.results( i ).pin_count(), 2 );

        if( response.items( i ).entry_name() == "R_0603_1608Metric" )
            foundPlain = true;
    }

    BOOST_CHECK( foundPlain );

    // A footprint library is not searched by the symbol handler's type
    command.set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( FootprintLibraryTableEditsAddAndDeleteALibrary )
{
    // A second library: a copy of the one the table lists
    std::error_code ec;
    std::filesystem::copy( m_dir / "Resistor_SMD.pretty", m_dir / "Extra.pretty", ec );
    BOOST_REQUIRE( !ec );

    API_HANDLER_FP_LIBRARIES handler;

    auto send = [&]( const google::protobuf::Message& aCommand )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( aCommand ) );

        API_RESULT result = handler.Handle( request );
        BOOST_REQUIRE( result.has_value() );

        kiapi::common::types::LibraryCommandStatus status;
        BOOST_REQUIRE( result->message().UnpackTo( &status ) );
        return status;
    };

    auto tableText = [&]()
    {
        std::ifstream in( m_dir / "fp-lib-table" );
        return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    };

    AddLibraryTableEntry add;
    add.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    add.mutable_entry()->set_type( kiapi::common::types::LibraryType::LT_FOOTPRINT );
    add.mutable_entry()->set_nickname( "Extra" );
    add.mutable_entry()->set_uri( "${KIPRJMOD}/Extra.pretty" );

    BOOST_CHECK_EQUAL( send( add ).code(), kiapi::common::types::LibraryCommandStatus::LCS_OK );

    std::string text = tableText();
    BOOST_CHECK( text.find( "(name \"Extra\")" ) != std::string::npos );
    BOOST_CHECK( text.find( "(type \"KiCad\")" ) != std::string::npos );
    BOOST_CHECK( text.find( "(name \"Resistor_SMD\")" ) != std::string::npos );

    std::optional<LIB_STATUS> loaded = m_adapter->GetLibraryStatus( wxS( "Extra" ) );
    BOOST_REQUIRE( loaded );
    BOOST_CHECK( loaded->load_status == LOAD_STATUS::LOADED );

    BOOST_CHECK_EQUAL( send( add ).code(), kiapi::common::types::LibraryCommandStatus::LCS_ALREADY_EXISTS );

    // The symbol type is left to the symbol handler
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        add.mutable_entry()->set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( add ) );

        API_RESULT result = handler.Handle( request );
        BOOST_REQUIRE( !result.has_value() );
        BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_UNHANDLED );
    }

    DeleteLibraryTableEntry del;
    del.set_type( kiapi::common::types::LibraryType::LT_FOOTPRINT );
    del.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    del.set_nickname( "Extra" );

    BOOST_CHECK_EQUAL( send( del ).code(), kiapi::common::types::LibraryCommandStatus::LCS_OK );

    text = tableText();
    BOOST_CHECK( text.find( "(name \"Extra\")" ) == std::string::npos );
    BOOST_CHECK( text.find( "(name \"Resistor_SMD\")" ) != std::string::npos );
    BOOST_CHECK( std::filesystem::exists( m_dir / "Extra.pretty" ) );
}


BOOST_AUTO_TEST_SUITE_END()
