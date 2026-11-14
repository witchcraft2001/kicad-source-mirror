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

#ifndef KICAD_API_HANDLER_LIBRARIES_H
#define KICAD_API_HANDLER_LIBRARIES_H

#include <functional>
#include <map>
#include <vector>

#include <api/api_handler.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/library_commands.pb.h>
#include <libraries/library_table.h>

class LIB_ID;
class LIB_TREE_ITEM;
class LIBRARY_MANAGER_ADAPTER;
class KIWAY;
struct KIFACE;
class PROJECT;

/**
 * Base class for API handlers related to library management.
 *
 * These are separate from the API_HANDLER_EDITOR derived classes because
 * libraries (other than design blocks) need to be opened in a kiface context,
 * but not associated with one particular frame.
 */
class API_HANDLER_LIBRARIES : public API_HANDLER
{
public:
    API_HANDLER_LIBRARIES( LIBRARY_TABLE_TYPE aType = LIBRARY_TABLE_TYPE::DESIGN_BLOCK );

    ~API_HANDLER_LIBRARIES() override;

    void SetKiway( KIWAY* aKiway ) { m_kiway = aKiway; }

    /// Set a callback to register the handler on the API server in headless mode
    void SetLibraryHandlerRegistrar( std::function<void( KIFACE* )> aRegistrar )
    {
        m_handlerRegisterCallback = std::move( aRegistrar );
    }


protected:
    HANDLER_RESULT<kiapi::common::commands::LibraryItemsResponse>
    handleGetLibraryItems( const HANDLER_CONTEXT<kiapi::common::commands::GetLibraryItems>& aCtx );

    /// Reports the rows of a library table; any instance can answer for every library type
    HANDLER_RESULT<kiapi::common::commands::LibraryTableResponse>
    handleGetLibraryTable( const HANDLER_CONTEXT<kiapi::common::commands::GetLibraryTable>& aCtx );

    /// Edits one row of a library table of this handler's type, then reloads the table
    HANDLER_RESULT<kiapi::common::types::LibraryCommandStatus>
    handleAddLibraryTableEntry( const HANDLER_CONTEXT<kiapi::common::commands::AddLibraryTableEntry>& aCtx );

    HANDLER_RESULT<kiapi::common::types::LibraryCommandStatus>
    handleUpdateLibraryTableEntry( const HANDLER_CONTEXT<kiapi::common::commands::UpdateLibraryTableEntry>& aCtx );

    HANDLER_RESULT<kiapi::common::types::LibraryCommandStatus>
    handleDeleteLibraryTableEntry( const HANDLER_CONTEXT<kiapi::common::commands::DeleteLibraryTableEntry>& aCtx );

    /// Ranks the items of the libraries against a query the way the library choosers do
    HANDLER_RESULT<kiapi::common::commands::SearchLibrariesResponse>
    handleSearchLibraries( const HANDLER_CONTEXT<kiapi::common::commands::SearchLibraries>& aCtx );

    HANDLER_RESULT<kiapi::common::commands::LibraryStatusResponse>
    handleGetLibraryStatuses( const HANDLER_CONTEXT<kiapi::common::commands::GetLibraryStatuses>& aCtx );

    HANDLER_RESULT<kiapi::common::types::LibraryCommandStatus>
    handleReloadLibrary( const HANDLER_CONTEXT<kiapi::common::commands::ReloadLibrary>& aCtx );

    HANDLER_RESULT<kiapi::common::types::LibraryCommandStatus>
    handleLoadAllLibraries( const HANDLER_CONTEXT<kiapi::common::commands::LoadAllLibraries>& aCtx );

    /// Starts a background load of all libraries of this handler's type.
    kiapi::common::types::LibraryCommandStatus loadAllLibraries();

    HANDLER_RESULT<kiapi::common::commands::GetItemsResponse>
    handleGetItemsFromLibrary( const HANDLER_CONTEXT<kiapi::common::commands::GetItemsFromLibrary>& aCtx );

    virtual bool packLibraryItem( const LIB_ID& aId, google::protobuf::Any& aOutput ) const
    {
        return false;
    }

    LIBRARY_TABLE_TYPE libraryType() const { return m_type; }

    virtual LIBRARY_MANAGER_ADAPTER* adapterForProject( PROJECT& aProject ) const;

    /**
     * Returns the names of all entries in the given library, or an empty vector if the
     * library is not loaded (or cannot be enumerated).
     */
    virtual std::vector<wxString> getItemNames( LIBRARY_MANAGER_ADAPTER& aAdapter, const wxString& aNickname ) const;

    /**
     * Returns the items of a loaded library for SearchLibraries; the pointers stay owned by the
     * library.  Only the handlers of types that can be searched override this together with
     * supportsSearch().
     */
    virtual std::vector<LIB_TREE_ITEM*> getSearchItems( LIBRARY_MANAGER_ADAPTER& aAdapter,
                                                        const wxString&          aNickname ) const
    {
        return {};
    }

    virtual bool supportsSearch() const { return false; }

    /**
     * Returns the IO plugin name for a library table row that points at the given library.
     *
     * @param aExpandedUri is the library location with environment variables expanded.
     * @return the table row type, "KiCad" unless the handler's type has other formats.
     */
    virtual wxString guessPluginType( const wxString& aExpandedUri ) const { return wxS( "KiCad" ); }

private:
    /**
     * Checks the parts of a library table edit that do not depend on the table.
     *
     * @return the error to answer with, or nullopt if the request may proceed.
     */
    std::optional<ApiResponseStatus> checkTableEdit( kiapi::common::types::LibraryType aType,
                                                     kiapi::common::types::LibraryTableScope aScope,
                                                     const std::string& aNickname ) const;

    LIBRARY_TABLE_TYPE m_type;

    /// Used to lazily load kifaces for LoadAllLibraries in headless; null in GUI mode.
    KIWAY* m_kiway = nullptr;

    /// Invoked with a lazily-loaded KIFACE so its handlers can be registered on the server.
    std::function<void( KIFACE* )> m_handlerRegisterCallback;
};

#endif //KICAD_API_HANDLER_LIBRARIES_H
