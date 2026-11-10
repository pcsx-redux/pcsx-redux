/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#pragma once

#include <stdint.h>

#include <list>
#include <string>
#include <vector>

#include "core/hwevents.h"
#include "gui/widgets/filedialog.h"

namespace PCSX {

class GUI;

namespace Widgets {

// Lists the structured hardware events. The main window reads
// g_emulator->m_hwEvents and arms its sources; stores loaded from disk open in
// windows of their own. Each view filters rows by source.
class HWEvents {
  public:
    HWEvents(bool& show, std::vector<std::string>& favorites);
    void draw(GUI* gui, const char* title);

    bool& m_show;

  private:
    struct View {
        uint32_t shown = ~0u;  // one bit per source, like the arm mask
        bool followTail = false;
        // Rows that pass the filter, rebuilt when the store grows or the filter changes.
        std::vector<uint32_t> rows;
        size_t scanned = 0;
        uint32_t rowsFilter = 0;
        const PCSX::HWEvents* rowsStore = nullptr;
    };
    struct Loaded {
        std::string name;
        PCSX::HWEvents events;
        View view;
        bool show = true;
    };

    void drawView(GUI* gui, const PCSX::HWEvents& events, View& view, bool live);

    View m_live;
    std::list<Loaded> m_loaded;
    std::string m_error;
    FileDialog<FileDialogMode::Save> m_saveDialog;
    FileDialog<FileDialogMode::Open> m_loadDialog;
};

}  // namespace Widgets
}  // namespace PCSX
