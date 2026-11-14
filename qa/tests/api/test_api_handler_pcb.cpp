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

#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <wx/filename.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <pcbnew_utils/board_test_utils.h>

#include <board.h>
#include <board_design_settings.h>
#include <board_connected_item.h>
#include <connectivity/connectivity_algo.h>
#include <connectivity/connectivity_data.h>
#include <drc/drc_engine.h>
#include <drc/drc_exclusion.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_marker.h>
#include <pcb_generator.h>
#include <pcb_track.h>
#include <pgm_base.h>
#include <settings/settings_manager.h>
#include <zone.h>
#include <zone_filler.h>

#include <api/api_handler_common.h>
#include <api/api_handler_pcb.h>
#include <api/api_pcb_utils.h>
#include <api/api_task_scheduler.h>
#include <api/headless_pcb_context.h>
#include <api/board/board_commands.pb.h>
#include <api/board/board_rules.pb.h>
#include <api/board/board_tasks.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/tasks.pb.h>
#include <api/common/types/base_types.pb.h>
#include <jobs/job_export_pcb_odb.h>


namespace
{

/// issue5830 is a four-copper-zone board with stable, human-readable zone UUIDs.
const wxString F_CU_ZONE   = wxS( "00000000-0000-0000-0000-00005c07d704" );
const wxString B_CU_ZONE   = wxS( "00000000-0000-0000-0000-00005c07d701" );
const wxString IN1_CU_ZONE = wxS( "00000000-0000-0000-0000-00005c07d707" );
const wxString IN2_CU_ZONE = wxS( "00000000-0000-0000-0000-00005c07d70a" );


struct API_HANDLER_PCB_FIXTURE
{
    SETTINGS_MANAGER                      m_settingsManager;
    std::unique_ptr<BOARD>                m_board;
    std::shared_ptr<HEADLESS_PCB_CONTEXT> m_context;

    // The context takes ownership of the board; the returned raw pointer lets the test inspect
    // zone state after the handler runs.
    BOARD* loadBoard( const wxString& aRelPath )
    {
        KI_TEST::LoadBoard( m_settingsManager, aRelPath, m_board );

        BOARD* board = m_board.get();
        m_context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( m_board ),
                                                            &m_settingsManager.Prj(), nullptr );
        return board;
    }

    kiapi::common::ApiRequest makeRefillRequest( BOARD* aBoard, const std::vector<wxString>& aZoneIds ) const
    {
        kiapi::board::commands::RefillZones command;
        command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
        command.mutable_board()->set_board_filename(
                wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

        for( const wxString& id : aZoneIds )
            command.add_zones()->set_value( id.ToStdString() );

        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

        return request;
    }

    ZONE* zoneByUuid( BOARD* aBoard, const wxString& aUuid ) const
    {
        for( ZONE* zone : aBoard->Zones() )
        {
            if( zone->m_Uuid.AsString() == aUuid )
                return zone;
        }

        return nullptr;
    }

    void unfillAll( BOARD* aBoard ) const
    {
        // Start from a clean slate so a positive IsFilled() result can only come from this fill
        for( ZONE* zone : aBoard->Zones() )
        {
            zone->UnFill();
            BOOST_REQUIRE( !zone->IsFilled() );
        }
    }
};


kiapi::common::ApiRequest makeBeginCommitRequest()
{
    // No header, so the pre-11.0 path assumes the PCB editor
    kiapi::common::commands::BeginCommit command;

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest makeRevertRequest( BOARD* aBoard )
{
    kiapi::common::commands::RevertDocument command;
    command.mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_document()->set_board_filename( wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::types::DocumentSpecifier makeBoardDocument( const BOARD& aBoard )
{
    kiapi::common::types::DocumentSpecifier document;
    document.set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    document.set_board_filename( wxFileName( aBoard.GetFileName() ).GetFullName().ToStdString() );

    return document;
}


kiapi::common::ApiRequest makeStartRefillZonesTaskRequest(
        const BOARD& aBoard, const std::vector<wxString>& aZoneIds = {} )
{
    kiapi::board::tasks::StartRefillZonesTask command;
    *command.mutable_board() = makeBoardDocument( aBoard );

    for( const wxString& id : aZoneIds )
        command.add_zones()->set_value( id.ToStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest makeStartDrcTaskRequest( const BOARD& aBoard, bool aTestSchematicParity = false,
                                                   bool aRequireZonesFilled = false )
{
    kiapi::board::tasks::StartDrcTask command;
    *command.mutable_board() = makeBoardDocument( aBoard );
    command.set_test_schematic_parity( aTestSchematicParity );
    command.set_require_zones_filled( aRequireZonesFilled );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest makeGetDrcTaskResultRequest( const std::string& aToken )
{
    kiapi::board::tasks::GetDrcTaskResult command;
    command.mutable_token()->set_value( aToken );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}

kiapi::common::ApiRequest makeUnroutedRequest( BOARD* aBoard, const std::vector<std::string>& aNets )
{
    kiapi::board::commands::GetUnroutedConnections command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename( wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

    for( const std::string& net : aNets )
        command.add_nets()->set_name( net );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}

kiapi::common::ApiRequest makeNetLengthsRequest( BOARD* aBoard, const std::vector<std::string>& aNets,
                                                 bool aDelay )
{
    kiapi::board::commands::GetNetLengths command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename( wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );
    command.set_include_delay( aDelay );

    for( const std::string& net : aNets )
        command.add_nets()->set_name( net );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}

kiapi::common::ApiRequest makeQueryConstraintRequest( BOARD* aBoard, kiapi::board::CustomRuleConstraintType aType,
                                                      const KIID& aItemA, const KIID* aItemB )
{
    kiapi::board::commands::QueryConstraint command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename( wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );
    command.set_constraint( aType );
    command.mutable_item_a()->set_value( aItemA.AsStdString() );

    if( aItemB )
        command.mutable_item_b()->set_value( aItemB->AsStdString() );

    command.set_include_report( true );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest packRequest( const google::protobuf::Message& aCommand )
{
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( aCommand ) );

    return request;
}


/// A tuning pattern with every setting filled, as a client must send it
kiapi::board::types::TuningPattern makeTuningPattern( kiapi::board::types::TuningPatternMode aMode,
                                                      const VECTOR2I& aOrigin, const VECTOR2I& aEnd,
                                                      int aSpacingNm )
{
    using namespace kiapi::board::types;

    TuningPattern pattern;
    pattern.set_layer( BoardLayer::BL_F_Cu );
    pattern.mutable_origin()->set_x_nm( aOrigin.x );
    pattern.mutable_origin()->set_y_nm( aOrigin.y );
    pattern.mutable_end()->set_x_nm( aEnd.x );
    pattern.mutable_end()->set_y_nm( aEnd.y );
    pattern.set_mode( aMode );
    pattern.set_target_mode( TuningPatternTargetMode::TPTM_LENGTH );
    pattern.set_override_custom_rules( true );

    TuningPatternSettings* settings = pattern.mutable_settings();
    settings->set_initial_side( TuningPatternMeanderSide::TPMS_DEFAULT );
    settings->set_corner_style( TuningPatternCornerStyle::TPCS_ROUNDED );
    settings->set_corner_radius_percent( 80 );
    settings->mutable_min_amplitude()->set_value_nm( pcbIUScale.mmToIU( 0.2 ) );
    settings->mutable_max_amplitude()->set_value_nm( pcbIUScale.mmToIU( 1.5 ) );
    settings->mutable_min_spacing()->set_value_nm( aSpacingNm );

    return pattern;
}


/// Create one item through the API and return its creation result
kiapi::common::commands::ItemCreationResult createItem( API_HANDLER_PCB& aHandler, const BOARD& aBoard,
                                                        const google::protobuf::Message& aItem )
{
    kiapi::common::commands::CreateItems command;
    *command.mutable_header()->mutable_document() = makeBoardDocument( aBoard );
    command.add_items()->PackFrom( aItem );

    kiapi::common::ApiRequest request = packRequest( command );
    API_RESULT                result = aHandler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::common::commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );

    return response.created_items( 0 );
}


/// Routed length of a net from pad to pad, as GetNetLengths reports it
int64_t netLength( API_HANDLER_PCB& aHandler, BOARD* aBoard, const std::string& aNet )
{
    kiapi::common::ApiRequest request = makeNetLengthsRequest( aBoard, { aNet }, false );
    API_RESULT                result = aHandler.Handle( request );

    BOOST_REQUIRE( result.has_value() );

    kiapi::board::commands::NetLengthsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.nets_size(), 1 );

    return response.nets( 0 ).total_length().value_nm();
}

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerPcb, API_HANDLER_PCB_FIXTURE )


BOOST_AUTO_TEST_CASE( OdbJobRequestMapsVariants )
{
    using PACKAGING = JOB_EXPORT_PCB_ODB::VARIANT_PACKAGING;
    kiapi::board::jobs::RunBoardJobExportODB request;
    request.add_variant_names( "var1" );
    request.add_variant_names( "var2" );
    request.set_variant( "legacy" );
    request.set_variant_packaging( kiapi::board::jobs::ODBVP_COMBINED );
    request.set_manufacturer_part_number_column( "MPN" );
    JOB_EXPORT_PCB_ODB job;
    ApplyOdbJobRequest( request, job );
    BOOST_REQUIRE_EQUAL( job.m_variantNames.size(), 2u );
    BOOST_CHECK_EQUAL( job.m_variantNames[0], wxS( "var1" ) );
    BOOST_CHECK_EQUAL( job.m_variantNames[1], wxS( "var2" ) );
    BOOST_CHECK( job.m_variantPackaging == PACKAGING::COMBINED );
    BOOST_CHECK_EQUAL( job.m_colMfgPn, wxString( wxS( "MPN" ) ) );

    kiapi::board::jobs::RunBoardJobExportODB legacyRequest;
    legacyRequest.set_variant( "legacy" );
    JOB_EXPORT_PCB_ODB legacyJob;
    ApplyOdbJobRequest( legacyRequest, legacyJob );
    BOOST_REQUIRE_EQUAL( legacyJob.m_variantNames.size(), 1u );
    BOOST_CHECK_EQUAL( legacyJob.m_variantNames[0], wxS( "legacy" ) );
    BOOST_CHECK( legacyJob.m_variantPackaging == PACKAGING::SEPARATE );
}


BOOST_AUTO_TEST_CASE( OdbJobRequestMapsExportOptions )
{
    kiapi::board::jobs::RunBoardJobExportODB request;
    request.set_origin( kiapi::board::jobs::ODBO_GRID );
    request.set_product_name( "board product" );
    request.mutable_content()->set_data_set( kiapi::board::jobs::FDS_ASSEMBLY );
    request.mutable_content()->set_sections( "KP" );
    request.mutable_content()->set_net_names( kiapi::board::jobs::FNNP_ANONYMIZE );

    JOB_EXPORT_PCB_ODB job;
    ApplyOdbJobRequest( request, job );
    BOOST_CHECK( job.m_origin == JOB_EXPORT_PCB_ODB::ORIGIN::GRID );
    BOOST_CHECK_EQUAL( job.m_productName, wxString( wxS( "board product" ) ) );
    BOOST_CHECK( job.m_dataSet == JOB_EXPORT_PCB_FAB::DATA_SET::ASSEMBLY );
    BOOST_REQUIRE( job.m_sections );
    BOOST_CHECK_EQUAL( *job.m_sections, wxString( wxS( "KP" ) ) );
    BOOST_CHECK( job.m_netNames == JOB_EXPORT_PCB_FAB::NET_NAMES::ANONYMIZE );

    kiapi::board::jobs::RunBoardJobExportODB legacy;
    JOB_EXPORT_PCB_ODB                       defaultJob;
    ApplyOdbJobRequest( legacy, defaultJob );
    BOOST_CHECK( defaultJob.m_origin == JOB_EXPORT_PCB_ODB::ORIGIN::ABSOLUTE_COORDS );
    BOOST_CHECK( defaultJob.m_dataSet == JOB_EXPORT_PCB_FAB::DATA_SET::USERDEF );
    BOOST_CHECK( defaultJob.m_netNames == JOB_EXPORT_PCB_FAB::NET_NAMES::INCLUDE );
}


BOOST_AUTO_TEST_CASE( RefillZonesSubset )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, { F_CU_ZONE, IN1_CU_ZONE } );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    ZONE* fCu   = zoneByUuid( board, F_CU_ZONE );
    ZONE* bCu   = zoneByUuid( board, B_CU_ZONE );
    ZONE* in1Cu = zoneByUuid( board, IN1_CU_ZONE );
    ZONE* in2Cu = zoneByUuid( board, IN2_CU_ZONE );

    BOOST_REQUIRE( fCu && bCu && in1Cu && in2Cu );

    // Exactly the requested zones must be filled; the others must be untouched.
    BOOST_CHECK( fCu->IsFilled() );
    BOOST_CHECK( in1Cu->IsFilled() );
    BOOST_CHECK( !bCu->IsFilled() );
    BOOST_CHECK( !in2Cu->IsFilled() );
}


BOOST_AUTO_TEST_CASE( RefillZonesSubsetRebuildsConnectivity )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    // Baseline ratsnest with every zone empty; the GND planes are unfilled so their pads still
    // ratsnest together.
    board->BuildConnectivity();
    const unsigned baseline = board->GetConnectivity()->GetUnconnectedCount( false );
    BOOST_REQUIRE_MESSAGE( baseline > 0, "expected an unconnected baseline with zones empty" );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, { F_CU_ZONE, IN1_CU_ZONE } );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    const unsigned afterFill = board->GetConnectivity()->GetUnconnectedCount( false );

    // Filling the GND planes bridges GND pads that previously ratsnested, so the unconnected
    // count drops.  Push cleared the ratsnest, so if the handler skipped the connectivity
    // rebuild this would read zero instead of the reduced-but-nonzero count.
    BOOST_CHECK_MESSAGE( afterFill > 0, "connectivity was cleared, not rebuilt, after the fill" );
    BOOST_CHECK_MESSAGE( afterFill < baseline,
                         "filling the GND planes should reduce the unconnected count ("
                                 << afterFill << " vs baseline " << baseline << ")" );
}


BOOST_AUTO_TEST_CASE( RefillZonesAllHeadless )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, {} );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    // With no frame the empty-zones request must fill everything synchronously
    for( ZONE* zone : board->Zones() )
        BOOST_CHECK_MESSAGE( zone->IsFilled(), "zone " << zone->m_Uuid.AsStdString() << " not filled" );
}


BOOST_AUTO_TEST_CASE( RefillZonesUnknownIdRejected )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request =
            makeRefillRequest( board, { wxS( "deadbeef-0000-0000-0000-000000000000" ) } );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


// RevertDocument reloads the board, freeing every item an in-flight client commit points at.
// The handler must refuse with AS_BUSY while a commit is open, or the pointers dangle
// (use-after-free). Latent UAF found via #24803.
BOOST_AUTO_TEST_CASE( RevertDocumentRejectedWithOpenCommit )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    // Open a client transaction, as a client staging edits would
    kiapi::common::ApiRequest beginRequest = makeBeginCommitRequest();
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    kiapi::common::ApiRequest request = makeRevertRequest( board );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );
    BOOST_CHECK( result.error().error_message().find( "commit" ) != std::string::npos );
}


// With no open commit the guard passes; the reload then needs a running editor, so a headless
// handler reports AS_UNIMPLEMENTED. This confirms the guard does not reject the normal path.
BOOST_AUTO_TEST_CASE( RevertDocumentWithoutCommitPassesGuard )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRevertRequest( board );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


/*
 * Long-running API tasks: StartRefillZonesTask must fill the requested zones via the task
 * scheduler and report COMPLETED; StartDrcTask must run DRC headlessly and produce a result
 * retrievable through GetDrcTaskResult.
 */

BOOST_AUTO_TEST_CASE( StartRefillZonesTaskCompletes )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB handler( m_context );

    kiapi::board::tasks::StartRefillZonesTask command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename(
            wxFileName( board->GetFileName() ).GetFullName().ToStdString() );
    command.add_zones()->set_value( F_CU_ZONE.ToStdString() );
    command.add_zones()->set_value( IN1_CU_ZONE.ToStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );

    kiapi::common::tasks::StartTaskResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE( !response.token().value().empty() );

    // No event loop here: drain the scheduler synchronously until the task finishes
    API_TASK_SCHEDULER::Instance().Pump();

    auto status = API_TASK_SCHEDULER::Instance().GetStatus( response.token().value() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::tasks::TS_COMPLETED );

    ZONE* fCu   = zoneByUuid( board, F_CU_ZONE );
    ZONE* in1Cu = zoneByUuid( board, IN1_CU_ZONE );
    ZONE* bCu   = zoneByUuid( board, B_CU_ZONE );

    BOOST_REQUIRE( fCu && in1Cu && bCu );
    BOOST_CHECK( fCu->IsFilled() );
    BOOST_CHECK( in1Cu->IsFilled() );
    BOOST_CHECK( !bCu->IsFilled() );
}


BOOST_AUTO_TEST_CASE( StartRefillZonesTaskUnknownIdRejected )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::board::tasks::StartRefillZonesTask command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename(
            wxFileName( board->GetFileName() ).GetFullName().ToStdString() );
    command.add_zones()->set_value( "deadbeef-0000-0000-0000-000000000000" );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );

    // Validation happens at request time, before any task starts
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( StartRefillZonesTaskEmptyListFillsAllZones )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB handler( m_context );

    // An empty zone list means "all zones", matching the synchronous RefillZones command
    kiapi::common::ApiRequest request = makeStartRefillZonesTaskRequest( *board );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    kiapi::common::tasks::StartTaskResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE( !response.token().value().empty() );

    API_TASK_SCHEDULER::Instance().Pump();

    auto status = API_TASK_SCHEDULER::Instance().GetStatus( response.token().value() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::tasks::TS_COMPLETED );

    for( ZONE* zone : board->Zones() )
    {
        if( !zone->GetIsRuleArea() )
            BOOST_CHECK_MESSAGE( zone->IsFilled(), "zone " << zone->GetZoneName() << " was not filled" );
    }

    // Headless contexts track modification themselves; the task must report it like the
    // synchronous command so clients know the board needs saving.
    kiapi::common::commands::GetDocumentModifiedState stateCommand;
    *stateCommand.mutable_document() = makeBoardDocument( *board );

    kiapi::common::ApiRequest stateRequest;
    stateRequest.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( stateRequest.mutable_message()->PackFrom( stateCommand ) );

    API_RESULT stateResult = handler.Handle( stateRequest );
    BOOST_REQUIRE( stateResult.has_value() );

    kiapi::common::commands::GetDocumentModifiedStateResponse state;
    BOOST_REQUIRE( stateResult->message().UnpackTo( &state ) );
    BOOST_CHECK_EQUAL( state.state(), kiapi::common::commands::DMS_MODIFIED );
}


BOOST_AUTO_TEST_CASE( StartDrcTaskCompletesWithResults )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::board::tasks::StartDrcTask command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename(
            wxFileName( board->GetFileName() ).GetFullName().ToStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    kiapi::common::tasks::StartTaskResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE( !response.token().value().empty() );

    API_TASK_SCHEDULER::Instance().Pump();

    auto status = API_TASK_SCHEDULER::Instance().GetStatus( response.token().value() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::tasks::TS_COMPLETED );

    // Fetch the result via a GetDrcTaskResult request, like a real client
    kiapi::board::tasks::GetDrcTaskResult resultRequest;
    resultRequest.mutable_token()->set_value( response.token().value() );

    kiapi::common::ApiRequest resultApiRequest;
    resultApiRequest.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( resultApiRequest.mutable_message()->PackFrom( resultRequest ) );

    API_RESULT resultResponse = handler.Handle( resultApiRequest );
    BOOST_REQUIRE( resultResponse.has_value() );

    kiapi::board::tasks::DrcTaskResult drcResult;
    BOOST_REQUIRE( resultResponse->message().UnpackTo( &drcResult ) );

    // issue5830 has unfilled zones and minimal clearances: DRC must find violations, and
    // the run must have produced board markers retrievable via the standard item API
    BOOST_CHECK_GT( drcResult.violations_size(), 0 );
    BOOST_CHECK_EQUAL( board->Markers().size(), drcResult.violations_size() );

    // Each violation entry must carry a description and a valid error type
    for( const kiapi::board::DrcMarker& violation : drcResult.violations() )
    {
        BOOST_CHECK( !violation.description().empty() );
        BOOST_CHECK( violation.error_type() != kiapi::board::DrcErrorType::DRCET_UNKNOWN );
    }
}


BOOST_AUTO_TEST_CASE( GetDrcTaskResultRejectsUnknownToken )
{
    loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::board::tasks::GetDrcTaskResult resultRequest;
    resultRequest.mutable_token()->set_value( "deadbeef-0000-0000-0000-000000000000" );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( resultRequest ) );

    API_RESULT result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( StartDrcTaskRequiresFilledZones )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    auto makeStartRequest = [&]() -> kiapi::common::ApiRequest
    {
        kiapi::board::tasks::StartDrcTask command;
        command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
        command.mutable_board()->set_board_filename(
                wxFileName( board->GetFileName() ).GetFullName().ToStdString() );
        command.set_require_zones_filled( true );

        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return request;
    };

    // Stale zones: unfill everything; the task must refuse to run DRC
    unfillAll( board );

    kiapi::common::ApiRequest request = makeStartRequest();
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    kiapi::common::tasks::StartTaskResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    API_TASK_SCHEDULER::Instance().Pump();

    auto status = API_TASK_SCHEDULER::Instance().GetStatus( response.token().value() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::tasks::TS_FAILED );
    BOOST_CHECK( status->error_message().find( "zone" ) != std::string::npos );

    // Filled zones: the task must now run
    for( ZONE* zone : board->Zones() )
    {
        if( !zone->GetIsRuleArea() )
        {
            ZONE_FILLER filler( board, nullptr );
            std::vector<ZONE*> toFill = { zone };
            BOOST_REQUIRE( filler.Fill( toFill ) );
        }
    }

    kiapi::common::ApiRequest request2 = makeStartRequest();
    API_RESULT result2 = handler.Handle( request2 );
    BOOST_REQUIRE( result2.has_value() );

    kiapi::common::tasks::StartTaskResponse response2;
    BOOST_REQUIRE( result2->message().UnpackTo( &response2 ) );

    API_TASK_SCHEDULER::Instance().Pump();

    auto status2 = API_TASK_SCHEDULER::Instance().GetStatus( response2.token().value() );
    BOOST_REQUIRE( status2.has_value() );
    BOOST_CHECK_EQUAL( status2->state(), kiapi::common::tasks::TS_COMPLETED );
}


BOOST_AUTO_TEST_CASE( GetDrcTaskResultOnFailedTaskReturnsInternalError )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB handler( m_context );

    kiapi::common::ApiRequest startRequest =
            makeStartDrcTaskRequest( *board, false, true /* requireFilledZones */ );
    API_RESULT start = handler.Handle( startRequest );
    BOOST_REQUIRE( start.has_value() );

    kiapi::common::tasks::StartTaskResponse response;
    BOOST_REQUIRE( start->message().UnpackTo( &response ) );

    API_TASK_SCHEDULER::Instance().Pump();

    auto status = API_TASK_SCHEDULER::Instance().GetStatus( response.token().value() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::tasks::TS_FAILED );

    kiapi::common::ApiRequest resultRequest =
            makeGetDrcTaskResultRequest( response.token().value() );
    API_RESULT result = handler.Handle( resultRequest );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_INTERNAL_ERROR );
    BOOST_CHECK( result.error().error_message().find( "zone" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( StartDrcTaskWithParityWithoutKiwayFails )
{
    BOARD* board = loadBoard( wxS( "via_off_center" ) );

    // The QA loader reads boards from a stream, which leaves the board without a file name.
    // Set it so the parity lookup finds the schematic next to this board and reaches the
    // kiface step; the headless test context has no KIWAY.
    board->SetFileName( wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() )
                        + wxS( "via_off_center.kicad_pcb" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::common::ApiRequest startRequest =
            makeStartDrcTaskRequest( *board, true /* testFootprints */, false );
    API_RESULT start = handler.Handle( startRequest );
    BOOST_REQUIRE( start.has_value() );

    kiapi::common::tasks::StartTaskResponse response;
    BOOST_REQUIRE( start->message().UnpackTo( &response ) );

    API_TASK_SCHEDULER::Instance().Pump();

    auto status = API_TASK_SCHEDULER::Instance().GetStatus( response.token().value() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::tasks::TS_FAILED );
    BOOST_CHECK_MESSAGE( status->error_message().find( "parity tests" ) != std::string::npos,
                         "unexpected error: " << status->error_message() );
}


BOOST_AUTO_TEST_CASE( StartTaskWhileAnotherIsRunningIsRejected )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::common::ApiRequest startRequest = makeStartDrcTaskRequest( *board );
    API_RESULT first = handler.Handle( startRequest );
    BOOST_REQUIRE( first.has_value() );

    kiapi::common::tasks::StartTaskResponse response;
    BOOST_REQUIRE( first->message().UnpackTo( &response ) );
    BOOST_REQUIRE( !response.token().value().empty() );

    // The DRC run event has not been pumped, so the task is still running
    kiapi::common::ApiRequest secondRequest =
            makeStartRefillZonesTaskRequest( *board, { F_CU_ZONE } );
    API_RESULT second = handler.Handle( secondRequest );
    BOOST_REQUIRE( !second.has_value() );
    BOOST_CHECK_EQUAL( second.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );

    // Result queries are served during a task, but report that it is not finished yet
    kiapi::common::ApiRequest runningRequest =
            makeGetDrcTaskResultRequest( response.token().value() );
    API_RESULT runningResult = handler.Handle( runningRequest );
    BOOST_REQUIRE( !runningResult.has_value() );
    BOOST_CHECK_EQUAL( runningResult.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );

    API_TASK_SCHEDULER::Instance().Pump();

    kiapi::common::ApiRequest finishedRequest =
            makeGetDrcTaskResultRequest( response.token().value() );
    API_RESULT finishedResult = handler.Handle( finishedRequest );
    BOOST_REQUIRE( finishedResult.has_value() );
}


BOOST_AUTO_TEST_CASE( TaskQueriesAllowedDuringTask )
{
    loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    // Status/result retrieval is safe while a task runs
    BOOST_CHECK( handler.AllowsDuringTask( std::string( kiapi::board::tasks::GetDrcTaskResult().GetTypeName() ) ) );

    // Starting or mutating commands is not
    BOOST_CHECK( !handler.AllowsDuringTask( std::string( kiapi::board::tasks::StartDrcTask().GetTypeName() ) ) );
    BOOST_CHECK( !handler.AllowsDuringTask( std::string( kiapi::board::tasks::StartRefillZonesTask().GetTypeName() ) ) );
    BOOST_CHECK( !handler.AllowsDuringTask( std::string( kiapi::common::commands::GetItems().GetTypeName() ) ) );
}


BOOST_AUTO_TEST_CASE( DrcExclusionDropsMarkerReportFields )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );
    drcItem->SetItems( KIID( "12345678-1234-1234-1234-123456789abc" ),
                       KIID( "87654321-4321-4321-4321-cba987654321" ) );

    DRC_RULE rule( wxS( "custom clearance" ) );
    drcItem->SetViolatingRule( &rule );

    PCB_MARKER* marker = new PCB_MARKER( drcItem, VECTOR2I( 1000, 1000 ) );
    board->Add( marker );
    marker->SetExcluded( true, "Test exclusion comment" );

    kiapi::board::DrcMarker msg;
    PCB_MARKER::ToProto( msg, *marker );
    BOOST_CHECK_EQUAL( msg.violating_rule(), "custom clearance" );
    BOOST_CHECK( !msg.description().empty() );
    BOOST_CHECK( msg.excluded() );

    DRC_EXCLUSION exclusion = DRC_EXCLUSION::FromMarker( *marker );
    const kiapi::board::DrcMarker& canonical = exclusion.ToProto().marker();

    BOOST_CHECK( canonical.violating_rule().empty() );
    BOOST_CHECK( canonical.description().empty() );
    BOOST_CHECK_EQUAL( canonical.severity(), kiapi::common::types::RuleSeverity::RS_UNKNOWN );
    BOOST_CHECK( !canonical.excluded() );

    // Canonicalization is stable across a proto round trip
    DRC_EXCLUSION roundTripped = DRC_EXCLUSION::FromProto( exclusion.ToProto() );
    BOOST_CHECK( roundTripped == exclusion );

    const kiapi::board::DrcMarker& roundTrippedMsg = roundTripped.ToProto().marker();
    BOOST_CHECK( roundTrippedMsg.violating_rule().empty() );
    BOOST_CHECK( roundTrippedMsg.description().empty() );
    BOOST_CHECK( !roundTrippedMsg.excluded() );
}


BOOST_AUTO_TEST_CASE( GetUnroutedConnectionsMatchesRatsnest )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    // With the planes empty the GND pads ratsnest together, so there is work left to route
    unfillAll( board );
    board->BuildConnectivity();
    const unsigned expected = board->GetConnectivity()->GetUnconnectedCount( false );
    BOOST_REQUIRE( expected > 0 );

    API_HANDLER_PCB handler( m_context );
    kiapi::common::ApiRequest request = makeUnroutedRequest( board, {} );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::board::commands::UnroutedConnectionsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( static_cast<unsigned>( response.connections_size() ), expected );

    for( const kiapi::board::commands::UnroutedConnection& connection : response.connections() )
    {
        auto* fromItem = dynamic_cast<BOARD_CONNECTED_ITEM*>(
                board->ResolveItem( KIID( connection.from_item().value() ), true ) );
        auto* toItem = dynamic_cast<BOARD_CONNECTED_ITEM*>(
                board->ResolveItem( KIID( connection.to_item().value() ), true ) );

        BOOST_REQUIRE( fromItem );
        BOOST_REQUIRE( toItem );

        // Both ends belong to the reported net
        BOOST_CHECK_EQUAL( fromItem->GetNetname().ToStdString(), connection.net().name() );
        BOOST_CHECK_EQUAL( toItem->GetNetname().ToStdString(), connection.net().name() );

        VECTOR2I delta( connection.to_position().x_nm() - connection.from_position().x_nm(),
                        connection.to_position().y_nm() - connection.from_position().y_nm() );
        BOOST_CHECK_CLOSE( static_cast<double>( connection.length().value_nm() ), delta.EuclideanNorm(), 0.01 );
    }
}


BOOST_AUTO_TEST_CASE( GetUnroutedConnectionsFiltersByNet )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );
    board->BuildConnectivity();

    API_HANDLER_PCB handler( m_context );
    kiapi::common::ApiRequest allRequest = makeUnroutedRequest( board, {} );
    API_RESULT                all = handler.Handle( allRequest );
    BOOST_REQUIRE( all.has_value() );

    kiapi::board::commands::UnroutedConnectionsResponse allResponse;
    BOOST_REQUIRE( all->message().UnpackTo( &allResponse ) );
    BOOST_REQUIRE( allResponse.connections_size() > 0 );

    const std::string net = allResponse.connections( 0 ).net().name();
    int expected = 0;

    for( const kiapi::board::commands::UnroutedConnection& connection : allResponse.connections() )
        expected += connection.net().name() == net ? 1 : 0;

    kiapi::common::ApiRequest filteredRequest = makeUnroutedRequest( board, { net } );
    API_RESULT                filtered = handler.Handle( filteredRequest );
    BOOST_REQUIRE( filtered.has_value() );

    kiapi::board::commands::UnroutedConnectionsResponse filteredResponse;
    BOOST_REQUIRE( filtered->message().UnpackTo( &filteredResponse ) );
    BOOST_CHECK_EQUAL( filteredResponse.connections_size(), expected );

    for( const kiapi::board::commands::UnroutedConnection& connection : filteredResponse.connections() )
        BOOST_CHECK_EQUAL( connection.net().name(), net );
}


BOOST_AUTO_TEST_CASE( GetUnroutedConnectionsRejectsUnknownNet )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );
    kiapi::common::ApiRequest request = makeUnroutedRequest( board, { "NO_SUCH_NET" } );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( GetNetLengthsIsConsistentWithBoard )
{
    BOARD* board = loadBoard( wxS( "length_calculations" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeNetLengthsRequest( board, {}, true );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::board::commands::NetLengthsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE( response.nets_size() > 0 );

    bool sawRouted = false;

    for( const kiapi::board::commands::NetLength& net : response.nets() )
    {
        NETINFO_ITEM* info = board->FindNet( wxString::FromUTF8( net.net().name() ) );
        BOOST_REQUIRE( info );

        int vias = 0;

        for( PCB_TRACK* track : board->Tracks() )
        {
            if( track->Type() == PCB_VIA_T && track->GetNetCode() == info->GetNetCode() )
                vias++;
        }

        BOOST_CHECK_EQUAL( net.via_count(), vias );

        int64_t layerSum = 0;

        for( const kiapi::board::commands::NetLayerLength& layer : net.layers() )
            layerSum += layer.length().value_nm();

        BOOST_CHECK_EQUAL( layerSum, net.track_length().value_nm() );
        BOOST_CHECK_EQUAL( net.total_length().value_nm(), net.track_length().value_nm() + net.via_length().value_nm()
                                                                  + net.pad_to_die_length().value_nm() );

        if( net.track_length().value_nm() > 0 )
            sawRouted = true;
    }

    BOOST_CHECK( sawRouted );
}


BOOST_AUTO_TEST_CASE( GetNetLengthsReportsDelayWithTuningProfiles )
{
    // time_calculations assigns tuning profiles, so routed nets have a propagation delay
    BOARD* board = loadBoard( wxS( "time_calculations" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeNetLengthsRequest( board, {}, true );
    API_RESULT                result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    kiapi::board::commands::NetLengthsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    int withDelay = 0;

    for( const kiapi::board::commands::NetLength& net : response.nets() )
    {
        if( net.track_length().value_nm() > 0 && net.total_delay().value_as() > 0 )
            withDelay++;
    }

    BOOST_CHECK( withDelay > 0 );

    // Without the flag no delay is computed
    kiapi::common::ApiRequest noDelay = makeNetLengthsRequest( board, {}, false );
    API_RESULT                noDelayResult = handler.Handle( noDelay );
    BOOST_REQUIRE( noDelayResult.has_value() );

    kiapi::board::commands::NetLengthsResponse noDelayResponse;
    BOOST_REQUIRE( noDelayResult->message().UnpackTo( &noDelayResponse ) );

    for( const kiapi::board::commands::NetLength& net : noDelayResponse.nets() )
        BOOST_CHECK_EQUAL( net.total_delay().value_as(), 0 );
}


BOOST_AUTO_TEST_CASE( GetNetLengthsFiltersAndRejects )
{
    BOARD* board = loadBoard( wxS( "length_calculations" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest all = makeNetLengthsRequest( board, {}, false );
    API_RESULT                allResult = handler.Handle( all );
    BOOST_REQUIRE( allResult.has_value() );

    kiapi::board::commands::NetLengthsResponse allResponse;
    BOOST_REQUIRE( allResult->message().UnpackTo( &allResponse ) );
    BOOST_REQUIRE( allResponse.nets_size() > 0 );

    const kiapi::board::commands::NetLength& first = allResponse.nets( 0 );

    kiapi::common::ApiRequest one = makeNetLengthsRequest( board, { first.net().name() }, false );
    API_RESULT                oneResult = handler.Handle( one );
    BOOST_REQUIRE( oneResult.has_value() );

    kiapi::board::commands::NetLengthsResponse oneResponse;
    BOOST_REQUIRE( oneResult->message().UnpackTo( &oneResponse ) );
    BOOST_REQUIRE_EQUAL( oneResponse.nets_size(), 1 );
    BOOST_CHECK_EQUAL( oneResponse.nets( 0 ).total_length().value_nm(), first.total_length().value_nm() );

    kiapi::common::ApiRequest bad = makeNetLengthsRequest( board, { "NO_SUCH_NET" }, false );
    API_RESULT                badResult = handler.Handle( bad );
    BOOST_REQUIRE( !badResult.has_value() );
    BOOST_CHECK_EQUAL( badResult.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( QueryConstraintResolvesCustomRule )
{
    // The board's .kicad_dru limits uncoupled length of every diff pair to 5 mm
    BOARD* board = loadBoard( wxS( "diff_pair_uncoupled_tuning_drc" ) );

    PCB_TRACK* pairTrack = nullptr;

    for( PCB_TRACK* track : board->Tracks() )
    {
        wxString complement, base;

        if( track->Type() == PCB_TRACE_T
            && DRC_ENGINE::MatchDpSuffix( track->GetNetname(), complement, base ) != 0 )
        {
            pairTrack = track;
            break;
        }
    }

    BOOST_REQUIRE( pairTrack );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeQueryConstraintRequest(
            board, kiapi::board::CustomRuleConstraintType::CRCT_MAX_UNCOUPLED, pairTrack->m_Uuid, nullptr );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::board::commands::QueryConstraintResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    BOOST_CHECK_MESSAGE( response.found(), response.report() );
    BOOST_REQUIRE( response.has_max() );
    BOOST_CHECK_EQUAL( response.max(), pcbIUScale.mmToIU( 5.0 ) );
    BOOST_CHECK_MESSAGE( response.source().find( "DP uncoupled" ) != std::string::npos, response.source() );
    BOOST_CHECK( !response.report().empty() );
}


BOOST_AUTO_TEST_CASE( QueryConstraintClearanceBetweenItems )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    // Two copper items on different nets
    BOARD_CONNECTED_ITEM* a = nullptr;
    BOARD_CONNECTED_ITEM* b = nullptr;

    for( PAD* pad : board->GetPads() )
    {
        if( pad->GetNetCode() <= 0 )
            continue;

        if( !a )
            a = pad;
        else if( pad->GetNetCode() != a->GetNetCode() )
        {
            b = pad;
            break;
        }
    }

    BOOST_REQUIRE( a && b );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeQueryConstraintRequest(
            board, kiapi::board::CustomRuleConstraintType::CRCT_CLEARANCE, a->m_Uuid, &b->m_Uuid );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );

    kiapi::board::commands::QueryConstraintResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK( response.found() );
    BOOST_REQUIRE( response.has_min() );
    BOOST_CHECK( response.min() > 0 );
}


BOOST_AUTO_TEST_CASE( QueryConstraintRejectsUnknownItem )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB           handler( m_context );
    KIID                      missing;
    kiapi::common::ApiRequest request = makeQueryConstraintRequest(
            board, kiapi::board::CustomRuleConstraintType::CRCT_TRACK_WIDTH, missing, nullptr );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( RouteConnectionRoutesRatsnestLine )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    // With the planes empty, GND pads ratsnest together: route one of those lines
    unfillAll( board );
    board->BuildConnectivity();

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = board->GetConnectivity();
    const unsigned                     before = connectivity->GetUnconnectedCount( false );
    BOOST_REQUIRE( before > 0 );

    PAD* from = nullptr;
    PAD* to = nullptr;

    connectivity->RunOnUnconnectedEdges(
            [&]( CN_EDGE& aEdge )
            {
                auto* a = dynamic_cast<PAD*>( aEdge.GetSourceNode()->Parent() );
                auto* b = dynamic_cast<PAD*>( aEdge.GetTargetNode()->Parent() );

                if( a && b && a->IsOnLayer( F_Cu ) && b->IsOnLayer( F_Cu ) )
                {
                    from = a;
                    to = b;
                    return false;
                }

                return true;
            } );

    BOOST_REQUIRE( from && to );

    kiapi::board::commands::RouteConnection command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename( wxFileName( board->GetFileName() ).GetFullName().ToStdString() );
    command.mutable_start_item()->set_value( from->m_Uuid.AsStdString() );
    command.mutable_end_item()->set_value( to->m_Uuid.AsStdString() );
    command.set_layer( kiapi::board::types::BoardLayer::BL_F_Cu );
    command.set_mode( kiapi::board::commands::RouterMode::RTM_SINGLE );
    command.set_collision_mode( kiapi::board::commands::RouterCollisionMode::RCM_MARK_OBSTACLES );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    const size_t tracksBefore = board->Tracks().size();

    API_HANDLER_PCB handler( m_context );
    API_RESULT      result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::board::commands::RouteConnectionResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    BOOST_REQUIRE_MESSAGE( response.success(), response.failure_reason() );
    BOOST_CHECK( response.created_items_size() > 0 );
    BOOST_CHECK_EQUAL( board->Tracks().size(), tracksBefore + response.created_items_size() );

    // Every new track is on the requested layer and the start pad's net
    for( const google::protobuf::Any& any : response.created_items() )
    {
        kiapi::board::types::Track track;

        if( !any.UnpackTo( &track ) )
            continue;

        BOOST_CHECK_EQUAL( track.layer(), kiapi::board::types::BoardLayer::BL_F_Cu );
        BOOST_CHECK_EQUAL( track.net().name(), from->GetNetname().ToStdString() );
    }

    connectivity->RecalculateRatsnest();
    BOOST_CHECK_LT( connectivity->GetUnconnectedCount( false ), before );
}


BOOST_AUTO_TEST_CASE( RouteConnectionRejectsNonCopperItems )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    kiapi::board::commands::RouteConnection command;
    command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_board()->set_board_filename( wxFileName( board->GetFileName() ).GetFullName().ToStdString() );
    command.mutable_start_item()->set_value( KIID().AsStdString() );
    command.mutable_end_item()->set_value( KIID().AsStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_HANDLER_PCB handler( m_context );
    API_RESULT      result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( RouteConnectionChangesLayerWithVia )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );
    board->BuildConnectivity();

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = board->GetConnectivity();
    const unsigned                     before = connectivity->GetUnconnectedCount( false );

    PAD* from = nullptr;
    PAD* to = nullptr;

    // The longest ratsnest line between two front pads leaves room for two layer changes
    double longest = 0;

    connectivity->RunOnUnconnectedEdges(
            [&]( CN_EDGE& aEdge )
            {
                auto* a = dynamic_cast<PAD*>( aEdge.GetSourceNode()->Parent() );
                auto* b = dynamic_cast<PAD*>( aEdge.GetTargetNode()->Parent() );

                if( a && b && a->IsOnLayer( F_Cu ) && b->IsOnLayer( F_Cu )
                    && ( a->GetPosition() - b->GetPosition() ).EuclideanNorm() > longest )
                {
                    longest = ( a->GetPosition() - b->GetPosition() ).EuclideanNorm();
                    from = a;
                    to = b;
                }

                return true;
            } );

    BOOST_REQUIRE( from && to );

    const VECTOR2I delta = to->GetPosition() - from->GetPosition();
    const VECTOR2I first = from->GetPosition() + delta / 3;
    const VECTOR2I second = from->GetPosition() + delta * 2 / 3;

    kiapi::board::commands::RouteConnection command;
    *command.mutable_board() = makeBoardDocument( *board );
    command.mutable_start_item()->set_value( from->m_Uuid.AsStdString() );
    command.mutable_end_item()->set_value( to->m_Uuid.AsStdString() );
    command.set_layer( kiapi::board::types::BoardLayer::BL_F_Cu );
    command.set_mode( kiapi::board::commands::RouterMode::RTM_SINGLE );
    command.set_collision_mode( kiapi::board::commands::RouterCollisionMode::RCM_WALKAROUND );

    kiapi::board::commands::RouteWaypoint* down = command.add_route_waypoints();
    down->mutable_position()->set_x_nm( first.x );
    down->mutable_position()->set_y_nm( first.y );
    down->set_layer( kiapi::board::types::BoardLayer::BL_B_Cu );

    kiapi::board::commands::RouteWaypoint* up = command.add_route_waypoints();
    up->mutable_position()->set_x_nm( second.x );
    up->mutable_position()->set_y_nm( second.y );
    up->set_layer( kiapi::board::types::BoardLayer::BL_F_Cu );

    command.mutable_via_diameter()->set_value_nm( pcbIUScale.mmToIU( 0.6 ) );
    command.mutable_via_drill()->set_value_nm( pcbIUScale.mmToIU( 0.3 ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = packRequest( command );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::board::commands::RouteConnectionResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_MESSAGE( response.success(), response.failure_reason() );

    int  vias = 0;
    bool front = false;
    bool back = false;

    for( const google::protobuf::Any& any : response.created_items() )
    {
        kiapi::board::types::Via via;
        kiapi::board::types::Track track;

        if( any.UnpackTo( &via ) )
        {
            vias++;
            BOOST_CHECK_EQUAL( via.pad_stack().drill().diameter().x_nm(), pcbIUScale.mmToIU( 0.3 ) );
        }
        else if( any.UnpackTo( &track ) )
        {
            front |= track.layer() == kiapi::board::types::BoardLayer::BL_F_Cu;
            back |= track.layer() == kiapi::board::types::BoardLayer::BL_B_Cu;
        }
    }

    BOOST_CHECK_EQUAL( vias, 2 );
    BOOST_CHECK( front );
    BOOST_CHECK( back );

    connectivity->RecalculateRatsnest();
    BOOST_CHECK_LT( connectivity->GetUnconnectedCount( false ), before );
}


BOOST_AUTO_TEST_CASE( RouteConnectionRejectsMixedWaypoints )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    PAD* pad = nullptr;

    for( FOOTPRINT* footprint : board->Footprints() )
    {
        if( !footprint->Pads().empty() )
        {
            pad = footprint->Pads().front();
            break;
        }
    }

    BOOST_REQUIRE( pad );

    kiapi::board::commands::RouteConnection command;
    *command.mutable_board() = makeBoardDocument( *board );
    command.mutable_start_item()->set_value( pad->m_Uuid.AsStdString() );
    command.mutable_end_item()->set_value( pad->m_Uuid.AsStdString() );
    command.add_waypoints()->set_x_nm( 0 );
    command.add_route_waypoints()->mutable_position()->set_x_nm( 0 );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = packRequest( command );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}

BOOST_AUTO_TEST_CASE( CreateTuningPatternTunesSingleTrack )
{
    using namespace kiapi::board::types;

    BOARD* board = loadBoard( wxS( "length_calculations" ) );

    API_HANDLER_PCB   handler( m_context );
    const std::string net = "Net-(R1-Pad2)";
    const int64_t     before = netLength( handler, board, net );
    const int64_t     target = before + pcbIUScale.mmToIU( 5 );
    const int64_t     tolerance = pcbIUScale.mmToIU( 0.1 );

    // The net is one straight F.Cu segment from (122.301, 48.895) to (135.636, 48.895)
    TuningPattern pattern = makeTuningPattern( TuningPatternMode::TPM_SINGLE,
                                               VECTOR2I( pcbIUScale.mmToIU( 124 ), pcbIUScale.mmToIU( 48.895 ) ),
                                               VECTOR2I( pcbIUScale.mmToIU( 134 ), pcbIUScale.mmToIU( 48.895 ) ),
                                               pcbIUScale.mmToIU( 0.6 ) );
    pattern.mutable_target_length()->set_min( target - tolerance );
    pattern.mutable_target_length()->set_opt( target );
    pattern.mutable_target_length()->set_max( target + tolerance );

    kiapi::common::commands::ItemCreationResult created = createItem( handler, *board, pattern );
    BOOST_REQUIRE_MESSAGE( created.status().code() == kiapi::common::commands::ItemStatusCode::ISC_OK,
                           created.status().error_message() );

    TuningPattern result;
    BOOST_REQUIRE( created.item().UnpackTo( &result ) );

    BOOST_CHECK_EQUAL( result.state().status(), TuningPatternStatus::TPS_TUNED );
    BOOST_CHECK( result.members_size() > 0 );
    BOOST_CHECK_EQUAL( board->Generators().size(), 1 );

    const int64_t after = netLength( handler, board, net );
    BOOST_CHECK_LE( std::abs( after - target ), tolerance );
}


BOOST_AUTO_TEST_CASE( CreateTuningPatternTunesDiffPair )
{
    using namespace kiapi::board::types;

    BOARD* board = loadBoard( wxS( "diff_pair_uncoupled_tuning_drc" ) );

    API_HANDLER_PCB handler( m_context );
    const int64_t   p = netLength( handler, board, "/DIFF_P" );
    const int64_t   n = netLength( handler, board, "/DIFF_N" );
    const int64_t   target = std::max( p, n ) + pcbIUScale.mmToIU( 3 );
    const int64_t   tolerance = pcbIUScale.mmToIU( 0.1 );

    // /DIFF_P and /DIFF_N run coupled and straight on F.Cu from x = 136.24 mm to 154.33 mm
    TuningPattern pattern = makeTuningPattern( TuningPatternMode::TPM_DIFF_PAIR,
                                               VECTOR2I( pcbIUScale.mmToIU( 139 ), pcbIUScale.mmToIU( 93.275 ) ),
                                               VECTOR2I( pcbIUScale.mmToIU( 152 ), pcbIUScale.mmToIU( 93.275 ) ),
                                               pcbIUScale.mmToIU( 1.0 ) );
    pattern.mutable_target_length()->set_min( target - tolerance );
    pattern.mutable_target_length()->set_opt( target );
    pattern.mutable_target_length()->set_max( target + tolerance );

    kiapi::common::commands::ItemCreationResult created = createItem( handler, *board, pattern );
    BOOST_REQUIRE_MESSAGE( created.status().code() == kiapi::common::commands::ItemStatusCode::ISC_OK,
                           created.status().error_message() );

    TuningPattern result;
    BOOST_REQUIRE( created.item().UnpackTo( &result ) );

    BOOST_CHECK_EQUAL( result.state().status(), TuningPatternStatus::TPS_TUNED );
    BOOST_CHECK( result.members_size() > 0 );

    // Both legs are meandered together, so both get longer
    BOOST_CHECK_GT( netLength( handler, board, "/DIFF_P" ), p );
    BOOST_CHECK_GT( netLength( handler, board, "/DIFF_N" ), n );
    BOOST_CHECK_LE( std::abs( std::max( netLength( handler, board, "/DIFF_P" ),
                                        netLength( handler, board, "/DIFF_N" ) ) - target ),
                    tolerance );
}


BOOST_AUTO_TEST_CASE( CreateTuningPatternSetsSkew )
{
    using namespace kiapi::board::types;

    BOARD* board = loadBoard( wxS( "diff_pair_uncoupled_tuning_drc" ) );

    API_HANDLER_PCB handler( m_context );
    const int64_t   n = netLength( handler, board, "/DIFF_N" );

    // Make /DIFF_N 1 mm longer than /DIFF_P by meandering its coupled stretch only
    const int64_t skew = pcbIUScale.mmToIU( 1 );
    const int64_t tolerance = pcbIUScale.mmToIU( 0.05 );

    TuningPattern pattern = makeTuningPattern( TuningPatternMode::TPM_DIFF_PAIR_SKEW,
                                               VECTOR2I( pcbIUScale.mmToIU( 139 ), pcbIUScale.mmToIU( 93.275 ) ),
                                               VECTOR2I( pcbIUScale.mmToIU( 152 ), pcbIUScale.mmToIU( 93.275 ) ),
                                               pcbIUScale.mmToIU( 1.0 ) );
    pattern.mutable_target_skew()->set_min( skew - tolerance );
    pattern.mutable_target_skew()->set_opt( skew );
    pattern.mutable_target_skew()->set_max( skew + tolerance );

    kiapi::common::commands::ItemCreationResult created = createItem( handler, *board, pattern );
    BOOST_REQUIRE_MESSAGE( created.status().code() == kiapi::common::commands::ItemStatusCode::ISC_OK,
                           created.status().error_message() );

    TuningPattern result;
    BOOST_REQUIRE( created.item().UnpackTo( &result ) );

    BOOST_CHECK_EQUAL( result.state().status(), TuningPatternStatus::TPS_TUNED );
    BOOST_CHECK( result.members_size() > 0 );
    BOOST_CHECK_GT( netLength( handler, board, "/DIFF_N" ), n );
}


BOOST_AUTO_TEST_CASE( DeleteTuningPatternRestoresBaseline )
{
    using namespace kiapi::board::types;

    BOARD* board = loadBoard( wxS( "length_calculations" ) );

    API_HANDLER_PCB   handler( m_context );
    const std::string net = "Net-(R1-Pad2)";
    const int64_t     before = netLength( handler, board, net );
    const size_t      tracksBefore = board->Tracks().size();

    TuningPattern pattern = makeTuningPattern( TuningPatternMode::TPM_SINGLE,
                                               VECTOR2I( pcbIUScale.mmToIU( 124 ), pcbIUScale.mmToIU( 48.895 ) ),
                                               VECTOR2I( pcbIUScale.mmToIU( 134 ), pcbIUScale.mmToIU( 48.895 ) ),
                                               pcbIUScale.mmToIU( 0.6 ) );
    pattern.mutable_target_length()->set_opt( before + pcbIUScale.mmToIU( 4 ) );

    kiapi::common::commands::ItemCreationResult created = createItem( handler, *board, pattern );
    BOOST_REQUIRE( created.status().code() == kiapi::common::commands::ItemStatusCode::ISC_OK );
    BOOST_REQUIRE_GT( netLength( handler, board, net ), before );

    TuningPattern result;
    BOOST_REQUIRE( created.item().UnpackTo( &result ) );

    kiapi::common::commands::DeleteItems command;
    *command.mutable_header()->mutable_document() = makeBoardDocument( *board );
    *command.add_item_ids() = result.id();

    kiapi::common::ApiRequest deleteRequest = packRequest( command );
    API_RESULT                deleted = handler.Handle( deleteRequest );
    BOOST_REQUIRE( deleted.has_value() );
    BOOST_REQUIRE_EQUAL( deleted->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    BOOST_CHECK( board->Generators().empty() );
    BOOST_CHECK_LE( std::abs( netLength( handler, board, net ) - before ), pcbIUScale.mmToIU( 0.001 ) );
    BOOST_CHECK_LE( board->Tracks().size(), tracksBefore + 2 );
}


/// Send UpdateBoardStackup and return the handler result
API_RESULT updateStackup( API_HANDLER_PCB& aHandler, const BOARD& aBoard,
                          const kiapi::board::BoardStackup& aStackup )
{
    kiapi::board::commands::UpdateBoardStackup command;
    *command.mutable_board() = makeBoardDocument( aBoard );
    *command.mutable_stackup() = aStackup;

    kiapi::common::ApiRequest request = packRequest( command );
    return aHandler.Handle( request );
}


/// Copper layers of the board's stackup descriptor, top to bottom
std::vector<PCB_LAYER_ID> stackupCopper( BOARD* aBoard )
{
    std::vector<PCB_LAYER_ID> layers;

    for( BOARD_STACKUP_ITEM* item : aBoard->GetDesignSettings().GetStackupDescriptor().GetList() )
    {
        if( item->GetType() == BS_ITEM_TYPE_COPPER )
            layers.push_back( item->GetBrdLayerId() );
    }

    return layers;
}


BOOST_AUTO_TEST_CASE( UpdateBoardStackupRoundTrip )
{
    using namespace kiapi::board;

    BOARD* board = loadBoard( wxS( "issue5830" ) );
    BOOST_REQUIRE_EQUAL( board->GetCopperLayerCount(), 4 );

    BoardStackup stackup;
    board::PackBoardStackup( *board, stackup );

    BoardStackupLayer* dielectric = nullptr;
    BoardStackupLayer* mask = nullptr;

    for( BoardStackupLayer& layer : *stackup.mutable_layers() )
    {
        if( !dielectric && layer.type() == BoardStackupLayerType::BSLT_DIELECTRIC )
            dielectric = &layer;

        if( !mask && layer.type() == BoardStackupLayerType::BSLT_SOLDERMASK )
            mask = &layer;
    }

    BOOST_REQUIRE( dielectric && dielectric->dielectric().layer_size() > 0 );
    BOOST_REQUIRE( mask );

    const int thickness = pcbIUScale.mmToIU( 0.3 );
    BoardStackupDielectricProperties* props = dielectric->mutable_dielectric()->mutable_layer( 0 );
    const int oldThickness = static_cast<int>( props->thickness().value_nm() );
    props->mutable_thickness()->set_value_nm( thickness );
    props->set_epsilon_r( 3.8 );
    props->set_loss_tangent( 0.008 );
    props->set_material_name( "QA laminate" );
    mask->mutable_soldermask()->set_material_name( "QA mask" );
    stackup.mutable_impedance()->set_is_controlled( true );

    const int boardBefore = board->GetDesignSettings().GetBoardThickness();

    API_HANDLER_PCB handler( m_context );
    API_RESULT      result = updateStackup( handler, *board, stackup );
    BOOST_REQUIRE( result.has_value() );

    kiapi::board::commands::BoardStackupResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    const BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();
    BOOST_CHECK( bds.m_HasStackup );
    BOOST_CHECK( bds.GetStackupDescriptor().m_HasDielectricConstrains );
    BOOST_CHECK_EQUAL( bds.GetBoardThickness(), boardBefore + thickness - oldThickness );
    BOOST_CHECK_EQUAL( bds.GetBoardThickness(), bds.GetStackupDescriptor().BuildBoardThicknessFromStackup() );

    const BOARD_STACKUP_ITEM* firstDielectric = nullptr;
    const BOARD_STACKUP_ITEM* firstMask = nullptr;

    for( const BOARD_STACKUP_ITEM* item : bds.GetStackupDescriptor().GetList() )
    {
        if( !firstDielectric && item->GetType() == BS_ITEM_TYPE_DIELECTRIC )
            firstDielectric = item;

        if( !firstMask && item->GetType() == BS_ITEM_TYPE_SOLDERMASK )
            firstMask = item;
    }

    BOOST_REQUIRE( firstDielectric && firstMask );
    BOOST_CHECK_EQUAL( firstDielectric->GetThickness( 0 ), thickness );
    BOOST_CHECK_CLOSE( firstDielectric->GetEpsilonR( 0 ), 3.8, 1e-9 );
    BOOST_CHECK_CLOSE( firstDielectric->GetLossTangent( 0 ), 0.008, 1e-9 );
    BOOST_CHECK( firstDielectric->GetMaterial( 0 ) == wxS( "QA laminate" ) );
    BOOST_CHECK( firstMask->GetMaterial( 0 ) == wxS( "QA mask" ) );

    // The response is the stackup as stored, so sending it back changes nothing
    BoardStackup echoed = response.stackup();
    result = updateStackup( handler, *board, echoed );
    BOOST_REQUIRE( result.has_value() );

    kiapi::board::commands::BoardStackupResponse second;
    BOOST_REQUIRE( result->message().UnpackTo( &second ) );
    BOOST_CHECK_EQUAL( second.stackup().SerializeAsString(), echoed.SerializeAsString() );
}


BOOST_AUTO_TEST_CASE( UpdateBoardStackupRejectsCopperMismatch )
{
    using namespace kiapi::board;

    BOARD* board = loadBoard( wxS( "issue5830" ) );

    BoardStackup stackup;
    board::PackBoardStackup( *board, stackup );
    const std::string before = stackup.SerializeAsString();

    // Renaming In2.Cu to In3.Cu no longer matches the board's copper layers
    BoardStackup renamed = stackup;

    for( BoardStackupLayer& layer : *renamed.mutable_layers() )
    {
        if( layer.layer() == kiapi::board::types::BoardLayer::BL_In2_Cu )
            layer.set_layer( kiapi::board::types::BoardLayer::BL_In3_Cu );
    }

    API_HANDLER_PCB handler( m_context );
    API_RESULT      result = updateStackup( handler, *board, renamed );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );

    // Two copper layers with no dielectric between them are not a stackup
    BoardStackup noDielectric = stackup;
    auto*        layers = noDielectric.mutable_layers();

    for( int i = 0; i < layers->size(); ++i )
    {
        if( layers->Get( i ).type() == BoardStackupLayerType::BSLT_DIELECTRIC )
        {
            layers->DeleteSubrange( i, 1 );
            break;
        }
    }

    result = updateStackup( handler, *board, noDielectric );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );

    BoardStackup after;
    board::PackBoardStackup( *board, after );
    BOOST_CHECK_EQUAL( after.SerializeAsString(), before );
}


BOOST_AUTO_TEST_CASE( SetBoardEnabledLayersResyncsStackup )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    // Store the current stackup so the board has an explicit one
    kiapi::board::BoardStackup stackup;
    board::PackBoardStackup( *board, stackup );
    BOOST_REQUIRE( updateStackup( handler, *board, stackup ).has_value() );
    BOOST_REQUIRE_EQUAL( stackupCopper( board ).size(), 4 );

    kiapi::board::commands::SetBoardEnabledLayers command;
    *command.mutable_board() = makeBoardDocument( *board );
    command.set_copper_layer_count( 6 );
    board::PackLayerSet( *command.mutable_layers(), board->GetEnabledLayers() );

    kiapi::common::ApiRequest request = packRequest( command );
    API_RESULT                result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    std::vector<PCB_LAYER_ID> copper = stackupCopper( board );
    BOOST_REQUIRE_EQUAL( copper.size(), 6 );
    BOOST_CHECK( copper.front() == F_Cu );
    BOOST_CHECK( copper.back() == B_Cu );

    const BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();
    BOOST_CHECK_EQUAL( bds.GetBoardThickness(), bds.GetStackupDescriptor().BuildBoardThicknessFromStackup() );
}


/// Track delay of every net with a routed length, by net name
std::map<std::string, int64_t> trackDelays( API_HANDLER_PCB& aHandler, BOARD* aBoard )
{
    kiapi::common::ApiRequest request = makeNetLengthsRequest( aBoard, {}, true );
    API_RESULT                result = aHandler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    kiapi::board::commands::NetLengthsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    std::map<std::string, int64_t> delays;

    for( const kiapi::board::commands::NetLength& net : response.nets() )
    {
        if( net.track_length().value_nm() > 0 )
            delays[net.net().name()] = net.track_delay().value_as();
    }

    return delays;
}


kiapi::common::commands::TuningProfilesResponse getTuningProfiles( API_HANDLER_COMMON& aHandler )
{
    kiapi::common::commands::GetTuningProfiles command;
    kiapi::common::ApiRequest                  request = packRequest( command );
    API_RESULT                                 result = aHandler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    kiapi::common::commands::TuningProfilesResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    return response;
}


API_RESULT setTuningProfiles( API_HANDLER_COMMON& aHandler,
                              const google::protobuf::RepeatedPtrField<kiapi::common::project::TuningProfile>& aProfiles )
{
    kiapi::common::commands::SetTuningProfiles command;
    *command.mutable_profiles() = aProfiles;
    command.set_merge_mode( kiapi::common::types::MapMergeMode::MMM_MERGE );

    kiapi::common::ApiRequest request = packRequest( command );
    return aHandler.Handle( request );
}


BOOST_AUTO_TEST_CASE( SetTuningProfilesChangesNetDelays )
{
    using namespace kiapi::common::project;

    // The common handler reads the project of the global settings manager
    KI_TEST::LoadBoard( Pgm().GetSettingsManager(), wxS( "time_calculations" ), m_board );
    BOARD* board = m_board.get();
    m_context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( m_board ),
                                                        &Pgm().GetSettingsManager().Prj(), nullptr );

    API_HANDLER_COMMON common;
    API_HANDLER_PCB    pcb( m_context );

    kiapi::common::commands::TuningProfilesResponse before = getTuningProfiles( common );
    BOOST_REQUIRE_EQUAL( before.profiles_size(), 7 );

    const TuningProfile& class1 = before.profiles( 0 );
    BOOST_CHECK_EQUAL( class1.name(), "CLASS1" );
    BOOST_CHECK( class1.type() == TuningProfileType::TPT_SINGLE );
    BOOST_CHECK( class1.enable_time_domain_tuning() );
    BOOST_REQUIRE( class1.layers_size() > 0 );
    BOOST_CHECK( class1.layers( 0 ).signal_layer() == kiapi::board::types::BoardLayer::BL_F_Cu );
    BOOST_CHECK_EQUAL( class1.layers( 0 ).delay_per_mm(), 1000000 );

    std::map<std::string, int64_t> delaysBefore = trackDelays( pcb, board );
    int                            withDelay = 0;

    for( const auto& [name, delay] : delaysBefore )
        withDelay += delay > 0;

    BOOST_REQUIRE( withDelay > 0 );

    // Doubling every track delay doubles the track delay of every net
    google::protobuf::RepeatedPtrField<TuningProfile> doubled = before.profiles();

    for( TuningProfile& profile : doubled )
    {
        for( TuningProfileLayer& layer : *profile.mutable_layers() )
            layer.set_delay_per_mm( layer.delay_per_mm() * 2 );
    }

    BOOST_REQUIRE( setTuningProfiles( common, doubled ).has_value() );
    BOOST_REQUIRE( common.clearNetSettingsNotification() );
    static_cast<API_HANDLER&>( pcb ).onNetSettingsChanged();

    std::map<std::string, int64_t> delaysAfter = trackDelays( pcb, board );

    for( const auto& [name, delay] : delaysBefore )
    {
        BOOST_TEST_CONTEXT( name )
        {
            BOOST_CHECK_LE( std::abs( delaysAfter[name] - 2 * delay ), 2 );
        }
    }

    kiapi::common::commands::TuningProfilesResponse after = getTuningProfiles( common );
    BOOST_REQUIRE_EQUAL( after.profiles_size(), 7 );
    BOOST_CHECK_EQUAL( after.profiles( 0 ).layers( 0 ).delay_per_mm(), 2000000 );

    // Invalid requests leave the profiles alone
    google::protobuf::RepeatedPtrField<TuningProfile> duplicateName;
    *duplicateName.Add() = after.profiles( 0 );
    *duplicateName.Add() = after.profiles( 0 );
    API_RESULT result = setTuningProfiles( common, duplicateName );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );

    google::protobuf::RepeatedPtrField<TuningProfile> duplicateLayer;
    TuningProfile* bad = duplicateLayer.Add();
    *bad = after.profiles( 0 );
    *bad->add_layers() = bad->layers( 0 );
    BOOST_CHECK( !setTuningProfiles( common, duplicateLayer ).has_value() );

    bad->mutable_layers()->RemoveLast();
    bad->mutable_layers( 0 )->set_signal_layer( kiapi::board::types::BoardLayer::BL_F_SilkS );
    BOOST_CHECK( !setTuningProfiles( common, duplicateLayer ).has_value() );

    BOOST_CHECK_EQUAL( getTuningProfiles( common ).SerializeAsString(), after.SerializeAsString() );
}

BOOST_AUTO_TEST_SUITE_END()
