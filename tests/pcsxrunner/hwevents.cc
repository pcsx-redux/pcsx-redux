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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "gtest/gtest.h"
#include "main/main.h"

namespace {

struct Seen {
    uint64_t total = 0;
    uint64_t stampMismatches = 0;
    uint64_t backwards = 0;
    uint64_t lastCycle = 0;
    uint64_t firstCycle = 0;
    uint64_t irqAsserts = 0;
    uint64_t vblankAsserts = 0;
    uint64_t dmaStarts = 0;
    uint64_t dmaCompletes = 0;
    uint32_t startedChannels = 0;
    uint32_t completedChannels = 0;
    uint64_t unknownTypes = 0;
};

Seen s_seen;

// Runs while the emulator is still at the instant the event was stamped, so the
// live cycle counter and pc are what the record must hold.
void observe(const PCSX::HWEvent& e) {
    auto& regs = PCSX::g_emulator->m_cpu->m_regs;
    if (e.cycle != regs.cycle || e.pc != regs.pc) s_seen.stampMismatches++;
    if (s_seen.total == 0) s_seen.firstCycle = e.cycle;
    if (s_seen.total != 0 && e.cycle < s_seen.lastCycle) s_seen.backwards++;
    s_seen.lastCycle = e.cycle;
    s_seen.total++;
    switch (e.type) {
        case PCSX::HWEvents::IRQAssert:
            s_seen.irqAsserts++;
            if (e.payload[0] == 1) s_seen.vblankAsserts++;
            break;
        case PCSX::HWEvents::DMAStart:
            s_seen.dmaStarts++;
            s_seen.startedChannels |= 1u << e.payload[0];
            break;
        case PCSX::HWEvents::DMAComplete:
            s_seen.dmaCompletes++;
            s_seen.completedChannels |= 1u << e.payload[0];
            break;
        default:
            s_seen.unknownTypes++;
            break;
    }
}

struct ObserverScope {
    ObserverScope() {
        s_seen = Seen{};
        PCSX::HWEvents::s_recordObserver = observe;
    }
    ~ObserverScope() { PCSX::HWEvents::s_recordObserver = nullptr; }
};

}  // namespace

TEST(HWEvents, IRQAndDMAEventsAreStampedInOrder) {
    ObserverScope scope;
    auto luaOut = std::filesystem::temp_directory_path() / "pcsx-hwevents-lua.txt";
    std::filesystem::remove(luaOut);
    // Arms from Lua before the exe runs, and reads the whole store back through
    // PCSX.HWEvents.read when the emulator quits.
    std::string lua =
        "PCSX.HWEvents.arm('IRQ', 'DMA') "
        "hwEventsQuit = PCSX.Events.createEventListener('Quitting', function() "
        "local arr, n = PCSX.HWEvents.read() "
        "local f = io.open('" +
        luaOut.generic_string() +
        "', 'w') "
        "local sized = PCSX.HWEvents.size() == n "
        "PCSX.HWEvents.clear() "
        "local cleared = PCSX.HWEvents.size() == 0 and PCSX.HWEvents.read() == nil "
        "local armedMask = PCSX.HWEvents.mask() "
        "PCSX.HWEvents.disarm() "
        "local disarmed = PCSX.HWEvents.mask() == 0 "
        "f:write(string.format('%d %s %s %s %s %d %s', n, tostring(arr[0].cycle), "
        "tostring(arr[n - 1].cycle), tostring(sized), tostring(cleared), armedMask, "
        "tostring(disarmed))) "
        "f:close() end)";
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-exec", lua.c_str(), "-loadexe", "src/mips/tests/dma/dma.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    printf(
        "hwevents: %llu events, %llu IRQ asserts (%llu VBlank), %llu DMA starts, %llu completes, channels %02x/%02x\n",
        (unsigned long long)s_seen.total, (unsigned long long)s_seen.irqAsserts,
        (unsigned long long)s_seen.vblankAsserts, (unsigned long long)s_seen.dmaStarts,
        (unsigned long long)s_seen.dmaCompletes, s_seen.startedChannels, s_seen.completedChannels);
    EXPECT_GT(s_seen.total, 0u);
    EXPECT_EQ(s_seen.stampMismatches, 0u);
    EXPECT_EQ(s_seen.backwards, 0u);
    EXPECT_EQ(s_seen.unknownTypes, 0u);
    // VBlank is raised every frame whatever the program does.
    EXPECT_GT(s_seen.vblankAsserts, 0u);
    EXPECT_GT(s_seen.dmaStarts, 0u);
    EXPECT_GT(s_seen.dmaCompletes, 0u);
    // Every channel that completed was started first.
    EXPECT_EQ(s_seen.completedChannels & ~s_seen.startedChannels, 0u);

    std::ifstream in(luaOut);
    ASSERT_TRUE(in.good()) << "the Lua Quitting listener did not write " << luaOut;
    uint64_t luaCount = 0;
    std::string first, last, sized, cleared, disarmed;
    uint32_t armedMask = 0;
    in >> luaCount >> first >> last >> sized >> cleared >> armedMask >> disarmed;
    EXPECT_EQ(luaCount, s_seen.total);
    // LuaJIT prints a uint64_t cdata with a ULL suffix.
    EXPECT_EQ(first, std::to_string(s_seen.firstCycle) + "ULL");
    EXPECT_EQ(last, std::to_string(s_seen.lastCycle) + "ULL");
    EXPECT_EQ(sized, "true");
    EXPECT_EQ(cleared, "true");
    EXPECT_EQ(armedMask, PCSX::HWEvents::sourceBit(PCSX::HWEvents::Source::IRQ) |
                             PCSX::HWEvents::sourceBit(PCSX::HWEvents::Source::DMA));
    EXPECT_EQ(disarmed, "true");
    in.close();
    std::filesystem::remove(luaOut);
}

TEST(HWEvents, NothingRecordedWhenDisarmed) {
    ObserverScope scope;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-loadexe", "src/mips/tests/dma/dma.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(s_seen.total, 0u);
}

namespace {

struct CDSeen {
    uint64_t total = 0;
    uint64_t stampMismatches = 0;
    uint64_t backwards = 0;
    uint64_t lastCycle = 0;
    uint64_t commands = 0;
    uint64_t responses = 0;
    uint64_t sectors = 0;
    uint64_t setlocs = 0;
    uint64_t setlocChecked = 0;
    uint64_t setlocMismatches = 0;
    uint64_t dataReadyAfterSector = 0;
    uint64_t missingDataReady = 0;
    uint64_t irqRaisedResponses = 0;
    uint64_t irqWithoutAssert = 0;
    bool sawPVD = false;
    // Position a Setloc asked for, checked against the next sector read.
    int64_t pendingSetloc = -1;
    bool expectDataReady = false;
    PCSX::HWEvent previous{};
};

CDSeen s_cd;

unsigned fromBCD(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

void observeCD(const PCSX::HWEvent& e) {
    auto& regs = PCSX::g_emulator->m_cpu->m_regs;
    if (e.cycle != regs.cycle || e.pc != regs.pc) s_cd.stampMismatches++;
    if (s_cd.total != 0 && e.cycle < s_cd.lastCycle) s_cd.backwards++;
    s_cd.lastCycle = e.cycle;
    s_cd.total++;
    // The data ready a delivered sector raises is the very next CD-ROM event.
    if (s_cd.expectDataReady && (e.type >> 8) == 3) {
        s_cd.expectDataReady = false;
        if (e.type == PCSX::HWEvents::CDResponse && (e.payload[0] & 0xff) == 1) {
            s_cd.dataReadyAfterSector++;
        } else {
            s_cd.missingDataReady++;
        }
    }
    switch (e.type) {
        case PCSX::HWEvents::CDCommand: {
            s_cd.commands++;
            unsigned command = e.payload[0] & 0xff;
            unsigned count = (e.payload[0] >> 8) & 0xff;
            if (command == 2 && count == 3) {
                s_cd.setlocs++;
                uint32_t p = e.payload[1];
                s_cd.pendingSetloc =
                    (fromBCD(p & 0xff) * 60 + fromBCD((p >> 8) & 0xff)) * 75 + fromBCD((p >> 16) & 0xff);
            }
        } break;
        case PCSX::HWEvents::CDResponse:
            s_cd.responses++;
            if (e.payload[0] & (1u << 17)) {
                s_cd.irqRaisedResponses++;
                // The controller pulls the IRQ line before the response is recorded.
                if (s_cd.previous.type != PCSX::HWEvents::IRQAssert || s_cd.previous.payload[0] != 4 ||
                    s_cd.previous.cycle != e.cycle) {
                    s_cd.irqWithoutAssert++;
                }
            }
            break;
        case PCSX::HWEvents::CDSector:
            s_cd.sectors++;
            if (e.payload[0] == 150 + 16) s_cd.sawPVD = true;
            if (s_cd.pendingSetloc >= 0) {
                s_cd.setlocChecked++;
                if (e.payload[0] != uint32_t(s_cd.pendingSetloc)) s_cd.setlocMismatches++;
                s_cd.pendingSetloc = -1;
            }
            if (e.payload[1] != 0) s_cd.expectDataReady = true;
            break;
        default:
            break;
    }
    s_cd.previous = e;
}

struct CDObserverScope {
    CDObserverScope() {
        s_cd = CDSeen{};
        PCSX::HWEvents::s_recordObserver = observeCD;
    }
    ~CDObserverScope() { PCSX::HWEvents::s_recordObserver = nullptr; }
};

// Boots the test disc through OpenBIOS and quits once the CPU runs inside the
// disc's boot executable, as the cdrom disc boot test does.
const char c_cdBoot[] = R"(
PCSX.HWEvents.arm('CDROM', 'IRQ')
local f = assert(io.open('src/mips/monitor/hosts/retail/monitor-retail.ps-exe', 'rb'))
local exe = f:read('*a')
f:close()
local function u32(o)
    local a, b, c, d = exe:byte(o + 1, o + 4)
    return a + b * 0x100 + c * 0x10000 + d * 0x1000000
end
local tAddr, tSize = u32(0x18), u32(0x1c)
local text = exe:sub(0x801, 0x800 + tSize)
local frames = 0
CDEventsListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    local booted = false
    local pc = PCSX.getRegisters().pc
    if pc >= tAddr and pc < tAddr + tSize then
        booted = ffi.string(PCSX.getMemPtr() + bit.band(tAddr, 0x1fffff), tSize) == text
    end
    if not booted and frames < 6000 then return end
    local f = io.open(CDEventsOut, 'w')
    local arr, n = PCSX.HWEvents.read()
    local commands = 0
    for i = 0, n - 1 do
        if arr[i].type == PCSX.HWEvents.Types.CDCommand then commands = commands + 1 end
    end
    f:write(string.format('%d %d', PCSX.HWEvents.mask(), commands))
    f:close()
    PCSX.quit(booted and 0 or 1)
end)
)";

}  // namespace

TEST(HWEvents, CDROMCommandsResponsesAndSectors) {
    CDObserverScope scope;
    auto luaOut = std::filesystem::temp_directory_path() / "pcsx-hwevents-cd.txt";
    std::filesystem::remove(luaOut);
    // MainInvoker probes each argument as a path, so the slash in the output path
    // has to come early.
    std::string lua = std::string("CDEventsOut = '") + luaOut.generic_string() + "'\n" + c_cdBoot;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-iso", "test.cue", "-exec", lua.c_str());
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    printf(
        "hwevents cd: %llu events, %llu commands (%llu Setloc, %llu checked), %llu responses (%llu raising the IRQ), "
        "%llu sectors, %llu data readies after sectors\n",
        (unsigned long long)s_cd.total, (unsigned long long)s_cd.commands, (unsigned long long)s_cd.setlocs,
        (unsigned long long)s_cd.setlocChecked, (unsigned long long)s_cd.responses,
        (unsigned long long)s_cd.irqRaisedResponses, (unsigned long long)s_cd.sectors,
        (unsigned long long)s_cd.dataReadyAfterSector);
    EXPECT_EQ(s_cd.stampMismatches, 0u);
    EXPECT_EQ(s_cd.backwards, 0u);
    EXPECT_GT(s_cd.commands, 0u);
    // Every command answers at least once.
    EXPECT_GE(s_cd.responses, s_cd.commands);
    EXPECT_GT(s_cd.setlocChecked, 0u);
    EXPECT_EQ(s_cd.setlocMismatches, 0u);
    // The loader reads the primary volume descriptor at LBA 16.
    EXPECT_TRUE(s_cd.sawPVD);
    EXPECT_GT(s_cd.dataReadyAfterSector, 0u);
    EXPECT_EQ(s_cd.missingDataReady, 0u);
    EXPECT_GT(s_cd.irqRaisedResponses, 0u);
    EXPECT_EQ(s_cd.irqWithoutAssert, 0u);

    std::ifstream in(luaOut);
    ASSERT_TRUE(in.good()) << "the Lua listener did not write " << luaOut;
    uint32_t mask = 0;
    uint64_t luaCommands = 0;
    in >> mask >> luaCommands;
    EXPECT_EQ(mask, PCSX::HWEvents::sourceBit(PCSX::HWEvents::Source::CDROM) |
                        PCSX::HWEvents::sourceBit(PCSX::HWEvents::Source::IRQ));
    EXPECT_EQ(luaCommands, s_cd.commands);
    in.close();
    std::filesystem::remove(luaOut);
}

namespace {

struct GPUSeen {
    uint64_t total = 0;
    uint64_t stampMismatches = 0;
    uint64_t commands[4] = {};
    uint64_t polygons = 0;
    uint64_t controls = 0;
    uint64_t badOrigin = 0;
    uint64_t portValueMismatches = 0;
    uint64_t wordNotInBlock = 0;
    uint64_t chainOutsideDMA = 0;
    bool dmaActive = false;
};

GPUSeen s_gpu;

// True when `word` is one of the `count` words starting at physical address `addr`.
bool wordInRAM(uint32_t word, uint32_t addr, uint32_t count) {
    auto* ram = PCSX::g_emulator->m_mem->m_wram;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = (addr + i * 4) & 0x1ffffc;
        uint32_t w;
        memcpy(&w, ram + a, 4);
        if (w == word) return true;
    }
    return false;
}

void observeGPU(const PCSX::HWEvent& e) {
    auto& regs = PCSX::g_emulator->m_cpu->m_regs;
    if (e.cycle != regs.cycle || e.pc != regs.pc) s_gpu.stampMismatches++;
    s_gpu.total++;
    switch (e.type) {
        case PCSX::HWEvents::DMAStart:
            if (e.payload[0] == 2) s_gpu.dmaActive = true;
            break;
        case PCSX::HWEvents::DMAComplete:
            if (e.payload[0] == 2) s_gpu.dmaActive = false;
            break;
        case PCSX::HWEvents::GPUControl:
            s_gpu.controls++;
            break;
        case PCSX::HWEvents::GPUCommand: {
            uint32_t word = e.payload[0], origin = e.payload[1], value = e.payload[2], length = e.payload[3];
            if ((word >> 29) == 1) s_gpu.polygons++;
            switch (origin) {
                case 0:  // port write: the value is the word itself
                    if (value != word || length != 1) s_gpu.portValueMismatches++;
                    break;
                case 2:  // block DMA from MADR
                    if (!wordInRAM(word, value, length)) s_gpu.wordNotInBlock++;
                    break;
                case 3:  // linked-list node, its words follow the header
                    if (!wordInRAM(word, value + 4, length)) s_gpu.wordNotInBlock++;
                    if (!s_gpu.dmaActive) s_gpu.chainOutsideDMA++;
                    break;
                default:
                    s_gpu.badOrigin++;
                    break;
            }
            if (origin < 4) s_gpu.commands[origin]++;
        } break;
        default:
            break;
    }
}

struct GPUObserverScope {
    GPUObserverScope() {
        s_gpu = GPUSeen{};
        PCSX::HWEvents::s_recordObserver = observeGPU;
    }
    ~GPUObserverScope() { PCSX::HWEvents::s_recordObserver = nullptr; }
};

}  // namespace

TEST(HWEvents, GPUCommandsCarryTheirWordAndOrigin) {
    GPUObserverScope scope;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        // MainInvoker probes each argument as a path, so a slash comes early.
                        "-exec", "-- /\nPCSX.HWEvents.arm('GPU', 'DMA')", "-loadexe",
                        "src/mips/tests/gpu-dma-dreq/gpu-dma-dreq.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    printf(
        "hwevents gpu: %llu events, GP0 %llu port / %llu block / %llu list, %llu polygons, %llu GP1 writes\n",
        (unsigned long long)s_gpu.total, (unsigned long long)s_gpu.commands[0], (unsigned long long)s_gpu.commands[2],
        (unsigned long long)s_gpu.commands[3], (unsigned long long)s_gpu.polygons,
        (unsigned long long)s_gpu.controls);
    EXPECT_EQ(s_gpu.stampMismatches, 0u);
    EXPECT_GT(s_gpu.controls, 0u);
    // gpu-dma-dreq feeds GP0 through the port, block DMA and linked-list DMA.
    EXPECT_GT(s_gpu.commands[0], 0u);
    EXPECT_GT(s_gpu.commands[2], 0u);
    EXPECT_GT(s_gpu.commands[3], 0u);
    EXPECT_GT(s_gpu.polygons, 0u);
    EXPECT_EQ(s_gpu.badOrigin, 0u);
    EXPECT_EQ(s_gpu.portValueMismatches, 0u);
    EXPECT_EQ(s_gpu.wordNotInBlock, 0u);
    EXPECT_EQ(s_gpu.chainOutsideDMA, 0u);
}

namespace {

struct SPUSeen {
    uint64_t total = 0;
    uint64_t stampMismatches = 0;
    uint64_t writes = 0;
    uint64_t outOfRange = 0;
    uint64_t keyOns = 0;
    uint64_t keyOffs = 0;
    uint64_t unpairedKeys = 0;
    uint64_t unkeyedKonWrites = 0;
    uint32_t keyedOn = 0;
    uint32_t keyedOff = 0;
    bool pendingKey = false;
    PCSX::HWEvent previous{};
};

SPUSeen s_spu;

void observeSPU(const PCSX::HWEvent& e) {
    auto& regs = PCSX::g_emulator->m_cpu->m_regs;
    if (e.cycle != regs.cycle || e.pc != regs.pc) s_spu.stampMismatches++;
    s_spu.total++;
    // A KON or KOFF write is followed by its key event at once.
    if (s_spu.pendingKey && e.type != PCSX::HWEvents::SPUKeyOn && e.type != PCSX::HWEvents::SPUKeyOff) {
        s_spu.unkeyedKonWrites++;
    }
    s_spu.pendingKey = false;
    switch (e.type) {
        case PCSX::HWEvents::SPURegWrite: {
            s_spu.writes++;
            uint32_t addr = e.payload[0];
            if (addr < 0x1f801c00 || addr >= 0x1f801e00) s_spu.outOfRange++;
            uint32_t reg = addr & ~3u;
            if ((reg == 0x1f801d88 || reg == 0x1f801d8c) && !(addr & 1)) s_spu.pendingKey = true;
        } break;
        case PCSX::HWEvents::SPUKeyOn:
        case PCSX::HWEvents::SPUKeyOff: {
            bool on = e.type == PCSX::HWEvents::SPUKeyOn;
            (on ? s_spu.keyOns : s_spu.keyOffs)++;
            (on ? s_spu.keyedOn : s_spu.keyedOff) |= e.payload[0];
            // The register write it came from, at the same instant, names the same voices.
            const auto& w = s_spu.previous;
            uint32_t base = on ? 0x1f801d88 : 0x1f801d8c;
            uint32_t shift = (w.payload[0] == base + 2) ? 16 : 0;
            if (w.type != PCSX::HWEvents::SPURegWrite || w.cycle != e.cycle || (w.payload[0] & ~3u) != base ||
                ((w.payload[1] << shift) & 0xffffff) != e.payload[0]) {
                s_spu.unpairedKeys++;
            }
        } break;
        default:
            break;
    }
    s_spu.previous = e;
}

struct SPUObserverScope {
    SPUObserverScope() {
        s_spu = SPUSeen{};
        PCSX::HWEvents::s_recordObserver = observeSPU;
    }
    ~SPUObserverScope() { PCSX::HWEvents::s_recordObserver = nullptr; }
};

}  // namespace

TEST(HWEvents, SPUWritesAndKeys) {
    SPUObserverScope scope;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        // MainInvoker probes each argument as a path, so a slash comes early.
                        "-exec", "-- /\nPCSX.HWEvents.arm('SPU')", "-loadexe", "src/mips/tests/spu/spu.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    printf("hwevents spu: %llu events, %llu register writes, %llu key ons (voices %06x), %llu key offs (voices %06x)\n",
           (unsigned long long)s_spu.total, (unsigned long long)s_spu.writes, (unsigned long long)s_spu.keyOns,
           s_spu.keyedOn, (unsigned long long)s_spu.keyOffs, s_spu.keyedOff);
    EXPECT_EQ(s_spu.stampMismatches, 0u);
    EXPECT_GT(s_spu.writes, 0u);
    EXPECT_EQ(s_spu.outOfRange, 0u);
    EXPECT_GT(s_spu.keyOns, 0u);
    EXPECT_GT(s_spu.keyOffs, 0u);
    // spu.ps-exe keys voices on both halves of KON, and there are only 24 voices.
    EXPECT_NE(s_spu.keyedOn >> 16, 0u);
    EXPECT_EQ(s_spu.keyedOn & ~0xffffffu, 0u);
    EXPECT_EQ(s_spu.keyedOff & ~0xffffffu, 0u);
    EXPECT_EQ(s_spu.unpairedKeys, 0u);
    EXPECT_EQ(s_spu.unkeyedKonWrites, 0u);
}

// Saves the store from Lua, loads it back, and compares every byte; then checks
// that a file with a bad magic and a truncated file are both refused and leave
// the store empty.
TEST(HWEvents, SaveAndLoadRoundTrip) {
    auto dir = std::filesystem::temp_directory_path();
    auto saved = dir / "pcsx-hwevents-save.bin";
    auto bad = dir / "pcsx-hwevents-bad.bin";
    auto cut = dir / "pcsx-hwevents-cut.bin";
    auto luaOut = dir / "pcsx-hwevents-save.txt";
    for (auto& p : {saved, bad, cut, luaOut}) std::filesystem::remove(p);
    // MainInvoker probes each argument as a path, so a slash comes early.
    std::string lua = "-- /\nlocal saved, bad, cut, out = '" + saved.generic_string() + "', '" + bad.generic_string() +
                      "', '" + cut.generic_string() + "', '" + luaOut.generic_string() + R"('
PCSX.HWEvents.arm('IRQ', 'DMA')
HWEventsSaveQuit = PCSX.Events.createEventListener('Quitting', function()
    local before, n = PCSX.HWEvents.read()
    local bytes = ffi.string(before, n * ffi.sizeof('PCSX_HWEvent'))
    local wrote = PCSX.HWEvents.save(saved)
    PCSX.HWEvents.clear()
    local loaded = PCSX.HWEvents.load(saved)
    local after, m = PCSX.HWEvents.read()
    local same = m == n and ffi.string(after, m * ffi.sizeof('PCSX_HWEvent')) == bytes
    local f = io.open(saved, 'rb')
    local file = f:read('*a')
    f:close()
    f = io.open(bad, 'wb')
    f:write('PCSXTRC' .. file:sub(8))
    f:close()
    f = io.open(cut, 'wb')
    f:write(file:sub(1, #file - 16))
    f:close()
    local badRefused = not PCSX.HWEvents.load(bad) and PCSX.HWEvents.size() == 0
    PCSX.HWEvents.load(saved)
    local cutRefused = not PCSX.HWEvents.load(cut) and PCSX.HWEvents.size() == 0
    f = io.open(out, 'w')
    f:write(string.format('%d %d %s %s %s %s %s', n, #file, tostring(wrote), tostring(loaded), tostring(same),
        tostring(badRefused), tostring(cutRefused)))
    f:close()
end)
)";
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-exec", lua.c_str(), "-loadexe", "src/mips/tests/dma/dma.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    std::ifstream in(luaOut);
    ASSERT_TRUE(in.good()) << "the Lua Quitting listener did not write " << luaOut;
    uint64_t count = 0, fileSize = 0;
    std::string wrote, loaded, same, badRefused, cutRefused;
    in >> count >> fileSize >> wrote >> loaded >> same >> badRefused >> cutRefused;
    printf("hwevents save: %llu events, %llu bytes\n", (unsigned long long)count, (unsigned long long)fileSize);
    EXPECT_GT(count, 0u);
    // 24-byte header, then the raw records.
    EXPECT_EQ(fileSize, 24 + count * sizeof(PCSX::HWEvent));
    EXPECT_EQ(wrote, "true");
    EXPECT_EQ(loaded, "true");
    EXPECT_EQ(same, "true");
    EXPECT_EQ(badRefused, "true");
    EXPECT_EQ(cutRefused, "true");
    in.close();
    for (auto& p : {saved, bad, cut, luaOut}) std::filesystem::remove(p);
}

TEST(HWEvents, Describe) {
    auto make = [](uint16_t type, uint32_t p0, uint32_t p1 = 0, uint32_t p2 = 0, uint32_t p3 = 0) {
        PCSX::HWEvent e{};
        e.type = type;
        e.payload[0] = p0;
        e.payload[1] = p1;
        e.payload[2] = p2;
        e.payload[3] = p3;
        return PCSX::HWEvents::describe(e);
    };
    EXPECT_EQ(make(PCSX::HWEvents::IRQAssert, 4, 1, 0xd), "IRQ assert 004, I_STAT 001, I_MASK 00d");
    EXPECT_EQ(make(PCSX::HWEvents::DMAStart, 2, 0x80010000, 0x10, 0x01000401),
              "DMA2 start, MADR 80010000, BCR 00000010, CHCR 01000401");
    EXPECT_EQ(make(PCSX::HWEvents::DMAComplete, 6, 0x84000000), "DMA6 complete, DICR 84000000");
    // Setloc 00:02:16
    EXPECT_EQ(make(PCSX::HWEvents::CDCommand, 0x0302, 0x160200), "CD command 02, params 00 02 16");
    EXPECT_EQ(make(PCSX::HWEvents::CDResponse, 0x30103, 0x02), "CD response INT3, 02, IRQ");
    EXPECT_EQ(make(PCSX::HWEvents::CDResponse, 0x00101, 0x22), "CD response INT1, 22, queued");
    EXPECT_EQ(make(PCSX::HWEvents::CDSector, 166, 2048, 0x8002, 0x00080101),
              "CD sector 00:02:16, 2048 bytes to host, mode 2, Setmode 80, subheader 00080101");
    EXPECT_EQ(make(PCSX::HWEvents::GPUCommand, 0x20ff0000, 0, 0x20ff0000, 1), "GP0 20ff0000, port");
    EXPECT_EQ(make(PCSX::HWEvents::GPUCommand, 0x28000000, 3, 0x000f1000, 5),
              "GP0 28000000, list DMA node 000f1000, 5 words");
    EXPECT_EQ(make(PCSX::HWEvents::GPUControl, 0x03000000), "GP1 03000000");
    EXPECT_EQ(make(PCSX::HWEvents::SPURegWrite, 0x1f801d88, 0x00ff, 16), "SPU write 1f801d88 = 00ff, 16-bit");
    EXPECT_EQ(make(PCSX::HWEvents::SPUKeyOn, 0xff0000), "SPU key on, voices ff0000");
    EXPECT_EQ(make(0x7f01, 1, 2, 3, 4), "type 7f01, 00000001 00000002 00000003 00000004");
    EXPECT_STREQ(PCSX::HWEvents::sourceName(3), "CDROM");
    EXPECT_EQ(PCSX::HWEvents::sourceName(0), nullptr);
}
