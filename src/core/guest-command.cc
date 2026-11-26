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

#include "core/guest-command.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <random>
#include <vector>

#include "core/alloc-tracker.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "mips/common/hardware/allocdesc.h"
#include "mips/common/hardware/pcsxcmd.h"
#include "mips/common/hardware/pcsxprof.h"

namespace {

// Field offsets into struct pcsx_CommandHeader.
constexpr uint32_t c_offMagic = 0;
constexpr uint32_t c_offOp = 4;
constexpr uint32_t c_offSize = 8;
constexpr uint32_t c_offStatus = 12;
constexpr uint32_t c_headerSize = 16;

std::optional<uint32_t> peek(uint32_t address) {
    auto* ptr = PCSX::g_emulator->m_mem->getPointer<uint32_t>(address);
    if (!ptr) return std::nullopt;
    return *ptr;
}

// Writes straight through the memory map rather than via Memory::write32,
// because this is the emulator answering, not the guest storing: it must not
// trip write breakpoints or msan bookkeeping.
bool poke(uint32_t address, uint32_t value) {
    auto* ptr = PCSX::g_emulator->m_mem->getPointer<uint32_t>(address);
    if (!ptr) return false;
    *ptr = value;
    return true;
}

void setStatus(uint32_t structAddress, uint32_t status) { poke(structAddress + c_offStatus, status); }

void doQuery(uint32_t structAddress, uint32_t declaredSize) {
    // header(16) + protocolVersion + opCount + opBitmap[4]
    constexpr uint32_t c_needed = c_headerSize + 4 + 4 + 16;
    if (declaredSize < c_needed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }
    uint32_t bitmap[4] = {0, 0, 0, 0};
    // One bit per implemented operation. Keep this in step with the dispatch
    // switch below: a bit set here is a promise the switch honors it.
    for (uint32_t op :
         {uint32_t(PCSX_CMD_QUERY), uint32_t(PCSX_CMD_REGISTER_ALLOCATOR), uint32_t(PCSX_CMD_ALLOC_REPORT),
          uint32_t(PCSX_CMD_SET_STACK_BOUNDS), uint32_t(PCSX_CMD_REGISTER_PROFILER), uint32_t(PCSX_CMD_GET_RANDOM),
          uint32_t(PCSX_CMD_REPORT_WRITE)}) {
        bitmap[op >> 5] |= 1u << (op & 31);
    }
    if (!poke(structAddress + c_headerSize + 0, PCSX_CMD_PROTOCOL_VERSION)) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    poke(structAddress + c_headerSize + 4, PCSX_CMD_COUNT);
    for (uint32_t i = 0; i < 4; i++) {
        poke(structAddress + c_headerSize + 8 + i * 4, bitmap[i]);
    }
    setStatus(structAddress, PCSX_CMDSTATUS_OK);
}

void doRegisterAllocator(uint32_t structAddress, uint32_t declaredSize) {
    // header(16) + nameAddr + heapStart + heapEnd + entryCount
    constexpr uint32_t c_fixed = c_headerSize + 16;
    if (declaredSize < c_fixed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }
    auto entryCount = peek(structAddress + c_headerSize + 12);
    if (!entryCount) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    // The declared size is what bounds the entry array; a descriptor claiming
    // more entries than it declared room for is malformed rather than something
    // to read past the end of.
    uint64_t needed = uint64_t(c_fixed) + uint64_t(*entryCount) * 8;
    if (declaredSize < needed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }

    std::string error;
    uint32_t status =
        PCSX::g_emulator->m_allocTracker->registerDescriptor(structAddress + c_headerSize, *entryCount, &error);
    if (status != PCSX_CMDSTATUS_OK) {
        PCSX::g_system->printf(_("Rejected allocator descriptor at %8.8lx: %s\n"), structAddress, error.c_str());
    }
    setStatus(structAddress, status);
}

void doAllocReport(uint32_t structAddress, uint32_t declaredSize) {
    // header(16) + arenaIndex + 7 counters + 6 finding counts
    constexpr uint32_t c_needed = c_headerSize + 4 + 7 * 4 + 6 * 4;
    if (declaredSize < c_needed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }
    auto arenaIndex = peek(structAddress + c_headerSize);
    if (!arenaIndex) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    const auto& arenas = PCSX::g_emulator->m_allocTracker->arenas();
    uint32_t out = structAddress + c_headerSize + 4;
    poke(out + 0, uint32_t(arenas.size()));
    if (*arenaIndex >= arenas.size()) {
        setStatus(structAddress, PCSX_CMDSTATUS_BAD_ARGUMENT);
        return;
    }
    const auto& arena = arenas[*arenaIndex];
    poke(out + 4, uint32_t(arena.live.size()));
    poke(out + 8, uint32_t(arena.liveBytes));
    poke(out + 12, uint32_t(arena.peakBytes));
    poke(out + 16, uint32_t(arena.allocCount));
    poke(out + 20, uint32_t(arena.freeCount));
    poke(out + 24, uint32_t(PCSX::g_emulator->m_allocTracker->pendingCaptures()));

    uint32_t counts[PCSX_ALLOCFINDING_COUNT] = {0, 0, 0, 0, 0, 0};
    for (const auto& finding : PCSX::g_emulator->m_allocTracker->findings()) {
        if (finding.arena != *arenaIndex) continue;
        auto index = uint32_t(finding.kind);
        if (index < PCSX_ALLOCFINDING_COUNT) counts[index]++;
    }
    for (uint32_t i = 0; i < PCSX_ALLOCFINDING_COUNT; i++) {
        poke(out + 28 + i * 4, counts[i]);
    }
    setStatus(structAddress, PCSX_CMDSTATUS_OK);
}

void doSetStackBounds(uint32_t structAddress, uint32_t declaredSize) {
    // header(16) + low + high
    constexpr uint32_t c_needed = c_headerSize + 8;
    if (declaredSize < c_needed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }
    auto low = peek(structAddress + c_headerSize);
    auto high = peek(structAddress + c_headerSize + 4);
    if (!low || !high) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    if (*low > *high) {
        setStatus(structAddress, PCSX_CMDSTATUS_BAD_ARGUMENT);
        return;
    }
    PCSX::g_emulator->m_cpu->setStackBounds(*low, *high);
    setStatus(structAddress, PCSX_CMDSTATUS_OK);
}

// The bytes to write, or nothing when the host has no entropy to give.
std::optional<std::vector<uint8_t>> randomBytes(uint32_t length) {
    try {
        static std::random_device s_device;
        std::vector<uint8_t> bytes(length);
        uint32_t bits = 0;
        for (uint32_t i = 0; i < length; i++) {
            if ((i & 3) == 0) bits = s_device();
            bytes[i] = uint8_t(bits >> ((i & 3) * 8));
        }
        return bytes;
    } catch (const std::exception& e) {
        PCSX::g_system->printf(_("No host entropy for a guest random request: %s\n"), e.what());
        return std::nullopt;
    }
}

void doGetRandom(uint32_t structAddress, uint32_t declaredSize) {
    // header(16) + buffer + length
    constexpr uint32_t c_needed = c_headerSize + 8;
    if (declaredSize < c_needed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }
    auto buffer = peek(structAddress + c_headerSize);
    auto length = peek(structAddress + c_headerSize + 4);
    if (!buffer || !length) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    if (*length == 0) {
        setStatus(structAddress, PCSX_CMDSTATUS_OK);
        return;
    }
    uint64_t end = uint64_t(*buffer) + *length;
    if (end > 0x100000000ull) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    auto* mem = PCSX::g_emulator->m_mem.get();

    // The scratchpad sits outside the LUTs, on the same page as the hardware
    // registers, so a range there has to stay inside its first 1KB.
    uint8_t* scratchpad = nullptr;
    uint32_t page = *buffer >> 16;
    if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
        uint32_t offset = *buffer & 0xffff;
        if (uint64_t(offset) + *length > 0x400) {
            setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
            return;
        }
        scratchpad = mem->m_hard + offset;
    } else {
        // The write LUT decides writability, so the ROM is refused. Every page,
        // and every msan byte, is checked before the first byte is written, so
        // a refused range leaves memory as it was.
        for (uint64_t p = page; p <= (end - 1) >> 16; p++) {
            if (!mem->m_writeLUT[p]) {
                setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
                return;
            }
        }
        if (mem->msanInitialized()) {
            for (uint64_t address = *buffer; address < end; address++) {
                if (!PCSX::Memory::inMsanRange(uint32_t(address))) continue;
                // Usability alone decides: a freed block keeps its initialized
                // bits, so msanGetStatus would still call it OK.
                uint32_t offset = uint32_t(address) - PCSX::Memory::c_msanStart;
                if (!(mem->m_msanUsableBitmap[offset / 8] & (1 << (offset % 8)))) {
                    setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
                    return;
                }
            }
        }
    }

    // A host without entropy leaves the poisoned status in place, the same
    // answer an emulator without this operation gives, and writes nothing.
    auto bytes = randomBytes(*length);
    if (!bytes) return;

    if (scratchpad) {
        memcpy(scratchpad, bytes->data(), *length);
        setStatus(structAddress, PCSX_CMDSTATUS_OK);
        return;
    }
    for (uint32_t i = 0; i < *length; i++) {
        uint32_t address = *buffer + i;
        mem->m_writeLUT[address >> 16][address & 0xffff] = (*bytes)[i];
    }
    if (mem->msanInitialized() && *buffer < PCSX::Memory::c_msanEnd) {
        uint64_t msanEnd = std::min(end, uint64_t(PCSX::Memory::c_msanEnd));
        if (msanEnd > PCSX::Memory::c_msanStart) {
            uint32_t from = std::max(*buffer, PCSX::Memory::c_msanStart);
            mem->msanDmaWrite(from, uint32_t(msanEnd - from));
        }
    }
    for (uint64_t word = *buffer & ~3u; word < end; word += 4) {
        PCSX::g_emulator->m_cpu->ClearStore(uint32_t(word));
    }
    setStatus(structAddress, PCSX_CMDSTATUS_OK);
}

void doRegisterProfiler(uint32_t structAddress, uint32_t declaredSize) {
    // header(16) + profHeader
    constexpr uint32_t c_needed = c_headerSize + 4;
    if (declaredSize < c_needed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }
    auto header = peek(structAddress + c_headerSize);
    if (!header) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    if (*header == 0) {
        PCSX::g_emulator->m_mem->m_profilerHeader = 0;
        setStatus(structAddress, PCSX_CMDSTATUS_OK);
        return;
    }
    if (*header & 3) {
        setStatus(structAddress, PCSX_CMDSTATUS_BAD_ARGUMENT);
        return;
    }
    auto magic = peek(*header + offsetof(pcsx_ProfHeader, magic));
    auto sizes = peek(*header + offsetof(pcsx_ProfHeader, version));
    auto capacity = peek(*header + offsetof(pcsx_ProfHeader, capacity));
    auto entries = peek(*header + offsetof(pcsx_ProfHeader, entries));
    if (!magic || !sizes || !capacity || !entries) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    if (!pcsx_profHeaderValid(*magic, *sizes & 0xffff, *sizes >> 16, *capacity, *entries)) {
        setStatus(structAddress, PCSX_CMDSTATUS_BAD_ARGUMENT);
        return;
    }
    PCSX::g_emulator->m_mem->m_profilerHeader = *header;
    setStatus(structAddress, PCSX_CMDSTATUS_OK);
}

std::filesystem::path s_reportPath;
FILE* s_report = nullptr;

void doReportWrite(uint32_t structAddress, uint32_t declaredSize) {
    // header(16) + data + length + collecting
    constexpr uint32_t c_needed = c_headerSize + 12;
    if (declaredSize < c_needed) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }
    auto data = peek(structAddress + c_headerSize);
    auto length = peek(structAddress + c_headerSize + 4);
    if (!data || !length) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    bool collecting = !s_reportPath.empty();
    if (collecting && *length) {
        auto& mem = *PCSX::g_emulator->m_mem;
        auto* first = mem.getPointer<uint8_t>(*data);
        auto* last = mem.getPointer<uint8_t>(*data + *length - 1);
        if (!first || last != first + *length - 1) {
            setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
            return;
        }
        if (!s_report) s_report = std::fopen(s_reportPath.string().c_str(), "wb");
        if (!s_report) {
            PCSX::g_system->printf(_("Could not create the report file %s\n"), s_reportPath.string().c_str());
            s_reportPath.clear();
            collecting = false;
        } else if (std::fwrite(first, 1, *length, s_report) != *length) {
            PCSX::g_system->printf(_("Could not write the report file %s\n"), s_reportPath.string().c_str());
            std::fclose(s_report);
            s_report = nullptr;
            s_reportPath.clear();
            collecting = false;
        }
    }
    poke(structAddress + c_headerSize + 8, collecting ? 1 : 0);
    setStatus(structAddress, PCSX_CMDSTATUS_OK);
}

}  // namespace

void PCSX::GuestCommand::setReportPath(const std::filesystem::path& path) {
    closeReport();
    s_reportPath = path;
}

void PCSX::GuestCommand::closeReport() {
    if (s_report && std::fclose(s_report) != 0) {
        g_system->printf(_("Could not close the report file %s\n"), s_reportPath.string().c_str());
    }
    s_report = nullptr;
    s_reportPath.clear();
}

void PCSX::GuestCommand::dispatch(uint32_t structAddress) {
    auto magic = peek(structAddress + c_offMagic);
    if (!magic) {
        // Not mapped: there is nowhere to write a status, so the guest will see
        // its own poison survive, which is the correct thing for it to see.
        g_system->printf(_("Guest command struct at %8.8lx is not in mapped memory\n"), structAddress);
        return;
    }
    if (*magic != PCSX_CMD_MAGIC) {
        setStatus(structAddress, PCSX_CMDSTATUS_BAD_MAGIC);
        return;
    }
    auto op = peek(structAddress + c_offOp);
    auto size = peek(structAddress + c_offSize);
    if (!op || !size) {
        setStatus(structAddress, PCSX_CMDSTATUS_UNMAPPED);
        return;
    }
    if (*size < c_headerSize) {
        setStatus(structAddress, PCSX_CMDSTATUS_SHORT_STRUCT);
        return;
    }

    switch (*op) {
        case PCSX_CMD_QUERY:
            doQuery(structAddress, *size);
            break;
        case PCSX_CMD_REGISTER_ALLOCATOR:
            doRegisterAllocator(structAddress, *size);
            break;
        case PCSX_CMD_ALLOC_REPORT:
            doAllocReport(structAddress, *size);
            break;
        case PCSX_CMD_SET_STACK_BOUNDS:
            doSetStackBounds(structAddress, *size);
            break;
        case PCSX_CMD_REGISTER_PROFILER:
            doRegisterProfiler(structAddress, *size);
            break;
        case PCSX_CMD_GET_RANDOM:
            doGetRandom(structAddress, *size);
            break;
        case PCSX_CMD_REPORT_WRITE:
            doReportWrite(structAddress, *size);
            break;
        default:
            setStatus(structAddress, PCSX_CMDSTATUS_UNKNOWN_OP);
            break;
    }
}
