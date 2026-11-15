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

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/psxemulator.h"
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
    uint64_t completedBeforeStart = 0;
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
            if ((s_seen.startedChannels & (1u << e.payload[0])) == 0) s_seen.completedBeforeStart++;
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
    auto luaOut = std::filesystem::temp_directory_path() /
                  ("pcsx-hwevents-lua-" + std::to_string(std::random_device{}()) + ".txt");
    std::filesystem::remove(luaOut);
    // Arms from Lua before the exe runs, and reads the whole store back through
    // PCSX.HWEvents.read when the emulator quits.
    std::string lua =
        "PCSX.HWEvents.arm('IRQ', 'DMA') "
        "hwEventsQuit = PCSX.Events.createEventListener('Quitting', function() "
        "local arr, n = PCSX.HWEvents.read() "
        "local f = io.open([==[" +
        luaOut.generic_string() +
        "]==], 'w') "
        "local sized = PCSX.HWEvents.size() == n and not PCSX.HWEvents.full() "
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
                        "-exec", lua.c_str(), "-loadexe", "src/mips/tests/dma/dma/dma.ps-exe");
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
    // No channel completed before it had started.
    EXPECT_EQ(s_seen.completedBeforeStart, 0u);

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
                        "-loadexe", "src/mips/tests/dma/dma/dma.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(s_seen.total, 0u);
}
