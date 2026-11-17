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

#include <memory>
#include <vector>

namespace PCSX {

// One hardware event. Fixed 32 bytes so the store is a flat, index-addressable
// array, the same way TraceEntry is for the CPU trace. `cycle` is the emulated
// CPU cycle counter, the clock the CPU trace stamps its records with, so the two
// streams can be merged on it. `pc` is the CPU's pc register when the event was
// emitted; while an instruction is executing, that is the address after it.
// Both are exact under the interpreter only. The dynarecs update the cycle counter
// at the end of a block and the pc at branches, so an event raised by an
// instruction in the middle of a block (a DMA start) carries the block's start.
// `type` carries the source in its high byte. What `payload` holds is given per
// type below.
struct HWEvent {
    uint64_t cycle;
    uint32_t pc;
    uint16_t type;
    uint16_t flags;
    uint32_t payload[4];
};
static_assert(sizeof(HWEvent) == 32, "HWEvent must stay a fixed 32 bytes");

class HWEvents {
  public:
    enum class Source : uint8_t {
        IRQ = 1,
        DMA = 2,
        CDROM = 3,
        GPU = 4,
    };

    enum Type : uint16_t {
        // payload: bits being asserted, I_STAT before, I_MASK
        IRQAssert = 0x0101,
        // payload: channel, MADR, BCR, CHCR (as written, start bit set)
        DMAStart = 0x0201,
        // payload: channel, DICR before the completion updates it
        DMAComplete = 0x0202,
        // A command the controller starts executing, rejected ones included.
        // payload: command | parameter count << 8, then the first 12 parameter
        // bytes, packed little-endian
        CDCommand = 0x0301,
        // A response the controller produces, at the moment it produces it.
        // payload: cause | byte count << 8 | posted << 16 | IRQ raised << 17 |
        // dropped << 18, then the first 12 response bytes, packed little-endian.
        // posted means it went straight into the slot the host reads, and IRQ
        // raised that its cause got through the interrupt mask there; otherwise
        // it waits behind an older one and reaches the host later, or, with both
        // slots full, is dropped.
        CDResponse = 0x0302,
        // A data sector read off the disc.
        // payload: position as (m * 60 + s) * 75 + f, so 00:02:00 is 150; bytes
        // handed to the host, 0 when the sector was withheld, like real-time XA
        // audio; sector mode | Setmode value << 8; Mode 2 subheader (file,
        // channel, submode, coding) or 0
        CDSector = 0x0303,
        // A word the GPU reads as the start of a GP0 command, NOPs included.
        // payload: the command word, how it arrived (0 port write, 2 block DMA,
        // 3 linked-list DMA), the port value or the DMA address of the block or
        // list node, and the word count of that write or block
        GPUCommand = 0x0401,
        // A GP1 write. payload: the value written
        GPUControl = 0x0402,
    };

    // Packs up to 12 bytes little-endian into three payload words.
    static void packBytes(const uint8_t* bytes, unsigned count, uint32_t out[3]) {
        out[0] = out[1] = out[2] = 0;
        if (count > 12) count = 12;
        for (unsigned i = 0; i < count; i++) out[i / 4] |= uint32_t(bytes[i]) << ((i % 4) * 8);
    }

    static constexpr uint32_t sourceBit(Source source) { return 1u << static_cast<unsigned>(source); }

    // Emit sites test this before doing anything else, so a source that is not
    // armed costs a load and a branch.
    bool armed(Source source) const { return m_mask & sourceBit(source); }
    uint32_t mask() const { return m_mask; }
    void setMask(uint32_t mask) { m_mask = mask; }

    // Stamps the event with the current cycle and pc. Not a hot path: callers
    // have already checked armed().
    void record(Type type, uint32_t p0 = 0, uint32_t p1 = 0, uint32_t p2 = 0, uint32_t p3 = 0, uint16_t flags = 0);

    static constexpr size_t kRecordsPerChunk = 64 * 1024;  // 2 MiB per chunk

    size_t size() const { return m_count; }
    bool empty() const { return m_count == 0; }
    const HWEvent& operator[](size_t idx) const { return m_chunks[idx / kRecordsPerChunk][idx % kRecordsPerChunk]; }
    void clear();

    // Recording stops once the store holds this many events. 0 means no limit.
    void setLimit(size_t records) { m_limit = records; }
    size_t limit() const { return m_limit; }
    bool full() const { return m_limit != 0 && m_count >= m_limit; }

    // Test hook: when set, called with each event right after it is stored, while
    // the emulator is still at the instant the event was stamped. Null in normal
    // operation. Must not call clear().
    using RecordObserver = void (*)(const HWEvent& event);
    static inline RecordObserver s_recordObserver = nullptr;

    // 256 MiB of events.
    static constexpr size_t kDefaultLimit = 8 * 1024 * 1024;

  private:
    std::vector<std::unique_ptr<HWEvent[]>> m_chunks;
    size_t m_count = 0;
    size_t m_limit = kDefaultLimit;
    uint32_t m_mask = 0;
};

}  // namespace PCSX
