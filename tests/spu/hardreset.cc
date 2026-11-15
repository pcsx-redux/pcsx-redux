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

// A hard reset is a power cycle for the SPU: after one, an SPU that has been playing reads and
// plays like one just opened, and its sound RAM holds the value main RAM is filled with (#2530).

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "gtest/gtest.h"
#include "spu/interface.h"
#include "spu/registers.h"

namespace {

class TestSystem final : public PCSX::System {
  public:
    void softReset() override {}
    void hardReset() override {}
    void biosPutc(int) override {}
    const PCSX::Arguments& getArgs() const override { throw std::logic_error("unused"); }
    void printf(std::string&&) override {}
    void log(PCSX::LogClass, std::string&&) override {}
    void message(std::string&&) override {}
    void luaMessage(const std::string&, bool) override {}
    void update(bool) override {}
    void close() override {}
    void purgeAllEvents() override {}
    void testQuit(int) override {}
};

class SPUHardReset : public ::testing::Test {
  protected:
    void SetUp() override {
        m_oldSystem = PCSX::g_system;
        m_oldEmulator = PCSX::g_emulator;
        PCSX::g_system = &m_system;
        m_emulator = std::make_unique<PCSX::Emulator>();
        PCSX::g_emulator = m_emulator.get();
        m_emulator->m_cpu = PCSX::Cpus::Interpreted();
        ASSERT_NE(m_emulator->m_cpu, nullptr);
        m_emulator->m_cpu->m_regs.cycle = 0;
    }
    void TearDown() override {
        m_emulator.reset();
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }

    static constexpr size_t c_frames = 4410;
    static constexpr size_t c_chunk = 64;
    static constexpr size_t c_keyOnSample = 101;
    static constexpr uint64_t c_cyclesPerSample = PCSX::Emulator::m_psxClockSpeed / 44100;

    // psx-spx "Room" preset, registers 1F801DC0h..1F801DFEh, and its work area at the top of RAM.
    static constexpr uint16_t c_room[32] = {
        0x007d, 0x005b, 0x6d80, 0x54b8, 0xbed0, 0x0000, 0x0000, 0xba80, 0x5800, 0x5300, 0x04d6,
        0x0333, 0x03f0, 0x0227, 0x0374, 0x01ef, 0x0334, 0x01b5, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x01b4, 0x0136, 0x00b8, 0x005c, 0x8000, 0x8000,
    };
    static constexpr uint16_t c_roomBase = (0x80000 - 0x26c0) >> 3;

    // Voice 0 loops a square wave at 1000h into the reverb. Voice 1 plays it with its ADSR never
    // written, so on the envelope a fresh SPU starts with, and voice 2 plays noise at the fastest noise
    // clock. All three are keyed on at sample 101 and sound to the end. The volumes keep the mix below
    // full scale, so a difference in any voice reaches the output instead of being clipped away.
    void setUp(PCSX::SPU::impl& spu) {
        m_emulator->m_cpu->m_regs.cycle = 0;
        spu.writeRegister(H_SPUctrl, 0xff80);  // on, unmuted, the fastest noise clock, reverb on
        spu.writeRegister(H_SPUmvolL, 0x3fff);
        spu.writeRegister(H_SPUmvolR, 0x3fff);
        for (unsigned i = 0; i < 32; i++) spu.writeRegister(H_Reverb + i * 2, c_room[i]);
        spu.writeRegister(H_SPUReverbAddr, c_roomBase);
        spu.writeRegister(H_SPUrvolL, 0x1000);
        spu.writeRegister(H_SPUrvolR, 0x1000);
        // Two blocks of one square wave period each. The first latches the loop address (flag 4); the
        // second ends and repeats (flags 3, the only end flags decodeNextBlock() repeats on).
        uint8_t blocks[32] = {0x00, 0x04};
        blocks[17] = 0x03;
        for (int b = 0; b < 32; b += 16) {
            for (int i = 2; i < 9; i++) blocks[b + i] = 0x77;
            for (int i = 9; i < 16; i++) blocks[b + i] = 0x99;
        }
        std::vector<uint16_t> words(16);
        memcpy(words.data(), blocks, sizeof(blocks));
        spu.writeRegister(H_SPUaddr, 0x1000 >> 3);
        spu.writeDMAMem(words.data(), static_cast<int>(words.size()));
        spu.writeRegister(0x0c00, 0x1000);  // voice 0 volume left
        spu.writeRegister(0x0c02, 0x0800);  // voice 0 volume right
        spu.writeRegister(0x0c04, 0x0c00);  // pitch 0.75
        spu.writeRegister(0x0c06, 0x1000 >> 3);
        spu.writeRegister(0x0c08, 0x000f);  // fastest attack, sustain level max
        spu.writeRegister(0x0c0a, 0x0000);
        spu.writeRegister(0x0c10, 0x0800);  // voice 1 volume left
        spu.writeRegister(0x0c12, 0x0800);  // voice 1 volume right
        spu.writeRegister(0x0c14, 0x1000);  // pitch 1.0
        spu.writeRegister(0x0c16, 0x1000 >> 3);
        spu.writeRegister(0x0c20, 0x0800);  // voice 2 volume left
        spu.writeRegister(0x0c22, 0x0800);  // voice 2 volume right
        spu.writeRegister(0x0c24, 0x1000);  // pitch 1.0
        spu.writeRegister(0x0c26, 0x1000 >> 3);
        spu.writeRegister(0x0c28, 0x000f);
        spu.writeRegister(0x0c2a, 0x0000);
        spu.writeRegister(H_Noise1, 0x0004);
        spu.writeRegister(H_RVBon1, 0x0001);
        m_emulator->m_cpu->m_regs.cycle = (c_keyOnSample - 1) * c_cyclesPerSample;
        spu.writeRegister(H_SPUon1, 0x0007);
        m_emulator->m_cpu->m_regs.cycle = 0;
    }

    std::unique_ptr<PCSX::SPU::impl> open() {
        auto spu = std::make_unique<PCSX::SPU::impl>();
        spu->init();
        EXPECT_TRUE(spu->openSynchronous());
        return spu;
    }

    static std::vector<int16_t> mix(PCSX::SPU::impl& spu, size_t frames) {
        std::vector<int16_t> out(frames * 2);
        for (size_t done = 0; done < frames; done += c_chunk) {
            spu.mixSynchronous(out.data() + done * 2, std::min(c_chunk, frames - done));
        }
        return out;
    }

    // An SPU that has played for a while, its voices still sounding and the reverb work area
    // written, then hard reset at the restarted CPU clock, as Emulator::reset() does.
    std::unique_ptr<PCSX::SPU::impl> playedThenReset() {
        auto spu = open();
        setUp(*spu);
        mix(*spu, c_frames);
        m_emulator->m_cpu->m_regs.cycle = 0;
        spu->reset();
        return spu;
    }

    // Every register from 1F801C00h to 1F801E7Eh, as the CPU reads it, except the data port (a read
    // of it moves the transfer address).
    static std::vector<uint16_t> registers(PCSX::SPU::impl& spu) {
        std::vector<uint16_t> regs;
        for (uint32_t reg = 0x1f801c00; reg < 0x1f801e80; reg += 2) {
            if (reg == 0x1f801da8) continue;
            regs.push_back(spu.readRegister(reg));
        }
        return regs;
    }

    // How many samples differ, and where the first one is, so a failure does not print 8820
    // samples.
    static ::testing::AssertionResult same(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
        if (a.size() != b.size()) return ::testing::AssertionFailure() << "sizes " << a.size() << " vs " << b.size();
        size_t diffs = 0, first = 0;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i] == b[i]) continue;
            if (!diffs) first = i;
            diffs++;
        }
        if (!diffs) return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure()
               << diffs << " of " << a.size() << " samples differ, first at frame " << first / 2
               << (first & 1 ? " right" : " left") << ": " << a[first] << " vs " << b[first];
    }

    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

}  // namespace

TEST_F(SPUHardReset, SoundRamTakesTheFillValue) {
    for (uint8_t fill : {0x00, 0xa5}) {
        SCOPED_TRACE(::testing::Message() << "fill " << int(fill));
        m_emulator->settings.get<PCSX::Emulator::SettingMemoryFillValue>().value = fill;
        auto spu = playedThenReset();
        const uint8_t* ram = spu->getSpuRam();
        size_t wrong = 0;
        for (size_t i = 0; i < PCSX::SPUInterface::c_spuRamSize; i++) wrong += ram[i] != fill;
        EXPECT_EQ(wrong, 0u);
        spu->close();
    }
}

TEST_F(SPUHardReset, ReadsAsJustOpened) {
    auto fresh = open();
    const auto expected = registers(*fresh);
    fresh->close();

    auto spu = playedThenReset();
    EXPECT_EQ(registers(*spu), expected);
    spu->close();
}

// The reset at cycle 0 restarts the mixer clock at the first sample after it, sample 1, as a state
// load does, where an SPU just opened starts at sample 0. Every write lands on the same sample in
// both, so the reset SPU plays what the one just opened plays from sample 1 on.
TEST_F(SPUHardReset, PlaysAsJustOpened) {
    auto fresh = open();
    setUp(*fresh);
    auto expected = mix(*fresh, c_frames + 1);
    expected.erase(expected.begin(), expected.begin() + 2);
    fresh->close();
    int peak = 0;
    for (int16_t s : expected) peak = std::max(peak, std::abs(int(s)));
    EXPECT_LT(peak, 32767);
    // The voices still sound in the last frames, not only a reverb tail, so the whole comparison is of
    // a playing SPU.
    EXPECT_TRUE(std::any_of(expected.end() - 128, expected.end(), [](int16_t s) { return std::abs(int(s)) > 4096; }));

    auto spu = playedThenReset();
    setUp(*spu);
    EXPECT_TRUE(same(mix(*spu, c_frames), expected));
    spu->close();
}
