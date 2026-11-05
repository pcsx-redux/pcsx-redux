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

#include "core/shmdisplay.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/gpu.h"
#include "core/psxemulator.h"

PCSX::ShmDisplay::ShmDisplay() : m_listener(g_system->m_eventBus) {
    static_assert(sizeof(ShmDisplayHeader) <= c_vramOffset);
    if (!m_mem.init("display", c_vramOffset + c_vramSize, true)) {
        g_system->message(_("Failed to create the shared display memory block\n"));
    }
    m_header = reinterpret_cast<ShmDisplayHeader*>(m_mem.getPtr());
    m_header->magic = ShmDisplayHeader::c_magic;
    m_header->version = ShmDisplayHeader::c_version;
    m_header->headerSize = sizeof(ShmDisplayHeader);
    m_header->vramOffset = c_vramOffset;
    for (unsigned port = 0; port < 2; port++) {
        m_header->hostPads[port] = 0xffff;
        m_header->hostMultitap[port] = 0;
        for (auto& slot : m_header->hostSlotPads[port]) slot = ShmDisplayHeader::c_slotEmpty;
        m_header->hostDevice[port] = 0;
        m_header->hostMouseDX[port] = 0;
        m_header->hostMouseDY[port] = 0;
        m_header->hostMouseButtons[port] = 0;
    }
    m_listener.listen<Events::GPU::VSync>([this](const auto& event) { publish(); });
}

void PCSX::ShmDisplay::publish() {
    auto& sequence = m_header->sequence;
    sequence.fetch_add(1, std::memory_order_acq_rel);

    auto& gpu = g_emulator->m_gpu;
    auto vram = gpu->getVRAM();
    std::memcpy(m_mem.getPtr() + c_vramOffset, vram.data(), std::min<size_t>(vram.size(), c_vramSize));
    auto& display = gpu->m_display;
    m_header->displayX = display.start.x();
    m_header->displayY = display.start.y();
    m_header->displayWidth = display.size.x();
    m_header->displayHeight = display.size.y();
    m_header->displayDepth24 = display.info.depth == GPU::CtrlDisplayMode::CD_24BITS ? 1 : 0;
    m_header->displayEnabled = (gpu->readStatus() & (1 << 23)) ? 0 : 1;
    m_header->frame++;

    sequence.fetch_add(1, std::memory_order_acq_rel);
}

uint16_t PCSX::ShmDisplay::hostPad(int port) const {
    if (m_header->hostInput.load(std::memory_order_acquire) == 0) return 0xffff;
    return m_header->hostPads[port].load(std::memory_order_relaxed);
}

bool PCSX::ShmDisplay::hostMultitap(int port) const {
    if (!hostInput()) return false;
    return m_header->hostMultitap[port].load(std::memory_order_relaxed) != 0;
}

bool PCSX::ShmDisplay::hostSlotPad(int port, int slot, uint16_t& buttons) const {
    buttons = 0xffff;
    if (!hostInput() || slot < 1 || slot > 3) return false;
    uint32_t value = m_header->hostSlotPads[port][slot].load(std::memory_order_relaxed);
    if (value & 0xffff0000) return false;
    buttons = value;
    return true;
}

int PCSX::ShmDisplay::hostDevice(int port) const {
    if (!hostInput()) return -1;
    uint32_t value = m_header->hostDevice[port].load(std::memory_order_relaxed);
    if (value == 0 || value > 6) return -1;
    return value - 1;
}

void PCSX::ShmDisplay::hostMouse(int port, int8_t& dx, int8_t& dy, uint32_t& buttons) {
    dx = dy = 0;
    buttons = 0;
    if (!hostInput()) return;
    // Send what fits in the reply, and hand the rest back to the accumulator for the next read.
    auto take = [](std::atomic<int32_t>& accumulator) -> int8_t {
        int32_t value = accumulator.exchange(0, std::memory_order_acq_rel);
        int32_t sent = std::clamp<int32_t>(value, -128, 127);
        if (value != sent) accumulator.fetch_add(value - sent, std::memory_order_acq_rel);
        return sent;
    };
    dx = take(m_header->hostMouseDX[port]);
    dy = take(m_header->hostMouseDY[port]);
    buttons = m_header->hostMouseButtons[port].load(std::memory_order_relaxed);
}
