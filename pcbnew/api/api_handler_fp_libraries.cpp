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

#include <api/api_handler_fp_libraries.h>

#include <footprint.h>
#include <footprint_library_adapter.h>
#include <pgm_base.h>
#include <pcb_io/pcb_io_mgr.h>
#include <project_pcb.h>
#include <settings/settings_manager.h>

#include <api/board/board_types.pb.h>


LIBRARY_MANAGER_ADAPTER* API_HANDLER_FP_LIBRARIES::adapterForProject( PROJECT& aProject ) const
{
    return PROJECT_PCB::FootprintLibAdapter( &aProject );
}


std::vector<wxString> API_HANDLER_FP_LIBRARIES::getItemNames( LIBRARY_MANAGER_ADAPTER& aAdapter,
                                                              const wxString& aNickname ) const
{
    return static_cast<FOOTPRINT_LIBRARY_ADAPTER&>( aAdapter ).GetFootprintNames( aNickname, true );
}


std::vector<LIB_TREE_ITEM*> API_HANDLER_FP_LIBRARIES::getSearchItems( LIBRARY_MANAGER_ADAPTER& aAdapter,
                                                                      const wxString&          aNickname ) const
{
    std::vector<LIB_TREE_ITEM*> items;

    for( FOOTPRINT* footprint : static_cast<FOOTPRINT_LIBRARY_ADAPTER&>( aAdapter ).GetFootprints( aNickname, true ) )
        items.push_back( footprint );

    return items;
}


bool API_HANDLER_FP_LIBRARIES::packLibraryItem( const LIB_ID& aId, google::protobuf::Any& aOutput ) const
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter =
            static_cast<FOOTPRINT_LIBRARY_ADAPTER*>( adapterForProject( Pgm().GetSettingsManager().Prj() ) );

    std::unique_ptr<FOOTPRINT> footprint( adapter->LoadFootprint( aId, false ) );

    if( !footprint )
        return false;

    kiapi::board::types::Footprint definition;
    footprint->SerializeDefinition( &definition );

    if( !aOutput.PackFrom( definition ) )
        wxFAIL_MSG( wxString::Format( wxT( "Failed to pack footprint '%s'" ), aId.GetUniStringLibId() ) );

    return true;
}


wxString API_HANDLER_FP_LIBRARIES::guessPluginType( const wxString& aExpandedUri ) const
{
    PCB_IO_MGR::PCB_FILE_T type = PCB_IO_MGR::GuessPluginTypeFromLibPath( aExpandedUri );

    // A location that cannot be read (yet) is taken to be a KiCad library
    if( type == PCB_IO_MGR::FILE_TYPE_NONE )
        return API_HANDLER_LIBRARIES::guessPluginType( aExpandedUri );

    return PCB_IO_MGR::ShowType( type );
}
