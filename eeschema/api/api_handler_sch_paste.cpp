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
 * @file api_handler_sch_paste.cpp
 *
 * ParseAndCreateItemsFromString for the schematic: the clipboard paste of SCH_EDITOR_CONTROL
 * without a window, a selection or the move tool.
 */

#include <api/api_handler_sch.h>
#include <api/api_enums.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <api/sch_context.h>

#include <common.h>
#include <fmt.h>
#include <io/kicad/kicad_io_utils.h>
#include <ki_error.h>
#include <project.h>
#include <richio.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_group.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <schematic_settings.h>
#include <string_utils.h>
#include <tools/sch_editor_control.h>
#include <tools/sch_tool_utils.h>

#include <api/common/commands/editor_commands.pb.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

using namespace kiapi::common::commands;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;


namespace
{

/**
 * Bookkeeping for one paste: the symbols that received instance data of the pasted text, which
 * have to be reconciled with the hierarchy at the end.
 */
struct PASTE_STATE
{
    std::set<SCH_SYMBOL*> m_symbols;
    wxString              m_projectName;
};


/**
 * Give a pasted symbol the instance data of the sheet path it is pasted to, as
 * SCH_EDITOR_CONTROL::updatePastedSymbol does.  The reference is kept; duplicates are
 * renumbered later.
 *
 * @param aClipPath is the path of the pasted symbol's parent inside the pasted text.
 */
void updatePastedSymbol( PASTE_STATE& aState, SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aPastePath,
                         const KIID_PATH& aClipPath )
{
    SCH_SYMBOL_INSTANCE newInstance;
    bool                instanceFound = false;

    aState.m_symbols.insert( aSymbol );

    for( const SCH_SYMBOL_INSTANCE& tmp : aSymbol->GetInstances() )
    {
        if( ( tmp.m_Path.empty() && aClipPath.empty() ) || ( !aClipPath.empty() && tmp.m_Path.EndsWith( aClipPath ) ) )
        {
            newInstance = tmp;
            instanceFound = true;
            break;
        }
    }

    if( !instanceFound )
    {
        // Text taken from a schematic file or an older version carries no clipboard-relative
        // instance data: the fields say what the symbol is.
        SCH_FIELD* valueField = aSymbol->GetField( FIELD_T::VALUE );
        valueField->SetText( UnescapeString( valueField->GetText() ) );

        newInstance.m_Reference = aSymbol->GetField( FIELD_T::REFERENCE )->GetText();
        newInstance.m_Unit = aSymbol->GetUnit();
    }

    newInstance.m_Path = aPastePath.Path();
    newInstance.m_ProjectName = aState.m_projectName;

    aSymbol->AddHierarchicalReference( newInstance );

    // The unit of the pasted text stays selected
    aSymbol->SetUnit( newInstance.m_Unit );
}


/**
 * Update the symbols and sub-sheets below a pasted sheet for one of the paths it is pasted at.
 *
 * @return the path of the pasted sheet.
 */
SCH_SHEET_PATH updatePastedSheet( PASTE_STATE& aState, SCH_SHEET* aSheet, const SCH_SHEET_PATH& aPastePath,
                                  const KIID_PATH& aClipPath, SCH_SHEET_LIST* aPastedSheets,
                                  std::map<SCH_SHEET_PATH, SCH_REFERENCE_LIST>& aPastedSymbols )
{
    SCH_SHEET_PATH sheetPath = aPastePath;
    sheetPath.push_back( aSheet );
    aPastedSheets->push_back( sheetPath );

    if( aSheet->GetScreen() == nullptr )
        return sheetPath;

    bool isSharedPath = sheetPath.IsSharedPath();

    for( SCH_ITEM* item : aSheet->GetScreen()->Items() )
    {
        if( item->IsConnectable() )
            item->SetConnectivityDirty();

        if( item->Type() == SCH_SYMBOL_T )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            // Only do this once if the symbol is shared across multiple sheets.
            if( !aState.m_symbols.count( symbol ) )
            {
                if( !isSharedPath )
                    const_cast<KIID&>( symbol->m_Uuid ) = KIID();

                for( SCH_PIN* pin : symbol->GetGraphicalPins( ALL_UNITS, ALL_BODY_STYLES ) )
                {
                    if( !isSharedPath )
                        const_cast<KIID&>( pin->m_Uuid ) = KIID();

                    pin->SetConnectivityDirty();
                }
            }

            updatePastedSymbol( aState, symbol, sheetPath, aClipPath );
        }
        else if( item->Type() == SCH_SHEET_T )
        {
            SCH_SHEET* subsheet = static_cast<SCH_SHEET*>( item );

            if( !aPastedSheets->ContainsSheet( subsheet ) )
            {
                if( !isSharedPath )
                    const_cast<KIID&>( subsheet->m_Uuid ) = KIID();

                for( SCH_SHEET_PIN* pin : subsheet->GetPins() )
                {
                    if( !isSharedPath )
                        const_cast<KIID&>( pin->m_Uuid ) = KIID();

                    pin->SetConnectivityDirty();
                }
            }

            KIID_PATH newClipPath = aClipPath;
            newClipPath.push_back( subsheet->m_Uuid );

            updatePastedSheet( aState, subsheet, sheetPath, newClipPath, aPastedSheets, aPastedSymbols );
        }
    }

    sheetPath.GetSymbols( aPastedSymbols[aPastePath], SYMBOL_FILTER_ALL );

    return sheetPath;
}


ApiResponseStatus badRequest( const std::string& aMessage )
{
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( aMessage );
    return e;
}

} // namespace


HANDLER_RESULT<CreateItemsResponse> API_HANDLER_SCH::handleParseAndCreateItemsFromString(
        const HANDLER_CONTEXT<ParseAndCreateItemsFromString>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( aCtx.Request.contents().empty() )
        return tl::unexpected( badRequest( "no contents to paste" ) );

    SCHEMATIC* sch = schematic();

    if( !sch->HasHierarchy() )
        sch->RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = sch->Hierarchy();
    SCH_SHEET_PATH pasteRoot = m_context->GetCurrentSheet().value_or( *hierarchy.begin() );

    if( aCtx.Request.document().has_sheet_path() )
    {
        std::optional<SCH_SHEET_PATH> path =
                hierarchy.GetSheetPathByKIIDPath( UnpackSheetPath( aCtx.Request.document().sheet_path() ) );

        if( !path )
            return tl::unexpected( badRequest( "sheet path not found" ) );

        pasteRoot = *path;
    }

    SCH_SCREEN* targetScreen = pasteRoot.LastScreen();

    if( !targetScreen )
        return tl::unexpected( badRequest( "the target sheet has no screen" ) );

    const VECTOR2I offset = aCtx.Request.has_offset()
                                    ? kiapi::common::UnpackVector2( aCtx.Request.offset(), schIUScale )
                                    : VECTOR2I( 0, 0 );

    // Parse into a screen of its own; nothing reaches the schematic until the text is known good.
    SCH_SHEET   tempSheet;
    SCH_SCREEN* tempScreen = new SCH_SCREEN( sch ); // owned by the sheet

    tempSheet.SetScreen( tempScreen );

    try
    {
        STRING_LINE_READER  reader( aCtx.Request.contents(), "ParseAndCreateItemsFromString" );
        SCH_IO_KICAD_SEXPR  plugin;

        plugin.LoadContent( reader, &tempSheet );
    }
    catch( const IO_ERROR& e )
    {
        return tl::unexpected( badRequest( fmt::format( "could not parse the contents: {}", e.What().ToStdString() ) ) );
    }

    tempScreen->MigrateSimModels();

    PASTE_STATE state;
    state.m_projectName = project().GetProjectName();

    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );

    SCHEMATIC_SETTINGS& settings = sch->Settings();
    wxFileName          destFn = pasteRoot.Last()->GetFileName();

    if( destFn.IsRelative() )
        destFn.MakeAbsolute( project().GetProjectPath() );

    // Paths in the hierarchy that show the target screen: the paste goes to every one of them
    SCH_SHEET_LIST sheetPathsForScreen = hierarchy.FindAllSheetsForScreen( targetScreen );
    sheetPathsForScreen.SortByPageNumbers();

    // Screens of the design, so that a pasted sheet shares the screen of the same file
    std::map<wxString, SCH_SCREEN*> loadedScreens;

    for( const SCH_SHEET_PATH& item : hierarchy )
    {
        if( item.LastScreen() )
            loadedScreens[item.Last()->GetFileName()] = item.LastScreen();
    }

    std::set<wxString> existingSheetNames = targetScreen->GetSheetNames();
    std::set<wxString> existingSuffixes;
    std::set<wxString> composedSuffixes;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        const SCH_SHEET* sheet = path.Last();

        composedSuffixes.insert( sch->GetRefSuffix( path ) );

        if( const SCH_FIELD* suffixField = sheet->GetField( FIELD_T::SHEET_REF_SUFFIX ) )
        {
            SCH_SHEET_PATH parentPath = path;
            parentPath.pop_back();

            existingSuffixes.insert( suffixField->GetText() );
            existingSuffixes.insert( sheet->GetRefSuffix( parentPath ) );
        }
    }

    // References in use, to renumber the duplicates among the pasted symbols
    SCH_REFERENCE_LIST existingRefs;
    hierarchy.GetSymbols( existingRefs, SYMBOL_FILTER_ALL );
    existingRefs.SortByReferenceOnly();

    // Items of the design by identifier: a copy of one of them starts with its resolved state
    std::unordered_map<KIID, EDA_ITEM*> itemMap;
    hierarchy.FillItemMap( itemMap );

    std::map<SCH_SHEET_PATH, SCH_REFERENCE_LIST> pastedSymbols;
    std::map<SCH_SHEET_PATH, SCH_SHEET_LIST>     pastedSheets;

    std::vector<SCH_ITEM*> loadedItems;
    std::vector<SCH_ITEM*> sortedSheets;

    for( SCH_ITEM* item : tempScreen->Items() )
    {
        if( item->Type() == SCH_SHEET_T )
            sortedSheets.push_back( item );
        else
            loadedItems.push_back( item );
    }

    std::sort( sortedSheets.begin(), sortedSheets.end(),
               []( SCH_ITEM* aFirst, SCH_ITEM* aSecond )
               {
                   return StrNumCmp( static_cast<SCH_SHEET*>( aFirst )->GetName(),
                                     static_cast<SCH_SHEET*>( aSecond )->GetName(), CMP_T::CASE_SENSITIVE ) < 0;
               } );

    loadedItems.insert( loadedItems.end(), sortedSheets.begin(), sortedSheets.end() );

    // The items are taken over from the temporary screen
    tempScreen->Clear( false );

    CreateItemsResponse response;
    std::vector<SCH_ITEM*> added;
    bool                   sheetsPasted = false;

    auto reject =
            [&]( SCH_ITEM* aItem, ItemStatusCode aCode, const std::string& aMessage )
            {
                ItemCreationResult* result = response.add_created_items();
                result->mutable_status()->set_code( aCode );
                result->mutable_status()->set_error_message( aMessage );

                // A pasted group must not keep a refused member
                if( EDA_GROUP* group = aItem->GetParentGroup() )
                    group->RemoveItem( aItem );

                delete aItem;
            };

    for( SCH_ITEM* schItem : loadedItems )
    {
        KIID_PATH clipPath( wxT( "/" ) ); // the text is at the root

        if( !s_allowedTypes.contains( schItem->Type() ) )
        {
            reject( schItem, ItemStatusCode::ISC_INVALID_TYPE,
                    fmt::format( "{} cannot be created through the API", schItem->GetClass().ToStdString() ) );
            continue;
        }

        if( schItem->IsConnectable() )
            schItem->SetConnectivityDirty();

        schItem->SetLocked( false );

        if( schItem->Type() == SCH_SYMBOL_T )
        {
            SCH_SYMBOL*       symbol = static_cast<SCH_SYMBOL*>( schItem );
            const LIB_SYMBOL* source = SCH_EDITOR_CONTROL::ChoosePasteLibSymbol(
                    tempScreen, targetScreen, symbol->GetSchSymbolLibraryName() );

            if( !source )
            {
                reject( schItem, ItemStatusCode::ISC_INVALID_DATA,
                        fmt::format( "the library symbol {} is not part of the contents; paste it together with "
                                     "the (lib_symbols) it uses",
                                     symbol->GetSchSymbolLibraryName().ToStdString() ) );
                continue;
            }

            symbol->SetLibSymbol( new LIB_SYMBOL( *source ) );

            for( const LIB_ID& alternate : symbol->GetVariantSymbolOverrides() )
            {
                wxString key = alternate.Format().wx_str();
                auto     it = tempScreen->GetLibSymbols().find( key );

                if( it != tempScreen->GetLibSymbols().end() && it->second && !targetScreen->GetLibSymbols().contains( key ) )
                    targetScreen->AddLibSymbol( key, it->second->Flatten() );
            }

            for( const SCH_SHEET_PATH& sheetPath : sheetPathsForScreen )
                updatePastedSymbol( state, symbol, sheetPath, clipPath );

            // A paste is a copy: new identifiers for the symbol and its pins
            const_cast<KIID&>( symbol->m_Uuid ) = KIID();

            for( SCH_PIN* pin : symbol->GetGraphicalPins( ALL_UNITS, ALL_BODY_STYLES ) )
            {
                const_cast<KIID&>( pin->m_Uuid ) = KIID();
                pin->SetConnectivityDirty();
            }

            for( const SCH_SHEET_PATH& sheetPath : sheetPathsForScreen )
            {
                SCH_REFERENCE schReference( symbol, sheetPath );
                schReference.SetSheetNumber( sheetPath.GetPageNumberAsInt() );
                pastedSymbols[sheetPath].AddItem( schReference );
            }
        }
        else if( schItem->Type() == SCH_SHEET_T )
        {
            SCH_SHEET* sheet = static_cast<SCH_SHEET*>( schItem );
            wxFileName srcFn = sheet->GetFileName();

            if( srcFn.IsRelative() )
                srcFn.MakeAbsolute( project().GetProjectPath() );

            SCH_SHEET_LIST sheetHierarchy( sheet );

            if( hierarchy.TestForRecursion( sheetHierarchy, destFn.GetFullPath( wxPATH_UNIX ) ) )
            {
                reject( schItem, ItemStatusCode::ISC_INVALID_DATA,
                        fmt::format( "the sheet {} has the destination sheet or one of its parents as a "
                                     "sub-sheet",
                                     sheet->GetFileName().ToStdString() ) );
                continue;
            }

            wxFileName  fn = sheet->GetFileName();
            SCH_SCREEN* existingScreen = nullptr;

            if( !fn.IsAbsolute() )
            {
                wxFileName currentSheetFileName = targetScreen->GetFileName();
                fn.Normalize( FN_NORMALIZE_FLAGS | wxPATH_NORM_ENV_VARS, currentSheetFileName.GetPath() );
            }

            // The sheet shares the screen of the same file in the design, which is how a channel is
            // repeated.  Files outside the design are not loaded here.
            if( !sch->Root().SearchHierarchy( fn.GetFullPath( wxPATH_UNIX ), &existingScreen ) )
            {
                auto it = loadedScreens.find( sheet->GetFileName() );

                if( it != loadedScreens.end() )
                    existingScreen = it->second;
            }

            if( !existingScreen && fn.FileExists() )
            {
                reject( schItem, ItemStatusCode::ISC_INVALID_DATA,
                        fmt::format( "the sheet file {} exists but is not part of this schematic; loading sheet "
                                     "files is not supported, add the sheet with CreateItems",
                                     fn.GetFullPath().ToStdString() ) );
                continue;
            }

            // Sheet names are unique on a screen
            SCH_FIELD* nameField = sheet->GetField( FIELD_T::SHEET_NAME );
            wxString   baseName = nameField->GetText();
            wxString   candidateName = baseName;
            wxString   number;

            while( !baseName.IsEmpty() && wxIsdigit( baseName.Last() ) )
            {
                number = baseName.Last() + number;
                baseName.RemoveLast();
            }

            int uniquifier = std::max( 0, wxAtoi( number ) ) + 1;

            while( existingSheetNames.count( candidateName ) )
                candidateName = wxString::Format( wxT( "%s%d" ), baseName, uniquifier++ );

            nameField->SetText( candidateName );
            existingSheetNames.emplace( candidateName );

            sheet->SetParent( pasteRoot.Last() );
            sheet->SetScreen( nullptr );

            if( existingScreen )
            {
                sheet->SetScreen( existingScreen );
            }
            else
            {
                sheet->SetScreen( new SCH_SCREEN( sch ) );
                sheet->GetScreen()->SetFileName( fn.GetFullPath() );
                sheet->GetScreen()->SetContentModified();
            }

            sheetsPasted = true;

            // The non-const GetField would add a suffix field to every pasted sheet
            if( std::as_const( *sheet ).GetField( FIELD_T::SHEET_REF_SUFFIX ) )
            {
                SCH_FIELD*     suffixField = sheet->GetField( FIELD_T::SHEET_REF_SUFFIX );
                wxString       pastedSuffix = suffixField->GetText();
                SCH_SHEET_LIST subtree( sheet );

                // Suffixes concatenate, so the sheet or any child below it can compose to one in use
                auto composedBelowPaste =
                        [&]()
                        {
                            std::vector<wxString> composed;

                            for( const SCH_SHEET_PATH& parentPath : sheetPathsForScreen )
                            {
                                for( const SCH_SHEET_PATH& subPath : subtree )
                                {
                                    SCH_SHEET_PATH fullPath = parentPath;

                                    for( size_t ii = 0; ii < subPath.size(); ++ii )
                                        fullPath.push_back( subPath.at( ii ) );

                                    composed.push_back( fullPath.ComposeRefSuffix() );
                                }
                            }

                            return composed;
                        };

                auto resolvesTaken =
                        [&]( const wxString& aCandidate )
                        {
                            suffixField->SetText( aCandidate );

                            for( const SCH_SHEET_PATH& parentPath : sheetPathsForScreen )
                            {
                                if( existingSuffixes.contains( sheet->GetRefSuffix( parentPath ) ) )
                                    return true;
                            }

                            for( const wxString& composed : composedBelowPaste() )
                            {
                                if( composedSuffixes.contains( composed ) )
                                    return true;
                            }

                            return false;
                        };

                if( settings.m_IncrementPastedSuffix )
                    pastedSuffix = SCH_SHEET::NextRefSuffix( pastedSuffix, existingSuffixes, resolvesTaken );

                suffixField->SetText( pastedSuffix );
                existingSuffixes.insert( pastedSuffix );

                for( const SCH_SHEET_PATH& parentPath : sheetPathsForScreen )
                    existingSuffixes.insert( sheet->GetRefSuffix( parentPath ) );

                for( const wxString& composed : composedBelowPaste() )
                    composedSuffixes.insert( composed );
            }

            // The clipboard path of what is below the sheet uses the old identifier
            clipPath.push_back( sheet->m_Uuid );

            const_cast<KIID&>( sheet->m_Uuid ) = KIID();

            for( SCH_SHEET_PIN* pin : sheet->GetPins() )
            {
                const_cast<KIID&>( pin->m_Uuid ) = KIID();
                pin->SetConnectivityDirty();
            }

            for( const SCH_SHEET_PATH& sheetPath : sheetPathsForScreen )
                updatePastedSheet( state, sheet, sheetPath, clipPath, &pastedSheets[sheetPath], pastedSymbols );
        }
        else
        {
            auto source = itemMap.find( schItem->m_Uuid );

            const_cast<KIID&>( schItem->m_Uuid ) = KIID();

            if( source != itemMap.end() )
            {
                if( SCH_ITEM* sourceItem = dynamic_cast<SCH_ITEM*>( source->second ) )
                {
                    schItem->SetConnectivityDirty( true );
                    schItem->SetLastResolvedState( sourceItem );
                }
            }

            // Named groups stay unique on the screen, the multichannel tool matches them by name
            if( SCH_GROUP* group = dynamic_cast<SCH_GROUP*>( schItem ); group && !group->GetName().IsEmpty() )
                group->SetName( UniqueGroupName( targetScreen, group->GetName() ) );
        }

        added.push_back( schItem );
    }

    // Members of a pasted group move with it
    for( SCH_ITEM* item : added )
    {
        if( !item->GetParentGroup() )
            item->Move( offset );
    }

    for( SCH_ITEM* item : added )
    {
        if( m_frame && targetScreen == frame()->GetScreen() )
            frame()->AddToScreen( item, targetScreen );
        else if( !targetScreen->CheckIfOnDrawList( item ) )
            targetScreen->Append( item );

        commit->Added( item, targetScreen );
    }

    if( sheetsPasted )
    {
        // The hierarchy needs the new sheets before pages and annotation are assigned
        sch->RefreshHierarchy();

        for( SCH_SHEET_PATH& sheetPath : sheetPathsForScreen )
        {
            for( SCH_SHEET_PATH& pastedSheet : pastedSheets[sheetPath] )
            {
                int      page = 1;
                wxString pageNum = wxString::Format( "%d", page );

                while( hierarchy.PageNumberExists( pageNum ) )
                    pageNum = wxString::Format( "%d", ++page );

                int virtualPageNumber = page;

                if( page == hierarchy.GetLastVirtualPageNumber() )
                    virtualPageNumber = hierarchy.GetLastVirtualPageNumber() + 1;

                pastedSheet.SetVirtualPageNumber( virtualPageNumber );

                SCH_SHEET_INSTANCE sheetInstance;

                sheetInstance.m_Path = pastedSheet.Path();

                // Don't include the actual sheet in the instance path.
                sheetInstance.m_Path.pop_back();
                sheetInstance.m_PageNumber = pageNum;
                sheetInstance.m_ProjectName = state.m_projectName;

                SCH_SHEET* sheet = pastedSheet.Last();

                sheet->AddInstance( sheetInstance );
                hierarchy.push_back( pastedSheet );

                // Remove all pasted sheet instance data that is not part of this project.
                std::vector<KIID_PATH> instancesToRemove;

                for( const SCH_SHEET_INSTANCE& instance : sheet->GetInstances() )
                {
                    if( !hierarchy.HasPath( instance.m_Path ) )
                        instancesToRemove.push_back( instance.m_Path );
                }

                for( const KIID_PATH& instancePath : instancesToRemove )
                    sheet->RemoveInstance( instancePath );

                // The sheet paths of the references were copied while the virtual page number was 1
                for( auto& [path, refs] : pastedSymbols )
                {
                    for( SCH_REFERENCE& ref : refs )
                    {
                        if( ref.GetSheetPath() == pastedSheet )
                        {
                            ref.GetSheetPath().SetVirtualPageNumber( virtualPageNumber );
                            ref.SetSheetNumber( virtualPageNumber );
                        }
                    }
                }
            }
        }

        sch->SetSheetNumberAndCount();
        hierarchy = sch->Hierarchy();
    }

    // Renumber the references the paste duplicated; the others keep the numbers of the text
    std::map<SCH_SHEET_PATH, SCH_REFERENCE_LIST> annotatedSymbols;

    for( const SCH_SHEET_PATH& sheetPath : sheetPathsForScreen )
    {
        for( size_t i = 0; i < pastedSymbols[sheetPath].GetCount(); i++ )
            annotatedSymbols[sheetPath].AddItem( pastedSymbols[sheetPath][i] );

        for( const SCH_SHEET_PATH& pastedSheetPath : pastedSheets[sheetPath] )
        {
            for( size_t i = 0; i < pastedSymbols[pastedSheetPath].GetCount(); i++ )
                annotatedSymbols[pastedSheetPath].AddItem( pastedSymbols[pastedSheetPath][i] );
        }
    }

    ANNOTATE_ALGO_T annotateAlgo = static_cast<ANNOTATE_ALGO_T>( settings.m_AnnotateMethod );

    for( const SCH_SHEET_PATH& path : sheetPathsForScreen )
    {
        annotatedSymbols[path].SortByReferenceOnly();
        annotatedSymbols[path].SetRefDesTracker( settings.m_refDesTracker );
        annotatedSymbols[path].ReannotateDuplicates( existingRefs, annotateAlgo );
        annotatedSymbols[path].UpdateAnnotation();

        for( size_t i = 0; i < annotatedSymbols[path].GetCount(); i++ )
            existingRefs.AddItem( annotatedSymbols[path][i] );

        for( const SCH_SHEET_PATH& pastedSheetPath : pastedSheets[path] )
        {
            annotatedSymbols[pastedSheetPath].SortByReferenceOnly();
            annotatedSymbols[pastedSheetPath].SetRefDesTracker( settings.m_refDesTracker );
            annotatedSymbols[pastedSheetPath].ReannotateDuplicates( existingRefs, annotateAlgo );
            annotatedSymbols[pastedSheetPath].UpdateAnnotation();

            for( size_t i = 0; i < annotatedSymbols[pastedSheetPath].GetCount(); i++ )
                existingRefs.AddItem( annotatedSymbols[pastedSheetPath][i] );
        }
    }

    pasteRoot.UpdateAllScreenReferences();

    // The text brings instance paths that are not valid in this project; drop them so they do not
    // accumulate in the saved schematic.
    for( SCH_SYMBOL* symbol : state.m_symbols )
        PrunePastedSymbolInstances( symbol, *sch );

    SCH_SHEET_LIST sheets = sch->Hierarchy();
    SCH_SCREENS    allScreens( sch->Root() );

    allScreens.PruneOrphanedSymbolInstances( state.m_projectName, sheets );
    allScreens.PruneOrphanedSheetInstances( state.m_projectName, sheets );

    // With reused sheet suffixes the editor annotates the pasted symbols once more with the full
    // annotator, which gives the instances of a reused sheet one base reference
    if( settings.m_AnnotateReusedSheetSuffixes && !added.empty() )
    {
        SCH_REFERENCE_LIST           references;
        SCH_MULTI_UNIT_REFERENCE_MAP locked;
        SCH_SHEET_LIST               pastedSubSheets;

        for( SCH_ITEM* item : added )
        {
            if( item->Type() == SCH_SYMBOL_T )
            {
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

                pasteRoot.AppendMultiUnitSymbol( locked, symbol, SYMBOL_FILTER_NON_POWER );
                pasteRoot.AppendSymbol( references, symbol, SYMBOL_FILTER_NON_POWER );
            }
            else if( item->Type() == SCH_SHEET_T )
            {
                SCH_SHEET_PATH subPath = pasteRoot;
                subPath.push_back( static_cast<SCH_SHEET*>( item ) );
                sheets.GetSheetsWithinPath( pastedSubSheets, subPath );
            }
        }

        pastedSubSheets.GetMultiUnitSymbols( locked, SYMBOL_FILTER_NON_POWER );
        pastedSubSheets.GetSymbolsWithinPath( references, pasteRoot, SYMBOL_FILTER_NON_POWER );
        sch->SetSheetNumberAndCount();

        // Sheets that compose to the same suffix cannot share a base; the duplicates were already
        // renumbered above, which is what the editor leaves in that case as well
        if( !FindRefSuffixCollision( references, *sch ) )
        {
            AnnotateReferences( references, std::move( locked ), *sch, ANNOTATE_SELECTION,
                                static_cast<ANNOTATE_ORDER_T>( settings.m_AnnotateSortOrder ), annotateAlgo,
                                settings.m_AnnotateStartNum, false,
                                [&]( SCH_REFERENCE& aRef )
                                {
                                    commit->Modify( aRef.GetSymbol(), aRef.GetSheetPath().LastScreen() );
                                } );
        }
    }

    for( SCH_ITEM* item : added )
    {
        ItemCreationResult* result = response.add_created_items();
        result->mutable_status()->set_code( ItemStatusCode::ISC_OK );
        packSchItem( *result->mutable_item(), item, pasteRoot );
    }

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Pasted items via API" ) );

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}
