/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Jon Evans <jon@craftyjon.com>
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

#include <api/api_handler_libraries.h>

#include <magic_enum.hpp>
#include <algorithm>
#include <ranges>
#include <wx/tokenzr.h>

#include <api/api_enums.h>
#include <api/api_utils.h>
#include <libraries/library_manager.h>
#include <lib_id.h>
#include <lib_tree_item.h>
#include <lib_tree_model.h>
#include <kiway.h>
#include <pgm_base.h>
#include <settings/settings_manager.h>

#include <api/common/types/base_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/types/library_types.pb.h>

#include <design_block_library_adapter.h>

using namespace kiapi::common::commands;
using kiapi::common::types::LibraryCommandStatus;
using kiapi::common::types::LibraryLoadStatus;
using kiapi::common::types::LibraryTableScope;
using kiapi::common::types::LibraryType;

API_HANDLER_LIBRARIES::API_HANDLER_LIBRARIES( LIBRARY_TABLE_TYPE aType ) :
        m_type( aType )
{
    registerHandler<GetLibraryItems, LibraryItemsResponse>( &API_HANDLER_LIBRARIES::handleGetLibraryItems );
    registerHandler<GetLibraryTable, LibraryTableResponse>( &API_HANDLER_LIBRARIES::handleGetLibraryTable );
    registerHandler<SearchLibraries, SearchLibrariesResponse>( &API_HANDLER_LIBRARIES::handleSearchLibraries );
    registerHandler<AddLibraryTableEntry, LibraryCommandStatus>(
            &API_HANDLER_LIBRARIES::handleAddLibraryTableEntry );
    registerHandler<UpdateLibraryTableEntry, LibraryCommandStatus>(
            &API_HANDLER_LIBRARIES::handleUpdateLibraryTableEntry );
    registerHandler<DeleteLibraryTableEntry, LibraryCommandStatus>(
            &API_HANDLER_LIBRARIES::handleDeleteLibraryTableEntry );
    registerHandler<GetLibraryStatuses, LibraryStatusResponse>(
            &API_HANDLER_LIBRARIES::handleGetLibraryStatuses );
    registerHandler<ReloadLibrary, LibraryCommandStatus>( &API_HANDLER_LIBRARIES::handleReloadLibrary );
    registerHandler<GetItemsFromLibrary, GetItemsResponse>( &API_HANDLER_LIBRARIES::handleGetItemsFromLibrary );

    // Only the design-block instance registers LoadAllLibraries: it is the dispatcher that
    // lazily loads the schematic and PCB kifaces and forwards to their per-type loads.  If
    // the per-type (SYMBOL/FOOTPRINT) handlers registered it too, the server's handler set
    // (ordered by pointer, not registration order) could route the command to an instance
    // without a KIWAY, which would fail the request.
    if( aType == LIBRARY_TABLE_TYPE::DESIGN_BLOCK )
        registerHandler<LoadAllLibraries, LibraryCommandStatus>(
                &API_HANDLER_LIBRARIES::handleLoadAllLibraries );
}


API_HANDLER_LIBRARIES::~API_HANDLER_LIBRARIES() = default;


LIBRARY_MANAGER_ADAPTER* API_HANDLER_LIBRARIES::adapterForProject( PROJECT& aProject ) const
{
    return aProject.DesignBlockLibs();
}


std::vector<wxString> API_HANDLER_LIBRARIES::getItemNames( LIBRARY_MANAGER_ADAPTER& aAdapter,
                                                           const wxString& aNickname ) const
{
    return static_cast<DESIGN_BLOCK_LIBRARY_ADAPTER&>( aAdapter ).GetDesignBlockNames( aNickname );
}


HANDLER_RESULT<LibraryItemsResponse>
API_HANDLER_LIBRARIES::handleGetLibraryItems( const HANDLER_CONTEXT<GetLibraryItems>& aCtx )
{
    if( aCtx.Request.type() != ToProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( m_type ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    PROJECT& project = Pgm().GetSettingsManager().Prj();
    LIBRARY_MANAGER_ADAPTER* adapter = adapterForProject( project );

    if( !adapter )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "no library adapter is available for this library type" );
        return tl::unexpected( e );
    }

    std::vector<wxString> nicknames;

    if( aCtx.Request.nickname_size() > 0 )
    {
        for( const std::string& nickname : aCtx.Request.nickname() )
        {
            wxString nicknameStr = wxString::FromUTF8( nickname );

            // TODO(JE) decide whether or not to do blocking loads for explicit requests
            adapter->LoadLibraryEntry( nicknameStr );

            nicknames.emplace_back( nicknameStr );
        }
    }
    else
    {
        for( const LIBRARY_TABLE_ROW* row : adapter->Rows() )
            nicknames.emplace_back( row->Nickname() );
    }

    LibraryItemsResponse response;

    for( const wxString& nickname : nicknames )
    {
        for( const wxString& itemName : getItemNames( *adapter, nickname ) )
        {
            kiapi::common::types::LibraryIdentifier* id = response.add_items();
            id->set_library_nickname( nickname.ToUTF8() );
            id->set_entry_name( itemName.ToUTF8() );
        }
    }

    return response;
}


static void packTableEntry( kiapi::common::types::LibraryTableEntry& aEntry, LIBRARY_TABLE_TYPE aTableType,
                            const LIBRARY_TABLE_ROW& aRow, bool aSubstituted = false )
{
    aEntry.set_nickname( aRow.Nickname().ToUTF8() );
    aEntry.set_uri( LIBRARY_MANAGER::GetFullURI( &aRow, aSubstituted ).ToUTF8() );
    aEntry.set_type( ToProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( aTableType ) );
    aEntry.set_scope( ToProtoEnum<LIBRARY_TABLE_SCOPE, LibraryTableScope>( aRow.Scope() ) );
    aEntry.set_description( aRow.Description().ToUTF8() );

    for( const auto& [key, value] : aRow.GetOptionsMap() )
        ( *aEntry.mutable_options() )[key] = value;

    if( std::optional<LIBRARY_MANAGER_ADAPTER*> adapter = Pgm().GetLibraryManager().Adapter( aTableType ) )
        aEntry.set_writable( ( *adapter )->IsWritable( aRow.Nickname() ) );

    aEntry.set_disabled( aRow.Disabled() );
    aEntry.set_hidden( aRow.Hidden() );
}


HANDLER_RESULT<LibraryTableResponse>
API_HANDLER_LIBRARIES::handleGetLibraryTable( const HANDLER_CONTEXT<GetLibraryTable>& aCtx )
{
    LIBRARY_TABLE_TYPE tableType = FromProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( aCtx.Request.type() );
    LIBRARY_TABLE_SCOPE scope = FromProtoEnum<LIBRARY_TABLE_SCOPE>( aCtx.Request.scope() );

    if( tableType == LIBRARY_TABLE_TYPE::UNINITIALIZED )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "a valid library type is required" );
        return tl::unexpected( e );
    }

    if( scope == LIBRARY_TABLE_SCOPE::UNINITIALIZED )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "unknown library table scope" );
        return tl::unexpected( e );
    }

    LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();
    LibraryTableResponse response;

    // Describe the table file itself when one table was asked for.  The manager's Table() would
    // create an empty project table, so the project table is taken from the adapter instead.
    kiapi::common::types::LibraryTable* table = response.mutable_table();
    table->set_scope( aCtx.Request.scope() );
    table->set_valid( true );

    if( scope != LIBRARY_TABLE_SCOPE::BOTH )
    {
        std::optional<LIBRARY_TABLE*> libTable;

        if( scope == LIBRARY_TABLE_SCOPE::GLOBAL )
        {
            libTable = manager.Table( tableType, scope );
        }
        else if( std::optional<LIBRARY_MANAGER_ADAPTER*> adapter = manager.Adapter( tableType ) )
        {
            libTable = ( *adapter )->ProjectTable();
        }

        if( libTable && *libTable )
        {
            table->set_path( ( *libTable )->Path().ToUTF8() );
            table->set_valid( ( *libTable )->IsOk() );

            if( !( *libTable )->IsOk() )
                table->set_error_message( ( *libTable )->ErrorDescription().ToUTF8() );
        }
    }

    for( const LIBRARY_TABLE_ROW* row : manager.Rows( tableType, scope, true ) )
        packTableEntry( *response.add_rows(), tableType, *row, aCtx.Request.substituted() );

    return response;
}


static LibraryCommandStatus makeLibraryStatus( LibraryCommandStatus::Code aCode,
                                               const wxString&            aMessage = wxEmptyString )
{
    LibraryCommandStatus status;
    status.set_code( aCode );

    if( !aMessage.empty() )
        status.set_error_message( aMessage.ToUTF8() );

    return status;
}


std::optional<ApiResponseStatus>
API_HANDLER_LIBRARIES::checkTableEdit( LibraryType aType, LibraryTableScope aScope,
                                       const std::string& aNickname ) const
{
    auto fail =
            []( ApiStatusCode aCode, const std::string& aMessage = std::string() )
            {
                ApiResponseStatus e;
                e.set_status( aCode );

                if( !aMessage.empty() )
                    e.set_error_message( aMessage );

                return e;
            };

    if( aType == LibraryType::LT_UNKNOWN )
        return fail( ApiStatusCode::AS_BAD_REQUEST, "a valid library type is required" );

    // Each type has its own handler instance; the others pass the request on
    if( aType != ToProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( m_type ) )
        return fail( ApiStatusCode::AS_UNHANDLED );

    if( aScope != LibraryTableScope::LTS_GLOBAL && aScope != LibraryTableScope::LTS_PROJECT )
        return fail( ApiStatusCode::AS_BAD_REQUEST, "choose the global or the project table to modify" );

    if( aNickname.empty() )
        return fail( ApiStatusCode::AS_BAD_REQUEST, "a library nickname is required" );

    // The nickname is the first part of a LIB_ID, so it cannot hold the separator or the other
    // characters that LIB_ID refuses
    if( LIB_ID::FindIllegalLibraryNameChar( UTF8( aNickname ) ) )
    {
        return fail( ApiStatusCode::AS_BAD_REQUEST,
                     fmt::format( "'{}' is not a valid library nickname", aNickname ) );
    }

    return std::nullopt;
}


/**
 * Finds the table that an edit changes.  The table is not returned when it is missing, damaged
 * (saving would overwrite what the user has in the file) or read-only; aStatus then tells why.
 */
static LIBRARY_TABLE* editableTable( LIBRARY_TABLE_TYPE aType, LIBRARY_TABLE_SCOPE aScope,
                                     LibraryCommandStatus& aStatus )
{
    std::optional<LIBRARY_TABLE*> table = Pgm().GetLibraryManager().Table( aType, aScope );

    if( !table || !*table )
    {
        aStatus = makeLibraryStatus( LibraryCommandStatus::LCS_ERROR,
                                     aScope == LIBRARY_TABLE_SCOPE::PROJECT
                                             ? wxS( "There is no project library table; is a project open?" )
                                             : wxS( "There is no global library table of this type" ) );
        return nullptr;
    }

    if( !( *table )->IsOk() )
    {
        aStatus = makeLibraryStatus( LibraryCommandStatus::LCS_ERROR,
                                     wxString::Format( wxS( "The library table '%s' is not valid: %s" ),
                                                       ( *table )->Path(), ( *table )->ErrorDescription() ) );
        return nullptr;
    }

    if( ( *table )->IsReadOnly() )
    {
        aStatus = makeLibraryStatus( LibraryCommandStatus::LCS_READ_ONLY,
                                     wxString::Format( wxS( "The library table '%s' is read-only" ),
                                                       ( *table )->Path() ) );
        return nullptr;
    }

    return *table;
}


/**
 * Writes an edited copy of a table and makes the library manager read it back.  The live table
 * is never edited in place: other code keeps pointers to its rows, which an erase would leave
 * dangling.  The reload replaces the table, and it unloads the other libraries of the scope,
 * which are loaded again when they are next needed.
 */
static LibraryCommandStatus commitTableEdit( LIBRARY_TABLE& aEdited, LIBRARY_TABLE_TYPE aType,
                                             LIBRARY_TABLE_SCOPE aScope )
{
    if( LIBRARY_RESULT<void> saved = aEdited.Save(); !saved )
    {
        return makeLibraryStatus( LibraryCommandStatus::LCS_ERROR,
                                  wxString::Format( wxS( "The library table could not be saved: %s" ),
                                                    saved.error().message ) );
    }

    Pgm().GetLibraryManager().ReloadTables( aScope, { aType } );

    return makeLibraryStatus( LibraryCommandStatus::LCS_OK );
}


/// Fills the settings of a table row that a client can give
static void applyEntry( LIBRARY_TABLE_ROW& aRow, const kiapi::common::types::LibraryTableEntry& aEntry )
{
    std::map<std::string, UTF8> options;

    for( const auto& [key, value] : aEntry.options() )
        options[key] = UTF8( value );

    aRow.SetOptions( LIBRARY_TABLE::FormatOptions( &options ).wx_str() );
    aRow.SetDescription( wxString::FromUTF8( aEntry.description() ) );
    aRow.SetDisabled( aEntry.disabled() );
    aRow.SetHidden( aEntry.hidden() );
}


HANDLER_RESULT<LibraryCommandStatus>
API_HANDLER_LIBRARIES::handleAddLibraryTableEntry( const HANDLER_CONTEXT<AddLibraryTableEntry>& aCtx )
{
    const kiapi::common::types::LibraryTableEntry& entry = aCtx.Request.entry();

    if( std::optional<ApiResponseStatus> error =
                checkTableEdit( entry.type(), aCtx.Request.scope(), entry.nickname() ) )
    {
        return tl::unexpected( *error );
    }

    if( entry.uri().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the library location is required" );
        return tl::unexpected( e );
    }

    const LIBRARY_TABLE_SCOPE scope = FromProtoEnum<LIBRARY_TABLE_SCOPE>( aCtx.Request.scope() );
    const wxString            nickname = wxString::FromUTF8( entry.nickname() );
    const wxString            uri = wxString::FromUTF8( entry.uri() );
    LIBRARY_MANAGER&          manager = Pgm().GetLibraryManager();

    LibraryCommandStatus status;
    LIBRARY_TABLE*       live = editableTable( m_type, scope, status );

    if( !live )
        return status;

    // A row of a nested table counts as well: the nickname has to stay unique in the scope
    if( live->HasRow( nickname ) || manager.GetRow( m_type, nickname, scope ) )
    {
        return makeLibraryStatus( LibraryCommandStatus::LCS_ALREADY_EXISTS,
                                  wxString::Format( wxS( "Library '%s' is already in the table" ), nickname ) );
    }

    LIBRARY_TABLE edited( *live );

    LIBRARY_TABLE_ROW& row = edited.InsertRow();
    row.SetNickname( nickname );
    row.SetURI( uri );
    row.SetType( guessPluginType( LIBRARY_MANAGER::ExpandURI( uri, Pgm().GetSettingsManager().Prj() ) ) );
    applyEntry( row, entry );

    status = commitTableEdit( edited, m_type, scope );

    if( status.code() != LibraryCommandStatus::LCS_OK )
        return status;

    // Load the new library now so that the first query does not have to wait for it, and so that
    // a library that cannot be read is reported to the caller.
    if( std::optional<LIBRARY_MANAGER_ADAPTER*> adapter = manager.Adapter( m_type ); adapter && !row.Disabled() )
    {
        std::optional<LIB_STATUS> loaded = ( *adapter )->LoadLibraryEntry( nickname );

        if( loaded && loaded->load_status == LOAD_STATUS::LOAD_ERROR )
        {
            status.set_error_message( wxString::Format( wxS( "The row was added but the library did not load: %s" ),
                                                        loaded->error ? loaded->error->message : wxString( "?" ) )
                                              .ToUTF8() );
        }
    }

    return status;
}


HANDLER_RESULT<LibraryCommandStatus>
API_HANDLER_LIBRARIES::handleUpdateLibraryTableEntry( const HANDLER_CONTEXT<UpdateLibraryTableEntry>& aCtx )
{
    const kiapi::common::types::LibraryTableEntry& entry = aCtx.Request.entry();

    if( std::optional<ApiResponseStatus> error =
                checkTableEdit( entry.type(), aCtx.Request.scope(), entry.nickname() ) )
    {
        return tl::unexpected( *error );
    }

    if( entry.uri().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the library location is required" );
        return tl::unexpected( e );
    }

    const LIBRARY_TABLE_SCOPE scope = FromProtoEnum<LIBRARY_TABLE_SCOPE>( aCtx.Request.scope() );
    const wxString            nickname = wxString::FromUTF8( entry.nickname() );
    const wxString            uri = wxString::FromUTF8( entry.uri() );
    LIBRARY_MANAGER&          manager = Pgm().GetLibraryManager();

    LibraryCommandStatus status;
    LIBRARY_TABLE*       live = editableTable( m_type, scope, status );

    if( !live )
        return status;

    LIBRARY_TABLE edited( *live );

    std::optional<LIBRARY_TABLE_ROW*> row = edited.Row( nickname );

    if( !row )
    {
        // A library of a nested table (the stock libraries) cannot be changed from here
        if( manager.GetRow( m_type, nickname, scope ) )
        {
            return makeLibraryStatus( LibraryCommandStatus::LCS_READ_ONLY,
                                      wxString::Format( wxS( "Library '%s' belongs to a nested table" ), nickname ) );
        }

        return makeLibraryStatus( LibraryCommandStatus::LCS_NOT_FOUND,
                                  wxString::Format( wxS( "Library '%s' is not in the table" ), nickname ) );
    }

    if( ( *row )->Type() == LIBRARY_TABLE_ROW::TABLE_TYPE_NAME )
    {
        return makeLibraryStatus( LibraryCommandStatus::LCS_READ_ONLY,
                                  wxString::Format( wxS( "'%s' is a nested table, not a library" ), nickname ) );
    }

    // The type follows the location; guessing it again would reset what a client chose
    if( ( *row )->URI() != uri )
    {
        ( *row )->SetURI( uri );
        ( *row )->SetType( guessPluginType( LIBRARY_MANAGER::ExpandURI( uri, Pgm().GetSettingsManager().Prj() ) ) );
    }

    applyEntry( **row, entry );

    status = commitTableEdit( edited, m_type, scope );

    if( status.code() == LibraryCommandStatus::LCS_OK )
    {
        if( std::optional<LIBRARY_MANAGER_ADAPTER*> adapter = manager.Adapter( m_type ); adapter && !entry.disabled() )
            ( *adapter )->LoadLibraryEntry( nickname );
    }

    return status;
}


HANDLER_RESULT<LibraryCommandStatus>
API_HANDLER_LIBRARIES::handleDeleteLibraryTableEntry( const HANDLER_CONTEXT<DeleteLibraryTableEntry>& aCtx )
{
    if( std::optional<ApiResponseStatus> error =
                checkTableEdit( aCtx.Request.type(), aCtx.Request.scope(), aCtx.Request.nickname() ) )
    {
        return tl::unexpected( *error );
    }

    const LIBRARY_TABLE_SCOPE scope = FromProtoEnum<LIBRARY_TABLE_SCOPE>( aCtx.Request.scope() );
    const wxString            nickname = wxString::FromUTF8( aCtx.Request.nickname() );

    LibraryCommandStatus status;
    LIBRARY_TABLE*       live = editableTable( m_type, scope, status );

    if( !live )
        return status;

    LIBRARY_TABLE edited( *live );

    std::optional<LIBRARY_TABLE_ROW*> row = edited.Row( nickname );

    if( !row )
    {
        if( Pgm().GetLibraryManager().GetRow( m_type, nickname, scope ) )
        {
            return makeLibraryStatus( LibraryCommandStatus::LCS_READ_ONLY,
                                      wxString::Format( wxS( "Library '%s' belongs to a nested table" ), nickname ) );
        }

        return makeLibraryStatus( LibraryCommandStatus::LCS_NOT_FOUND,
                                  wxString::Format( wxS( "Library '%s' is not in the table" ), nickname ) );
    }

    // The files of the library stay where they are
    std::erase_if( edited.Rows(),
                   [&]( const LIBRARY_TABLE_ROW& aRow )
                   {
                       return aRow.Nickname() == nickname;
                   } );

    return commitTableEdit( edited, m_type, scope );
}


HANDLER_RESULT<SearchLibrariesResponse>
API_HANDLER_LIBRARIES::handleSearchLibraries( const HANDLER_CONTEXT<SearchLibraries>& aCtx )
{
    if( aCtx.Request.type() != ToProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( m_type ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( !supportsSearch() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "searching this type of library is not supported" );
        return tl::unexpected( e );
    }

    LIBRARY_MANAGER_ADAPTER* adapter = adapterForProject( Pgm().GetSettingsManager().Prj() );

    if( !adapter )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "no library adapter is available for this library type" );
        return tl::unexpected( e );
    }

    std::vector<wxString> nicknames;

    if( aCtx.Request.libraries_size() > 0 )
    {
        for( const std::string& nickname : aCtx.Request.libraries() )
        {
            wxString nicknameStr = wxString::FromUTF8( nickname );

            if( !adapter->GetRow( nicknameStr ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "unknown library '{}'", nickname ) );
                return tl::unexpected( e );
            }

            // A named library is worth waiting for
            adapter->LoadLibraryEntry( nicknameStr );
            nicknames.emplace_back( nicknameStr );
        }
    }
    else
    {
        bool pending = false;

        for( const LIBRARY_TABLE_ROW* row : adapter->Rows() )
        {
            if( row->Disabled() || row->Hidden() )
                continue;

            std::optional<LIB_STATUS> status = adapter->GetLibraryStatus( row->Nickname() );

            if( !status || status->load_status == LOAD_STATUS::INVALID || status->load_status == LOAD_STATUS::LOADING )
            {
                pending = true;
                continue;
            }

            if( status->load_status == LOAD_STATUS::LOADED )
                nicknames.emplace_back( row->Nickname() );
        }

        if( pending )
        {
            // Libraries that were never asked for are loaded in the background; the client asks again
            adapter->AsyncLoad();

            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_NOT_READY );
            e.set_error_message( "libraries are still loading" );
            return tl::unexpected( e );
        }
    }

    LIB_TREE_NODE_ROOT root;

    for( const wxString& nickname : nicknames )
    {
        std::vector<LIB_TREE_ITEM*> items = getSearchItems( *adapter, nickname );

        if( items.empty() )
            continue;

        LIB_TREE_NODE_LIBRARY& lib = root.AddLib( nickname, adapter->GetLibraryDescription( nickname ).value_or( wxEmptyString ) );

        for( LIB_TREE_ITEM* item : items )
        {
            if( item )
                lib.AddItem( item );
        }
    }

    root.AssignIntrinsicRanks( {} );

    for( std::unique_ptr<LIB_TREE_NODE>& lib : root.m_Children )
        lib->AssignIntrinsicRanks( {} );

    // The terms are split and matched as the chooser does; a stray paste cannot make this hang
    constexpr int MAX_TERMS = 100;

    wxString query = wxString::FromUTF8( aCtx.Request.query() );

    if( query.StartsWith( "::" ) )
        query = query.Mid( 2 );

    wxStringTokenizer                                  tokenizer( query, " \t\r\n", wxTOKEN_STRTOK );
    std::vector<std::unique_ptr<EDA_COMBINED_MATCHER>> matchers;

    while( tokenizer.HasMoreTokens() && matchers.size() < MAX_TERMS )
        matchers.emplace_back( std::make_unique<EDA_COMBINED_MATCHER>( tokenizer.GetNextToken().Lower(), CTX_LIBITEM ) );

    root.UpdateScore( matchers, nullptr );

    std::vector<const LIB_TREE_NODE*> hits;

    for( const std::unique_ptr<LIB_TREE_NODE>& lib : root.m_Children )
    {
        for( const std::unique_ptr<LIB_TREE_NODE>& node : lib->m_Children )
        {
            if( node->m_Type == LIB_TREE_NODE::TYPE::ITEM && node->m_Score > 0 )
                hits.push_back( node.get() );
        }
    }

    // Best first across libraries: an exact match outranks any sum of partial matches, as in
    // the chooser; within a library the chooser's own order breaks ties.
    std::stable_sort( hits.begin(), hits.end(),
                      []( const LIB_TREE_NODE* aA, const LIB_TREE_NODE* aB )
                      {
                          if( aA->m_ExactMatch != aB->m_ExactMatch )
                              return aA->m_ExactMatch;

                          if( aA->m_Score != aB->m_Score )
                              return aA->m_Score > aB->m_Score;

                          if( aA->m_Parent == aB->m_Parent )
                              return aA->m_IntrinsicRank > aB->m_IntrinsicRank;

                          return aA->m_Parent->m_Name.Cmp( aB->m_Parent->m_Name ) < 0;
                      } );

    size_t limit = aCtx.Request.max_results() > 0 ? static_cast<size_t>( aCtx.Request.max_results() ) : hits.size();
    SearchLibrariesResponse response;

    for( size_t i = 0; i < hits.size() && i < limit; ++i )
    {
        const LIB_TREE_NODE& node = *hits[i];

        kiapi::common::types::LibraryIdentifier* id = response.add_items();
        id->set_library_nickname( node.m_LibId.GetLibNickname().wx_str().ToUTF8() );
        id->set_entry_name( node.m_LibId.GetLibItemName().wx_str().ToUTF8() );

        LibrarySearchResult* result = response.add_results();
        result->mutable_id()->CopyFrom( *id );
        result->set_description( node.m_Desc.ToUTF8() );
        result->set_pin_count( node.m_PinCount );
        result->set_unit_count( std::max<int>( 1, node.m_Children.size() ) );
        result->set_footprint( node.m_Footprint.ToUTF8() );
        result->set_score( node.m_Score );
        result->set_exact_match( node.m_ExactMatch );
    }

    return response;
}


static void packLibraryStatusEntry( kiapi::common::types::LibraryStatusEntry* aEntry, LIBRARY_TABLE_TYPE aTableType,
                                    const LIBRARY_TABLE_ROW& aRow, const LIB_STATUS& aStatus )
{
    packTableEntry( *aEntry->mutable_entry(), aTableType, aRow );
    aEntry->set_status( ToProtoEnum<LOAD_STATUS, LibraryLoadStatus>( aStatus.load_status ) );

    if( aStatus.error )
        aEntry->set_error_message( aStatus.error->message.ToUTF8() );
}


HANDLER_RESULT<LibraryStatusResponse>
API_HANDLER_LIBRARIES::handleGetLibraryStatuses( const HANDLER_CONTEXT<GetLibraryStatuses>& aCtx )
{
    LIBRARY_TABLE_SCOPE scope = FromProtoEnum<LIBRARY_TABLE_SCOPE>( aCtx.Request.scope() );

    if( scope == LIBRARY_TABLE_SCOPE::UNINITIALIZED )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "unknown library table scope" );
        return tl::unexpected( e );
    }

    LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();
    LibraryStatusResponse response;

    std::vector types = { LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_TYPE::FOOTPRINT, LIBRARY_TABLE_TYPE::DESIGN_BLOCK };

    if( !aCtx.Request.types().empty() )
    {
        types.clear();

        for( int typeProto : aCtx.Request.types() )
        {
            LIBRARY_TABLE_TYPE type = FromProtoEnum<LIBRARY_TABLE_TYPE>( static_cast<LibraryType>( typeProto ) );

            if( type == LIBRARY_TABLE_TYPE::UNINITIALIZED )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( wxString::Format( "invalid library table type %d", typeProto ).ToUTF8() );
                return tl::unexpected( e );
            }

            types.push_back( type );
        }
    }

    for( LIBRARY_TABLE_TYPE tableType : types )
    {
        LIBRARY_MANAGER_ADAPTER* adapter = nullptr;

        if( std::optional<LIBRARY_MANAGER_ADAPTER*> optAdapter = manager.Adapter( tableType ) )
            adapter = *optAdapter;

        for( const LIBRARY_TABLE_ROW* row : manager.Rows( tableType, scope ) )
        {
            if( row->Disabled() )
                continue;

            kiapi::common::types::LibraryStatusEntry* entry = response.add_libraries();

            if( adapter )
            {
                if( std::optional<LIB_STATUS> status = adapter->GetLibraryStatus( row->Nickname() ) )
                {
                    packLibraryStatusEntry( entry, tableType, *row, *status );
                    continue;
                }
            }

            packLibraryStatusEntry( entry, tableType, *row, LIB_STATUS{} );
        }
    }

    return response;
}


HANDLER_RESULT<LibraryCommandStatus>
API_HANDLER_LIBRARIES::handleReloadLibrary( const HANDLER_CONTEXT<ReloadLibrary>& aCtx )
{
    auto makeStatus =
            []( LibraryCommandStatus::Code aCode, const wxString& aMessage = wxEmptyString )
            {
                LibraryCommandStatus status;
                status.set_code( aCode );

                if( !aMessage.empty() )
                    status.set_error_message( aMessage.ToUTF8() );

                return status;
            };

    if( aCtx.Request.type() == LibraryType::LT_UNKNOWN )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "a valid library type is required" );
        return tl::unexpected( e );
    }

    if( aCtx.Request.scope() == LibraryTableScope::LTS_BOTH || aCtx.Request.scope() == LibraryTableScope::LTS_UNKNOWN )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "LTS_BOTH is invalid for ReloadLibrary; choose one table" );
        return tl::unexpected( e );
    }

    const LIBRARY_TABLE_TYPE  tableType = FromProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( aCtx.Request.type() );
    const LIBRARY_TABLE_SCOPE scope = ( aCtx.Request.scope() == LibraryTableScope::LTS_GLOBAL )
                                              ? LIBRARY_TABLE_SCOPE::GLOBAL
                                              : LIBRARY_TABLE_SCOPE::PROJECT;

    LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();

    if( !manager.Adapter( tableType ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::vector<wxString> nicknames;

    if( aCtx.Request.nickname_size() > 0 )
    {
        for( const std::string& nickname : aCtx.Request.nickname() )
            nicknames.emplace_back( wxString::FromUTF8( nickname ) );
    }
    else
    {
        for( const LIBRARY_TABLE_ROW* row : manager.Rows( tableType, scope ) )
        {
            if( !row->Disabled() )
                nicknames.emplace_back( row->Nickname() );
        }
    }

    if( nicknames.empty() )
        return makeStatus( LibraryCommandStatus::LCS_NOT_FOUND, "No libraries found to reload" );

    for( const wxString& nickname : nicknames )
    {
        if( !manager.GetRow( tableType, nickname, scope ) )
        {
            return makeStatus( LibraryCommandStatus::LCS_NOT_FOUND,
                               wxString::Format( "Library '%s' not found", nickname ) );
        }
    }

    for( const wxString& nickname : nicknames )
        manager.ReloadLibraryEntry( tableType, nickname, scope );

    return makeStatus( LibraryCommandStatus::LCS_OK );
}


HANDLER_RESULT<LibraryCommandStatus>
API_HANDLER_LIBRARIES::handleLoadAllLibraries( const HANDLER_CONTEXT<LoadAllLibraries>& aCtx )
{
    auto makeStatus = []( LibraryCommandStatus::Code aCode, const wxString& aMessage = wxEmptyString )
    {
        LibraryCommandStatus status;
        status.set_code( aCode );

        if( !aMessage.empty() )
            status.set_error_message( aMessage.ToUTF8() );

        return status;
    };

    if( Pgm().IsGUI() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "LoadAllLibraries is not available in GUI mode" );
        return tl::unexpected( e );
    }

    std::map<LIBRARY_TABLE_TYPE, KIWAY::FACE_T> typesToLoad;

    if( aCtx.Request.type_size() == 0 )
    {
        typesToLoad = { { LIBRARY_TABLE_TYPE::SYMBOL, KIWAY::FACE_SCH },
                        { LIBRARY_TABLE_TYPE::FOOTPRINT, KIWAY::FACE_PCB },
                        { LIBRARY_TABLE_TYPE::DESIGN_BLOCK, KIWAY::KIWAY_FACE_COUNT } };
    }
    else
    {
        for( int protoRaw : aCtx.Request.type() )
        {
            LibraryType protoType = static_cast<LibraryType>( protoRaw );
            LIBRARY_TABLE_TYPE type =
                    FromProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( static_cast<LibraryType>( protoType ) );

            switch( type )
            {
            case LIBRARY_TABLE_TYPE::SYMBOL:        typesToLoad.emplace( type, KIWAY::FACE_SCH );         break;
            case LIBRARY_TABLE_TYPE::FOOTPRINT:     typesToLoad.emplace( type, KIWAY::FACE_PCB );         break;
            case LIBRARY_TABLE_TYPE::DESIGN_BLOCK:  typesToLoad.emplace( type, KIWAY::KIWAY_FACE_COUNT ); break;

            default:
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message(
                        wxString::Format( "invalid library type %s", magic_enum::enum_name( protoType ) ).ToUTF8() );
                return tl::unexpected( e );
            }
            }
        }
    }

    // should always be set in this path
    if( !m_kiway )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "internal error while attempting to load all libraries" );
        return tl::unexpected( e );
    }

    KIWAY& kiway = *m_kiway;

    for( const auto& [type, face] : typesToLoad )
    {
        if( type == LIBRARY_TABLE_TYPE::DESIGN_BLOCK )
        {
            loadAllLibraries();
            continue;
        }

        KIFACE* kiface = kiway.KiFACE( face );

        wxCHECK2( kiface, continue );

        if( m_handlerRegisterCallback )
            m_handlerRegisterCallback( kiface );

        kiface->LoadAllLibraries();
    }

    return makeStatus( LibraryCommandStatus::LCS_OK );
}


LibraryCommandStatus API_HANDLER_LIBRARIES::loadAllLibraries()
{
    // This base implementation handles design blocks
    LIBRARY_MANAGER_ADAPTER* adapter = adapterForProject( Pgm().GetSettingsManager().Prj() );

    LibraryCommandStatus status;

    if( !adapter )
        return status;

    adapter->AsyncLoad();
    status.set_code( LibraryCommandStatus::LCS_OK );
    return status;
}


HANDLER_RESULT<GetItemsResponse>
API_HANDLER_LIBRARIES::handleGetItemsFromLibrary( const HANDLER_CONTEXT<GetItemsFromLibrary>& aCtx )
{
    if( aCtx.Request.type() != ToProtoEnum<LIBRARY_TABLE_TYPE, LibraryType>( m_type ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    wxString projectPath = wxString::FromUTF8( aCtx.Request.document().project().path() );
    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    PROJECT* project = projectPath.IsEmpty() ? &mgr.Prj() : mgr.GetProjectForPath( projectPath );

    if( !project )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the requested project is not open" );
        return tl::unexpected( e );
    }

    LIBRARY_MANAGER_ADAPTER* adapter = adapterForProject( *project );

    if( !adapter )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "no library adapter is available for this library type" );
        return tl::unexpected( e );
    }

    GetItemsResponse response;
    response.mutable_header()->mutable_document()->CopyFrom( aCtx.Request.document() );
    google::protobuf::Any any;

    if( aCtx.Request.item_ids().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "specify at least one lib_id to retrieve" );
        return tl::unexpected( e );
    }

    std::map<wxString, std::vector<wxString>> byNickname;

    for( const kiapi::common::types::LibraryIdentifier& id : aCtx.Request.item_ids() )
    {
        byNickname[wxString::FromUTF8( id.library_nickname() )].emplace_back( wxString::FromUTF8( id.entry_name() ) );
    }

    for( auto& [nickname, entryNames] : byNickname )
    {
        // TODO(JE) decide whether or not to do blocking loads for explicit requests
        adapter->LoadLibraryEntry( nickname );

        for( const wxString& entryName : entryNames )
        {
            if( packLibraryItem( LIB_ID( nickname, entryName ), any ) )
            {
                any.Swap( response.add_items() );
                any.Clear();
            }
        }
    }

    if( response.items().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested items were found" );
        return tl::unexpected( e );
    }

    response.set_status( kiapi::common::types::ItemRequestStatus::IRS_OK );
    return response;
}
