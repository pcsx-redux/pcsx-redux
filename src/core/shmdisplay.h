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

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "core/system.h"
#include "support/eventbus.h"
#include "support/sharedmem.h"

namespace PCSX {

// Publishes the emulated display and accepts pad input through a shared memory block, so that a host
// application can show the running game in its own window. Enabled with -shmdisplay. The block is named
// pcsx-redux-display-<pid>, as a POSIX shm object or a Windows file mapping.
//
// The block starts with ShmDisplayHeader, and a copy of the 1024x512 16bpp VRAM follows at vramOffset.
// The header and VRAM are written on each vblank. sequence is odd while a write is in progress, so a
// reader copies what it needs, re-reads sequence, and retries if it changed or was odd.
// The pad words are written by the host. They use the controller's active-low button layout, and are
// ANDed with the emulated controller input while hostInput is non-zero.
//
// Version 2 appends the fields after hostPads; every version 1 field keeps its offset, so a version 1
// writer keeps working, and sees the version 2 fields at their power-on values. All of them are only
// looked at while hostInput is non-zero; with hostInput at 0, input behaves exactly as without the block.
//   hostMultitap[port]: non-zero plugs a multitap into that port. 0 leaves it to the port's Multitap
//     setting, so the host can add a tap but not remove one the user configured.
//   hostSlotPads[port][slot]: the pads in multitap slots B, C and D (slot 1, 2, 3). Bits 0-15 are the
//     active-low button mask, and a slot only holds a pad while bits 16-31 are all 0; any of them set
//     leaves the slot empty. Their power-on value is 0xffffffff, all slots empty. Slot A (slot 0) is the
//     port's own pad, whose host mask stays hostPads[port] with or without a tap, so hostSlotPads[port][0]
//     is ignored.
//   hostDevice[port]: 0 keeps the device type from the port's settings, 1 + n makes it device type n
//     (0 Digital, 1 Analog, 2 Mouse, 3 Negcon, 4 Gun, 5 Guncon), plugged in even when the port's settings
//     leave it disconnected. Other values are ignored.
//   hostMouseDX[port], hostMouseDY[port]: mouse motion accumulators for a Mouse on that port. The host
//     adds motion with an atomic add; each mouse read takes the whole value with an exchange to 0, sends
//     what fits in a signed byte, and adds what did not fit back, so no motion is lost.
//   hostMouseButtons[port]: bit 0 left button, bit 1 right button, 1 = pressed.
// For a Mouse, the buttons and motion come from these fields, and not from the host's mouse, while
// hostInput is non-zero.
struct ShmDisplayHeader {
    static constexpr uint32_t c_magic = 0x44535850;  // "PXSD"
    static constexpr uint32_t c_version = 2;
    static constexpr uint32_t c_slotEmpty = 0xffffffff;

    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t vramOffset;
    std::atomic<uint32_t> sequence;
    uint32_t frame;
    int32_t displayX, displayY, displayWidth, displayHeight;
    uint32_t displayDepth24;
    uint32_t displayEnabled;
    std::atomic<uint32_t> hostInput;
    std::atomic<uint32_t> hostPads[2];
    // version 2
    std::atomic<uint32_t> hostMultitap[2];
    std::atomic<uint32_t> hostSlotPads[2][4];
    std::atomic<uint32_t> hostDevice[2];
    std::atomic<int32_t> hostMouseDX[2];
    std::atomic<int32_t> hostMouseDY[2];
    std::atomic<uint32_t> hostMouseButtons[2];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>) == 4);
static_assert(std::atomic<int32_t>::is_always_lock_free && sizeof(std::atomic<int32_t>) == 4);
// The layout is shared with other programs: these offsets must never move.
static_assert(offsetof(ShmDisplayHeader, sequence) == 16);
static_assert(offsetof(ShmDisplayHeader, displayEnabled) == 44);
static_assert(offsetof(ShmDisplayHeader, hostInput) == 48);
static_assert(offsetof(ShmDisplayHeader, hostPads) == 52);
static_assert(offsetof(ShmDisplayHeader, hostMultitap) == 60);
static_assert(offsetof(ShmDisplayHeader, hostSlotPads) == 68);
static_assert(offsetof(ShmDisplayHeader, hostDevice) == 100);
static_assert(offsetof(ShmDisplayHeader, hostMouseDX) == 108);
static_assert(offsetof(ShmDisplayHeader, hostMouseDY) == 116);
static_assert(offsetof(ShmDisplayHeader, hostMouseButtons) == 124);
static_assert(sizeof(ShmDisplayHeader) == 132);

class ShmDisplay {
  public:
    static constexpr size_t c_vramOffset = 4096;
    static constexpr size_t c_vramSize = 1024 * 512 * 2;

    ShmDisplay();

    bool hostInput() const { return m_header->hostInput.load(std::memory_order_acquire) != 0; }
    // These return the power-on answer, the one that changes nothing, while hostInput is 0.
    uint16_t hostPad(int port) const;
    bool hostMultitap(int port) const;
    // Slots 1 to 3 (B to D); false when the slot is empty.
    bool hostSlotPad(int port, int slot, uint16_t& buttons) const;
    // -1 for no override, otherwise the device type.
    int hostDevice(int port) const;
    // Takes the accumulated motion; buttons as in hostMouseButtons.
    void hostMouse(int port, int8_t& dx, int8_t& dy, uint32_t& buttons);

  private:
    void publish();

    SharedMem m_mem;
    ShmDisplayHeader* m_header = nullptr;
    EventBus::Listener m_listener;
};

}  // namespace PCSX
