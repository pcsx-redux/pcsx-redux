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

#include "core/disr3000a.h"
#include "support/file.h"

namespace PCSX {

// One captured instruction. Fixed 40 bytes so the trace store is a flat,
// index-addressable array of records: no per-record length and no offset table.
// `code` is the schema - re-disassembling it tells the viewer which of the
// sparse value slots are meaningful and what each one means. The values are the
// PRE-execution operand snapshot: destination registers still hold their old
// value, and no writeback / post-execution state is captured (the post value is
// almost always just the next instruction's input). `cycle` is the emulated CPU
// cycle counter when the instruction was captured, the same clock hardware
// events are stamped with; records loaded from a version 1 file carry
// kUnknownCycle there.
struct TraceEntry {
    uint32_t pc;        // program counter of this instruction
    uint32_t code;      // raw instruction word - the rendering schema
    uint32_t regRs;     // GPR[rs] (also the load/store base register)
    uint32_t regRt;     // GPR[rt]
    uint32_t regRd;     // GPR[rd] (old value, pre-writeback)
    uint32_t hilo;      // hi or lo (mfhi/mflo) - never both in one instruction
    uint32_t cop;       // CP0 / CP2D / CP2C register value - mutually exclusive
    uint32_t memValue;  // value read at the effective address (loads/stores)
    uint64_t cycle;     // m_regs.cycle at capture, or kUnknownCycle
};
static_assert(sizeof(TraceEntry) == 40, "TraceEntry must stay a fixed 40 bytes");
inline constexpr uint64_t kUnknownCycle = UINT64_MAX;

// Feeds a captured entry's sparse values back into the disassembler so the exact
// same "with values" traversal renders an identical annotated line from recorded
// data. gpr() maps a register number to its role slot by matching it against the
// rs/rt/rd fields decoded from `code`; a register that fills two roles resolves
// to the same value, so collisions are harmless.
struct PlaybackValueSource : public Disasm::ValueSource {
    const TraceEntry& e;
    explicit PlaybackValueSource(const TraceEntry& entry) : e(entry) {}
    uint32_t gpr(uint8_t reg) override {
        if (reg == ((e.code >> 21) & 0x1f)) return e.regRs;
        if (reg == ((e.code >> 16) & 0x1f)) return e.regRt;
        if (reg == ((e.code >> 11) & 0x1f)) return e.regRd;
        return 0;
    }
    uint32_t cp0(uint8_t) override { return e.cop; }
    uint32_t cp2d(uint8_t) override { return e.cop; }
    uint32_t cp2c(uint8_t) override { return e.cop; }
    uint32_t hi() override { return e.hilo; }
    uint32_t lo() override { return e.hilo; }
    uint8_t mem8(uint32_t) override { return e.memValue & 0xff; }
    uint16_t mem16(uint32_t) override { return e.memValue & 0xffff; }
    uint32_t mem32(uint32_t) override { return e.memValue; }
};

// Bulk-allocated, fixed-record trace store. Records live in chained
// fixed-size chunks; index N is at chunk N/kRecordsPerChunk, slot N%..., so both
// append and random access are O(1) with no auxiliary index. Capture is gated at
// the interpreter call site (the Trace debug setting), so this object is pure
// storage plus the per-instruction capture step.
class CpuTrace {
  public:
    static constexpr size_t kRecordsPerChunk = 64 * 1024;  // 2.5 MiB per chunk

    // Capture one instruction's pre-execution operand snapshot. Hot path.
    void capture(uint32_t pc, uint32_t code);

    size_t size() const { return m_count; }
    bool empty() const { return m_count == 0; }
    const TraceEntry& operator[](size_t idx) const { return m_chunks[idx / kRecordsPerChunk][idx % kRecordsPerChunk]; }
    void clear();

    // Capture stops once the store holds this many records. 0 means no limit.
    void setLimit(size_t records) { m_limit = records; }
    size_t limit() const { return m_limit; }
    bool full() const { return m_limit != 0 && m_count >= m_limit; }

    // One annotated disassembly line per record, the same text the CPU log used to
    // get, so traces can be read, searched and compared in a text editor.
    bool exportText(IO<File> file, size_t start = 0, size_t count = SIZE_MAX) const;
    // Raw records behind a small header. load() replaces the contents, and returns
    // false, leaving the store empty, on a file that is not a trace.
    bool save(IO<File> file) const;
    bool load(IO<File> file);

    // Test hook: when set, called with each record right after it is filled, while
    // the CPU is still in the exact pre-execution state the record was taken from.
    // Null in normal operation. Must not call clear() or otherwise mutate the store.
    using CaptureObserver = void (*)(const TraceEntry& entry);
    static inline CaptureObserver s_captureObserver = nullptr;

  private:
    TraceEntry& alloc();
    bool loadV1(IO<File> file);
    std::vector<std::unique_ptr<TraceEntry[]>> m_chunks;
    size_t m_count = 0;
    size_t m_limit = kDefaultLimit;

  public:
    // 1 GiB of records.
    static constexpr size_t kDefaultLimit = 1024 * 1024 * 1024 / sizeof(TraceEntry);
};

}  // namespace PCSX
