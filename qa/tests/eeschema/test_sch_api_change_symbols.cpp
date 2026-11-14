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

#include <api/api_handler_sch.h>
#include <api/api_utils.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/headless_sch_context.h>
#include <api/schematic/schematic_commands.pb.h>
#include <libraries/library_manager.h>
#include <libraries/symbol_library_adapter.h>
#include <lib_symbol.h>
#include <pgm_base.h>
#include <project_sch.h>
#include <sch_field.h>
#include <sch_line.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/settings_manager.h>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>


namespace
{

using kiapi::common::ApiStatusCode;
using namespace kiapi::schematic::commands;


/// A library written by the tests: a resistor with longer pins and a footprint, a two-unit part and
/// a single-unit part with another reference prefix
const char* const ALT_LIBRARY = R"((kicad_symbol_lib
	(version 20241209)
	(generator "kicad_qa")
	(symbol "R_LONG"
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(property "Reference" "R" (at 2.54 0 90) (effects (font (size 1.27 1.27))))
		(property "Value" "R_LONG" (at 0 0 90) (effects (font (size 1.27 1.27))))
		(property "Footprint" "Resistor_SMD:R_0805_2012Metric" (at -1.778 0 90) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Description" "Long resistor" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(symbol "R_LONG_0_1"
			(rectangle (start -1.016 -3.81) (end 1.016 3.81) (stroke (width 0.254) (type default)) (fill (type none)))
		)
		(symbol "R_LONG_1_1"
			(pin passive line (at 0 5.08 270) (length 1.27) (name "~" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
			(pin passive line (at 0 -5.08 90) (length 1.27) (name "~" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
		)
		(embedded_fonts no)
	)
	(symbol "DUAL"
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(property "Reference" "U" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
		(property "Value" "DUAL" (at 0 -5.08 0) (effects (font (size 1.27 1.27))))
		(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Description" "Two units" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(symbol "DUAL_1_1"
			(rectangle (start -2.54 -2.54) (end 2.54 2.54) (stroke (width 0.254) (type default)) (fill (type none)))
			(pin input line (at -5.08 0 0) (length 2.54) (name "A" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
			(pin output line (at 5.08 0 180) (length 2.54) (name "B" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
		)
		(symbol "DUAL_2_1"
			(rectangle (start -2.54 -2.54) (end 2.54 2.54) (stroke (width 0.254) (type default)) (fill (type none)))
			(pin input line (at -5.08 0 0) (length 2.54) (name "C" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
			(pin output line (at 5.08 0 180) (length 2.54) (name "D" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
		)
		(embedded_fonts no)
	)
	(symbol "SINGLE"
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(property "Reference" "U" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
		(property "Value" "SINGLE" (at 0 -5.08 0) (effects (font (size 1.27 1.27))))
		(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Description" "One unit" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(symbol "SINGLE_1_1"
			(rectangle (start -2.54 -2.54) (end 2.54 2.54) (stroke (width 0.254) (type default)) (fill (type none)))
			(pin input line (at -5.08 0 0) (length 2.54) (name "A" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
			(pin output line (at 5.08 0 180) (length 2.54) (name "B" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
		)
		(embedded_fonts no)
	)
)
)";


/**
 * A project with the Device library of qa/data/libraries/test_project (loaded) and the Alt library
 * above (not loaded until a command needs it), and a schematic built in code with a resistor R1
 * whose upper pin has a wire.
 */
struct API_CHANGE_SYMBOLS_FIXTURE
{
    API_CHANGE_SYMBOLS_FIXTURE()
    {
        std::error_code ec;
        m_dir = std::filesystem::temp_directory_path( ec )
                / std::filesystem::path( wxString::Format( wxS( "kicad_qa_apichsym_%lu_%p" ), wxGetProcessId(),
                                                           (void*) this ).ToStdString() );
        std::filesystem::create_directories( m_dir, ec );

        wxFileName source( wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ), wxEmptyString );
        source.AppendDir( wxS( "libraries" ) );
        source.AppendDir( wxS( "test_project" ) );

        for( const wxString& name : { wxS( "test_project.kicad_pro" ), wxS( "Device.kicad_sym" ) } )
        {
            wxFileName from( source.GetPath(), name );
            wxFileName to( wxString( m_dir.string() ), name );
            BOOST_REQUIRE( wxCopyFile( from.GetFullPath(), to.GetFullPath() ) );
        }

        writeFile( "Alt.kicad_sym", ALT_LIBRARY );
        writeFile( "sym-lib-table",
                   "(sym_lib_table\n  (version 7)\n"
                   "  (lib (name \"Device\")(type \"KiCad\")(uri \"${KIPRJMOD}/Device.kicad_sym\")(options \"\")"
                   "(descr \"\"))\n"
                   "  (lib (name \"Alt\")(type \"KiCad\")(uri \"${KIPRJMOD}/Alt.kicad_sym\")(options \"\")"
                   "(descr \"\"))\n)\n" );

        SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
        mgr.LoadProject( wxString( ( m_dir / "test_project.kicad_pro" ).string() ) );
        m_project = &mgr.Prj();

        Pgm().GetLibraryManager().LoadProjectTables( m_project->GetProjectDirectory() );
        m_adapter = PROJECT_SCH::SymbolLibAdapter( m_project );
        m_adapter->LoadLibraryEntry( wxS( "Device" ) );

        m_schematic = std::make_unique<SCHEMATIC>( m_project );
        m_schematic->Reset();
        SCH_SHEET* defaultSheet = m_schematic->GetTopLevelSheet( 0 );

        m_screen = new SCH_SCREEN( m_schematic.get() );
        m_sheet = new SCH_SHEET( m_schematic.get() );
        m_sheet->SetScreen( m_screen );
        m_schematic->AddTopLevelSheet( m_sheet );
        m_schematic->RemoveTopLevelSheet( defaultSheet );
        delete defaultSheet;
        m_schematic->RefreshHierarchy();
        m_path = m_schematic->Hierarchy().at( 0 );

        m_slot = m_schematic.get();
        m_context = std::make_shared<HEADLESS_SCH_CONTEXT>( &m_slot, m_project );

        m_r1 = place( m_path, wxS( "Device" ), wxS( "R" ), wxS( "R1" ), VECTOR2I( 50800000, 50800000 ) );
        m_r1->SetValueFieldText( wxS( "10k" ) );
        m_r1->SetOrientation( SYM_ORIENT_90 );

        SCH_FIELD* mpn = m_r1->AddField( SCH_FIELD( m_r1, FIELD_T::USER, wxS( "MPN" ) ) );
        mpn->SetText( wxS( "RC0603-10K" ) );

        m_wireEnd = pinPosition( m_r1, wxS( "1" ) );
        m_wire = new SCH_LINE( m_wireEnd, LAYER_WIRE );
        m_wire->SetEndPoint( m_wireEnd - VECTOR2I( 5080000, 0 ) );
        m_screen->Append( m_wire );
    }

    ~API_CHANGE_SYMBOLS_FIXTURE()
    {
        m_context.reset();
        m_schematic.reset();
        Pgm().GetSettingsManager().UnloadProject( m_project, false );

        std::error_code ec;
        std::filesystem::remove_all( m_dir, ec );
    }

    void writeFile( const std::string& aName, const std::string& aText )
    {
        std::ofstream out( m_dir / aName, std::ios::trunc );
        out << aText;
    }

    /// Replace the first occurrence of aOld in a library file and move its timestamp forward
    void editFile( const std::string& aName, const std::string& aOld, const std::string& aNew )
    {
        std::filesystem::path file = m_dir / aName;
        std::string           text;

        {
            std::ifstream in( file );
            text.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        }

        size_t at = text.find( aOld );
        BOOST_REQUIRE( at != std::string::npos );
        text.replace( at, aOld.size(), aNew );
        writeFile( aName, text );

        std::error_code ec;
        std::filesystem::last_write_time( file, std::filesystem::last_write_time( file, ec )
                                                        + std::chrono::seconds( 10 ), ec );
        BOOST_REQUIRE( !ec );
    }

    SCH_SYMBOL* place( const SCH_SHEET_PATH& aPath, const wxString& aLib, const wxString& aName,
                       const wxString& aRef, const VECTOR2I& aPos, int aUnit = 1 )
    {
        m_adapter->LoadLibraryEntry( aLib );
        LIB_SYMBOL* libSymbol = m_adapter->LoadSymbol( aLib, aName );
        BOOST_REQUIRE( libSymbol );

        SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSymbol, LIB_ID( aLib, aName ), &aPath, aUnit, 0, aPos );
        symbol->SetRef( &aPath, aRef );
        aPath.LastScreen()->Append( symbol );
        return symbol;
    }

    static VECTOR2I pinPosition( SCH_SYMBOL* aSymbol, const wxString& aNumber )
    {
        SCH_PIN* pin = aSymbol->GetPin( aNumber );
        BOOST_REQUIRE( pin );
        return pin->GetPosition();
    }

    kiapi::common::types::ItemHeader header() const
    {
        kiapi::common::types::ItemHeader header;
        header.mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_SCHEMATIC );
        kiapi::common::PackProject( *header.mutable_document()->mutable_project(), *m_project );
        return header;
    }

    /// One ChangeSymbols request; a symbol with a unit is given as { symbol, unit }
    ChangeSymbols command( const std::vector<std::pair<SCH_SYMBOL*, int>>& aSymbols, const LIB_ID* aLibId,
                           bool aDryRun = false, const ChangeSymbolsOptions* aOptions = nullptr ) const
    {
        ChangeSymbols command;
        *command.mutable_header() = header();

        for( const auto& [symbol, unit] : aSymbols )
        {
            SymbolChange* change = command.add_symbols();
            change->mutable_symbol()->set_value( symbol->m_Uuid.AsStdString() );

            if( unit > 0 )
                change->set_unit( unit );
        }

        if( aLibId )
            kiapi::common::PackLibId( command.mutable_lib_id(), *aLibId );

        if( aOptions )
            *command.mutable_options() = *aOptions;

        command.set_dry_run( aDryRun );
        return command;
    }

    template <typename MESSAGE>
    API_RESULT send( API_HANDLER_SCH& aHandler, const MESSAGE& aMessage ) const
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( aMessage ) );
        return aHandler.Handle( request );
    }

    ChangeSymbolsResponse change( const std::vector<std::pair<SCH_SYMBOL*, int>>& aSymbols,
                                  const LIB_ID* aLibId = nullptr, bool aDryRun = false,
                                  const ChangeSymbolsOptions* aOptions = nullptr )
    {
        API_HANDLER_SCH handler( m_context );
        API_RESULT      result = send( handler, command( aSymbols, aLibId, aDryRun, aOptions ) );

        BOOST_REQUIRE( result.has_value() );
        BOOST_REQUIRE_EQUAL( result->status().status(), ApiStatusCode::AS_OK );

        ChangeSymbolsResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response;
    }

    std::filesystem::path                 m_dir;
    PROJECT*                              m_project = nullptr;
    SYMBOL_LIBRARY_ADAPTER*               m_adapter = nullptr;
    std::unique_ptr<SCHEMATIC>            m_schematic;
    SCHEMATIC*                            m_slot = nullptr;
    SCH_SCREEN*                           m_screen = nullptr;
    SCH_SHEET*                            m_sheet = nullptr;
    SCH_SHEET_PATH                        m_path;
    std::shared_ptr<HEADLESS_SCH_CONTEXT> m_context;
    SCH_SYMBOL*                           m_r1 = nullptr;
    SCH_LINE*                             m_wire = nullptr;
    VECTOR2I                              m_wireEnd;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerChangeSymbols, API_CHANGE_SYMBOLS_FIXTURE )


BOOST_AUTO_TEST_CASE( RejectsBadRequests )
{
    API_HANDLER_SCH handler( m_context );

    // No symbols: also how a client finds out that the command exists
    API_RESULT result = send( handler, command( {}, nullptr ) );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_BAD_REQUEST );

    // Unit 0, and a library id without an entry name
    ChangeSymbols bad = command( { { m_r1, 0 } }, nullptr );
    bad.mutable_symbols( 0 )->set_unit( 0 );
    result = send( handler, bad );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_BAD_REQUEST );

    LIB_ID incomplete( wxS( "Alt" ), wxEmptyString );
    result = send( handler, command( { { m_r1, 0 } }, &incomplete ) );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_BAD_REQUEST );

    // A board is for another handler
    ChangeSymbols board = command( { { m_r1, 0 } }, nullptr );
    board.mutable_header()->mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    result = send( handler, board );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_UNHANDLED );

    BOOST_CHECK( m_r1->GetLibId() == LIB_ID( wxS( "Device" ), wxS( "R" ) ) );
}


BOOST_AUTO_TEST_CASE( ChangeKeepsUuidRefPoseUnitFieldsAndPinUuids )
{
    // The handler has to load the Alt library itself
    BOOST_REQUIRE( !m_adapter->IsLibraryLoaded( wxS( "Alt" ) ) );

    const KIID      id = m_r1->m_Uuid;
    const VECTOR2I  pos = m_r1->GetPosition();
    const TRANSFORM transform = m_r1->GetTransform();
    const KIID      pin1 = m_r1->GetPin( wxS( "1" ) )->m_Uuid;
    const KIID      pin2 = m_r1->GetPin( wxS( "2" ) )->m_Uuid;
    LIB_ID          target( wxS( "Alt" ), wxS( "R_LONG" ) );

    ChangeSymbolsResponse response = change( { { m_r1, 0 } }, &target );
    BOOST_REQUIRE_EQUAL( response.results_size(), 1 );

    const SymbolChangeResult& r = response.results( 0 );
    BOOST_CHECK_EQUAL( r.status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK_EQUAL( r.lib_id().entry_name(), "R_LONG" );
    BOOST_REQUIRE_EQUAL( r.references_size(), 1 );
    BOOST_CHECK_EQUAL( r.references( 0 ), "R1" );
    BOOST_CHECK( r.has_item() );

    // The same object, where it was, with its reference, value and own fields
    BOOST_CHECK( m_r1->m_Uuid == id );
    BOOST_CHECK( m_r1->GetLibId() == target );
    BOOST_CHECK( m_r1->GetPosition() == pos );
    BOOST_CHECK( m_r1->GetTransform() == transform );
    BOOST_CHECK_EQUAL( m_r1->GetUnit(), 1 );
    BOOST_CHECK_EQUAL( m_r1->GetRef( &m_path, false ), wxS( "R1" ) );
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::VALUE )->GetText(), wxS( "10k" ) );
    BOOST_REQUIRE( m_r1->GetField( wxS( "MPN" ) ) );
    BOOST_CHECK_EQUAL( m_r1->GetField( wxS( "MPN" ) )->GetText(), wxS( "RC0603-10K" ) );

    // The fields of the library symbol are taken (field text is reset when changing)
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::FOOTPRINT )->GetText(), wxS( "Resistor_SMD:R_0805_2012Metric" ) );
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::DESCRIPTION )->GetText(), wxS( "Long resistor" ) );

    // Pins keep their UUIDs by number, and now sit further out
    BOOST_CHECK( m_r1->GetPin( wxS( "1" ) )->m_Uuid == pin1 );
    BOOST_CHECK( m_r1->GetPin( wxS( "2" ) )->m_Uuid == pin2 );
    BOOST_CHECK( pinPosition( m_r1, wxS( "1" ) ) != m_wireEnd );

    // The wire stays where it was: it no longer reaches the pin
    BOOST_CHECK( m_wire->GetStartPoint() == m_wireEnd );
    BOOST_CHECK( m_screen->Items().contains( m_wire ) );
}


BOOST_AUTO_TEST_CASE( DryRunChangesNothing )
{
    const wxString footprint = m_r1->GetField( FIELD_T::FOOTPRINT )->GetText();
    LIB_ID         target( wxS( "Alt" ), wxS( "R_LONG" ) );

    ChangeSymbolsResponse response = change( { { m_r1, 0 } }, &target, true );
    BOOST_REQUIRE_EQUAL( response.results_size(), 1 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK( !response.results( 0 ).has_item() );

    BOOST_CHECK( m_r1->GetLibId() == LIB_ID( wxS( "Device" ), wxS( "R" ) ) );
    BOOST_CHECK( pinPosition( m_r1, wxS( "1" ) ) == m_wireEnd );
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::FOOTPRINT )->GetText(), footprint );
}


BOOST_AUTO_TEST_CASE( UpdateModeUnchangedThenPicksUpEditedLibrary )
{
    // Placed from the library a moment ago: an update has nothing to do
    ChangeSymbolsResponse before = change( { { m_r1, 0 } } );
    BOOST_REQUIRE_EQUAL( before.results_size(), 1 );
    BOOST_CHECK_EQUAL( before.results( 0 ).status(), SymbolChangeStatus::SCS_UNCHANGED );
    BOOST_CHECK( !before.results( 0 ).has_item() );

    editFile( "Device.kicad_sym", "(property \"Description\" \"Resistor\"",
              "(property \"Description\" \"Edited resistor\"" );

    ChangeSymbolsResponse after = change( { { m_r1, 0 } } );
    BOOST_REQUIRE_EQUAL( after.results_size(), 1 );
    BOOST_CHECK_EQUAL( after.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::DESCRIPTION )->GetText(), wxS( "Edited resistor" ) );

    // An update keeps the value
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::VALUE )->GetText(), wxS( "10k" ) );
}


BOOST_AUTO_TEST_CASE( ValueAndReferenceTakenOnlyWhenListed )
{
    LIB_ID               target( wxS( "Alt" ), wxS( "SINGLE" ) );
    ChangeSymbolsOptions options;
    options.add_fields( "Reference" );
    options.add_fields( "Value" );

    ChangeSymbolsResponse response = change( { { m_r1, 0 } }, &target, false, &options );
    BOOST_REQUIRE_EQUAL( response.results_size(), 1 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_REQUIRE_EQUAL( response.results( 0 ).references_size(), 1 );
    BOOST_CHECK_EQUAL( response.results( 0 ).references( 0 ), "U1" );
    BOOST_CHECK_EQUAL( m_r1->GetRef( &m_path, false ), wxS( "U1" ) );
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::VALUE )->GetText(), wxS( "SINGLE" ) );

    // Only the listed fields are updated
    BOOST_CHECK_EQUAL( m_r1->GetField( FIELD_T::DESCRIPTION )->GetText(), wxS( "Resistor" ) );
}


BOOST_AUTO_TEST_CASE( ChangeModeResetsFieldLayoutUpdateModeDoesNot )
{
    // The Footprint field is updated by default (Reference and Value are not)
    SCH_FIELD*     footprint = m_r1->GetField( FIELD_T::FOOTPRINT );
    const VECTOR2I moved = footprint->GetTextPos() + VECTOR2I( 2540000, 2540000 );
    footprint->SetTextPos( moved );

    editFile( "Device.kicad_sym", "(property \"Description\" \"Resistor\"",
              "(property \"Description\" \"Edited resistor\"" );

    ChangeSymbolsResponse update = change( { { m_r1, 0 } } );
    BOOST_CHECK_EQUAL( update.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK( m_r1->GetField( FIELD_T::FOOTPRINT )->GetTextPos() == moved );

    // Changing, even to the same library symbol, puts the field where the library has it
    LIB_ID                same( wxS( "Device" ), wxS( "R" ) );
    ChangeSymbolsResponse changed = change( { { m_r1, 0 } }, &same );
    BOOST_CHECK_EQUAL( changed.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK( m_r1->GetField( FIELD_T::FOOTPRINT )->GetTextPos() != moved );

    // A field outside the list keeps its place
    SCH_FIELD*     value = m_r1->GetField( FIELD_T::VALUE );
    const VECTOR2I valuePos = value->GetTextPos() + VECTOR2I( 2540000, 0 );
    value->SetTextPos( valuePos );
    change( { { m_r1, 0 } }, &same );
    BOOST_CHECK( m_r1->GetField( FIELD_T::VALUE )->GetTextPos() == valuePos );
}


BOOST_AUTO_TEST_CASE( RefusedUnderActiveVariant )
{
    m_schematic->AddVariant( wxS( "Assembly" ) );
    m_schematic->SetCurrentVariant( wxS( "Assembly" ) );

    API_HANDLER_SCH handler( m_context );
    LIB_ID          target( wxS( "Alt" ), wxS( "R_LONG" ) );
    API_RESULT      result = send( handler, command( { { m_r1, 0 } }, &target ) );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), ApiStatusCode::AS_BAD_REQUEST );
    BOOST_CHECK( m_r1->GetLibId() == LIB_ID( wxS( "Device" ), wxS( "R" ) ) );

    // An update from the library does not change the base symbol, so it is allowed
    ChangeSymbolsResponse update = change( { { m_r1, 0 } } );
    BOOST_CHECK_EQUAL( update.results( 0 ).status(), SymbolChangeStatus::SCS_UNCHANGED );
}


BOOST_AUTO_TEST_CASE( TooFewUnitsSkipsOnlyThatSymbol )
{
    SCH_SYMBOL* a = place( m_path, wxS( "Alt" ), wxS( "DUAL" ), wxS( "U1" ), VECTOR2I( 101600000, 50800000 ), 1 );
    SCH_SYMBOL* b = place( m_path, wxS( "Alt" ), wxS( "DUAL" ), wxS( "U1" ), VECTOR2I( 127000000, 50800000 ), 2 );
    LIB_ID      target( wxS( "Alt" ), wxS( "SINGLE" ) );

    ChangeSymbolsResponse response = change( { { a, 0 }, { b, 0 } }, &target );
    BOOST_REQUIRE_EQUAL( response.results_size(), 2 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK_EQUAL( response.results( 1 ).status(), SymbolChangeStatus::SCS_TOO_FEW_UNITS );
    BOOST_CHECK( a->GetLibId() == target );
    BOOST_CHECK( b->GetLibId() == LIB_ID( wxS( "Alt" ), wxS( "DUAL" ) ) );
}


BOOST_AUTO_TEST_CASE( UnitRemapAndUnlistedUnitsNamed )
{
    SCH_SYMBOL* a = place( m_path, wxS( "Alt" ), wxS( "DUAL" ), wxS( "U1" ), VECTOR2I( 101600000, 50800000 ), 1 );
    SCH_SYMBOL* b = place( m_path, wxS( "Alt" ), wxS( "DUAL" ), wxS( "U1" ), VECTOR2I( 127000000, 50800000 ), 2 );
    LIB_ID      target( wxS( "Alt" ), wxS( "SINGLE" ) );

    // Unit B becomes the one unit of SINGLE; unit A is not listed and keeps DUAL
    ChangeSymbolsResponse response = change( { { b, 1 } }, &target );
    BOOST_REQUIRE_EQUAL( response.results_size(), 1 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK_EQUAL( b->GetUnit(), 1 );
    BOOST_CHECK_EQUAL( b->GetUnitSelection( &m_path ), 1 );
    BOOST_CHECK( b->GetLibId() == target );
    BOOST_CHECK( a->GetLibId() == LIB_ID( wxS( "Alt" ), wxS( "DUAL" ) ) );
    BOOST_CHECK_MESSAGE( response.results( 0 ).message().find( "other units of U1" ) != std::string::npos,
                         response.results( 0 ).message() );
}


BOOST_AUTO_TEST_CASE( SharedSymbolOnReusedSheetListsBothRefs )
{
    // Two sheets show the same screen; its symbol has a reference on each
    SCH_SCREEN* shared = new SCH_SCREEN( m_schematic.get() );
    shared->SetFileName( wxString( ( m_dir / "shared.kicad_sch" ).string() ) );

    for( int ii = 0; ii < 2; ++ii )
    {
        SCH_SHEET* sheet = new SCH_SHEET( m_sheet, VECTOR2I( 25400000 * ( ii + 1 ), 101600000 ) );
        sheet->SetScreen( shared );
        sheet->SetFileName( wxS( "shared.kicad_sch" ) );
        m_screen->Append( sheet );
    }

    m_schematic->RefreshHierarchy();
    SCH_SHEET_LIST paths = m_schematic->Hierarchy().FindAllSheetsForScreen( shared );
    BOOST_REQUIRE_EQUAL( paths.size(), 2u );

    SCH_SYMBOL* r = place( paths[0], wxS( "Device" ), wxS( "R" ), wxS( "R10" ), VECTOR2I( 25400000, 25400000 ) );
    r->SetRef( &paths[1], wxS( "R11" ) );

    LIB_ID                target( wxS( "Alt" ), wxS( "R_LONG" ) );
    ChangeSymbolsResponse response = change( { { r, 0 } }, &target );
    BOOST_REQUIRE_EQUAL( response.results_size(), 1 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );

    std::set<std::string> refs( response.results( 0 ).references().begin(),
                                response.results( 0 ).references().end() );
    BOOST_CHECK( refs == std::set<std::string>( { "R10", "R11" } ) );
    BOOST_CHECK( r->GetLibId() == target );
}


BOOST_AUTO_TEST_CASE( PerItemFailuresAndTheOpenCommit )
{
    API_HANDLER_SCH handler( m_context );

    // Inside a commit of the client: a duplicate, a missing item, a wire, a missing symbol
    kiapi::common::commands::BeginCommit begin;
    *begin.mutable_header() = header();
    API_RESULT result = send( handler, begin );
    BOOST_REQUIRE( result.has_value() );

    LIB_ID        target( wxS( "Alt" ), wxS( "R_LONG" ) );
    ChangeSymbols cmd = command( { { m_r1, 0 }, { m_r1, 0 } }, &target );
    cmd.add_symbols()->mutable_symbol()->set_value( KIID().AsStdString() );
    cmd.add_symbols()->mutable_symbol()->set_value( m_wire->m_Uuid.AsStdString() );

    result = send( handler, cmd );
    BOOST_REQUIRE( result.has_value() );
    ChangeSymbolsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.results_size(), 4 );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_CHANGED );
    BOOST_CHECK_EQUAL( response.results( 1 ).status(), SymbolChangeStatus::SCS_NONEXISTENT );
    BOOST_CHECK_EQUAL( response.results( 1 ).message(), "listed more than once" );
    BOOST_CHECK_EQUAL( response.results( 2 ).message(), "no such item" );
    BOOST_CHECK_EQUAL( response.results( 3 ).message(), "not a symbol" );

    // A symbol the open commit deletes cannot be changed
    kiapi::common::commands::DeleteItems remove;
    *remove.mutable_header() = header();
    remove.add_item_ids()->set_value( m_r1->m_Uuid.AsStdString() );
    result = send( handler, remove );
    BOOST_REQUIRE( result.has_value() );

    result = send( handler, command( { { m_r1, 0 } }, &target ) );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_NONEXISTENT );

    // A library symbol that does not exist
    LIB_ID missing( wxS( "Alt" ), wxS( "NOTHING" ) );
    SCH_SYMBOL* other = place( m_path, wxS( "Device" ), wxS( "C" ), wxS( "C1" ), VECTOR2I( 76200000, 50800000 ) );
    result = send( handler, command( { { other, 0 } }, &missing ) );
    BOOST_REQUIRE( result.has_value() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( response.results( 0 ).status(), SymbolChangeStatus::SCS_NOT_FOUND );
    BOOST_CHECK_EQUAL( response.results( 0 ).message(), "the library does not contain the symbol" );
}


BOOST_AUTO_TEST_SUITE_END()
