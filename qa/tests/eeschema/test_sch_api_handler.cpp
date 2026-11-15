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

#include <boost/test/unit_test.hpp>

#include <api/api_handler_sch.h>
#include <api/api_sch_utils.h>
#include <api/api_task_scheduler.h>
#include <api/api_utils.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/tasks.pb.h>
#include <api/headless_sch_context.h>
#include <api/schematic/schematic_commands.pb.h>
#include <api/schematic/schematic_tasks.pb.h>
#include <api/schematic/schematic_types.pb.h>
#include <schematic_utils/schematic_file_util.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <richio.h>
#include <tools/sch_selection.h>
#include <settings/settings_manager.h>

#include <schematic.h>
#include <schematic_settings.h>
#include <advanced_config.h>
#include <connection_graph.h>
#include <sch_group.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_screen.h>
#include <sch_pin.h>
#include <sch_sheet.h>
#include <sch_symbol.h>
#include <sch_text.h>
#include <sch_bus_entry.h>
#include <set>
#include <scoped_set_reset.h>


namespace
{

struct API_HANDLER_SCH_FIXTURE
{
    SETTINGS_MANAGER                      m_settingsManager;
    std::unique_ptr<SCHEMATIC>            m_schematic;
    SCHEMATIC*                            m_schematicSlot = nullptr;
    std::shared_ptr<HEADLESS_SCH_CONTEXT> m_context;

    SCHEMATIC* loadSchematic( const wxString& aRelPath )
    {
        KI_TEST::LoadSchematic( m_settingsManager, aRelPath, m_schematic );
        m_schematicSlot = m_schematic.get();
        m_context = std::make_shared<HEADLESS_SCH_CONTEXT>( &m_schematicSlot,
                                                            &m_settingsManager.Prj() );
        return m_schematic.get();
    }
};


kiapi::common::types::DocumentSpecifier makeDocument( const SCHEMATIC& aSchematic )
{
    kiapi::common::types::DocumentSpecifier document;
    document.set_type( kiapi::common::types::DocumentType::DOCTYPE_SCHEMATIC );
    kiapi::common::PackProject( *document.mutable_project(), aSchematic.Project() );

    return document;
}


kiapi::common::ApiRequest makeBeginCommitRequest( const SCHEMATIC& aSchematic )
{
    kiapi::common::commands::BeginCommit command;
    *command.mutable_header()->mutable_document() = makeDocument( aSchematic );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest makeRevertRequest( const SCHEMATIC& aSchematic )
{
    kiapi::common::commands::RevertDocument command;
    *command.mutable_document() = makeDocument( aSchematic );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest makeStartErcTaskRequest( const SCHEMATIC& aSchematic )
{
    kiapi::schematic::tasks::StartErcTask command;
    *command.mutable_document() = makeDocument( aSchematic );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest makeGetErcTaskResultRequest( const std::string& aToken )
{
    kiapi::schematic::tasks::GetErcTaskResult command;
    command.mutable_token()->set_value( aToken );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


}


BOOST_FIXTURE_TEST_SUITE( ApiHandlerSch, API_HANDLER_SCH_FIXTURE )


BOOST_AUTO_TEST_CASE( RevertDocumentRejectedWithOpenCommit )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );

    kiapi::common::ApiRequest beginRequest = makeBeginCommitRequest( *schematic );
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    kiapi::common::ApiRequest request = makeRevertRequest( *schematic );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );
    BOOST_CHECK( result.error().error_message().find( "commit" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectedSheetCreateLeavesNoChanges )
{
    SCHEMATIC*      model = loadSchematic( wxS( "api_kitchen_sink" ) );
    API_HANDLER_SCH handler( m_context );

    schematic::types::SheetSymbol sheet;
    const KIID                    id;
    sheet.mutable_id()->set_value( id.AsStdString() );
    sheet.mutable_name_field()->mutable_text()->set_text( "RejectedSheet" );
    sheet.mutable_filename_field()->mutable_text()->set_text( "rejected.kicad_sch" );
    sheet.set_page_number( "999" );
    auto* field = sheet.add_user_fields();
    field->set_name( "Reference Suffix" );
    field->mutable_text()->set_text( "_M1" );
    auto* variant = sheet.mutable_variants()->add_variants();
    variant->set_name( "rejected-variant" );
    ( *variant->mutable_fields() )["Reference Suffix"] = "_M2";
    variant->set_dnp( true );

    commands::CreateItems command;
    *command.mutable_header()->mutable_document() = makeDocument( *model );
    BOOST_REQUIRE( command.add_items()->PackFrom( sheet ) );
    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );
    BOOST_CHECK_EQUAL( response.created_items( 0 ).status().code(), commands::ISC_INVALID_DATA );
    BOOST_CHECK( !model->Hierarchy().ResolveItem( id, nullptr, true ) );
    BOOST_CHECK( !model->HasVariant( wxS( "rejected-variant" ) ) );
}


BOOST_AUTO_TEST_CASE( RejectedSheetUpdateLeavesNoChanges )
{
    SCHEMATIC*      model = loadSchematic( wxS( "issue13212" ) );
    API_HANDLER_SCH handler( m_context );
    BOOST_REQUIRE_GE( model->Hierarchy().size(), 2 );
    SCH_SHEET_PATH child = model->Hierarchy()[1];
    SCH_SHEET*     original = child.Last();
    SCH_SHEET_PATH parent = child;
    parent.pop_back();
    const wxString originalName = original->GetName();
    const wxString originalPage = child.GetPageNumber();

    schematic::types::SheetSymbol sheet;
    BOOST_REQUIRE( PackSheet( &sheet, original, parent ) );
    sheet.mutable_name_field()->mutable_text()->set_text( "RejectedName" );
    sheet.set_page_number( "999" );
    auto* field = sheet.add_user_fields();
    field->set_name( "Reference Suffix" );
    field->mutable_text()->set_text( "_M1" );
    auto* variant = sheet.mutable_variants()->add_variants();
    variant->set_name( "rejected-variant" );
    ( *variant->mutable_fields() )["Reference Suffix"] = "_M2";

    commands::UpdateItems command;
    *command.mutable_header()->mutable_document() = makeDocument( *model );
    BOOST_REQUIRE( command.add_items()->PackFrom( sheet ) );
    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    commands::UpdateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.updated_items_size(), 1 );
    BOOST_CHECK_EQUAL( response.updated_items( 0 ).status().code(), commands::ISC_INVALID_DATA );
    BOOST_CHECK_EQUAL( original->GetName(), originalName );
    BOOST_CHECK_EQUAL( child.GetPageNumber(), originalPage );
    BOOST_CHECK( !model->HasVariant( wxS( "rejected-variant" ) ) );
}


BOOST_AUTO_TEST_CASE( SheetBatchKeepsValidItems )
{
    SCHEMATIC*            model = loadSchematic( wxS( "api_kitchen_sink" ) );
    API_HANDLER_SCH       handler( m_context );
    commands::CreateItems command;
    *command.mutable_header()->mutable_document() = makeDocument( *model );
    std::vector<KIID> ids( 3 );

    for( int i = 0; i < 3; ++i )
    {
        schematic::types::SheetSymbol sheet;
        sheet.mutable_id()->set_value( ids[i].AsStdString() );
        sheet.mutable_name_field()->mutable_text()->set_text( "Sheet" + std::to_string( i ) );
        sheet.mutable_filename_field()->mutable_text()->set_text( "batch" + std::to_string( i ) + ".kicad_sch" );
        sheet.set_page_number( std::to_string( 20 + i ) );
        auto* suffix = sheet.add_user_fields();
        suffix->set_name( "Reference Suffix" );
        suffix->mutable_text()->set_text( "_M1" );
        auto* field = sheet.add_user_fields();
        field->set_name( "Channel" );
        field->mutable_text()->set_text( "base" );
        auto* variant = sheet.mutable_variants()->add_variants();
        variant->set_name( "batch-variant-" + std::to_string( i ) );
        variant->set_dnp( true );
        ( *variant->mutable_fields() )["Channel"] = "override";

        if( i == 1 )
            ( *variant->mutable_fields() )["Reference Suffix"] = "_M2";

        BOOST_REQUIRE( command.add_items()->PackFrom( sheet ) );
    }

    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 3 );
    BOOST_CHECK_EQUAL( response.status(), types::IRS_OK );

    SCH_SHEET_PATH parent = model->Hierarchy()[0];

    for( int i = 0; i < 3; ++i )
    {
        const wxString variantName = wxString::Format( wxS( "batch-variant-%d" ), i );
        auto*          sheet = dynamic_cast<SCH_SHEET*>( model->Hierarchy().ResolveItem( ids[i], nullptr, true ) );
        BOOST_CHECK_EQUAL( response.created_items( i ).status().code(),
                           i == 1 ? commands::ISC_INVALID_DATA : commands::ISC_OK );
        BOOST_CHECK_EQUAL( model->HasVariant( variantName ), i != 1 );

        if( i == 1 )
        {
            BOOST_CHECK( !sheet );
            continue;
        }

        BOOST_REQUIRE( sheet );
        BOOST_CHECK( sheet->GetDNP( &parent, variantName ) );
        BOOST_CHECK( sheet->GetFieldText( wxS( "Channel" ), &parent, variantName ) == wxS( "override" ) );
        schematic::types::SheetSymbol stored;
        BOOST_REQUIRE( PackSheet( &stored, sheet, parent ) );
        BOOST_CHECK_EQUAL( stored.page_number(), std::to_string( 20 + i ) );
    }
}


BOOST_AUTO_TEST_CASE( CustomPropertyCannotDuplicateSystemProperty )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );
    ApiRequest beginRequest = makeBeginCommitRequest( *schematic );
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    commands::CreateItems command;
    command.mutable_header()->mutable_document()->CopyFrom( makeDocument( *schematic ) );

    schematic::types::SchematicLine line;
    line.mutable_id()->set_value( "83618809-06d6-4cbe-ba0e-ec60fe58921c" );
    line.set_type( schematic::types::SchematicLineType::SLT_WIRE );
    line.add_custom_properties()->set_key( "linE Width" );
    line.add_custom_properties()->set_key( "start X" );

    BOOST_REQUIRE( command.add_items()->PackFrom( line ) );

    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );

    const commands::ItemStatus& status = response.created_items( 0 ).status();

    // Should be rejected; duplicates built in property
    BOOST_CHECK_EQUAL( status.code(), kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA );
    BOOST_CHECK_NE( status.error_message().find( "Invalid custom properties" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( SymbolFieldTakesPrecedenceOverCustomProperty )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );
    ApiRequest beginRequest = makeBeginCommitRequest( *schematic );
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    commands::CreateItems command;
    command.mutable_header()->mutable_document()->CopyFrom( makeDocument( *schematic ) );

    schematic::types::SchematicSymbolInstance symbol;
    symbol.mutable_id()->set_value( "f8688bac-2fcb-4184-ab0e-0cf46a139c43" );
    symbol.mutable_transform()->set_orientation( schematic::types::SchematicSymbolOrientation::SSO_0 );

    symbol.mutable_definition()->mutable_id()->set_entry_name( "R" );

    schematic::types::SchematicField* field = symbol.add_user_fields();
    field->set_name( "MPN" );
    field->mutable_text()->set_text( "123" );

    types::CustomProperty* prop = symbol.add_custom_properties();
    prop->set_key( "MPN" );
    prop->set_value( "456" );

    BOOST_REQUIRE( command.add_items()->PackFrom( symbol ) );

    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );

    const commands::ItemStatus& status = response.created_items( 0 ).status();

    // The field takes precedence: a custom property duplicating a field name is rejected.
    BOOST_CHECK_EQUAL( status.code(), kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA );
    BOOST_CHECK_NE( status.error_message().find( "MPN" ), std::string::npos );
}

BOOST_AUTO_TEST_CASE( NetlistRebuildsSelectedConnectivityAndFiltersNativeItems )
{
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, false );
    SCHEMATIC* model = loadSchematic( wxS( "net_chains_four_nets_labeled" ) );
    const auto path = model->Hierarchy().front();
    SCH_LABEL* label = nullptr;

    for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_LABEL_T ) )
    {
        if( static_cast<SCH_LABEL*>( item )->GetText() == wxS( "SIG" ) )
            label = static_cast<SCH_LABEL*>( item );
    }

    BOOST_REQUIRE( label );
    API_HANDLER_SCH handler( m_context );

    for( bool published : { false, true } )
    {
        BOOST_TEST_CONTEXT( "published=" << published )
        {
            enabled = published;
            const wxString name = published ? wxS( "PUBLISHED_API" ) : wxS( "LEGACY_API" );
            label->SetText( name );

            // Only the engine handler rebuilds; legacy answers from the graph the editor keeps current
            if( published )
                model->ConnectionGraph()->Reset();
            else
                model->RebuildConnectivity();

            for( bool filtered : { false, true } )
            {
                kiapi::schematic::commands::GetSchematicNetlist command;
                *command.mutable_document() = makeDocument( *model );

                if( filtered )
                    command.add_types( kiapi::common::types::KOT_SCH_LABEL );

                ApiRequest request;
                request.mutable_header()->set_client_name( "kicad.qa" );
                BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
                API_RESULT result = handler.Handle( request );
                BOOST_REQUIRE( result.has_value() );
                kiapi::schematic::commands::SchematicNetlistResponse response;
                BOOST_REQUIRE( result->message().UnpackTo( &response ) );
                std::set<std::string> items;

                for( const auto& net : response.nets() )
                {
                    if( net.name() != ( wxS( "/" ) + name ).ToStdString() )
                        continue;

                    for( const auto& sheet : net.sheets() )
                    {
                        for( const auto& item : sheet.items() )
                            items.insert( item.value() );
                    }
                }

                BOOST_CHECK( items.contains( label->m_Uuid.AsStdString() ) );

                if( filtered )
                    BOOST_CHECK_EQUAL( items.size(), 1 );
                else
                    BOOST_CHECK_GT( items.size(), 1 );
            }
        }
    }
}

BOOST_AUTO_TEST_CASE( NetlistPreservesNativeHierarchyMembershipAndExcludesBuses )
{
    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, false );
    SCHEMATIC* model = loadSchematic( wxS( "netlists/hierarchy_aliases/hierarchy_aliases" ) );
    auto* root = model->Hierarchy().front().LastScreen();
    auto labels = root->Items().OfType( SCH_LABEL_T );
    BOOST_REQUIRE( labels.begin() != labels.end() );
    auto* unusedBus = static_cast<SCH_LABEL*>( ( *labels.begin() )->Clone() );
    const_cast<KIID&>( unusedBus->m_Uuid ) = KIID();
    unusedBus->SetText( wxS( "API_UNUSED[0..1]" ) );
    unusedBus->SetPosition( VECTOR2I( 0, 0 ) );
    root->Append( unusedBus );
    SCH_LINE* undriven = nullptr;

    for( SCH_ITEM* item : root->Items().OfType( SCH_LINE_T ) )
    {
        auto* line = static_cast<SCH_LINE*>( item );

        if( line->IsWire() )
        {
            undriven = static_cast<SCH_LINE*>( line->Clone() );
            break;
        }
    }

    BOOST_REQUIRE( undriven );
    const_cast<KIID&>( undriven->m_Uuid ) = KIID();
    undriven->Move( VECTOR2I( -1000000, -1000000 ) - undriven->GetStartPoint() );
    root->Append( undriven );
    API_HANDLER_SCH handler( m_context );
    using CONTENTS = std::map<std::string, std::map<std::vector<std::string>, std::set<std::string>>>;

    for( bool filtered : { false, true } )
    {
        CONTENTS expected;

        for( bool published : { false, true } )
        {
            BOOST_TEST_CONTEXT( "published=" << published << " filtered=" << filtered )
            {
                enabled = published;

                // Only the engine handler rebuilds; legacy answers from the graph the editor keeps current
                if( published )
                    model->ConnectionGraph()->Reset();
                else
                    model->RebuildConnectivity();

                kiapi::schematic::commands::GetSchematicNetlist command;
                *command.mutable_document() = makeDocument( *model );

                if( filtered )
                    command.add_types( kiapi::common::types::KOT_SCH_PIN );

                ApiRequest request;
                request.mutable_header()->set_client_name( "kicad.qa" );
                BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
                API_RESULT result = handler.Handle( request );
                BOOST_REQUIRE( result.has_value() );
                kiapi::schematic::commands::SchematicNetlistResponse response;
                BOOST_REQUIRE( result->message().UnpackTo( &response ) );
                CONTENTS actual;
                size_t nestedItems = 0;

                for( const auto& net : response.nets() )
                {
                    BOOST_CHECK_GT( net.sheets_size(), 0 );
                    BOOST_CHECK( net.name().find( '{' ) == std::string::npos );
                    BOOST_CHECK( net.name().find( "API_UNUSED" ) == std::string::npos );

                    for( const auto& sheet : net.sheets() )
                    {
                        std::vector<std::string> path;

                        for( const auto& id : sheet.path().path() )
                            path.push_back( id.value() );

                        auto& items = actual[net.name()][path];

                        for( const auto& item : sheet.items() )
                        {
                            BOOST_CHECK_NE( item.value(), undriven->m_Uuid.AsStdString() );
                            items.insert( item.value() );
                        }

                        if( path.size() > 2 )
                            nestedItems += items.size();
                    }
                }

                BOOST_CHECK( actual.contains( "/S0.BOOT.SDA" ) );
                BOOST_CHECK( actual.contains( "/S1.BOOT.SDA" ) );
                BOOST_CHECK_GT( nestedItems, 0 );

                if( published )
                {
                    BOOST_CHECK( actual == expected );
                }
                else
                {
                    expected = std::move( actual );
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE( NetlistReportsRebuildFailureAndAllowsRetry )
{
    struct FAILING_LABEL : SCH_LABEL
    {
        explicit FAILING_LABEL( const SCH_LABEL& source ) : SCH_LABEL( source ) {}

        wxString GetShownText( const SCH_SHEET_PATH* path, RESOLUTION_CONTEXT context, int depth = 0 ) const override
        {
            if( fail )
                throw std::runtime_error( "Native label extraction failed" );

            return SCH_LABEL::GetShownText( path, context, depth );
        }

        bool fail = false;
    };

    auto& enabled = const_cast<ADVANCED_CFG&>( ADVANCED_CFG::GetCfg() ).m_ConnectivityEngine;
    SCOPED_SET_RESET restore( enabled, false );
    SCHEMATIC* model = loadSchematic( wxS( "net_chains_four_nets_labeled" ) );
    auto* screen = model->Hierarchy().front().LastScreen();
    auto labels = screen->Items().OfType( SCH_LABEL_T );
    BOOST_REQUIRE( labels.begin() != labels.end() );
    auto* original = static_cast<SCH_LABEL*>( *labels.begin() );
    auto* replacement = new FAILING_LABEL( *original );
    screen->Remove( original );
    delete original;
    screen->Append( replacement );
    model->ConnectionGraph()->Reset();
    replacement->fail = true;
    enabled = true;
    API_HANDLER_SCH handler( m_context );
    kiapi::schematic::commands::GetSchematicNetlist command;
    *command.mutable_document() = makeDocument( *model );
    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_UNKNOWN );
    BOOST_CHECK_EQUAL( result.error().error_message(), "Native label extraction failed" );
    replacement->fail = false;
    result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    kiapi::schematic::commands::SchematicNetlistResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_GT( response.nets_size(), 0 );
}


BOOST_AUTO_TEST_CASE( StartErcTaskCompletesWithResults )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "NoConnectOnPin" ) );

    API_HANDLER_SCH handler( m_context );

    kiapi::common::ApiRequest startRequest = makeStartErcTaskRequest( *schematic );
    API_RESULT start = handler.Handle( startRequest );
    BOOST_REQUIRE( start.has_value() );

    kiapi::common::tasks::StartTaskResponse startResponse;
    BOOST_REQUIRE( start->message().UnpackTo( &startResponse ) );
    BOOST_REQUIRE( !startResponse.token().value().empty() );

    API_TASK_SCHEDULER::Instance().Pump();

    auto status = API_TASK_SCHEDULER::Instance().GetStatus( startResponse.token().value() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::tasks::TS_COMPLETED );

    kiapi::common::ApiRequest resultRequest = makeGetErcTaskResultRequest( startResponse.token().value() );
    API_RESULT result = handler.Handle( resultRequest );
    BOOST_REQUIRE( result.has_value() );

    kiapi::schematic::tasks::ErcTaskResult ercResult;
    BOOST_REQUIRE( result->message().UnpackTo( &ercResult ) );

    BOOST_CHECK_GT( ercResult.violations_size(), 0 );
    BOOST_CHECK_EQUAL( ercResult.excluded_count(), 0 );

    BOOST_CHECK_GT( ercResult.ignored_count(), 0 );

    int markerCount = 0;
    SCH_SCREENS screens( schematic->Root() );

    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->Type() == SCH_MARKER_T )
                ++markerCount;
        }
    }

    BOOST_CHECK_EQUAL( markerCount, ercResult.violations_size() );

    for( const kiapi::schematic::ErcMarker& violation : ercResult.violations() )
    {
        BOOST_CHECK( !violation.description().empty() );
        BOOST_CHECK( violation.error_type() != kiapi::schematic::ErcErrorType::ERCET_UNKNOWN );
    }
}


BOOST_AUTO_TEST_CASE( StartErcTaskWhileRunningIsRejected )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "NoConnectOnPin" ) );

    API_HANDLER_SCH handler( m_context );

    kiapi::common::ApiRequest startRequest = makeStartErcTaskRequest( *schematic );
    API_RESULT start = handler.Handle( startRequest );
    BOOST_REQUIRE( start.has_value() );

    kiapi::common::tasks::StartTaskResponse startResponse;
    BOOST_REQUIRE( start->message().UnpackTo( &startResponse ) );
    BOOST_REQUIRE( !startResponse.token().value().empty() );

    // The ERC run event has not been pumped, so the task is still running
    kiapi::common::ApiRequest secondRequest = makeStartErcTaskRequest( *schematic );
    API_RESULT second = handler.Handle( secondRequest );
    BOOST_REQUIRE( !second.has_value() );
    BOOST_CHECK_EQUAL( second.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );

    kiapi::common::ApiRequest runningRequest = makeGetErcTaskResultRequest( startResponse.token().value() );
    API_RESULT runningResult = handler.Handle( runningRequest );
    BOOST_REQUIRE( !runningResult.has_value() );
    BOOST_CHECK_EQUAL( runningResult.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );

    API_TASK_SCHEDULER::Instance().Pump();

    kiapi::common::ApiRequest finishedRequest =
            makeGetErcTaskResultRequest( startResponse.token().value() );
    API_RESULT finishedResult = handler.Handle( finishedRequest );
    BOOST_REQUIRE( finishedResult.has_value() );
}


BOOST_AUTO_TEST_CASE( GetErcTaskResultRejectsUnknownToken )
{
    loadSchematic( wxS( "NoConnectOnPin" ) );

    API_HANDLER_SCH handler( m_context );

    kiapi::common::ApiRequest request = makeGetErcTaskResultRequest( "deadbeef-0000-0000-0000-000000000000" );
    API_RESULT result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}

namespace
{

kiapi::schematic::commands::SymbolPinsResponse getSymbolPins( API_HANDLER_SCH& aHandler, const SCHEMATIC& aSchematic,
                                                              const std::vector<KIID>& aSymbols )
{
    kiapi::schematic::commands::GetSymbolPins command;
    *command.mutable_document() = makeDocument( aSchematic );

    for( const KIID& id : aSymbols )
        command.add_symbols()->set_value( id.AsStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = aHandler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::schematic::commands::SymbolPinsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    return response;
}


SCH_SYMBOL* firstMultiPinSymbol( SCHEMATIC& aSchematic, SCH_SHEET_PATH& aPath )
{
    for( const SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->GetPins( &path ).size() >= 2 && !symbol->IsPower() )
            {
                aPath = path;
                return symbol;
            }
        }
    }

    return nullptr;
}

// The text the editor puts on the clipboard for these items of aPath's screen
std::string formatItems( SCHEMATIC& aSchematic, SCH_SHEET_PATH& aPath, const std::vector<SCH_ITEM*>& aItems )
{
    SCH_SELECTION selection( aPath.LastScreen() );

    for( SCH_ITEM* item : aItems )
        selection.Add( item );

    STRING_FORMATTER   formatter;
    SCH_IO_KICAD_SEXPR plugin;

    plugin.Format( &selection, &aPath, aSchematic, &formatter, true );
    return formatter.GetString();
}


kiapi::common::ApiRequest makePasteRequest( const SCHEMATIC& aSchematic, const std::string& aContents,
                                            const VECTOR2I& aOffset, const SCH_SHEET_PATH* aPath = nullptr )
{
    kiapi::common::commands::ParseAndCreateItemsFromString command;
    *command.mutable_document() = makeDocument( aSchematic );
    command.set_contents( aContents );
    kiapi::common::PackVector2( *command.mutable_offset(), aOffset, schIUScale );

    if( aPath )
        kiapi::common::PackSheetPath( *command.mutable_document()->mutable_sheet_path(), aPath->Path() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


API_RESULT paste( API_HANDLER_SCH& aHandler, const SCHEMATIC& aSchematic, const std::string& aContents,
                  const VECTOR2I& aOffset, const SCH_SHEET_PATH* aPath = nullptr )
{
    kiapi::common::ApiRequest request = makePasteRequest( aSchematic, aContents, aOffset, aPath );
    return aHandler.Handle( request );
}


// Every non-power, single-unit reference of the design is unique
void checkUniqueReferences( SCHEMATIC& aSchematic )
{
    std::set<wxString> refs;

    for( const SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->IsPower() || symbol->GetUnitCount() > 1 )
                continue;

            wxString ref = symbol->GetRef( &path, false );

            if( !ref.EndsWith( wxS( "?" ) ) )
                BOOST_CHECK_MESSAGE( refs.insert( path.PathAsString() + ref ).second, "duplicate reference " << ref );
        }
    }
}


std::set<KIID> allIds( SCHEMATIC& aSchematic )
{
    std::set<KIID> ids;

    for( const SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items() )
            ids.insert( item->m_Uuid );
    }

    return ids;
}

} // namespace


BOOST_AUTO_TEST_CASE( GetSymbolPinsReportsSheetPositions )
{
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );
    auto            response = getSymbolPins( handler, *model, {} );

    size_t expectedSymbols = 0;

    for( const SCH_SHEET_PATH& path : model->Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            wxUnusedVar( item );
            expectedSymbols++;
        }
    }

    BOOST_CHECK_EQUAL( static_cast<size_t>( response.symbols_size() ), expectedSymbols );

    for( const kiapi::schematic::commands::SymbolPins& entry : response.symbols() )
    {
        auto* symbol = dynamic_cast<SCH_SYMBOL*>( model->ResolveItem( KIID( entry.symbol().value() ) ) );
        BOOST_REQUIRE( symbol );

        SCH_SHEET_PATH path;
        model->ResolveItem( symbol->m_Uuid, &path );

        for( const kiapi::schematic::commands::PlacedSymbolPin& info : entry.pins() )
        {
            SCH_PIN* pin = nullptr;

            for( SCH_PIN* candidate : symbol->GetPins( &path ) )
            {
                if( candidate->m_Uuid.AsStdString() == info.id().value() )
                    pin = candidate;
            }

            BOOST_REQUIRE( pin );
            BOOST_CHECK( kiapi::common::UnpackVector2( info.position(), schIUScale ) == pin->GetPosition() );
            BOOST_CHECK_EQUAL( info.number(), pin->GetNumber().ToStdString() );
        }
    }
}


BOOST_AUTO_TEST_CASE( GetSymbolPinsFollowsRotation )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH path;
    SCH_SYMBOL*    symbol = firstMultiPinSymbol( *model, path );
    BOOST_REQUIRE( symbol );

    symbol->SetOrientation( SYM_ORIENT_0 );

    API_HANDLER_SCH handler( m_context );
    auto            before = getSymbolPins( handler, *model, { symbol->m_Uuid } );

    symbol->SetOrientation( SYM_ORIENT_90 );
    auto after = getSymbolPins( handler, *model, { symbol->m_Uuid } );

    BOOST_REQUIRE_EQUAL( before.symbols_size(), 1 );
    BOOST_REQUIRE_EQUAL( after.symbols_size(), 1 );
    BOOST_REQUIRE_EQUAL( before.symbols( 0 ).pins_size(), after.symbols( 0 ).pins_size() );

    const VECTOR2I anchor = symbol->GetPosition();

    // Rotating 90 degrees counter-clockwise on a Y-down sheet maps an offset (x, y) to (y, -x)
    for( int i = 0; i < before.symbols( 0 ).pins_size(); ++i )
    {
        VECTOR2I p0 = kiapi::common::UnpackVector2( before.symbols( 0 ).pins( i ).position(), schIUScale ) - anchor;
        VECTOR2I p1 = kiapi::common::UnpackVector2( after.symbols( 0 ).pins( i ).position(), schIUScale ) - anchor;

        BOOST_CHECK_EQUAL( p1.x, p0.y );
        BOOST_CHECK_EQUAL( p1.y, -p0.x );
    }
}


BOOST_AUTO_TEST_CASE( GetBoundingBoxReportsSchematicItems )
{
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );

    SCH_SHEET_PATH path;
    SCH_SYMBOL*    symbol = firstMultiPinSymbol( *model, path );
    BOOST_REQUIRE( symbol );

    SCH_LABEL_BASE* label = nullptr;
    SCH_SHEET*      sheet = nullptr;

    for( const SCH_SHEET_PATH& p : model->Hierarchy() )
    {
        for( SCH_ITEM* item : p.LastScreen()->Items() )
        {
            if( !label && item->IsType( { SCH_LABEL_T, SCH_GLOBAL_LABEL_T, SCH_HIER_LABEL_T } ) )
                label = static_cast<SCH_LABEL_BASE*>( item );

            if( !sheet && item->Type() == SCH_SHEET_T )
                sheet = static_cast<SCH_SHEET*>( item );
        }
    }

    BOOST_REQUIRE( label );
    BOOST_REQUIRE( sheet );

    API_HANDLER_SCH handler( m_context );

    auto query = [&]( const std::vector<KIID>& aIds, kiapi::common::commands::BoundingBoxMode aMode )
    {
        kiapi::common::commands::GetBoundingBox command;
        *command.mutable_header()->mutable_document() = makeDocument( *model );
        command.set_mode( aMode );

        for( const KIID& id : aIds )
            command.add_items()->set_value( id.AsStdString() );

        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

        API_RESULT result = handler.Handle( request );
        BOOST_REQUIRE( result.has_value() );
        BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

        kiapi::common::commands::GetBoundingBoxResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response;
    };

    auto box = []( const kiapi::common::commands::GetBoundingBoxResponse& aResponse, int aIndex )
    {
        return kiapi::common::UnpackBox2( aResponse.boxes( aIndex ), schIUScale );
    };

    KIID unknown;
    auto withText = query( { symbol->m_Uuid, label->m_Uuid, sheet->m_Uuid, unknown },
                           kiapi::common::commands::BBM_ITEM_AND_CHILD_TEXT );
    auto bodyOnly = query( { symbol->m_Uuid, label->m_Uuid, sheet->m_Uuid },
                           kiapi::common::commands::BBM_ITEM_ONLY );

    // The unknown id is skipped
    BOOST_REQUIRE_EQUAL( withText.items_size(), 3 );
    BOOST_REQUIRE_EQUAL( withText.boxes_size(), 3 );
    BOOST_REQUIRE_EQUAL( bodyOnly.boxes_size(), 3 );

    BOOST_CHECK_EQUAL( withText.items( 0 ).value(), symbol->m_Uuid.AsStdString() );
    BOOST_CHECK( box( withText, 0 ) == symbol->GetBoundingBox() );
    BOOST_CHECK( box( bodyOnly, 0 ) == symbol->GetBodyAndPinsBoundingBox() );

    BOOST_CHECK( box( withText, 1 ) == label->GetBoundingBox() );
    BOOST_CHECK( box( bodyOnly, 1 ) == label->GetBodyBoundingBox( nullptr ) );

    BOOST_CHECK( box( withText, 2 ) == sheet->GetBoundingBox() );
    BOOST_CHECK( box( bodyOnly, 2 ) == sheet->GetBodyBoundingBox() );

    // The box with child text always contains the body box
    for( int i = 0; i < 3; ++i )
        BOOST_CHECK( box( withText, i ).Contains( box( bodyOnly, i ) ) );

    // A request for a different document type is left for another handler
    kiapi::common::commands::GetBoundingBox boardCommand;
    boardCommand.mutable_header()->mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( boardCommand ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( PasteCreatesCopiesWithNewIds )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    SCH_SCREEN*    screen = root.LastScreen();

    std::vector<SCH_ITEM*> source;
    std::set<VECTOR2I>     symbolPositions;
    size_t                 symbols = 0;

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() == SCH_SHEET_T )
            continue;

        source.push_back( item );

        if( item->Type() == SCH_SYMBOL_T )
        {
            symbolPositions.insert( item->GetPosition() );
            symbols++;
        }
    }

    BOOST_REQUIRE_GT( symbols, 0u );

    const std::string text = formatItems( *model, root, source );
    const size_t      before = screen->Items().size();
    const VECTOR2I    offset( schIUScale.MilsToIU( 4000 ), schIUScale.MilsToIU( 500 ) );
    std::set<KIID>    idsBefore = allIds( *model );

    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = paste( handler, *model, text, offset );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::common::commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    BOOST_CHECK_EQUAL( static_cast<size_t>( response.created_items_size() ), source.size() );
    BOOST_CHECK_EQUAL( screen->Items().size(), before + source.size() );

    for( const kiapi::common::commands::ItemCreationResult& created : response.created_items() )
        BOOST_CHECK_EQUAL( created.status().code(), kiapi::common::commands::ItemStatusCode::ISC_OK );

    // Every pasted item is new, symbols moved by the offset and carry their library symbol
    size_t pastedSymbols = 0;

    for( SCH_ITEM* item : screen->Items() )
    {
        if( idsBefore.contains( item->m_Uuid ) )
            continue;

        if( item->Type() == SCH_SYMBOL_T )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
            bool        moved = false;

            for( const VECTOR2I& pos : symbolPositions )
                moved |= symbol->GetPosition() == pos + offset;

            BOOST_CHECK_MESSAGE( moved, "symbol not shifted by the offset" );
            BOOST_CHECK( symbol->GetLibSymbolRef() );
            pastedSymbols++;

            for( SCH_PIN* pin : symbol->GetPins( &root ) )
                BOOST_CHECK( !idsBefore.contains( pin->m_Uuid ) );
        }
    }

    BOOST_CHECK_EQUAL( pastedSymbols, symbols );
    checkUniqueReferences( *model );

    // The same text pasted again does not repeat an identifier or a reference.  It goes
    // elsewhere: wires on top of existing ones would be merged by the cleanup of the commit.
    result = paste( handler, *model, text, offset + offset );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    for( const kiapi::common::commands::ItemCreationResult& created : response.created_items() )
        BOOST_CHECK_MESSAGE( created.status().code() == kiapi::common::commands::ItemStatusCode::ISC_OK,
                             created.status().error_message() );

    size_t symbolCount = 0;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        wxUnusedVar( item );
        symbolCount++;
    }

    BOOST_CHECK_EQUAL( symbolCount, 3 * symbols );
    checkUniqueReferences( *model );
}


BOOST_AUTO_TEST_CASE( PasteRejectsInvalidContents )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    const size_t   before = root.LastScreen()->Items().size();

    API_HANDLER_SCH handler( m_context );

    for( const std::string& text : { std::string( "this is not a schematic" ), std::string( "(wire (pts (xy 0 0)" ),
                                     std::string() } )
    {
        API_RESULT result = paste( handler, *model, text, VECTOR2I( 0, 0 ) );

        BOOST_REQUIRE( !result.has_value() );
        BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
        BOOST_CHECK_EQUAL( root.LastScreen()->Items().size(), before );
    }
}


BOOST_AUTO_TEST_CASE( PasteFindsLibrarySymbolInDestination )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    SCH_SYMBOL*    symbol = nullptr;

    for( SCH_ITEM* item : root.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        symbol = static_cast<SCH_SYMBOL*>( item );
        break;
    }

    BOOST_REQUIRE( symbol );

    // Only the symbol, without the (lib_symbols) section the editor adds for it
    std::string text = formatItems( *model, root, { symbol } );
    size_t      start = text.find( "(lib_symbols" );
    BOOST_REQUIRE( start != std::string::npos );

    int depth = 0;
    size_t end = start;

    for( ; end < text.size(); ++end )
    {
        depth += text[end] == '(' ? 1 : text[end] == ')' ? -1 : 0;

        if( depth == 0 )
            break;
    }

    text.erase( start, end - start + 1 );

    const size_t    before = root.LastScreen()->Items().size();
    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = paste( handler, *model, text, VECTOR2I( 0, 0 ) );

    // The destination screen knows the symbol, so it still pastes; a symbol it does not know
    // would be refused
    BOOST_REQUIRE( result.has_value() );
    kiapi::common::commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );
    BOOST_CHECK_EQUAL( response.created_items( 0 ).status().code(), kiapi::common::commands::ItemStatusCode::ISC_OK );
    BOOST_CHECK_EQUAL( root.LastScreen()->Items().size(), before + 1 );

    // A symbol that neither the text nor the destination defines is refused
    size_t idPos = text.find( "(lib_id \"" );
    BOOST_REQUIRE( idPos != std::string::npos );
    size_t nameEnd = text.find( '"', idPos + 9 );
    text.replace( idPos + 9, nameEnd - idPos - 9, "Nowhere:Missing" );

    result = paste( handler, *model, text, VECTOR2I( 0, 0 ) );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );
    BOOST_CHECK_EQUAL( response.created_items( 0 ).status().code(),
                       kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA );
    BOOST_CHECK_EQUAL( root.LastScreen()->Items().size(), before + 1 );
}


BOOST_AUTO_TEST_CASE( PasteDropsRefusedMembersFromGroups )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    SCH_SCREEN*    screen = root.LastScreen();
    SCH_GROUP*     group = nullptr;
    SCH_SYMBOL*    symbol = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_GROUP_T ) )
        group = static_cast<SCH_GROUP*>( item );

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        symbol = static_cast<SCH_SYMBOL*>( item );
        break;
    }

    BOOST_REQUIRE( group && symbol );

    std::vector<SCH_ITEM*> source = { group, symbol };

    for( EDA_ITEM* member : group->GetItems() )
        source.push_back( static_cast<SCH_ITEM*>( member ) );

    // The symbol joins the group in the text, but refers to a library symbol nobody has
    std::string text = formatItems( *model, root, source );
    size_t      start = text.find( "(lib_symbols" );
    BOOST_REQUIRE( start != std::string::npos );

    int    depth = 0;
    size_t end = start;

    for( ; end < text.size(); ++end )
    {
        depth += text[end] == '(' ? 1 : text[end] == ')' ? -1 : 0;

        if( depth == 0 )
            break;
    }

    text.erase( start, end - start + 1 );

    size_t idPos = text.find( "(lib_id \"" );
    BOOST_REQUIRE( idPos != std::string::npos );
    text.replace( idPos + 9, text.find( '"', idPos + 9 ) - idPos - 9, "Nowhere:Missing" );

    size_t membersPos = text.find( "(members " );
    BOOST_REQUIRE( membersPos != std::string::npos );
    text.insert( membersPos + 9, "\"" + symbol->m_Uuid.AsStdString() + "\" " );

    const VECTOR2I  offset( schIUScale.MilsToIU( 3000 ), schIUScale.MilsToIU( 1000 ) );
    std::set<KIID>  idsBefore = allIds( *model );
    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = paste( handler, *model, text, offset );

    BOOST_REQUIRE( result.has_value() );
    kiapi::common::commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    int refused = 0;

    for( const kiapi::common::commands::ItemCreationResult& created : response.created_items() )
    {
        if( created.status().code() == kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA )
            refused++;
    }

    BOOST_CHECK_EQUAL( refused, 1 );

    SCH_GROUP* pasted = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_GROUP_T ) )
    {
        if( !idsBefore.contains( item->m_Uuid ) )
            pasted = static_cast<SCH_GROUP*>( item );
    }

    // The group keeps the members that were pasted and moves them with it
    BOOST_REQUIRE( pasted );
    BOOST_CHECK_EQUAL( pasted->GetItems().size(), group->GetItems().size() );
    BOOST_CHECK_EQUAL( pasted->GetBoundingBox().GetPosition(), group->GetBoundingBox().GetPosition() + offset );
}


BOOST_AUTO_TEST_CASE( PasteSheetSharesBaseReferencesInSuffixMode )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    SCH_SHEET*     sheet = nullptr;

    for( SCH_ITEM* item : root.LastScreen()->Items().OfType( SCH_SHEET_T ) )
    {
        sheet = static_cast<SCH_SHEET*>( item );
        break;
    }

    BOOST_REQUIRE( sheet && sheet->GetScreen() );

    model->Settings().m_AnnotateReusedSheetSuffixes = true;
    model->Settings().m_IncrementPastedSuffix = true;
    sheet->GetField( FIELD_T::SHEET_REF_SUFFIX )->SetText( wxS( "A" ) );

    SCH_SCREEN*       subScreen = sheet->GetScreen();
    const std::string text = formatItems( *model, root, { sheet } );

    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = paste( handler, *model, text, VECTOR2I( 0, schIUScale.MilsToIU( 2000 ) ) );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    SCH_SHEET_LIST paths = model->Hierarchy().FindAllSheetsForScreen( subScreen );
    BOOST_REQUIRE_EQUAL( paths.size(), 2 );

    // Both instances of the reused sheet number their symbols from one base, told apart by the suffix
    for( SCH_ITEM* item : subScreen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->IsPower() )
            continue;

        BOOST_CHECK_EQUAL( symbol->GetBaseRef( &paths[0] ), symbol->GetBaseRef( &paths[1] ) );
        BOOST_CHECK_NE( symbol->GetRef( &paths[0], false ), symbol->GetRef( &paths[1], false ) );
    }

    // A copy of one of its symbols pasted into the reused sheet takes a new base, the same for both
    SCH_SYMBOL* original = nullptr;

    for( SCH_ITEM* item : subScreen->Items().OfType( SCH_SYMBOL_T ) )
    {
        if( !static_cast<SCH_SYMBOL*>( item )->IsPower() )
            original = static_cast<SCH_SYMBOL*>( item );
    }

    BOOST_REQUIRE( original );

    std::set<KIID> idsBefore = allIds( *model );
    result = paste( handler, *model, formatItems( *model, paths[0], { original } ),
                    VECTOR2I( schIUScale.MilsToIU( 1000 ), 0 ), &paths[0] );
    BOOST_REQUIRE( result.has_value() );

    SCH_SYMBOL* copy = nullptr;

    for( SCH_ITEM* item : subScreen->Items().OfType( SCH_SYMBOL_T ) )
    {
        if( !idsBefore.contains( item->m_Uuid ) )
            copy = static_cast<SCH_SYMBOL*>( item );
    }

    BOOST_REQUIRE( copy );
    BOOST_CHECK_NE( copy->GetBaseRef( &paths[0] ), original->GetBaseRef( &paths[0] ) );
    BOOST_CHECK_EQUAL( copy->GetBaseRef( &paths[0] ), copy->GetBaseRef( &paths[1] ) );
}


BOOST_AUTO_TEST_CASE( PasteIntoSubsheet )
{
    SCHEMATIC*      model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_LIST  hierarchy = model->Hierarchy();
    SCH_SHEET_PATH* sub = nullptr;

    for( SCH_SHEET_PATH& path : hierarchy )
    {
        if( path.size() > 1 )
            sub = &path;
    }

    BOOST_REQUIRE( sub );

    SCH_SHEET_PATH root = hierarchy.at( 0 );
    SCH_LABEL_BASE* label = nullptr;

    for( SCH_ITEM* item : root.LastScreen()->Items() )
    {
        if( item->Type() == SCH_LABEL_T )
        {
            label = static_cast<SCH_LABEL_BASE*>( item );
            break;
        }
    }

    BOOST_REQUIRE( label );

    const std::string text = formatItems( *model, root, { label } );
    const size_t      rootBefore = root.LastScreen()->Items().size();
    const size_t      subBefore = sub->LastScreen()->Items().size();

    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = paste( handler, *model, text, VECTOR2I( 0, 0 ), sub );

    BOOST_REQUIRE( result.has_value() );
    BOOST_CHECK_EQUAL( root.LastScreen()->Items().size(), rootBefore );
    BOOST_CHECK_EQUAL( sub->LastScreen()->Items().size(), subBefore + 1 );
}


BOOST_AUTO_TEST_CASE( PasteSheetReusesScreen )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    SCH_SHEET*     sheet = nullptr;

    for( SCH_ITEM* item : root.LastScreen()->Items().OfType( SCH_SHEET_T ) )
    {
        sheet = static_cast<SCH_SHEET*>( item );
        break;
    }

    BOOST_REQUIRE( sheet );

    SCH_SCREEN* subScreen = sheet->GetScreen();
    BOOST_REQUIRE( subScreen );

    const std::string text = formatItems( *model, root, { sheet } );
    const size_t      sheetsBefore = model->Hierarchy().size();
    std::set<KIID>    idsBefore = allIds( *model );

    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = paste( handler, *model, text, VECTOR2I( 0, schIUScale.MilsToIU( 2000 ) ) );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::common::commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );
    BOOST_REQUIRE_EQUAL( response.created_items( 0 ).status().code(), kiapi::common::commands::ItemStatusCode::ISC_OK );

    SCH_SHEET* pasted = nullptr;

    for( SCH_ITEM* item : root.LastScreen()->Items().OfType( SCH_SHEET_T ) )
    {
        if( !idsBefore.contains( item->m_Uuid ) )
            pasted = static_cast<SCH_SHEET*>( item );
    }

    BOOST_REQUIRE( pasted );
    BOOST_CHECK( pasted->GetScreen() == subScreen );
    BOOST_CHECK_NE( pasted->GetName(), sheet->GetName() );

    // The sheet is a second instance of the same file
    BOOST_CHECK_EQUAL( model->Hierarchy().size(), sheetsBefore + 1 );

    for( const SCH_SHEET_PATH& path : model->Hierarchy().FindAllSheetsForScreen( subScreen ) )
    {
        for( SCH_ITEM* item : subScreen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL*         symbol = static_cast<SCH_SYMBOL*>( item );
            SCH_SYMBOL_INSTANCE instance;

            BOOST_CHECK_MESSAGE( symbol->GetInstance( instance, path.Path() ),
                                 "symbol without an instance for " << path.PathAsString() );
        }
    }
}


BOOST_AUTO_TEST_CASE( PasteSheetWithFileOutsideHierarchyIsRejected )
{
    SCHEMATIC*     model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    SCH_SHEET*     sheet = nullptr;

    for( SCH_ITEM* item : root.LastScreen()->Items().OfType( SCH_SHEET_T ) )
    {
        sheet = static_cast<SCH_SHEET*>( item );
        break;
    }

    BOOST_REQUIRE( sheet );

    // Points at a file that exists next to the schematic but is not part of it
    std::string text = formatItems( *model, root, { sheet } );
    size_t      pos = text.find( "erc_test_dynamic_power_symbol_subsheet.kicad_sch" );
    BOOST_REQUIRE( pos != std::string::npos );
    text.replace( pos, std::string( "erc_test_dynamic_power_symbol_subsheet.kicad_sch" ).size(),
                  "erc_label_test.kicad_sch" );

    const size_t sheetsBefore = model->Hierarchy().size();
    const size_t itemsBefore = root.LastScreen()->Items().size();

    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = paste( handler, *model, text, VECTOR2I( 0, 0 ) );

    BOOST_REQUIRE( result.has_value() );

    kiapi::common::commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );
    BOOST_CHECK_EQUAL( response.created_items( 0 ).status().code(),
                       kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA );
    BOOST_CHECK_EQUAL( root.LastScreen()->Items().size(), itemsBefore );
    BOOST_CHECK_EQUAL( model->Hierarchy().size(), sheetsBefore );
}


BOOST_AUTO_TEST_CASE( AnnotateSchematicNumbersUnannotatedSymbols )
{
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );

    // Clear every non-power reference so the handler has to number all of them
    std::map<wxString, int> expectedPerPrefix;

    for( const SCH_SHEET_PATH& path : model->Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->IsPower() )
                continue;

            wxString prefix = symbol->GetPrefix();
            symbol->SetRef( &path, prefix + wxS( "?" ) );
            expectedPerPrefix[prefix]++;
        }
    }

    BOOST_REQUIRE( !expectedPerPrefix.empty() );

    kiapi::schematic::commands::AnnotateSchematic command;
    *command.mutable_document() = makeDocument( *model );
    command.set_scope( kiapi::schematic::commands::ANS_ALL );
    command.set_start_number( 1 );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_HANDLER_SCH handler( m_context );
    API_RESULT      result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::schematic::commands::AnnotateSchematicResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    int expectedChanges = 0;

    for( const auto& [prefix, count] : expectedPerPrefix )
        expectedChanges += count;

    BOOST_CHECK_GE( response.changes_size(), 1 );

    // Every symbol now has a numbered, unique reference and numbering starts at 1 per prefix
    std::set<wxString> refs;
    std::map<wxString, int> lowest;

    for( const SCH_SHEET_PATH& path : model->Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->IsPower() )
                continue;

            wxString ref = symbol->GetRef( &path, false );
            BOOST_CHECK_MESSAGE( !ref.EndsWith( wxS( "?" ) ), "unannotated: " << ref );

            if( symbol->GetUnitCount() == 1 )
                BOOST_CHECK_MESSAGE( refs.insert( ref ).second, "duplicate reference " << ref );

            long number = 0;
            ref.Mid( symbol->GetPrefix().length() ).ToLong( &number );

            auto it = lowest.find( symbol->GetPrefix() );
            lowest[symbol->GetPrefix()] = it == lowest.end() ? number : std::min<long>( it->second, number );
        }
    }

    for( const auto& [prefix, number] : lowest )
        BOOST_CHECK_MESSAGE( number == 1, prefix << " numbering starts at " << number );

    BOOST_CHECK_LE( response.changes_size(), expectedChanges );
}



BOOST_AUTO_TEST_CASE( BusMembershipAndAliasUndoImages )
{
    using namespace kiapi::schematic::commands;
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    API_HANDLER_SCH handler( m_context );
    auto set = [&]( const std::string& vector )
    {
        SetSchematicBusAliases command;
        *command.mutable_document() = makeDocument( *model );
        auto* alias = command.add_aliases();
        alias->set_name( "DATA" );
        alias->add_members( vector );
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        auto result = handler.Handle( request );
        BOOST_REQUIRE( result.has_value() );
    };
    auto count = [&]()
    {
        GetSchematicBuses command;
        *command.mutable_document() = makeDocument( *model );
        command.set_expression( "{DATA}" );
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        auto result = handler.Handle( request );
        BOOST_REQUIRE( result.has_value() );
        SchematicBusesResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response.expression_members_size();
    };
    set( "D[0..3]" );
    BOOST_CHECK_EQUAL( count(), 4 );
    model->Root().CaptureBusAliasesForUndo();
    std::unique_ptr<SCH_SHEET> image( static_cast<SCH_SHEET*>( model->Root().Clone() ) );
    model->Root().ClearBusAliasesUndo();
    set( "D[0..7]" );
    BOOST_CHECK_EQUAL( count(), 8 );
    model->Root().SwapItemData( image.get() );
    BOOST_CHECK_EQUAL( count(), 4 );
    model->Root().SwapItemData( image.get() );
    BOOST_CHECK_EQUAL( count(), 8 );
}

BOOST_AUTO_TEST_CASE( SheetSourceSwapRestoresBackingScreen )
{
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET* sheet = nullptr;
    for( SCH_ITEM* item : model->RootScreen()->Items().OfType( SCH_SHEET_T ) )
        sheet = static_cast<SCH_SHEET*>( item );
    BOOST_REQUIRE( sheet );
    SCH_SCREEN* original = sheet->GetScreen();
    std::unique_ptr<SCH_SHEET> image( static_cast<SCH_SHEET*>( sheet->Clone() ) );
    SCH_SCREEN* replacement = new SCH_SCREEN( model );
    sheet->SetScreen( replacement );
    sheet->SwapItemData( image.get() );
    BOOST_CHECK( sheet->GetScreen() == original );
    sheet->SwapItemData( image.get() );
    BOOST_CHECK( sheet->GetScreen() == replacement );
}

BOOST_AUTO_TEST_CASE( PastePackageRejectsIncompleteHierarchyWithoutMutation )
{
    using namespace kiapi::common;
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH root = model->Hierarchy().at( 0 );
    SCH_SHEET* child = nullptr;
    for( SCH_ITEM* item : root.LastScreen()->Items().OfType( SCH_SHEET_T ) )
        child = static_cast<SCH_SHEET*>( item );
    BOOST_REQUIRE( child );
    std::string text = formatItems( *model, root, { child } );
    commands::ParseAndCreateItemsFromString command;
    *command.mutable_document() = makeDocument( *model );
    command.set_contents( text );
    // Even an unused malformed package must be rejected before any live item is touched.
    ( *command.mutable_schematic_sources() )["invalid.kicad_sch"] = "not a schematic";
    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    API_HANDLER_SCH handler( m_context );
    const size_t before = root.LastScreen()->Items().size();
    auto result = handler.Handle( request );
    BOOST_CHECK( !result.has_value() );
    BOOST_CHECK_EQUAL( root.LastScreen()->Items().size(), before );
}


BOOST_AUTO_TEST_CASE( LayoutGeometryReportsNativeBodiesFieldsAndDrawingSheet )
{
    using namespace kiapi::schematic::commands;
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    model->RootScreen()->Append( new SCH_TEXT( VECTOR2I( 1000000, 1000000 ),
                                             wxS( "Layout note ${PROJECTNAME}" ) ) );
    API_HANDLER_SCH handler( m_context );
    GetSchematicLayoutGeometry command;
    *command.mutable_document() = makeDocument( *model );
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    auto result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    SchematicLayoutGeometryResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( response.sheets_size(), model->Hierarchy().size() );
    std::set<std::string> kinds;

    for( const auto& sheet : response.sheets() )
    {
        BOOST_CHECK_GT( sheet.page().size().x_nm(), 0 );
        BOOST_CHECK_GT( sheet.page().size().y_nm(), 0 );
        BOOST_CHECK( !sheet.file().empty() );

        for( const auto& element : sheet.elements() )
        {
            kinds.insert( element.kind() );
            BOOST_CHECK_GE( element.box().size().x_nm(), 0 );
            BOOST_CHECK_GE( element.box().size().y_nm(), 0 );

            if( element.kind() == "field" )
                BOOST_CHECK( !element.key().empty() );
        }
    }

    BOOST_CHECK( kinds.count( "body" ) );
    BOOST_CHECK( kinds.count( "pin" ) );
    BOOST_CHECK( kinds.count( "field" ) );
    BOOST_CHECK( kinds.count( "text" ) );
    BOOST_CHECK( kinds.count( "drawing_sheet" ) );
}


BOOST_AUTO_TEST_CASE( LayoutRejectsWrongResultAndRestoresDirtyState )
{
    using namespace kiapi::schematic::commands;
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    API_HANDLER_SCH handler( m_context );
    auto call = [&]( const auto& command )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa.layout" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return handler.Handle( request );
    };
    auto capture = [&]()
    {
        GetSchematicLayoutGeometry command;
        *command.mutable_document() = makeDocument( *model );
        auto result = call( command );
        BOOST_REQUIRE( result.has_value() );
        SchematicLayoutGeometryResponse geometry;
        BOOST_REQUIRE( result->message().UnpackTo( &geometry ) );
        return geometry;
    };
    auto before = capture();
    SCH_SHEET_PATH path;
    SCH_SYMBOL* symbol = firstMultiPinSymbol( *model, path );
    BOOST_REQUIRE( symbol );
    const bool dirty = path.LastScreen()->IsContentModified();
    const auto originalPage = path.LastScreen()->GetPageSettings().GetType();
    ApplySchematicLayout command;
    *command.mutable_document() = makeDocument( *model );
    command.set_expected_revision( before.revision() );
    command.set_expected_project_revision( before.project_revision() );
    command.set_result_revision( "not-the-verified-result" );
    auto* sheet = command.add_sheets();
    PackSheetPath( *sheet->mutable_path(), path );
    sheet->mutable_page()->set_page_size( kiapi::common::types::PS_A2 );
    auto* move = sheet->add_moves();
    move->mutable_item()->set_value( symbol->m_Uuid.AsStdString() );
    move->mutable_offset()->set_x_nm( 12700000 );
    schematic::types::SchematicLine wire;
    wire.mutable_id()->set_value( KIID().AsStdString() );
    wire.set_type( schematic::types::SLT_WIRE );
    wire.mutable_start()->set_x_nm( 10000000 );
    wire.mutable_start()->set_y_nm( 10000000 );
    wire.mutable_end()->set_x_nm( 20000000 );
    wire.mutable_end()->set_y_nm( 10000000 );
    BOOST_REQUIRE( sheet->add_create_items()->PackFrom( wire ) );
    auto rejected = call( command );
    BOOST_REQUIRE( !rejected.has_value() );
    BOOST_CHECK_EQUAL( capture().revision(), before.revision() );
    BOOST_CHECK_EQUAL( path.LastScreen()->IsContentModified(), dirty );
    BOOST_CHECK( path.LastScreen()->GetPageSettings().GetType() == originalPage );
    BOOST_CHECK( !model->Hierarchy().ResolveItem( KIID( wire.id().value() ), nullptr, true ) );

    command.clear_result_revision();
    command.set_evaluate( true );
    auto result = call( command );
    BOOST_REQUIRE( result.has_value() );
    ApplySchematicLayoutResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK( response.revision() != before.revision() );
    BOOST_CHECK_EQUAL( response.revision(), capture().revision() );
    BOOST_CHECK( path.LastScreen()->GetPageSettings().GetType() == PAGE_SIZE_TYPE::A2 );
}


BOOST_AUTO_TEST_CASE( LayoutRejectsLockedItemsAndRevisionConflicts )
{
    using namespace kiapi::schematic::commands;
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    API_HANDLER_SCH handler( m_context );
    SCH_SHEET_PATH path;
    SCH_SYMBOL* symbol = firstMultiPinSymbol( *model, path );
    BOOST_REQUIRE( symbol );
    symbol->SetLocked( true );
    auto call = [&]( const auto& command )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa.layout" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
        return handler.Handle( request );
    };
    GetSchematicLayoutGeometry read;
    *read.mutable_document() = makeDocument( *model );
    auto result = call( read );
    BOOST_REQUIRE( result.has_value() );
    SchematicLayoutGeometryResponse geometry;
    BOOST_REQUIRE( result->message().UnpackTo( &geometry ) );
    ApplySchematicLayout command;
    *command.mutable_document() = makeDocument( *model );
    command.set_evaluate( true );
    command.set_expected_revision( geometry.revision() );
    command.set_expected_project_revision( geometry.project_revision() );
    auto* changes = command.add_sheets();
    PackSheetPath( *changes->mutable_path(), path );
    changes->add_moves()->mutable_item()->set_value( symbol->m_Uuid.AsStdString() );
    const VECTOR2I position = symbol->GetPosition();
    BOOST_REQUIRE( !call( command ).has_value() );
    BOOST_CHECK( symbol->GetPosition() == position );
    command.clear_sheets();
    command.set_expected_revision( "stale-revision" );
    BOOST_REQUIRE( !call( command ).has_value() );
}


BOOST_AUTO_TEST_CASE( LayoutGeometryExcludesHiddenPinText )
{
    using namespace kiapi::schematic::commands;
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    API_HANDLER_SCH handler( m_context );
    SCH_SHEET_PATH path;
    SCH_SYMBOL* symbol = firstMultiPinSymbol( *model, path );
    BOOST_REQUIRE( symbol );
    symbol->SetShowPinNames( false );
    symbol->SetShowPinNumbers( false );
    GetSchematicLayoutGeometry read;
    *read.mutable_document() = makeDocument( *model );
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa.layout" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( read ) );
    auto result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    SchematicLayoutGeometryResponse geometry;
    BOOST_REQUIRE( result->message().UnpackTo( &geometry ) );

    for( const auto& sheet : geometry.sheets() )
        for( const auto& element : sheet.elements() )
            if( element.owner().value() == symbol->m_Uuid.AsStdString() )
                BOOST_CHECK( element.kind() != "pin_name" && element.kind() != "pin_number" );
}


BOOST_AUTO_TEST_CASE( LayoutPageUndoImagesSwapTheActualScreenSettings )
{
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET& root = model->Root();
    PAGE_INFO original = root.GetScreen()->GetPageSettings();
    root.CaptureLayoutPageForUndo();
    std::unique_ptr<SCH_SHEET> image( static_cast<SCH_SHEET*>( root.Clone() ) );
    root.ClearLayoutPageUndo();
    PAGE_INFO larger( PAGE_SIZE_TYPE::A3 );
    root.GetScreen()->SetPageSettings( larger );
    root.SwapItemData( image.get() );
    BOOST_CHECK( root.GetScreen()->GetPageSettings().GetType() == original.GetType() );
    root.SwapItemData( image.get() );
    BOOST_CHECK( root.GetScreen()->GetPageSettings().GetType() == larger.GetType() );
}


BOOST_AUTO_TEST_CASE( LayoutGeometryMeasuresSharedInstanceReferencesAfterRotateAndMirror )
{
    using namespace kiapi::schematic::commands;
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH sourcePath;
    SCH_SYMBOL* source = firstMultiPinSymbol( *model, sourcePath );
    BOOST_REQUIRE( source );
    SCH_SCREEN* screen = new SCH_SCREEN( model );
    auto* symbol = static_cast<SCH_SYMBOL*>( source->Clone() );
    const_cast<KIID&>( symbol->m_Uuid ) = KIID();
    symbol->Rotate( symbol->GetPosition(), true );
    symbol->MirrorHorizontally( symbol->GetPosition().x );
    screen->Append( symbol );
    SCH_SHEET_PATH parent = model->Hierarchy().at( 0 );
    std::vector<SCH_SHEET*> placements;

    for( const wxString& name : { wxS( "LayoutA" ), wxS( "LayoutB" ) } )
    {
        auto* sheet = new SCH_SHEET( model );
        sheet->SetName( name );
        sheet->SetFileName( wxS( "layout-shared.kicad_sch" ) );
        sheet->SetScreen( screen );
        parent.LastScreen()->Append( sheet );
        placements.push_back( sheet );
    }

    model->RefreshHierarchy();
    std::vector<SCH_SHEET_PATH> paths;

    for( SCH_SHEET* placement : placements )
    {
        SCH_SHEET_PATH path = parent;
        path.push_back( placement );
        paths.push_back( path );
    }

    symbol->SetRef( &paths[0], wxS( "U1" ) );
    symbol->SetRef( &paths[1], wxS( "U123456789" ) );
    API_HANDLER_SCH handler( m_context );
    GetSchematicLayoutGeometry command;
    *command.mutable_document() = makeDocument( *model );
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa.layout" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );
    auto result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );
    SchematicLayoutGeometryResponse geometry;
    BOOST_REQUIRE( result->message().UnpackTo( &geometry ) );
    std::vector<int64_t> referenceExtents;

    for( const auto& sheet : geometry.sheets() )
        for( const auto& element : sheet.elements() )
            if( element.owner().value() == symbol->m_Uuid.AsStdString()
                && element.kind() == "field" && element.key() == "Reference" )
            {
                referenceExtents.push_back( std::max( element.box().size().x_nm(),
                                                     element.box().size().y_nm() ) );
            }

    BOOST_REQUIRE_EQUAL( referenceExtents.size(), 2 );
    BOOST_CHECK( referenceExtents[0] != referenceExtents[1] );
}

BOOST_AUTO_TEST_CASE( BusToBusSnapshotsPreserveIdentityAndAttributes )
{
    SCHEMATIC* model = loadSchematic( wxS( "api_kitchen_sink" ) );
    SCH_SHEET_PATH path = model->Hierarchy().at( 0 );
    auto* entry = new SCH_BUS_BUS_ENTRY( VECTOR2I( 25400000, 25400000 ) );
    const_cast<KIID&>( entry->m_Uuid ) = KIID( "ffffffff-ffff-ffff-ffff-ffffffffffff" );
    entry->SetSize( VECTOR2I( 1270000, 1270000 ) );
    entry->SetLocked( true );
    STROKE_PARAMS stroke = entry->GetStroke();
    stroke.SetWidth( schIUScale.mmToIU( 0.254 ) );
    entry->SetStroke( stroke );
    entry->SetCustomProperty( wxS( "layout-fixture" ), wxS( "preserve" ) );
    path.LastScreen()->Append( entry );
    const std::string first = formatItems( *model, path, { entry } );
    const std::string second = formatItems( *model, path, { entry } );
    BOOST_CHECK_EQUAL( first, second );
    BOOST_CHECK( first.find( entry->m_Uuid.AsStdString() ) != std::string::npos );
    BOOST_CHECK( first.find( "(locked yes)" ) != std::string::npos );
    BOOST_CHECK( first.find( "(width 0.254)" ) != std::string::npos );
    BOOST_CHECK( first.find( "layout-fixture" ) != std::string::npos );

    auto fullDocument = [&]()
    {
        STRING_FORMATTER formatter;
        SCH_IO_KICAD_SEXPR plugin;
        plugin.FormatSchematicToFormatter( &formatter, path.Last(), model );
        return formatter.GetString();
    };
    const std::string original = fullDocument();
    path.LastScreen()->Remove( entry );
    std::unique_ptr<SCH_BUS_BUS_ENTRY> removed( entry );
    auto* canonical = new SCH_LINE( entry->GetPosition(), LAYER_BUS );
    const_cast<KIID&>( canonical->m_Uuid ) = entry->m_Uuid;
    canonical->SetEndPoint( entry->GetEnd() );
    canonical->SetStroke( entry->GetStroke() );
    canonical->SetLocked( entry->IsLocked() );
    canonical->SetCustomProperties( entry->GetCustomProperties() );
    path.LastScreen()->Append( canonical );
    BOOST_CHECK_EQUAL( original, fullDocument() );
}


BOOST_AUTO_TEST_SUITE_END()
