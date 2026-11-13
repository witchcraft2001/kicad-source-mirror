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

#include <api/api_pns_iface.h>

#include <board.h>
#include <board_commit.h>
#include <board_item.h>
#include <router/pns_item.h>


API_PNS_IFACE::API_PNS_IFACE( BOARD* aBoard, TOOL_MANAGER* aToolManager, const wxString& aCommitMessage ) :
        m_commitMessage( aCommitMessage ),
        m_committed( false )
{
    SetBoard( aBoard );
    m_commit = std::make_unique<BOARD_COMMIT>( aToolManager );
}


void API_PNS_IFACE::AddItem( PNS::ITEM* aItem )
{
    PNS_KICAD_IFACE::AddItem( aItem );

    if( aItem->Parent() )
        m_added.push_back( aItem->Parent() );
}


void API_PNS_IFACE::RemoveItem( PNS::ITEM* aItem )
{
    // Pads report footprint offsets rather than removals; only record real board removals
    if( BOARD_ITEM* parent = aItem->Parent(); parent && parent->Type() != PCB_PAD_T )
        m_removed.push_back( parent->m_Uuid );

    PNS_KICAD_IFACE::RemoveItem( aItem );
}


void API_PNS_IFACE::Commit()
{
    // The interactive version also re-groups items and moves dragged footprints; neither applies
    // to routing requests, which never drag components.
    m_fpOffsets.clear();
    m_itemGroups.clear();
    m_removedItems.clear();
    m_replacementMap.clear();

    if( !m_commit->Empty() )
    {
        m_commit->Push( m_commitMessage );
        m_committed = true;
    }
}
