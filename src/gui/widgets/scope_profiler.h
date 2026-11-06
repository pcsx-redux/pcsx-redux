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

#include <deque>
#include <string>
#include <unordered_map>

#include "support/eventbus.h"

namespace PCSX {

class GUI;

namespace Widgets {

// Live timeline of the scopes a guest records with psyqo's scoped profiler
// (src/mips/psyqo/profiler.hh). The guest registers its ring buffer with
// PCSX_CMD_REGISTER_PROFILER; this drains it on every vsync and places each
// scope on the emulated cycle axis.
class ScopeProfiler {
  public:
    ScopeProfiler(bool& show);
    void draw(const char* title, GUI* gui);

    bool& m_show;

  private:
    struct Event {
        uint32_t zone;
        uint64_t start;
        uint64_t end;
        uint8_t depth;
        uint8_t stackDepth;
    };
    struct Zone {
        std::string name;
        std::string file;
        uint32_t line;
        uint32_t color;
    };

    void drain();
    void clear();
    const Zone& zone(uint32_t address);

    EventBus::Listener m_listener;
    std::deque<Event> m_events;
    std::deque<uint64_t> m_vsyncs;
    std::unordered_map<uint32_t, Zone> m_zones;
    uint32_t m_header = 0;
    uint32_t m_cursor = 0;
    uint32_t m_cyclesPer16Lines = 0;
    uint64_t m_dropped = 0;
    unsigned m_maxDepth = 0;
    bool m_frozen = false;
    bool m_resync = false;
    int m_window = 4;
    int m_scrollBack = 0;
};

}  // namespace Widgets
}  // namespace PCSX
