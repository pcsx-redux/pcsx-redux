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

// The embedding path: openSynchronous() + mixSynchronous() run the mixer on the caller's
// thread. A voice playing a looping ADPCM block has to come out the same every time, the
// same as the threaded mixer makes it, and an SPU with nothing keyed on has to stay silent.

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
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

// The SPU reads the CPU cycle and the SPU logger through g_emulator, and its audio sink
// subscribes to g_system's event bus.
class SPUSyncMix : public ::testing::Test {
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
    // With no audio device draining it, the threaded mixer blocks once the device ring (2048
    // frames) is full, so the comparison stays under that.
    static constexpr size_t c_threadedFrames = 1500;
    // The key on is stamped ahead of the samples mixed so far, at a sample that falls inside a
    // mixer chunk, so the mixer has to split the chunk there to apply it on time.
    static constexpr size_t c_keyOnSample = 101;
    static constexpr uint64_t c_cyclesPerSample = PCSX::Emulator::m_psxClockSpeed / 44100;

    // A single block that loops on itself: 14 samples at +7 << 12, then 14 at -7 << 12.
    static void loadVoice(PCSX::SPU::impl& spu) {
        uint8_t block[16] = {0x00, 0x07};
        for (int i = 2; i < 9; i++) block[i] = 0x77;
        for (int i = 9; i < 16; i++) block[i] = 0x99;
        std::vector<uint16_t> words(8);
        memcpy(words.data(), block, sizeof(block));
        spu.writeRegister(H_SPUaddr, 0x1000 >> 3);
        spu.writeDMAMem(words.data(), static_cast<int>(words.size()));
    }

    void setUp(PCSX::SPU::impl& spu, bool keyOn) {
        spu.writeRegister(H_SPUctrl, 0xc000);
        spu.writeRegister(H_SPUmvolL, 0x3fff);
        spu.writeRegister(H_SPUmvolR, 0x3fff);
        loadVoice(spu);
        spu.writeRegister(0x0c00, 0x3fff);  // voice 0 volume left
        spu.writeRegister(0x0c02, 0x3fff);  // voice 0 volume right
        spu.writeRegister(0x0c04, 0x1000);  // pitch 1.0
        spu.writeRegister(0x0c06, 0x1000 >> 3);
        spu.writeRegister(0x0c08, 0x000f);  // fastest attack, sustain level max
        spu.writeRegister(0x0c0a, 0x0000);
        if (!keyOn) return;
        m_emulator->m_cpu->m_regs.cycle = (c_keyOnSample - 1) * c_cyclesPerSample;
        spu.writeRegister(H_SPUon1, 0x0001);
    }

    std::vector<int16_t> mixSynchronous(bool keyOn, size_t frames = c_frames) {
        m_emulator->m_cpu->m_regs.cycle = 0;
        auto spu = std::make_unique<PCSX::SPU::impl>();
        spu->init();
        EXPECT_TRUE(spu->openSynchronous());
        setUp(*spu, keyOn);
        std::vector<int16_t> out(frames * 2, 0x5555);
        // In uneven pieces, to cross chunk boundaries at odd places.
        size_t done = 0;
        for (size_t n : {1, 44, 46, 1000}) {
            spu->mixSynchronous(out.data() + done * 2, n);
            done += n;
        }
        spu->mixSynchronous(out.data() + done * 2, frames - done);
        spu->close();
        return out;
    }

    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

bool silent(const std::vector<int16_t>& v) {
    for (auto s : v) {
        if (s != 0) return false;
    }
    return true;
}

}  // namespace

TEST_F(SPUSyncMix, VoiceIsAudibleAndDeterministic) {
    auto first = mixSynchronous(true);
    auto second = mixSynchronous(true);
    EXPECT_FALSE(silent(first));
    EXPECT_EQ(first, second);
    // Nothing before the key on's own sample.
    EXPECT_TRUE(silent(std::vector<int16_t>(first.begin(), first.begin() + c_keyOnSample * 2)));
    // Both halves of the square wave come out, on both channels.
    int16_t lo = 0, hi = 0;
    for (size_t i = 0; i < first.size(); i++) {
        lo = std::min(lo, first[i]);
        hi = std::max(hi, first[i]);
    }
    EXPECT_LT(lo, -1000);
    EXPECT_GT(hi, 1000);
}

TEST_F(SPUSyncMix, NothingKeyedOnStaysSilent) { EXPECT_TRUE(silent(mixSynchronous(false))); }

// The same writes through open(), the mixer thread and its WAV dump.
TEST_F(SPUSyncMix, MatchesThreadedMixer) {
    auto expected = mixSynchronous(true, c_threadedFrames);
    ASSERT_FALSE(silent(expected));

    auto path = std::filesystem::temp_directory_path() / "pcsx-redux-spu-syncmix-test.wav";
    m_emulator->m_cpu->m_regs.cycle = 0;
    auto spu = std::make_unique<PCSX::SPU::impl>();
    spu->init();
    ASSERT_TRUE(spu->open());
    ASSERT_TRUE(spu->setWavDump(path));
    setUp(*spu, true);
    // Let the mixer run to the last sample, and wait on it there (an ENDX read catches up).
    const uint64_t cycle = (c_threadedFrames - 1) * c_cyclesPerSample;
    m_emulator->m_cpu->m_regs.cycle = cycle;
    spu->advanceTo(cycle);
    spu->readRegister(0x0d9c);
    const auto bytes = 44 + c_threadedFrames * 4;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::filesystem::file_size(path) < bytes && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    spu->close();

    std::vector<int16_t> threaded(c_threadedFrames * 2);
    {
        std::ifstream in(path, std::ios::binary);
        in.seekg(44);
        in.read(reinterpret_cast<char*>(threaded.data()), threaded.size() * sizeof(int16_t));
        ASSERT_EQ(in.gcount(), static_cast<std::streamsize>(threaded.size() * sizeof(int16_t)));
    }
    std::filesystem::remove(path);
    EXPECT_EQ(expected, threaded);
}
