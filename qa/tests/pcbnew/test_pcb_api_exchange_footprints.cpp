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

#include <api/api_handler_pcb.h>
#include <api/api_utils.h>
#include <api/board/board_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/headless_pcb_context.h>
#include <board.h>
#include <component_classes/component_class_manager.h>
#include <pcb_group.h>
#include <zone.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <libraries/library_manager.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_field.h>
#include <pgm_base.h>
#include <project_pcb.h>
#include <settings/settings_manager.h>

#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/textfile.h>
#include <wx/utils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>


namespace
{

using kiapi::common::ApiStatusCode;
using namespace kiapi::board::commands;

const char* const NEW_MODEL = "${KIPRJMOD}/models/edited.step";


/**
 * A project whose footprint table lists a copy of qa/data/libraries/Resistor_SMD.pretty and a board
 * of two 0603 resistors: R1 at 90 degrees on the front side and R2 at 37 degrees on the back side.
 */
struct API_EXCHANGE_FOOTPRINTS_FIXTURE
{
    API_EXCHANGE_FOOTPRINTS_FIXTURE()
    {
        std::error_code ec;
        m_dir = std::filesystem::temp_directory_path( ec )
                / std::filesystem::path( wxString::Format( wxS( "kicad_qa_apixfp_%lu_%p" ), wxGetProcessId(),
                                                           (void*) this ).ToStdString() );
        std::filesystem::create_directories( m_dir / "Resistor_SMD.pretty", ec );

        wxFileName source( wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ), wxEmptyString );
        source.AppendDir( wxS( "libraries" ) );

        wxFileName pro( source.GetPath(), wxS( "test_project.kicad_pro" ) );
        pro.AppendDir( wxS( "test_project" ) );
        BOOST_REQUIRE( wxCopyFile( pro.GetFullPath(), wxString( ( m_dir / "fp_project.kicad_pro" ).string() ) ) );

        wxString pretty = wxFileName( source.GetPath(), wxEmptyString ).GetPath() + wxFileName::GetPathSeparator()
                          + wxS( "Resistor_SMD.pretty" );
        wxDir    dir( pretty );
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
        m_adapter->AsyncLoad();
        m_adapter->BlockUntilLoaded();

        auto board = std::make_unique<BOARD>();
        board->SetFileName( wxString( ( m_dir / "exchange.kicad_pcb" ).string() ) );
        m_board = board.get();
        m_context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( board ), m_project, nullptr );

        m_r1 = addResistor( "R1", VECTOR2I( 10000000, 20000000 ), 90.0, false );
        m_r2 = addResistor( "R2", VECTOR2I( 30000000, 20000000 ), 37.0, true );
        m_r2->SetLocked( true );
    }

    ~API_EXCHANGE_FOOTPRINTS_FIXTURE()
    {
        m_context.reset();
        Pgm().GetSettingsManager().UnloadProject( m_project, false );

        std::error_code ec;
        std::filesystem::remove_all( m_dir, ec );
    }

    FOOTPRINT* addResistor( const wxString& aRef, const VECTOR2I& aPos, double aAngle, bool aBack )
    {
        FOOTPRINT* fp = m_adapter->LoadFootprint( wxS( "Resistor_SMD" ), wxS( "R_0603_1608Metric" ), false );
        BOOST_REQUIRE( fp );

        fp->SetParent( m_board );
        fp->SetReference( aRef );
        fp->SetPosition( aPos );

        if( aBack )
            fp->SetLayerAndFlip( B_Cu );

        fp->SetOrientationDegrees( aAngle );

        for( PAD* pad : fp->Pads() )
        {
            wxString netName = wxString::Format( wxS( "%s_%s" ), aRef, pad->GetNumber() );
            NETINFO_ITEM* net = new NETINFO_ITEM( m_board, netName );
            m_board->Add( net );
            pad->SetNet( net );
        }

        m_board->Add( fp );
        return fp;
    }

    /// Replace the model of R_0603_1608Metric in the library on disk and make its timestamp move forward
    void editLibraryModel()
    {
        editLibrary( "${KICAD8_3DMODEL_DIR}/Resistor_SMD.3dshapes/R_0603_1608Metric.wrl", NEW_MODEL );
    }

    /// Replace the first occurrence of aOld in R_0603_1608Metric on disk and move its timestamp forward
    void editLibrary( const std::string& aOld, const std::string& aNew )
    {
        std::filesystem::path file = m_dir / "Resistor_SMD.pretty" / "R_0603_1608Metric.kicad_mod";

        std::string text;
        {
            std::ifstream in( file );
            text.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        }

        size_t at = text.find( aOld );
        BOOST_REQUIRE( at != std::string::npos );
        text.replace( at, aOld.size(), aNew );

        {
            std::ofstream out( file, std::ios::trunc );
            out << text;
        }

        std::error_code ec;
        std::filesystem::last_write_time( file, std::filesystem::last_write_time( file, ec )
                                                        + std::chrono::seconds( 10 ), ec );
        BOOST_REQUIRE( !ec );
    }

    kiapi::common::ApiRequest request( const std::vector<FOOTPRINT*>& aFootprints, const LIB_ID* aLibId,
                                       bool aDryRun, const ExchangeFootprintsOptions* aOptions = nullptr )
    {
        ExchangeFootprints command;
        command.mutable_header()->mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
        command.mutable_header()->mutable_document()->set_board_filename(
                wxFileName( m_board->GetFileName() ).GetFullName().ToStdString() );

        for( FOOTPRINT* fp : aFootprints )
            command.add_footprints()->set_value( fp->m_Uuid.AsStdString() );

        if( aLibId )
            PackLibId( command.mutable_lib_id(), *aLibId );

        if( aOptions )
            *command.mutable_options() = *aOptions;

        command.set_dry_run( aDryRun );

        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return request;
    }

    ExchangeFootprintsResponse exchange( const std::vector<FOOTPRINT*>& aFootprints, const LIB_ID* aLibId = nullptr,
                                         bool aDryRun = false, const ExchangeFootprintsOptions* aOptions = nullptr )
    {
        API_HANDLER_PCB         handler( m_context );
        kiapi::common::ApiRequest req = request( aFootprints, aLibId, aDryRun, aOptions );
        API_RESULT              result = handler.Handle( req );

        BOOST_REQUIRE( result.has_value() );
        BOOST_REQUIRE_EQUAL( result->status().status(), ApiStatusCode::AS_OK );

        ExchangeFootprintsResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response;
    }

    FOOTPRINT* byUuid( const KIID& aId )
    {
        BOARD_ITEM* item = m_board->ResolveItem( aId, true );
        BOOST_REQUIRE( item );
        BOOST_REQUIRE_EQUAL( (int) item->Type(), (int) PCB_FOOTPRINT_T );
        return static_cast<FOOTPRINT*>( item );
    }

    std::filesystem::path                 m_dir;
    PROJECT*                              m_project = nullptr;
    FOOTPRINT_LIBRARY_ADAPTER*            m_adapter = nullptr;
    BOARD*                                m_board = nullptr;
    std::shared_ptr<HEADLESS_PCB_CONTEXT> m_context;
    FOOTPRINT*                            m_r1 = nullptr;
    FOOTPRINT*                            m_r2 = nullptr;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerExchangeFootprints, API_EXCHANGE_FOOTPRINTS_FIXTURE )


BOOST_AUTO_TEST_CASE( ExchangeFootprintsPicksUpEditedLibrary )
{
    // The board footprints carry the library's original model, so an update has nothing to do yet
    ExchangeFootprintsResponse before = exchange( { m_r1 }, nullptr, true );
    BOOST_REQUIRE_EQUAL( before.results_size(), 1 );
    BOOST_CHECK_EQUAL( before.results( 0 ).status(), FootprintExchangeStatus::FES_UNCHANGED );

    editLibraryModel();

    // First as a dry run: the new model shows as a difference and nothing changes
    ExchangeFootprintsResponse dry = exchange( { m_r1, m_r2 }, nullptr, true );
    BOOST_REQUIRE_EQUAL( dry.results_size(), 2 );

    for( const FootprintExchangeResult& r : dry.results() )
    {
        BOOST_CHECK_EQUAL( r.status(), FootprintExchangeStatus::FES_UPDATED );
        BOOST_CHECK_MESSAGE( r.message().find( "3D models differ" ) != std::string::npos, r.message() );
        BOOST_CHECK( !r.has_item() );
    }

    BOOST_CHECK_EQUAL( m_board->Footprints().size(), 2u );
    BOOST_CHECK_EQUAL( m_r1->Models().front().m_Filename, wxS( "${KICAD8_3DMODEL_DIR}/Resistor_SMD.3dshapes/"
                                                                "R_0603_1608Metric.wrl" ) );

    const KIID    r1Id = m_r1->m_Uuid;
    const KIID    r2Id = m_r2->m_Uuid;
    const KIID    r1Pad1 = m_r1->Pads().front()->m_Uuid;
    const VECTOR2I r1Pos = m_r1->GetPosition();
    const VECTOR2I r2Pos = m_r2->GetPosition();

    ExchangeFootprintsResponse real = exchange( { m_r1, m_r2 } );
    BOOST_REQUIRE_EQUAL( real.results_size(), 2 );

    for( const FootprintExchangeResult& r : real.results() )
    {
        BOOST_CHECK_EQUAL( r.status(), FootprintExchangeStatus::FES_UPDATED );
        BOOST_CHECK( r.has_item() );
        BOOST_CHECK_EQUAL( r.lib_id().entry_name(), "R_0603_1608Metric" );
    }

    BOOST_CHECK_EQUAL( m_board->Footprints().size(), 2u );

    FOOTPRINT* r1 = byUuid( r1Id );
    FOOTPRINT* r2 = byUuid( r2Id );

    BOOST_REQUIRE_EQUAL( r1->Models().size(), 1u );
    BOOST_CHECK_EQUAL( r1->Models().front().m_Filename, wxString::FromUTF8( NEW_MODEL ) );
    BOOST_CHECK_EQUAL( r2->Models().front().m_Filename, wxString::FromUTF8( NEW_MODEL ) );

    // Pose, side, lock, reference and nets are kept
    BOOST_CHECK( r1->GetPosition() == r1Pos );
    BOOST_CHECK( r2->GetPosition() == r2Pos );
    BOOST_CHECK_CLOSE( r1->GetOrientationDegrees(), 90.0, 1e-6 );
    BOOST_CHECK_CLOSE( r2->GetOrientationDegrees(), 37.0, 1e-6 );
    BOOST_CHECK_EQUAL( (int) r1->GetLayer(), (int) F_Cu );
    BOOST_CHECK_EQUAL( (int) r2->GetLayer(), (int) B_Cu );
    BOOST_CHECK( !r1->IsLocked() );
    BOOST_CHECK( r2->IsLocked() );
    BOOST_CHECK_EQUAL( r1->GetReference(), wxS( "R1" ) );
    BOOST_CHECK_EQUAL( r2->GetReference(), wxS( "R2" ) );
    BOOST_CHECK_EQUAL( r1->Pads().front()->m_Uuid.AsString(), r1Pad1.AsString() );
    BOOST_CHECK_EQUAL( r1->FindPadByNumber( wxS( "1" ) )->GetNetname(), wxS( "R1_1" ) );
    BOOST_CHECK_EQUAL( r1->FindPadByNumber( wxS( "2" ) )->GetNetname(), wxS( "R1_2" ) );
    BOOST_CHECK_EQUAL( r2->FindPadByNumber( wxS( "1" ) )->GetNetname(), wxS( "R2_1" ) );

    // Updated again, nothing is left to do
    ExchangeFootprintsResponse again = exchange( { r1, r2 } );
    BOOST_CHECK_EQUAL( again.results( 0 ).status(), FootprintExchangeStatus::FES_UNCHANGED );
    BOOST_CHECK_EQUAL( again.results( 1 ).status(), FootprintExchangeStatus::FES_UNCHANGED );
}


BOOST_AUTO_TEST_CASE( ExchangeFootprintsDryRunChangesNothing )
{
    editLibraryModel();

    const KIID     r2Id = m_r2->m_Uuid;
    const VECTOR2I r2Pos = m_r2->GetPosition();

    ExchangeFootprintsResponse dry = exchange( { m_r2 }, nullptr, true );
    BOOST_REQUIRE_EQUAL( dry.results_size(), 1 );
    BOOST_CHECK_EQUAL( dry.results( 0 ).status(), FootprintExchangeStatus::FES_UPDATED );

    // The very same footprint object is still on the board, untouched
    BOOST_CHECK( byUuid( r2Id ) == m_r2 );
    BOOST_CHECK( m_r2->GetPosition() == r2Pos );
    BOOST_CHECK_EQUAL( m_r2->Models().front().m_Filename,
                       wxS( "${KICAD8_3DMODEL_DIR}/Resistor_SMD.3dshapes/R_0603_1608Metric.wrl" ) );
}


BOOST_AUTO_TEST_CASE( ExchangeFootprintsChangesLibId )
{
    const KIID r1Id = m_r1->m_Uuid;
    const KIID r2Id = m_r2->m_Uuid;
    LIB_ID     target( wxS( "Resistor_SMD" ), wxS( "R_0402_1005Metric" ) );

    ExchangeFootprintsResponse response = exchange( { m_r1, m_r2 }, &target );
    BOOST_REQUIRE_EQUAL( response.results_size(), 2 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), FootprintExchangeStatus::FES_UPDATED );
    BOOST_CHECK_EQUAL( response.results( 0 ).lib_id().entry_name(), "R_0402_1005Metric" );

    FOOTPRINT* r1 = byUuid( r1Id );
    FOOTPRINT* r2 = byUuid( r2Id );

    BOOST_CHECK_EQUAL( r1->GetFPID().GetLibItemName().wx_str(), wxS( "R_0402_1005Metric" ) );
    BOOST_CHECK_EQUAL( r2->GetFPID().GetLibItemName().wx_str(), wxS( "R_0402_1005Metric" ) );
    BOOST_CHECK_EQUAL( r1->GetReference(), wxS( "R1" ) );
    BOOST_CHECK_EQUAL( r1->FindPadByNumber( wxS( "1" ) )->GetNetname(), wxS( "R1_1" ) );
    BOOST_CHECK_EQUAL( r1->FindPadByNumber( wxS( "2" ) )->GetNetname(), wxS( "R1_2" ) );
    BOOST_CHECK_EQUAL( r2->FindPadByNumber( wxS( "2" ) )->GetNetname(), wxS( "R2_2" ) );
    BOOST_CHECK_CLOSE( r1->GetOrientationDegrees(), 90.0, 1e-6 );
    BOOST_CHECK_EQUAL( (int) r2->GetLayer(), (int) B_Cu );
    BOOST_CHECK_CLOSE( r2->GetOrientationDegrees(), 37.0, 1e-6 );
    BOOST_CHECK( r2->IsLocked() );

    // The 0402 pads are smaller than the 0603 ones
    BOOST_CHECK_LT( r1->FindPadByNumber( wxS( "1" ) )->GetSize( F_Cu ).x, 1000000 );
}


BOOST_AUTO_TEST_CASE( ExchangeFootprintsKeepsBoardModelsWhenAsked )
{
    // Give R1 a model of its own that the library does not know
    FP_3DMODEL own;
    own.m_Filename = wxS( "${KIPRJMOD}/models/own.step" );
    own.m_Offset = { 1.5, 0.0, 0.2 };
    m_r1->Models().clear();
    m_r1->Models().push_back( own );

    ExchangeFootprintsOptions options;
    options.set_reset_3d_models( false );

    // The library gets a new model and a wider pad, so the footprint is really exchanged
    editLibraryModel();
    editLibrary( "(size 0.8 0.95)", "(size 0.9 0.95)" );

    const KIID                 r1Id = m_r1->m_Uuid;
    FOOTPRINT*                 old = m_r1;
    ExchangeFootprintsResponse kept = exchange( { m_r1 }, nullptr, false, &options );
    BOOST_CHECK_EQUAL( kept.results( 0 ).status(), FootprintExchangeStatus::FES_UPDATED );
    BOOST_CHECK_MESSAGE( kept.results( 0 ).message().find( "3D models differ" ) == std::string::npos,
                         kept.results( 0 ).message() );

    FOOTPRINT* r1 = byUuid( r1Id );
    BOOST_CHECK( r1 != old );
    BOOST_CHECK_CLOSE( r1->FindPadByNumber( wxS( "1" ) )->GetSize( F_Cu ).x, 900000.0, 1e-6 );
    BOOST_REQUIRE_EQUAL( r1->Models().size(), 1u );
    BOOST_CHECK_EQUAL( r1->Models().front().m_Filename, own.m_Filename );
    BOOST_CHECK_CLOSE( r1->Models().front().m_Offset.x, 1.5, 1e-9 );

    // With the default the library's model replaces it
    m_r1 = r1;
    ExchangeFootprintsResponse response = exchange( { r1 } );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), FootprintExchangeStatus::FES_UPDATED );
    FOOTPRINT* again = byUuid( r1Id );
    BOOST_REQUIRE_EQUAL( again->Models().size(), 1u );
    BOOST_CHECK_EQUAL( again->Models().front().m_Filename, wxString::FromUTF8( NEW_MODEL ) );
}


BOOST_AUTO_TEST_CASE( ExchangeFootprintsResetsWhatWasChangedOnTheBoard )
{
    // The reference of R1 was moved on the board; the library did not change
    PCB_FIELD&     reference = m_r1->Reference();
    const VECTOR2I libraryPos = reference.GetFPRelativePosition();
    reference.SetFPRelativePosition( libraryPos + VECTOR2I( 2000000, 0 ) );

    const KIID r1Id = m_r1->m_Uuid;

    // An update with the defaults keeps the moved text and records nothing
    ExchangeFootprintsResponse plain = exchange( { m_r1 } );
    BOOST_CHECK_EQUAL( plain.results( 0 ).status(), FootprintExchangeStatus::FES_UNCHANGED );
    BOOST_CHECK( byUuid( r1Id ) == m_r1 );

    // Asked to reset the text positions, it puts the reference back where the library has it
    ExchangeFootprintsOptions options;
    options.set_reset_text_positions( true );

    ExchangeFootprintsResponse reset = exchange( { m_r1 }, nullptr, false, &options );
    BOOST_CHECK_EQUAL( reset.results( 0 ).status(), FootprintExchangeStatus::FES_UPDATED );

    FOOTPRINT* r1 = byUuid( r1Id );
    BOOST_CHECK( r1->Reference().GetFPRelativePosition() == libraryPos );
    BOOST_CHECK_EQUAL( r1->GetReference(), wxS( "R1" ) );
}


BOOST_AUTO_TEST_CASE( ExchangeFootprintsFindsFootprintsWithoutNickname )
{
    // A footprint of an old board names no library: it is looked for in every library
    m_r1->SetFPID( LIB_ID( wxEmptyString, wxS( "R_0603_1608Metric" ) ) );

    ExchangeFootprintsOptions options;
    options.set_reset_text_positions( true );

    ExchangeFootprintsResponse response = exchange( { m_r1 }, nullptr, false, &options );
    BOOST_REQUIRE_EQUAL( response.results_size(), 1 );
    BOOST_CHECK_MESSAGE( response.results( 0 ).status() != FootprintExchangeStatus::FES_NOT_FOUND,
                         response.results( 0 ).message() );
    BOOST_CHECK( response.results( 0 ).has_item() );
}


BOOST_AUTO_TEST_CASE( ExchangeFootprintsReportsPerItemFailures )
{
    LIB_ID missing( wxS( "Resistor_SMD" ), wxS( "No_Such_Footprint" ) );
    ExchangeFootprintsResponse response = exchange( { m_r1 }, &missing );
    BOOST_REQUIRE_EQUAL( response.results_size(), 1 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), FootprintExchangeStatus::FES_NOT_FOUND );
    BOOST_CHECK( !response.results( 0 ).message().empty() );

    LIB_ID noLibrary( wxS( "No_Such_Library" ), wxS( "R_0603_1608Metric" ) );
    response = exchange( { m_r1 }, &noLibrary );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), FootprintExchangeStatus::FES_NOT_FOUND );

    // A footprint that is not on the board, and a board item that is not a footprint
    ExchangeFootprints command;
    command.mutable_header()->mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_header()->mutable_document()->set_board_filename(
            wxFileName( m_board->GetFileName() ).GetFullName().ToStdString() );
    command.add_footprints()->set_value( KIID().AsStdString() );
    command.add_footprints()->set_value( m_r1->Pads().front()->m_Uuid.AsStdString() );
    command.add_footprints()->set_value( m_r1->m_Uuid.AsStdString() );
    command.add_footprints()->set_value( m_r1->m_Uuid.AsStdString() );

    kiapi::common::ApiRequest req;
    req.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( req.mutable_message()->PackFrom( command ) );

    API_HANDLER_PCB handler( m_context );
    API_RESULT      result = handler.Handle( req );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.results_size(), 4 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), FootprintExchangeStatus::FES_NONEXISTENT );
    BOOST_CHECK_EQUAL( response.results( 0 ).message(), "no such item" );
    BOOST_CHECK_EQUAL( response.results( 1 ).status(), FootprintExchangeStatus::FES_NONEXISTENT );
    BOOST_CHECK_EQUAL( response.results( 1 ).message(), "not a footprint" );
    BOOST_CHECK_EQUAL( response.results( 2 ).status(), FootprintExchangeStatus::FES_UNCHANGED );
    BOOST_CHECK_EQUAL( response.results( 3 ).status(), FootprintExchangeStatus::FES_NONEXISTENT );
    BOOST_CHECK_EQUAL( response.results( 3 ).message(), "listed more than once" );

    // Nothing was touched
    BOOST_CHECK_EQUAL( m_board->Footprints().size(), 2u );
}


BOOST_AUTO_TEST_CASE( ExchangeFootprintsRejectsBadRequests )
{
    API_HANDLER_PCB handler( m_context );

    // No footprints
    kiapi::common::ApiRequest empty = request( {}, nullptr, false );
    API_RESULT                result = handler.Handle( empty );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_BAD_REQUEST );

    // A lib_id without an entry name
    LIB_ID incomplete( wxS( "Resistor_SMD" ), wxEmptyString );
    kiapi::common::ApiRequest bad = request( { m_r1 }, &incomplete, false );
    result = handler.Handle( bad );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_BAD_REQUEST );

    // Another document is for another handler
    ExchangeFootprints command;
    command.mutable_header()->mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_SCHEMATIC );
    command.add_footprints()->set_value( m_r1->m_Uuid.AsStdString() );

    kiapi::common::ApiRequest req;
    req.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( req.mutable_message()->PackFrom( command ) );

    result = handler.Handle( req );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( PlaceFootprintFromLibrarySeesEditedLibrary )
{
    editLibraryModel();

    PlaceFootprintFromLibrary command;
    command.mutable_header()->mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_header()->mutable_document()->set_board_filename(
            wxFileName( m_board->GetFileName() ).GetFullName().ToStdString() );
    PackLibId( command.mutable_lib_id(), LIB_ID( wxS( "Resistor_SMD" ), wxS( "R_0603_1608Metric" ) ) );
    command.mutable_position()->set_x_nm( 0 );
    command.mutable_position()->set_y_nm( 0 );

    kiapi::common::ApiRequest req;
    req.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( req.mutable_message()->PackFrom( command ) );

    API_HANDLER_PCB handler( m_context );
    API_RESULT      result = handler.Handle( req );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), ApiStatusCode::AS_OK );

    kiapi::common::commands::PlaceFromLibraryResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    kiapi::board::types::FootprintInstance placed;
    BOOST_REQUIRE( response.item().UnpackTo( &placed ) );

    bool found = false;

    for( const google::protobuf::Any& any : placed.definition().items() )
    {
        kiapi::board::types::Footprint3DModel model;

        if( any.UnpackTo( &model ) )
        {
            BOOST_CHECK_EQUAL( model.filename(), NEW_MODEL );
            found = true;
        }
    }

    BOOST_CHECK( found );
}


BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE( ApiBoardPlacement, API_EXCHANGE_FOOTPRINTS_FIXTURE )

BOOST_AUTO_TEST_CASE( NativeGeometryAndAtomicPlacement )
{
    API_HANDLER_PCB handler( m_context );
    GetBoardPlacementGeometry query;
    query.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    query.mutable_board()->set_board_filename( "exchange.kicad_pcb" );

    auto invoke = [&]( const google::protobuf::Message& command )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa.placement" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return handler.Handle( request );
    };
    auto read = [&]()
    {
        auto result = invoke( query );
        BOOST_REQUIRE( result.has_value() );
        BOOST_REQUIRE_EQUAL( result->status().status(), ApiStatusCode::AS_OK );
        BoardPlacementGeometryResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response;
    };
    auto before = read();
    BOOST_CHECK_EQUAL( before.footprints_size(), 2 );
    BOOST_CHECK( !before.revision().empty() );
    BOOST_CHECK_EQUAL( read().revision(), before.revision() );

    for( const auto& footprint : before.footprints() )
    {
        BOOST_CHECK_GT( footprint.courtyard_size(), 0 );
        BOOST_CHECK_EQUAL( footprint.pads_size(), 2 );
        BOOST_CHECK_GT( footprint.pads( 0 ).copper_size(), 0 );
    }

    ApplyBoardPlacement apply;
    *apply.mutable_board() = query.board();
    apply.set_expected_revision( before.revision() );
    apply.set_expected_project_revision( before.project_revision() );
    apply.set_result_revision( "invalid" );
    auto* pose = apply.add_poses();
    pose->mutable_id()->set_value( m_r1->m_Uuid.AsStdString() );
    pose->mutable_position()->set_x_nm( 20000000 );
    pose->mutable_position()->set_y_nm( 30000000 );
    pose->mutable_orientation()->set_value_degrees( 180 );
    pose->set_layer( kiapi::board::types::BL_F_Cu );
    const VECTOR2I original = m_r1->GetPosition();
    const bool modified = m_context->IsContentModified();
    auto rejected = invoke( apply );
    BOOST_REQUIRE( !rejected.has_value() );
    BOOST_CHECK_EQUAL( rejected.error().status(), ApiStatusCode::AS_BAD_REQUEST );
    BOOST_CHECK( m_r1->GetPosition() == original );
    BOOST_CHECK_EQUAL( m_context->IsContentModified(), modified );
    BOOST_CHECK_EQUAL( read().revision(), before.revision() );

    apply.set_evaluate( true );
    auto evaluated = invoke( apply );
    BOOST_REQUIRE( evaluated.has_value() );
    BOOST_REQUIRE_EQUAL( evaluated->status().status(), ApiStatusCode::AS_OK );
    ApplyBoardPlacementResponse evaluation;
    BOOST_REQUIRE( evaluated->message().UnpackTo( &evaluation ) );
    BOOST_CHECK_EQUAL( read().revision(), before.revision() );
    apply.set_evaluate( false );
    apply.set_result_revision( evaluation.revision() );
    auto applied = invoke( apply );
    BOOST_REQUIRE( applied.has_value() );
    BOOST_CHECK_EQUAL( applied->status().status(), ApiStatusCode::AS_OK );
    BOOST_CHECK_EQUAL( read().revision(), evaluation.revision() );
    auto stale = invoke( apply );
    BOOST_REQUIRE( !stale.has_value() );
    BOOST_CHECK_EQUAL( stale.error().status(), ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( ComponentClassAreasAndIndependentDesignBlockInstances )
{
    m_r1->SetStaticComponentClass( m_board->GetComponentClassManager().GetEffectiveStaticComponentClass(
            { wxS( "analog" ) } ) );
    ZONE* area = new ZONE( m_board );
    area->SetIsRuleArea( true );
    area->SetLayer( F_Cu );
    area->SetPlacementAreaEnabled( true );
    area->SetPlacementAreaSourceType( PLACEMENT_SOURCE_T::COMPONENT_CLASS );
    area->SetPlacementAreaSource( "analog" );
    area->Outline()->NewOutline();
    area->Outline()->Append( 0, 0 );
    area->Outline()->Append( 50000000, 0 );
    area->Outline()->Append( 50000000, 50000000 );
    area->Outline()->Append( 0, 50000000 );
    m_board->Add( area );
    LIB_ID blockId;
    blockId.Parse( "LocalBlocks:Channel" );

    for( FOOTPRINT* fp : { m_r1, m_r2 } )
    {
        PCB_GROUP* group = new PCB_GROUP( m_board );
        group->SetName( fp->GetReference() );
        group->SetDesignBlockLibId( blockId );
        group->AddItem( fp );
        m_board->Add( group );
    }

    GetBoardPlacementGeometry command;
    command.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename( "exchange.kicad_pcb" );
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa.placement" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    API_HANDLER_PCB handler( m_context );
    auto result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    BoardPlacementGeometryResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.areas_size(), 1 );
    BOOST_REQUIRE_EQUAL( response.areas( 0 ).members_size(), 1 );
    BOOST_CHECK_EQUAL( response.areas( 0 ).members( 0 ).value(), m_r1->m_Uuid.AsStdString() );
    BOOST_REQUIRE_EQUAL( response.groups_size(), 2 );
    std::set<std::string> members;

    for( const auto& packed : response.groups() )
    {
        kiapi::board::types::Group group;
        BOOST_REQUIRE( packed.UnpackTo( &group ) );
        BOOST_CHECK_EQUAL( group.lib_id().library_nickname(), "LocalBlocks" );
        BOOST_CHECK_EQUAL( group.lib_id().entry_name(), "Channel" );
        BOOST_REQUIRE_EQUAL( group.items_size(), 1 );
        members.insert( group.items( 0 ).value() );
    }

    BOOST_CHECK_EQUAL( members.size(), 2 );
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE( ApiBoardDrag, API_EXCHANGE_FOOTPRINTS_FIXTURE )

BOOST_AUTO_TEST_CASE( AtomicRoutingTranslationFingerprintAndRollback )
{
    API_HANDLER_PCB handler( m_context );
    auto invoke = [&]( const google::protobuf::Message& command )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa.drag" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return handler.Handle( request );
    };
    GetBoardPlacementGeometry query;
    query.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    query.mutable_board()->set_board_filename( "exchange.kicad_pcb" );
    auto read = [&]()
    {
        auto result = invoke( query );
        BOOST_REQUIRE( result.has_value() );
        BoardPlacementGeometryResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response;
    };
    m_r1->SetValue( "Text (with parentheses) and \"quotes\"" );
    auto original = read();
    const bool dirty = m_context->IsContentModified();
    const VECTOR2I origin = m_r1->GetPosition();
    ApplyBoardRoutingChanges command;
    *command.mutable_board() = query.board();
    command.set_expected_revision( original.revision() );
    command.set_expected_project_revision( original.project_revision() );
    command.set_result_fingerprint( "forged" );
    auto* move = command.add_translations();
    move->mutable_id()->set_value( m_r1->m_Uuid.AsStdString() );
    move->mutable_delta()->set_y_nm( 2000000 );
    auto rejected = invoke( command );
    BOOST_REQUIRE( !rejected.has_value() );
    BOOST_CHECK_EQUAL( read().revision(), original.revision() );
    BOOST_CHECK_EQUAL( m_context->IsContentModified(), dirty );
    command.set_evaluate( true );
    auto evaluation = invoke( command );
    BOOST_REQUIRE( evaluation.has_value() );
    ApplyBoardRoutingChangesResponse expected;
    BOOST_REQUIRE( evaluation->message().UnpackTo( &expected ) );
    BOOST_CHECK_EQUAL( read().revision(), original.revision() );
    command.set_evaluate( false );
    command.set_result_fingerprint( expected.result_fingerprint() );
    BOOST_REQUIRE( invoke( command ).has_value() );
    BOOST_CHECK_EQUAL( read().revision(), expected.revision() );
    BOOST_CHECK( m_r1->GetPosition() == origin + VECTOR2I( 0, 2000000 ) );
    BOOST_CHECK( !invoke( command ).has_value() );
    command.set_expected_revision( expected.revision() );
    command.mutable_translations( 0 )->mutable_id()->set_value( m_r2->m_Uuid.AsStdString() );
    BOOST_CHECK( !invoke( command ).has_value() ); // locked source
    BOOST_CHECK_EQUAL( read().revision(), expected.revision() );
    command.clear_translations();
    kiapi::board::types::Track unknown;
    unknown.mutable_id()->set_value( KIID().AsStdString() );
    unknown.mutable_net()->set_name( "must-not-create-this-net" );
    command.add_add()->PackFrom( unknown );
    BOOST_CHECK( !invoke( command ).has_value() );
    BOOST_CHECK( m_board->FindNet( "must-not-create-this-net" ) == nullptr );
    BOOST_CHECK_EQUAL( read().revision(), expected.revision() );
}

BOOST_AUTO_TEST_CASE( NativeComponentDragAndLockedRefusal )
{
    API_HANDLER_PCB handler( m_context );
    ComputeBoardDrag command;
    command.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename( "exchange.kicad_pcb" );
    command.add_items()->set_value( m_r2->m_Uuid.AsStdString() );
    command.mutable_anchor()->set_x_nm( m_r2->GetPosition().x );
    command.mutable_anchor()->set_y_nm( m_r2->GetPosition().y );
    *command.mutable_destination() = command.anchor();
    command.mutable_destination()->set_y_nm( command.anchor().y_nm() + 1000000 );
    auto invoke = [&]()
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa.drag" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return handler.Handle( request );
    };
    BOOST_CHECK( !invoke().has_value() );
    const VECTOR2I origin = m_r1->GetPosition();
    command.mutable_items( 0 )->set_value( m_r1->m_Uuid.AsStdString() );
    command.mutable_anchor()->set_x_nm( origin.x );
    command.mutable_anchor()->set_y_nm( origin.y );
    *command.mutable_destination() = command.anchor();
    command.mutable_destination()->set_y_nm( origin.y + 1000000 );
    auto result = invoke();
    BOOST_REQUIRE( result.has_value() );
    ComputeBoardDragResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK( response.success() );
    BOOST_CHECK( m_r1->GetPosition() == origin + VECTOR2I( 0, 1000000 ) );
}

BOOST_AUTO_TEST_CASE( ZoneCreationKeepsNativeDefaults )
{
    ZONE defaults( m_board );
    ZONE restored( m_board );
    kiapi::board::types::Zone minimal;
    minimal.set_type( kiapi::board::types::ZT_COPPER );
    minimal.add_layers( kiapi::board::types::BL_F_Cu );
    auto* poly = minimal.mutable_outline()->add_polygons();
    for( const VECTOR2I point : { VECTOR2I( 0, 0 ), VECTOR2I( 10000000, 0 ),
                                  VECTOR2I( 10000000, 10000000 ), VECTOR2I( 0, 10000000 ) } )
    {
        auto* p = poly->mutable_outline()->add_nodes()->mutable_point();
        p->set_x_nm( point.x );
        p->set_y_nm( point.y );
    }
    google::protobuf::Any packed;
    packed.PackFrom( minimal );
    BOOST_REQUIRE( restored.Deserialize( packed ) );
    google::protobuf::Any expected;
    defaults.Serialize( expected );
    restored.Serialize( packed );
    kiapi::board::types::Zone reference;
    kiapi::board::types::Zone actual;
    BOOST_REQUIRE( expected.UnpackTo( &reference ) );
    BOOST_REQUIRE( packed.UnpackTo( &actual ) );
    BOOST_CHECK_EQUAL( actual.border().pitch().value_nm(), reference.border().pitch().value_nm() );
    BOOST_CHECK_EQUAL( actual.copper_settings().connection().zone_connection(),
                       reference.copper_settings().connection().zone_connection() );
    BOOST_CHECK_EQUAL( actual.copper_settings().connection().thermal_spokes().gap().value_nm(),
                       reference.copper_settings().connection().thermal_spokes().gap().value_nm() );
    BOOST_CHECK_EQUAL( actual.copper_settings().min_thickness().value_nm(),
                       reference.copper_settings().min_thickness().value_nm() );
    BOOST_CHECK_GT( actual.border().pitch().value_nm(), 0 );
    BOOST_CHECK_GT( actual.copper_settings().min_thickness().value_nm(), 0 );
}

BOOST_AUTO_TEST_SUITE_END()
