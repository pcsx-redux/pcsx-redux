/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include <limits>

#include "core/gpu.h"
#include "support/eventbus.h"

namespace PCSX {
class GPULogger;
namespace Widgets {

class GPULogger {
  public:
    GPULogger(bool& show);
    // replayAllowed: whether the GPU the logger records is stopped, so a replay will not be overwritten.
    void draw(PCSX::GPULogger* logger, const char* title, bool replayAllowed);

    bool& m_show;

  private:
    bool m_replay = false;
    bool m_showOrigins = false;
    bool m_expandAll = false;
    bool m_collapseAll = false;
    bool m_setHighlightRange = false;
    bool m_hoverHighlight = false;
    uint64_t m_frameCounterOrigin = 0;
    unsigned m_beginHighlight = 0;
    unsigned m_endHighlight = std::numeric_limits<unsigned>::max();
    union {
        int raw[2];
        struct {
            int x, y;
        };
    } m_filter = {0, 0};
    bool m_filterEnabled = false;
    bool m_filterProbing = false;
    EventBus::Listener m_listener;
    GPU::Logged::DrawLogSettings m_settings;
};

}  // namespace Widgets
}  // namespace PCSX
