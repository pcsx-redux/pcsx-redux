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

#include "core/hwevents.h"

#include "core/psxemulator.h"
#include "core/r3000a.h"

void PCSX::HWEvents::record(Type type, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3, uint16_t flags) {
    if (full()) return;
    size_t idx = m_count++;
    size_t chunk = idx / kRecordsPerChunk;
    if (chunk >= m_chunks.size()) {
        m_chunks.emplace_back(std::make_unique<HWEvent[]>(kRecordsPerChunk));
    }
    HWEvent& e = m_chunks[chunk][idx % kRecordsPerChunk];
    auto& regs = g_emulator->m_cpu->m_regs;
    e.cycle = regs.cycle;
    e.pc = regs.pc;
    e.type = type;
    e.flags = flags;
    e.payload[0] = p0;
    e.payload[1] = p1;
    e.payload[2] = p2;
    e.payload[3] = p3;
    if (s_recordObserver) [[unlikely]] {
        s_recordObserver(e);
    }
}

void PCSX::HWEvents::clear() {
    m_chunks.clear();
    m_count = 0;
}
