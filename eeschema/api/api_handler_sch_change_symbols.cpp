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

/**
 * @file api_handler_sch_change_symbols.cpp
 *
 * ChangeSymbols: the Update Symbols from Library and Change Symbols dialogs
 * (DIALOG_CHANGE_SYMBOLS::processSymbols) without a window.
 */

#include <api/api_handler_sch.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <api/sch_context.h>

#include <eeschema_settings.h>
#include <fmt.h>
#include <kiface_base.h>
#include <libraries/symbol_library_adapter.h>
#include <lib_symbol.h>
#include <project_sch.h>
#include <refdes_utils.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <schematic_settings.h>
#include <string_utils.h>
#include <template_fieldnames.h>

#include <api/schematic/schematic_commands.pb.h>

#include <map>
#include <set>

using namespace kiapi::schematic::commands;


namespace
{


/**
 * Load a symbol from the project's symbol libraries, flattened so that it can be given to a
 * placed symbol.
 *
 * The library adapter only hands out symbols of libraries that are loaded, so a library that is
 * not loaded yet is loaded first (once per request).  A library file changed on disk is read again
 * by the library cache itself.
 *
 * @param aError receives the reason when nothing could be loaded.
 * @param aLibraries the libraries already loaded in this request, with the error each one gave.
 */
std::unique_ptr<LIB_SYMBOL> loadLibrarySymbol( PROJECT& aProject, const LIB_ID& aId, wxString* aError,
                                               std::map<wxString, wxString>& aLibraries )
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &aProject );
    const wxString          nickname = aId.GetLibNickname();

    if( !adapter )
    {
        *aError = wxS( "the symbol libraries are not available" );
        return nullptr;
    }

    if( !aLibraries.contains( nickname ) )
    {
        wxString& error = aLibraries[nickname];

        try
        {
            std::optional<LIB_STATUS> status = adapter->GetLibraryStatus( nickname );

            if( !status || status->load_status != LOAD_STATUS::LOADED )
                status = adapter->LoadLibraryEntry( nickname );

            if( !status )
            {
                error = wxString::Format( wxS( "library '%s' is not in the symbol library tables" ), nickname );
            }
            else if( status->load_status == LOAD_STATUS::LOAD_ERROR )
            {
                error = wxString::Format( wxS( "library '%s' could not be loaded: %s" ), nickname,
                                          status->error ? status->error->message : wxString( wxS( "?" ) ) );
            }
        }
        catch( const IO_ERROR& e )
        {
            error = wxString::Format( wxS( "library '%s' could not be read: %s" ), nickname, e.What() );
        }
    }

    if( !aLibraries[nickname].IsEmpty() )
    {
        *aError = aLibraries[nickname];
        return nullptr;
    }

    LIB_SYMBOL* symbol = nullptr;

    try
    {
        symbol = adapter->LoadSymbol( aId );
    }
    catch( const IO_ERROR& e )
    {
        *aError = wxString::Format( wxS( "library '%s' could not be read: %s" ), nickname, e.What() );
        return nullptr;
    }

    if( !symbol )
    {
        *aError = wxS( "the library does not contain the symbol" );
        return nullptr;
    }

    // The library owns the symbol it hands out and may reload it: take a flattened copy now
    return symbol->Flatten();
}


/// The ChangeSymbols options with the defaults of the Update Symbols or Change Symbols dialog.
struct CHANGE_FLAGS
{
    std::set<wxString> fields;  ///< untranslated names; empty is every field but Reference and Value
    bool               removeExtraFields;
    bool               resetEmptyFields;
    bool               resetFieldText;
    bool               resetCustomPower;
    bool               resetFieldVisibilities;
    bool               resetFieldEffects;
    bool               resetFieldPositions;
    bool               resetAttributes;
    bool               resetPinTextVisibility;
    bool               resetAlternatePins;
    bool               resetPinMapOverrides;

    bool UpdatesField( const wxString& aUntranslatedName ) const
    {
        if( fields.empty() )
        {
            return aUntranslatedName != GetDefaultFieldName( FIELD_T::REFERENCE, UNTRANSLATED )
                   && aUntranslatedName != GetDefaultFieldName( FIELD_T::VALUE, UNTRANSLATED );
        }

        return fields.contains( aUntranslatedName );
    }
};


CHANGE_FLAGS resolveChangeFlags( const ChangeSymbolsOptions& aOptions, bool aChangeMode )
{
    CHANGE_FLAGS flags;

    for( const std::string& name : aOptions.fields() )
        flags.fields.insert( wxString::FromUTF8( name ) );

    flags.removeExtraFields = aOptions.has_remove_extra_fields() ? aOptions.remove_extra_fields() : false;
    flags.resetEmptyFields = aOptions.has_reset_empty_fields() ? aOptions.reset_empty_fields() : false;
    flags.resetFieldText = aOptions.has_reset_field_text() ? aOptions.reset_field_text() : true;
    flags.resetCustomPower = aOptions.has_reset_custom_power_value() ? aOptions.reset_custom_power_value()
                                                                      : false;
    flags.resetFieldVisibilities = aOptions.has_reset_field_visibilities() ? aOptions.reset_field_visibilities()
                                                                            : aChangeMode;
    flags.resetFieldEffects = aOptions.has_reset_field_effects() ? aOptions.reset_field_effects() : aChangeMode;
    flags.resetFieldPositions = aOptions.has_reset_field_positions() ? aOptions.reset_field_positions()
                                                                      : aChangeMode;
    flags.resetAttributes = aOptions.has_reset_attributes() ? aOptions.reset_attributes() : aChangeMode;
    flags.resetPinTextVisibility = aOptions.has_reset_pin_text_visibility() ? aOptions.reset_pin_text_visibility()
                                                                             : aChangeMode;
    flags.resetAlternatePins = aOptions.has_reset_alternate_pins() ? aOptions.reset_alternate_pins() : aChangeMode;
    flags.resetPinMapOverrides = aOptions.has_reset_pin_map_overrides() ? aOptions.reset_pin_map_overrides()
                                                                         : aChangeMode;
    return flags;
}


/// The references already used in the schematic and the renames made so far in one request.
struct REFERENCES
{
    std::set<wxString>           used;
    std::map<wxString, wxString> renamed;
};


/**
 * Give a placed symbol a library symbol and reset what the flags ask for, as the dialog does.
 *
 * @param aScreen the screen of the symbol, or null to work on a detached copy (no screen updates).
 * @param aUnit the unit the symbol shows afterwards, if it changes.
 * @param aAutoplace whether fields may be autoplaced (needs the schematic editor settings).
 */
void applySymbolChange( SCH_SYMBOL& aSymbol, std::unique_ptr<LIB_SYMBOL> aFlattened, const LIB_ID& aLibId,
                        const CHANGE_FLAGS& aFlags, const std::vector<SCH_SHEET_PATH>& aInstances,
                        REFERENCES& aRefs, SCH_SCREEN* aScreen, std::optional<int> aUnit, bool aAutoplace )
{
    if( aLibId != aSymbol.GetLibId() )
        aSymbol.SetLibId( aLibId );

    PIN_MAP_SET                       savedPinMaps;
    std::vector<ASSOCIATED_FOOTPRINT> savedAssociations;
    const bool                        keepPinMaps = !aFlags.resetPinMapOverrides;

    if( keepPinMaps && aSymbol.GetLibSymbolRef() )
    {
        savedPinMaps = aSymbol.GetLibSymbolRef()->GetPinMaps();
        savedAssociations = aSymbol.GetLibSymbolRef()->GetAssociatedFootprints();
    }

    aSymbol.SetLibSymbol( aFlattened.release() );

    if( aScreen )
        aScreen->IncludeVariantSymbols( aSymbol, true );

    if( keepPinMaps && aSymbol.GetLibSymbolRef() )
    {
        aSymbol.GetLibSymbolRef()->SetPinMaps( savedPinMaps );
        aSymbol.GetLibSymbolRef()->SetAssociatedFootprints( savedAssociations );
    }

    if( aUnit )
    {
        for( const SCH_SHEET_PATH& instance : aInstances )
            aSymbol.SetUnitSelection( &instance, *aUnit );

        aSymbol.SetUnit( *aUnit );
    }

    LIB_SYMBOL* libSymbol = aSymbol.GetLibSymbolRef().get();

    if( aFlags.resetAttributes )
    {
        // Taken from the flattened library symbol: derived symbols do not support them
        aSymbol.SetExcludedFromSim( libSymbol->GetExcludedFromSim() );
        aSymbol.SetExcludedFromBOM( libSymbol->GetExcludedFromBOM() );
        aSymbol.SetExcludedFromBoard( libSymbol->GetExcludedFromBoard() );
        aSymbol.SetExcludedFromPosFiles( libSymbol->GetExcludedFromPosFiles() );
    }

    if( aFlags.resetPinTextVisibility )
    {
        aSymbol.SetShowPinNames( libSymbol->GetShowPinNames() );
        aSymbol.SetShowPinNumbers( libSymbol->GetShowPinNumbers() );
    }

    for( int ii = (int) aSymbol.GetFields().size() - 1; ii >= 0; ii-- )
    {
        SCH_FIELD& field = aSymbol.GetFields()[ii];

        if( !field.IsPrivate() && !aFlags.UpdatesField( field.GetUntranslatedName() ) )
            continue;

        SCH_FIELD* libField = field.IsMandatory() ? libSymbol->GetField( field.GetId() )
                                                  : libSymbol->GetField( field.GetName() );

        if( !libField )
        {
            if( !field.IsMandatory() && aFlags.removeExtraFields )
                aSymbol.RemoveField( field.GetName() );

            continue;
        }

        field.SetPrivate( libField->IsPrivate() );

        bool resetText = libField->GetText().IsEmpty() ? aFlags.resetEmptyFields : aFlags.resetFieldText;

        if( resetText )
        {
            if( field.GetId() == FIELD_T::REFERENCE )
            {
                wxString prefix = UTIL::GetRefDesPrefix( libField->GetText() );

                for( const SCH_SHEET_PATH& instance : aInstances )
                {
                    wxString oldRef = aSymbol.GetBaseRef( &instance );
                    int      number = UTIL::GetRefDesNumber( oldRef );
                    wxString ref;
                    auto     renamed = aRefs.renamed.find( oldRef );

                    if( number < 0 )
                    {
                        ref = UTIL::GetRefDesUnannotated( prefix );
                    }
                    else if( renamed != aRefs.renamed.end()
                             && UTIL::GetRefDesPrefix( renamed->second ) == prefix )
                    {
                        // Keep the units of a multi-unit symbol together
                        ref = renamed->second;
                    }
                    else
                    {
                        ref.Printf( wxS( "%s%d" ), prefix, number );

                        if( ref != oldRef )
                        {
                            while( aRefs.used.count( ref ) )
                                ref.Printf( wxS( "%s%d" ), prefix, ++number );

                            aRefs.used.insert( ref );
                            aRefs.renamed[oldRef] = ref;
                        }
                    }

                    aSymbol.SetRef( &instance, ref );
                }
            }
            else if( field.GetId() == FIELD_T::VALUE )
            {
                if( !aSymbol.IsPower() || aFlags.resetCustomPower )
                    aSymbol.SetValueFieldText( UnescapeString( libField->GetText() ) );
            }
            else
            {
                field.SetText( libField->GetText() );
            }
        }

        if( aFlags.resetFieldVisibilities )
            field.SetVisible( libField->IsVisible() );

        if( aFlags.resetFieldEffects )
        {
            // SetAttributes() also sets the visibility and the position: keep those
            bool     visible = field.IsVisible();
            VECTOR2I pos = field.GetPosition();

            field.SetAttributes( *libField );

            field.SetVisible( visible );
            field.SetPosition( pos );
            field.SetNameShown( libField->IsNameShown() );
            field.SetCanAutoplace( libField->CanAutoplace() );
        }

        if( aFlags.resetFieldPositions )
            field.SetTextPos( aSymbol.GetPosition() + libField->GetTextPos() );
    }

    std::vector<SCH_FIELD*> libFields;
    libSymbol->GetFields( libFields );

    for( SCH_FIELD* libField : libFields )
    {
        if( libField->IsMandatory() || !aFlags.UpdatesField( libField->GetUntranslatedName() ) )
            continue;

        if( !aSymbol.GetField( libField->GetName() ) )
        {
            SCH_FIELD* field = aSymbol.AddField( SCH_FIELD( &aSymbol, FIELD_T::USER, libField->GetName() ) );

            // SetAttributes() also covers text angle, size, italic and bold
            field->SetAttributes( *libField );
            field->SetVisible( libField->IsVisible() );
            field->SetText( libField->GetText() );
            field->SetTextPos( aSymbol.GetPosition() + libField->GetTextPos() );
            field->SetPrivate( libField->IsPrivate() );
        }
    }

    aSymbol.SetSchSymbolLibraryName( wxEmptyString );

    if( aScreen )
        aScreen->Append( &aSymbol );

    if( aFlags.resetFieldPositions && aAutoplace )
    {
        AUTOPLACE_ALGO fieldsAutoplaced = aSymbol.GetFieldsAutoplaced();

        if( fieldsAutoplaced == AUTOPLACE_AUTO || fieldsAutoplaced == AUTOPLACE_MANUAL )
            aSymbol.AutoplaceFields( aScreen, fieldsAutoplaced );
    }

    for( const SCH_SHEET_PATH& instance : aInstances )
    {
        for( SCH_PIN* pin : aSymbol.GetPins( &instance ) )
        {
            // An alternate that names the base pin, or that the new pin does not have, is
            // always cleared (the dialog does the same)
            if( !pin->GetAlt().IsEmpty()
                && ( aFlags.resetAlternatePins || pin->GetAlt() == pin->GetBaseName()
                     || pin->GetAlternates().count( pin->GetAlt() ) == 0 ) )
            {
                pin->SetAlt( wxEmptyString );
            }
        }
    }

    if( aFlags.resetPinMapOverrides )
    {
        for( const SCH_SHEET_PATH& instance : aInstances )
            aSymbol.SetPinMapOverride( PIN_MAP_INSTANCE_OVERRIDE(), &instance );
    }
}


/// The references of a symbol on each of its sheets
std::vector<wxString> referencesOf( const SCH_SYMBOL& aSymbol, const std::vector<SCH_SHEET_PATH>& aInstances )
{
    std::vector<wxString> refs;

    for( const SCH_SHEET_PATH& instance : aInstances )
        refs.push_back( aSymbol.GetRef( &instance, false ) );

    return refs;
}


/// Whether a change left a symbol as it was: its library symbol, fields, pins, unit and references
bool sameSymbol( const SCH_SYMBOL& aBefore, const SCH_SYMBOL& aAfter, const std::vector<SCH_SHEET_PATH>& aInstances,
                 int aCompareFlags )
{
    if( !aBefore.GetLibSymbolRef() || !aAfter.GetLibSymbolRef() )
        return false;

    if( aAfter.GetLibSymbolRef()->Compare( *aBefore.GetLibSymbolRef(), aCompareFlags ) != 0 )
        return false;

    if( !( aBefore == static_cast<const SCH_ITEM&>( aAfter ) ) )
        return false;

    if( aBefore.GetShowPinNames() != aAfter.GetShowPinNames()
        || aBefore.GetShowPinNumbers() != aAfter.GetShowPinNumbers() )
    {
        return false;
    }

    for( const SCH_SHEET_PATH& instance : aInstances )
    {
        if( aBefore.GetUnitSelection( &instance ) != aAfter.GetUnitSelection( &instance ) )
            return false;
    }

    return referencesOf( aBefore, aInstances ) == referencesOf( aAfter, aInstances );
}

} // namespace


HANDLER_RESULT<ChangeSymbolsResponse>
API_HANDLER_SCH::handleChangeSymbols( const HANDLER_CONTEXT<ChangeSymbols>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( aCtx.Request.symbols_size() == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "symbols must list at least one symbol" );
        return tl::unexpected( e );
    }

    for( const SymbolChange& change : aCtx.Request.symbols() )
    {
        if( change.has_unit() && change.unit() < 1 )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "a unit is numbered from 1" );
            return tl::unexpected( e );
        }
    }

    const bool changeMode = aCtx.Request.has_lib_id();
    LIB_ID     changeId;

    if( changeMode )
    {
        changeId = UnpackLibId( aCtx.Request.lib_id() );

        if( !changeId.IsValid() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "lib_id must specify both a library nickname and an entry name" );
            return tl::unexpected( e );
        }

        // The canvas shows variant-resolved values, so changing the base symbol under an active
        // variant would rewrite every variant unseen (the editor refuses it too)
        if( !schematic()->GetCurrentVariant().IsEmpty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "symbols cannot be changed while a design variant is active" );
            return tl::unexpected( e );
        }
    }

    const CHANGE_FLAGS flags = resolveChangeFlags( aCtx.Request.options(), changeMode );
    const bool         dryRun = aCtx.Request.dry_run();
    const int          compareFlags = schematic()->Settings().SymbolCompareFlags();

    // Fields are only autoplaced where the schematic editor settings exist to say how
    const bool autoplace = frame() ? frame()->eeconfig()->m_AutoplaceFields.enable
                                   : dynamic_cast<EESCHEMA_SETTINGS*>( Kiface().KifaceSettings() ) != nullptr;

    if( !schematic()->HasHierarchy() )
        schematic()->RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();

    // The symbols of each reference, so that units left out of the request can be named
    REFERENCES                                        refs;
    std::map<wxString, std::set<const SCH_SYMBOL*>> byReference;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            refs.used.insert( symbol->GetBaseRef( &path ) );
            byReference[symbol->GetRef( &path, false )].insert( symbol );
        }
    }

    std::set<const SCH_SYMBOL*> listed;

    for( const SymbolChange& change : aCtx.Request.symbols() )
    {
        if( std::optional<SCH_ITEM*> item = getItemById( KIID( change.symbol().value() ) ) )
            listed.insert( static_cast<const SCH_SYMBOL*>( *item ) );
    }

    SCH_COMMIT*                  commit = nullptr;
    std::set<KIID>               done;
    std::map<wxString, wxString> libraries;
    std::set<SCH_SHEET_PATH>     touched;

    ChangeSymbolsResponse response;
    response.mutable_header()->CopyFrom( aCtx.Request.header() );

    for( const SymbolChange& change : aCtx.Request.symbols() )
    {
        SymbolChangeResult* result = response.add_results();
        result->mutable_symbol()->set_value( change.symbol().value() );

        KIID                     kiid( change.symbol().value() );
        SCH_SHEET_PATH           path;
        std::optional<SCH_ITEM*> item = getItemById( kiid, &path );

        if( !item || ( *item )->Type() != SCH_SYMBOL_T )
        {
            result->set_status( SymbolChangeStatus::SCS_NONEXISTENT );
            result->set_message( item ? "not a symbol" : "no such item" );
            continue;
        }

        if( !done.insert( kiid ).second )
        {
            result->set_status( SymbolChangeStatus::SCS_NONEXISTENT );
            result->set_message( "listed more than once" );
            continue;
        }

        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( *item );
        SCH_SCREEN* screen = path.LastScreen();

        // A symbol that this client's open commit removed is still on its screen until the commit
        // is pushed
        if( m_commits.contains( aCtx.ClientName ) )
        {
            COMMIT* open = m_commits.at( aCtx.ClientName ).second.get();

            if( open->GetStatus( symbol, screen ) & CHT_REMOVE )
            {
                result->set_status( SymbolChangeStatus::SCS_NONEXISTENT );
                result->set_message( "the symbol is removed in the open commit" );
                continue;
            }
        }

        std::vector<SCH_SHEET_PATH> instances;

        for( const SCH_SHEET_PATH& instance : hierarchy.FindAllSheetsForScreen( screen ) )
            instances.push_back( instance );

        const LIB_ID libId = changeMode ? changeId : symbol->GetLibId();
        PackLibId( result->mutable_lib_id(), libId );

        wxString                    loadError;
        std::unique_ptr<LIB_SYMBOL> flattened;

        if( libId.IsValid() )
            flattened = loadLibrarySymbol( project(), libId, &loadError, libraries );
        else
            loadError = wxS( "the symbol is not linked to a library symbol" );

        if( !flattened )
        {
            result->set_status( SymbolChangeStatus::SCS_NOT_FOUND );
            result->set_message( loadError.ToUTF8() );
            continue;
        }

        std::optional<int> unit;

        if( change.has_unit() )
            unit = change.unit();

        int needed = unit.value_or( symbol->GetUnit() );

        if( !unit )
        {
            for( const SCH_SHEET_PATH& instance : instances )
                needed = std::max( needed, symbol->GetUnitSelection( &instance ) );
        }

        if( flattened->GetUnitCount() < needed )
        {
            result->set_status( SymbolChangeStatus::SCS_TOO_FEW_UNITS );
            result->set_message( fmt::format( "the library symbol has {} unit(s), unit {} is needed",
                                              flattened->GetUnitCount(), needed ) );
            continue;
        }

        // Make the change on a detached copy first: it tells whether anything changes and is all a
        // dry run does
        std::unique_ptr<SCH_SYMBOL> copy( static_cast<SCH_SYMBOL*>( symbol->Clone() ) );
        REFERENCES                  probeRefs = refs;

        applySymbolChange( *copy, flattened->Flatten(), libId, flags, instances, dryRun ? refs : probeRefs,
                           nullptr, unit, false );

        bool changed = !sameSymbol( *symbol, *copy, instances, compareFlags );

        for( const wxString& ref : referencesOf( dryRun || !changed ? *copy : *symbol, instances ) )
            result->add_references( ref.ToUTF8() );

        // Other units of the same reference keep the old symbol unless they are listed too
        wxString others;

        for( const SCH_SHEET_PATH& instance : instances )
        {
            for( const SCH_SYMBOL* other : byReference[symbol->GetRef( &instance, false )] )
            {
                if( other != symbol && !listed.contains( other ) )
                {
                    others = wxString::Format( wxS( "other units of %s are not listed and keep their symbol" ),
                                               symbol->GetRef( &instance, false ) );
                }
            }
        }

        result->set_message( others.ToUTF8() );

        if( !changed )
        {
            result->set_status( SymbolChangeStatus::SCS_UNCHANGED );
            continue;
        }

        result->set_status( SymbolChangeStatus::SCS_CHANGED );

        if( dryRun )
            continue;

        if( !commit )
            commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );

        // Staged before the library symbol changes; a symbol already staged keeps its first image.
        // It leaves the screen first so that a library symbol no longer used is not kept in the file.
        commit->Modify( symbol, screen );
        screen->Remove( symbol );

        applySymbolChange( *symbol, std::move( flattened ), libId, flags, instances, refs, screen, unit, autoplace );

        result->clear_references();

        for( const wxString& ref : referencesOf( *symbol, instances ) )
            result->add_references( ref.ToUTF8() );

        kiapi::schematic::types::SchematicSymbolInstance packed;

        if( PackSymbol( &packed, symbol, path, schematic() ) )
            (void) result->mutable_item()->PackFrom( packed );

        touched.insert( instances.begin(), instances.end() );
    }

    for( const SCH_SHEET_PATH& instance : touched )
        instance.UpdateAllScreenReferences();

    if( commit && !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, changeMode ? _( "Changed symbols via API" )
                                                       : _( "Updated symbols via API" ) );

    return response;
}
