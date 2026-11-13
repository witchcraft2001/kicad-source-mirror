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

#ifndef KICAD_API_PNS_IFACE_H
#define KICAD_API_PNS_IFACE_H

#include <vector>

#include <router/pns_kicad_iface.h>

class BOARD;
class BOARD_ITEM;
class TOOL_MANAGER;

/**
 * Router interface for IPC API requests: drives the push-and-shove router without a view or an
 * interactive tool.  Routed items are committed to the board as one undoable change when the
 * router calls Commit(), so this works both in the editor and in a headless API server.
 */
class API_PNS_IFACE : public PNS_KICAD_IFACE
{
public:
    /**
     * @param aBoard is the board to route on.
     * @param aToolManager supplies the commit target (the editor's or the headless context's).
     * @param aCommitMessage is the undo entry text for the routed items.
     */
    API_PNS_IFACE( BOARD* aBoard, TOOL_MANAGER* aToolManager, const wxString& aCommitMessage );

    // Nothing to draw: there is no view
    void EraseView() override {}
    bool IsAnyLayerVisible( const PNS_LAYER_RANGE& aLayer ) const override { return true; }
    bool IsItemVisible( const PNS::ITEM* aItem ) const override { return true; }
    void HideItem( PNS::ITEM* aItem ) override {}
    void DisplayItem( const PNS::ITEM* aItem, int aClearance, bool aEdit = false, int aFlags = 0 ) override {}
    void DisplayPathLine( const SHAPE_LINE_CHAIN& aLine, int aImportance ) override {}
    void DisplayRatline( const SHAPE_LINE_CHAIN& aRatline, PNS::NET_HANDLE aNet ) override {}

    EDA_UNITS GetUnits() const override { return EDA_UNITS::MM; }

    void AddItem( PNS::ITEM* aItem ) override;
    void RemoveItem( PNS::ITEM* aItem ) override;

    /// Push the pending board changes as one commit
    void Commit() override;

    /// Board items created by the router, valid after Commit()
    const std::vector<BOARD_ITEM*>& AddedItems() const { return m_added; }

    /// IDs of board items the router removed (replaced or shoved), recorded before Commit()
    const std::vector<KIID>& RemovedItems() const { return m_removed; }

    bool Committed() const { return m_committed; }

private:
    wxString                 m_commitMessage;
    std::vector<BOARD_ITEM*> m_added;
    std::vector<KIID>        m_removed;
    bool                     m_committed;
};

#endif // KICAD_API_PNS_IFACE_H
