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

// The SPU Samples window credits a sample that repeats with no loop-start flag with the loop
// address the voice holds at its key-ons. These drive a real SPU, mixed synchronously, with the
// SPU logger on, and check which address the window credits.

#include <stdint.h>
#include <string.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/spulogger.h"
#include "core/system.h"
#include "gtest/gtest.h"
#include "gui/widgets/spusamples.h"
#include "spu/interface.h"

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

// Sample A at 1000h loops to its own first block, through a loop-start flag there. Sample B at
// 2000h ends on repeat with no loop-start flag, so it loops wherever the voice's loop address is.
constexpr uint32_t c_a = 0x1000;
constexpr uint32_t c_b = 0x2000;
constexpr int c_blocks = 4;

class SPUSamplesLoops : public ::testing::Test {
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
        m_spu = std::make_unique<PCSX::SPU::impl>();
        m_spu->init();
        ASSERT_TRUE(m_spu->openSynchronous());
        uint8_t* ram = m_spu->getSpuRam();
        put(ram, c_a, 0x04, 0x10);
        put(ram, c_b, 0, 0x30);
        m_logger = m_emulator->m_spuLogger.get();
        m_logger->clear();
        m_logger->setEnabled(true);
        write(0x0c04, 0x1000);  // voice 0 pitch, 44100 Hz
        write(0x0c08, 0x00ff);  // fastest attack, so the voice is audible at once
        write(0x0c0a, 0x0000);
    }
    void TearDown() override {
        if (m_spu) m_spu->close();
        m_spu.reset();
        m_emulator.reset();
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }

    static void put(uint8_t* ram, uint32_t start, uint8_t firstFlags, uint8_t seed) {
        for (int i = 0; i < c_blocks; i++) {
            uint8_t* b = ram + start + i * 16;
            b[0] = 0;
            b[1] = i == c_blocks - 1 ? 0x03 : 0;
            if (i == 0) b[1] |= firstFlags;
            memset(b + 2, seed + i, 14);
        }
    }

    void write(uint32_t reg, uint16_t value) { m_spu->writeRegister(0x1f801000 | reg, value); }

    // Mixes `frames` samples and moves the CPU clock to the last one mixed.
    void mix(size_t frames) {
        std::vector<int16_t> out(frames * 2);
        m_spu->mixSynchronous(out.data(), frames);
        m_frames += frames;
        m_emulator->m_cpu->m_regs.cycle = (m_frames - 1) * c_cyclesPerSample;
    }

    // Keys voice 0 on at `start`, and lets it play long enough to read its first blocks.
    void keyOn(uint32_t start) {
        write(0x0c06, uint16_t(start >> 3));
        mix(8);
        write(0x0d88, 1);
        mix(256);
    }

    const PCSX::Widgets::SPUSamples::Sample& rebuild() {
        m_widget.rebuild(m_logger, m_spu->getSpuRam());
        auto& samples = m_widget.samples();
        auto it = samples.find(c_b);
        if (it == samples.end()) throw std::runtime_error("sample B was not seen keyed on");
        return it->second;
    }

    static constexpr uint64_t c_cyclesPerSample = PCSX::Emulator::m_psxClockSpeed / 44100;

    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    std::unique_ptr<PCSX::SPU::impl> m_spu;
    PCSX::SPULogger* m_logger = nullptr;
    size_t m_frames = 0;
    bool m_show = true;
    std::vector<std::string> m_favorites;
    PCSX::Widgets::SPUSamples m_widget{m_show, m_favorites};
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

}  // namespace

// Control: the loop register written inside B before its key-on is B's loop.
TEST_F(SPUSamplesLoops, AWrittenLoopInsideTheSampleIsItsLoop) {
    write(0x0c0e, uint16_t((c_b + 0x10) >> 3));
    mix(8);
    keyOn(c_b);
    const auto& s = rebuild();
    EXPECT_TRUE(s.gameLoop);
    EXPECT_EQ(s.loop, c_b + 0x10);
}

// A's loop-start flag moves the voice's loop address to A after the CPU wrote one inside B. B,
// keyed next on the same voice, loops to A, outside itself.
TEST_F(SPUSamplesLoops, ALoopLatchedByTheLastSampleIsNotTheWrittenOne) {
    write(0x0c0e, uint16_t((c_b + 0x10) >> 3));
    mix(8);
    keyOn(c_a);
    keyOn(c_b);
    const auto& s = rebuild();
    EXPECT_FALSE(s.gameLoop) << std::hex << "credited with " << s.loop;
}

// The loop register is moved outside B while the logger is paused. B, keyed after the pause,
// loops there.
TEST_F(SPUSamplesLoops, ALoopWrittenWhileTheLoggerIsPausedCounts) {
    write(0x0c0e, uint16_t((c_b + 0x10) >> 3));
    mix(8);
    m_logger->setEnabled(false);
    write(0x0c0e, uint16_t(0x3000 >> 3));
    mix(8);
    m_logger->setEnabled(true);
    keyOn(c_b);
    const auto& s = rebuild();
    EXPECT_FALSE(s.gameLoop) << std::hex << "credited with " << s.loop;
}
