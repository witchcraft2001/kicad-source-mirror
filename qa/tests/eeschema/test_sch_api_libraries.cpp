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

#include <api/api_handler_sch_libraries.h>
#include <api/api_utils.h>
#include <api/common/commands/library_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <libraries/library_manager.h>
#include <libraries/symbol_library_adapter.h>
#include <pgm_base.h>
#include <project_sch.h>
#include <settings/settings_manager.h>

#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <filesystem>
#include <fstream>
#include <iterator>


namespace
{

using kiapi::common::ApiStatusCode;
using namespace kiapi::common::commands;

/**
 * A project directory with the small Device library of qa/data/libraries/test_project and a table
 * that also lists a library whose file does not exist.
 */
struct API_LIBRARIES_FIXTURE
{
    API_LIBRARIES_FIXTURE()
    {
        std::error_code ec;
        m_dir = std::filesystem::temp_directory_path( ec )
                / std::filesystem::path( wxString::Format( wxS( "kicad_qa_apilibs_%p" ), (void*) this ).ToStdString() );
        std::filesystem::create_directories( m_dir, ec );

        wxFileName source( wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ), wxEmptyString );
        source.AppendDir( wxS( "libraries" ) );
        source.AppendDir( wxS( "test_project" ) );

        for( const wxString& name : { wxS( "test_project.kicad_pro" ), wxS( "Device.kicad_sym" ),
                                      wxS( "sym-lib-table" ) } )
        {
            wxFileName from( source.GetPath(), name );
            wxFileName to( wxString( m_dir.string() ), name );
            BOOST_REQUIRE( wxCopyFile( from.GetFullPath(), to.GetFullPath() ) );
        }

        SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();

        mgr.LoadProject( wxString( ( m_dir / "test_project.kicad_pro" ).string() ) );
        m_project = &mgr.Prj();

        Pgm().GetLibraryManager().LoadProjectTables( m_project->GetProjectDirectory() );
        m_adapter = PROJECT_SCH::SymbolLibAdapter( m_project );
    }

    ~API_LIBRARIES_FIXTURE()
    {
        Pgm().GetSettingsManager().UnloadProject( m_project, false );

        std::error_code ec;
        std::filesystem::remove_all( m_dir, ec );
    }

    kiapi::common::ApiRequest search( const std::string& aQuery, const std::vector<std::string>& aLibraries = {},
                                      int aMax = 0 )
    {
        SearchLibraries command;
        command.set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
        command.set_query( aQuery );
        command.set_max_results( aMax );

        for( const std::string& lib : aLibraries )
            command.add_libraries( lib );

        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return request;
    }

    /// Sends a library table edit to a handler and unpacks the status
    template <typename COMMAND>
    API_RESULT edit( const COMMAND& aCommand )
    {
        API_HANDLER_SCH_LIBRARIES handler;
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( aCommand ) );
        return handler.Handle( request );
    }

    kiapi::common::types::LibraryCommandStatus status( const API_RESULT& aResult )
    {
        BOOST_REQUIRE( aResult.has_value() );
        BOOST_REQUIRE_EQUAL( aResult->status().status(), ApiStatusCode::AS_OK );

        kiapi::common::types::LibraryCommandStatus status;
        BOOST_REQUIRE( aResult->message().UnpackTo( &status ) );
        return status;
    }

    AddLibraryTableEntry addCommand( const std::string& aNickname, const std::string& aUri )
    {
        AddLibraryTableEntry command;
        command.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
        command.mutable_entry()->set_nickname( aNickname );
        command.mutable_entry()->set_uri( aUri );
        command.mutable_entry()->set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
        return command;
    }

    std::string tableText() const
    {
        std::ifstream in( m_dir / "sym-lib-table" );
        return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    }

    void copyDevice( const std::string& aName )
    {
        std::error_code ec;
        std::filesystem::copy_file( m_dir / "Device.kicad_sym", m_dir / aName,
                                    std::filesystem::copy_options::overwrite_existing, ec );
        BOOST_REQUIRE( !ec );
    }

    std::filesystem::path   m_dir;
    PROJECT*                m_project = nullptr;
    SYMBOL_LIBRARY_ADAPTER* m_adapter = nullptr;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerSchLibraries, API_LIBRARIES_FIXTURE )


BOOST_AUTO_TEST_CASE( GetLibraryTableListsTheProjectRows )
{
    BOOST_REQUIRE( m_adapter );

    GetLibraryTable command;
    command.set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
    command.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    command.set_substituted( false );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_HANDLER_SCH_LIBRARIES handler;
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), ApiStatusCode::AS_OK );

    LibraryTableResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    BOOST_CHECK( response.table().valid() );
    BOOST_CHECK( response.table().path().find( "sym-lib-table" ) != std::string::npos );
    BOOST_CHECK_EQUAL( response.table().scope(), kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    BOOST_REQUIRE_EQUAL( response.rows_size(), 2 );

    // Rows come back as written, with the variable of the table
    BOOST_CHECK_EQUAL( response.rows( 0 ).nickname(), "Device" );
    BOOST_CHECK_EQUAL( response.rows( 0 ).uri(), "${KIPRJMOD}/Device.kicad_sym" );
    BOOST_CHECK_EQUAL( response.rows( 0 ).scope(), kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    BOOST_CHECK_EQUAL( response.rows( 1 ).nickname(), "4xxx" );
    BOOST_CHECK_EQUAL( response.rows( 1 ).description(), "4xxx series symbols" );

    // ... or with the variable expanded
    command.set_substituted( true );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK( response.rows( 0 ).uri().find( "KIPRJMOD" ) == std::string::npos );
    BOOST_CHECK( response.rows( 0 ).uri().find( "Device.kicad_sym" ) != std::string::npos );

    // A table of any type can be read from any handler instance
    command.set_type( kiapi::common::types::LibraryType::LT_FOOTPRINT );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( response.rows_size(), 0 );
}


BOOST_AUTO_TEST_CASE( SearchWaitsForLibrariesToLoad )
{
    BOOST_REQUIRE( m_adapter );

    API_HANDLER_SCH_LIBRARIES handler;
    kiapi::common::ApiRequest request = search( "resistor" );
    API_RESULT                result = handler.Handle( request );

    // Nothing has loaded yet: the client is told to ask again
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_NOT_READY );

    m_adapter->BlockUntilLoaded();

    result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
}


BOOST_AUTO_TEST_CASE( SearchRanksAndDescribesMatches )
{
    BOOST_REQUIRE( m_adapter );
    m_adapter->AsyncLoad();
    m_adapter->BlockUntilLoaded();

    API_HANDLER_SCH_LIBRARIES handler;

    auto run = [&]( const std::string& aQuery, const std::vector<std::string>& aLibs = {}, int aMax = 0 )
    {
        kiapi::common::ApiRequest request = search( aQuery, aLibs, aMax );
        API_RESULT                result = handler.Handle( request );

        BOOST_REQUIRE( result.has_value() );
        BOOST_REQUIRE_EQUAL( result->status().status(), ApiStatusCode::AS_OK );

        SearchLibrariesResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        BOOST_REQUIRE_EQUAL( response.items_size(), response.results_size() );
        return response;
    };

    // The keyword and the description find the symbol; a library that cannot be read is skipped
    SearchLibrariesResponse found = run( "resistor" );
    BOOST_REQUIRE_GE( found.items_size(), 1 );
    BOOST_CHECK_EQUAL( found.items( 0 ).library_nickname(), "Device" );
    BOOST_CHECK_EQUAL( found.items( 0 ).entry_name(), "R" );
    BOOST_CHECK_EQUAL( found.results( 0 ).description(), "Resistor" );
    BOOST_CHECK_EQUAL( found.results( 0 ).pin_count(), 2 );
    BOOST_CHECK_EQUAL( found.results( 0 ).unit_count(), 1 );
    BOOST_CHECK_GT( found.results( 0 ).score(), 0 );

    // A name equal to the query comes first and is flagged
    SearchLibrariesResponse exact = run( "d" );
    BOOST_REQUIRE_GE( exact.items_size(), 1 );
    BOOST_CHECK_EQUAL( exact.items( 0 ).entry_name(), "D" );
    BOOST_CHECK( exact.results( 0 ).exact_match() );

    // Every term must match; wildcards work as in the chooser
    BOOST_CHECK_EQUAL( run( "cap diode" ).items_size(), 0 );
    BOOST_CHECK_GE( run( "cap*" ).items_size(), 1 );
    BOOST_CHECK_EQUAL( run( "nothing like this" ).items_size(), 0 );

    // An empty query lists everything, limited by max_results and the library filter
    BOOST_CHECK_EQUAL( run( "", { "Device" } ).items_size(), 3 );
    BOOST_CHECK_EQUAL( run( "", { "Device" }, 2 ).items_size(), 2 );

    // An unknown library is a bad request
    kiapi::common::ApiRequest request = search( "r", { "NoSuchLibrary" } );
    API_RESULT                result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( SearchOfOtherTypesIsLeftToTheirHandlers )
{
    API_HANDLER_SCH_LIBRARIES handler;

    SearchLibraries command;
    command.set_type( kiapi::common::types::LibraryType::LT_FOOTPRINT );
    command.set_query( "r" );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( AddLibraryTableEntryAddsALoadableLibrary )
{
    copyDevice( "Extra.kicad_sym" );

    kiapi::common::types::LibraryCommandStatus added =
            status( edit( addCommand( "Extra", "${KIPRJMOD}/Extra.kicad_sym" ) ) );
    BOOST_CHECK_EQUAL( added.code(), kiapi::common::types::LibraryCommandStatus::LCS_OK );
    BOOST_CHECK_EQUAL( added.error_message(), "" );

    // The row is in the file, with the location as it was given and the type that fits the file
    std::string text = tableText();
    BOOST_CHECK( text.find( "(name \"Extra\")" ) != std::string::npos );
    BOOST_CHECK( text.find( "${KIPRJMOD}/Extra.kicad_sym" ) != std::string::npos );
    BOOST_CHECK( text.find( "(type \"KiCad\")" ) != std::string::npos );

    // The rows that were there stay
    BOOST_CHECK( text.find( "(name \"Device\")" ) != std::string::npos );
    BOOST_CHECK( text.find( "4xxx series symbols" ) != std::string::npos );

    // The manager knows the row and the library is loaded
    std::optional<LIBRARY_TABLE_ROW*> row =
            Pgm().GetLibraryManager().GetRow( LIBRARY_TABLE_TYPE::SYMBOL, wxS( "Extra" ),
                                              LIBRARY_TABLE_SCOPE::PROJECT );
    BOOST_REQUIRE( row );

    std::optional<LIB_STATUS> loaded = m_adapter->GetLibraryStatus( wxS( "Extra" ) );
    BOOST_REQUIRE( loaded );
    BOOST_CHECK( loaded->load_status == LOAD_STATUS::LOADED );

    // ... and its symbols can be searched for
    m_adapter->BlockUntilLoaded();
    API_HANDLER_SCH_LIBRARIES handler;
    kiapi::common::ApiRequest request = search( "resistor", { "Extra" } );
    API_RESULT                result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    SearchLibrariesResponse found;
    BOOST_REQUIRE( result->message().UnpackTo( &found ) );
    BOOST_REQUIRE_GE( found.items_size(), 1 );
    BOOST_CHECK_EQUAL( found.items( 0 ).library_nickname(), "Extra" );

    // Adding the same nickname again changes nothing
    std::string before = tableText();
    kiapi::common::types::LibraryCommandStatus again =
            status( edit( addCommand( "Extra", "${KIPRJMOD}/Device.kicad_sym" ) ) );
    BOOST_CHECK_EQUAL( again.code(), kiapi::common::types::LibraryCommandStatus::LCS_ALREADY_EXISTS );
    BOOST_CHECK_EQUAL( tableText(), before );
}


BOOST_AUTO_TEST_CASE( AddLibraryTableEntryKeepsARowWhoseFileIsMissing )
{
    // The row is written: the file may be created later
    kiapi::common::types::LibraryCommandStatus added =
            status( edit( addCommand( "Missing", "${KIPRJMOD}/Missing.kicad_sym" ) ) );
    BOOST_CHECK_EQUAL( added.code(), kiapi::common::types::LibraryCommandStatus::LCS_OK );
    BOOST_CHECK( tableText().find( "(name \"Missing\")" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( UpdateLibraryTableEntryChangesTheRow )
{
    copyDevice( "Other.kicad_sym" );

    UpdateLibraryTableEntry command;
    command.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    command.mutable_entry()->set_nickname( "Device" );
    command.mutable_entry()->set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
    command.mutable_entry()->set_uri( "${KIPRJMOD}/Other.kicad_sym" );
    command.mutable_entry()->set_description( "Moved" );
    ( *command.mutable_entry()->mutable_options() )["key"] = "value";

    BOOST_CHECK_EQUAL( status( edit( command ) ).code(), kiapi::common::types::LibraryCommandStatus::LCS_OK );

    std::string text = tableText();
    BOOST_CHECK( text.find( "${KIPRJMOD}/Other.kicad_sym" ) != std::string::npos );
    BOOST_CHECK( text.find( "\"Moved\"" ) != std::string::npos );
    BOOST_CHECK( text.find( "key=value" ) != std::string::npos );

    // The other row is untouched
    BOOST_CHECK( text.find( "4xxx series symbols" ) != std::string::npos );

    // A row that is not there cannot be changed
    command.mutable_entry()->set_nickname( "NoSuchLibrary" );
    BOOST_CHECK_EQUAL( status( edit( command ) ).code(),
                       kiapi::common::types::LibraryCommandStatus::LCS_NOT_FOUND );
}


BOOST_AUTO_TEST_CASE( DeleteLibraryTableEntryRemovesTheRow )
{
    DeleteLibraryTableEntry command;
    command.set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
    command.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    command.set_nickname( "Device" );

    BOOST_CHECK_EQUAL( status( edit( command ) ).code(), kiapi::common::types::LibraryCommandStatus::LCS_OK );

    std::string text = tableText();
    BOOST_CHECK( text.find( "(name \"Device\")" ) == std::string::npos );
    BOOST_CHECK( text.find( "(name \"4xxx\")" ) != std::string::npos );

    // The library file stays
    BOOST_CHECK( std::filesystem::exists( m_dir / "Device.kicad_sym" ) );

    BOOST_CHECK( !Pgm().GetLibraryManager().GetRow( LIBRARY_TABLE_TYPE::SYMBOL, wxS( "Device" ),
                                                    LIBRARY_TABLE_SCOPE::PROJECT ) );

    // Deleting it again finds nothing
    BOOST_CHECK_EQUAL( status( edit( command ) ).code(),
                       kiapi::common::types::LibraryCommandStatus::LCS_NOT_FOUND );
}


BOOST_AUTO_TEST_CASE( LibraryTableEditsRejectBadRequests )
{
    const std::string before = tableText();

    auto expectError = [&]( const API_RESULT& aResult, ApiStatusCode aCode )
    {
        BOOST_REQUIRE( !aResult.has_value() );
        BOOST_CHECK_EQUAL( aResult.error().status(), aCode );
    };

    AddLibraryTableEntry add = addCommand( "Extra", "${KIPRJMOD}/Extra.kicad_sym" );

    add.set_scope( kiapi::common::types::LibraryTableScope::LTS_BOTH );
    expectError( edit( add ), ApiStatusCode::AS_BAD_REQUEST );

    add.set_scope( kiapi::common::types::LibraryTableScope::LTS_UNKNOWN );
    expectError( edit( add ), ApiStatusCode::AS_BAD_REQUEST );

    add = addCommand( "", "${KIPRJMOD}/Extra.kicad_sym" );
    expectError( edit( add ), ApiStatusCode::AS_BAD_REQUEST );

    add = addCommand( "Bad:Name", "${KIPRJMOD}/Extra.kicad_sym" );
    expectError( edit( add ), ApiStatusCode::AS_BAD_REQUEST );

    add = addCommand( "Extra", "" );
    expectError( edit( add ), ApiStatusCode::AS_BAD_REQUEST );

    add = addCommand( "Extra", "${KIPRJMOD}/Extra.kicad_sym" );
    add.mutable_entry()->set_type( kiapi::common::types::LibraryType::LT_UNKNOWN );
    expectError( edit( add ), ApiStatusCode::AS_BAD_REQUEST );

    // The footprint handler answers for footprint tables; this one passes the request on
    add.mutable_entry()->set_type( kiapi::common::types::LibraryType::LT_FOOTPRINT );
    expectError( edit( add ), ApiStatusCode::AS_UNHANDLED );

    DeleteLibraryTableEntry del;
    del.set_type( kiapi::common::types::LibraryType::LT_FOOTPRINT );
    del.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    del.set_nickname( "Device" );
    expectError( edit( del ), ApiStatusCode::AS_UNHANDLED );

    BOOST_CHECK_EQUAL( tableText(), before );
}


BOOST_AUTO_TEST_CASE( LibraryTableEditsRespectReadOnlyTables )
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::permissions( m_dir / "sym-lib-table", fs::perms::owner_read, ec );
    BOOST_REQUIRE( !ec );

    // A superuser can write to it anyway; there is nothing to check then
    std::ofstream probe( m_dir / "sym-lib-table", std::ios::app );

    if( probe.is_open() )
    {
        probe.close();
        fs::permissions( m_dir / "sym-lib-table", fs::perms::owner_all, ec );
        BOOST_TEST_MESSAGE( "the table can be written in spite of its permissions; skipped" );
        return;
    }

    const std::string before = tableText();

    BOOST_CHECK_EQUAL( status( edit( addCommand( "Extra", "${KIPRJMOD}/Extra.kicad_sym" ) ) ).code(),
                       kiapi::common::types::LibraryCommandStatus::LCS_READ_ONLY );

    DeleteLibraryTableEntry del;
    del.set_type( kiapi::common::types::LibraryType::LT_SYMBOL );
    del.set_scope( kiapi::common::types::LibraryTableScope::LTS_PROJECT );
    del.set_nickname( "Device" );
    BOOST_CHECK_EQUAL( status( edit( del ) ).code(), kiapi::common::types::LibraryCommandStatus::LCS_READ_ONLY );

    BOOST_CHECK_EQUAL( tableText(), before );
    fs::permissions( m_dir / "sym-lib-table", fs::perms::owner_all, ec );
}


BOOST_AUTO_TEST_SUITE_END()
