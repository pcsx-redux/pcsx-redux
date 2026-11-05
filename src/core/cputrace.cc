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

#include "core/cputrace.h"

#include <string.h>

#include <algorithm>
#include <string>

#include "core/psxemulator.h"
#include "core/r3000a.h"

namespace {

// Slim disassembler visitor that fills only the opcode-dependent slots of a
// TraceEntry (hilo / cop / memValue). The three GPR slots are filled branchless
// by the caller straight from the instruction's bitfields, so this visitor never
// needs to handle GPR operands; it fires only for the operands that genuinely
// require opcode awareness. It builds no string - the disassembly traversal is
// reused purely as an "operand enumerator".
struct RecordingDisasm : public PCSX::Disasm {
    PCSX::TraceEntry* m_entry = nullptr;

    static uint32_t readMem(uint32_t addr, int size) {
        auto& live = PCSX::Disasm::liveValueSource();
        switch (size) {
            case 1:
                return live.mem8(addr);
            case 2:
                return live.mem16(addr);
            case 4:
                return live.mem32(addr);
        }
        return 0;
    }

    void Invalid() override {}
    void OpCode(std::string_view) override {}
    void GPR(uint8_t) override {}
    void CP0(uint8_t reg) override { m_entry->cop = PCSX::g_emulator->m_cpu->m_regs.CP0.r[reg]; }
    void CP2D(uint8_t reg) override { m_entry->cop = PCSX::g_emulator->m_cpu->m_regs.CP2D.r[reg]; }
    void CP2C(uint8_t reg) override { m_entry->cop = PCSX::g_emulator->m_cpu->m_regs.CP2C.r[reg]; }
    void HI() override { m_entry->hilo = PCSX::g_emulator->m_cpu->m_regs.GPR.n.hi; }
    void LO() override { m_entry->hilo = PCSX::g_emulator->m_cpu->m_regs.GPR.n.lo; }
    void Imm16(int16_t) override {}
    void Imm16u(uint16_t) override {}
    void Imm32(uint32_t) override {}
    void Target(uint32_t) override {}
    void Sa(uint8_t) override {}
    void OfB(int16_t offset, uint8_t reg, int size) override {
        uint32_t addr = PCSX::g_emulator->m_cpu->m_regs.GPR.r[reg] + offset;
        m_entry->memValue = readMem(addr, size);
    }
    void BranchDest(uint32_t) override {}
    void Offset(uint32_t addr, int size) override { m_entry->memValue = readMem(addr, size); }
    void SyscallName(const char*) override {}
};

}  // namespace

void PCSX::CpuTrace::capture(uint32_t pc, uint32_t code) {
    static RecordingDisasm recorder;

    if (full()) [[unlikely]]
        return;
    TraceEntry& e = alloc();
    auto& regs = g_emulator->m_cpu->m_regs;
    e.pc = pc;
    e.code = code;
    // GPR slots: branchless from the bitfields. For instruction formats that do
    // not actually use a given field as a register (e.g. the rd bits of an
    // I-type), the stored value is simply never read back, because the viewer's
    // disassembly only requests the slots the opcode references.
    e.regRs = regs.GPR.r[(code >> 21) & 0x1f];
    e.regRt = regs.GPR.r[(code >> 16) & 0x1f];
    e.regRd = regs.GPR.r[(code >> 11) & 0x1f];
    e.hilo = 0;
    e.cop = 0;
    e.memValue = 0;
    // Opcode-dependent slots (hi/lo, coprocessor reg, memory word) come from the
    // disassembly traversal, which is the single source of truth for which of
    // those an instruction touches.
    recorder.m_entry = &e;
    recorder.process(code, 0, pc, nullptr);
    if (s_captureObserver) [[unlikely]] {
        s_captureObserver(e);
    }
}

PCSX::TraceEntry& PCSX::CpuTrace::alloc() {
    size_t idx = m_count++;
    size_t chunk = idx / kRecordsPerChunk;
    if (chunk >= m_chunks.size()) {
        m_chunks.emplace_back(std::make_unique<TraceEntry[]>(kRecordsPerChunk));
    }
    return m_chunks[chunk][idx % kRecordsPerChunk];
}

void PCSX::CpuTrace::clear() {
    m_chunks.clear();
    m_count = 0;
}

namespace {
constexpr char kMagic[8] = {'P', 'C', 'S', 'X', 'T', 'R', 'C', 0};
constexpr uint32_t kVersion = 1;
}  // namespace

bool PCSX::CpuTrace::exportText(IO<File> file, size_t start, size_t count) const {
    if (start >= m_count) return true;
    if (count > m_count - start) count = m_count - start;
    auto put = [&file](const std::string& s) {
        return file->write(s.data(), s.size()) == static_cast<ssize_t>(s.size());
    };
    std::string out;
    for (size_t i = start; i < start + count; i++) {
        const TraceEntry& e = (*this)[i];
        PlaybackValueSource source(e);
        out += Disasm::asString(e.code, 0, e.pc, nullptr, true, &source);
        out += '\n';
        if (out.size() >= 1024 * 1024) {
            if (!put(out)) return false;
            out.clear();
        }
    }
    return put(out);
}

bool PCSX::CpuTrace::save(IO<File> file) const {
    auto put = [&file](const void* data, size_t size) { return file->write(data, size) == static_cast<ssize_t>(size); };
    uint32_t version = kVersion;
    uint32_t recordSize = sizeof(TraceEntry);
    uint64_t count = m_count;
    if (!put(kMagic, sizeof(kMagic)) || !put(&version, sizeof(version)) || !put(&recordSize, sizeof(recordSize)) ||
        !put(&count, sizeof(count))) {
        return false;
    }
    size_t left = m_count;
    for (auto& chunk : m_chunks) {
        if (left == 0) break;
        size_t n = std::min(left, kRecordsPerChunk);
        if (!put(chunk.get(), n * sizeof(TraceEntry))) return false;
        left -= n;
    }
    return true;
}

bool PCSX::CpuTrace::load(IO<File> file) {
    clear();
    char magic[sizeof(kMagic)];
    if (file->read(magic, sizeof(magic)) != sizeof(magic)) return false;
    if (memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;
    if (file->read<uint32_t>() != kVersion) return false;
    if (file->read<uint32_t>() != sizeof(TraceEntry)) return false;
    uint64_t count = file->read<uint64_t>();
    while (m_count < count) {
        size_t n = std::min<uint64_t>(count - m_count, kRecordsPerChunk);
        m_chunks.emplace_back(std::make_unique<TraceEntry[]>(kRecordsPerChunk));
        ssize_t got = file->read(m_chunks.back().get(), n * sizeof(TraceEntry));
        if (got != static_cast<ssize_t>(n * sizeof(TraceEntry))) {
            clear();
            return false;
        }
        m_count += n;
    }
    return true;
}
