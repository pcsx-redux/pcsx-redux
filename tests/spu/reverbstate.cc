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

// The reverb unit's resampling FIR history belongs to one SPU. Two SPUs fed the same writes
// and mixed in alternation have to come out the same as one mixed on its own.

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/sstate.h"
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

class SPUReverbState : public ::testing::Test {
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

    static constexpr size_t c_frames = 8820;
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

    // Voice 0 plays one pass of the square wave into the reverb (frames 107 to 141); its return
    // comes back from frame 1973 on.
    void setUp(PCSX::SPU::impl& spu, bool reverb) {
        m_emulator->m_cpu->m_regs.cycle = 0;
        spu.writeRegister(H_SPUctrl, 0xc080);
        spu.writeRegister(H_SPUmvolL, 0x3fff);
        spu.writeRegister(H_SPUmvolR, 0x3fff);
        for (unsigned i = 0; i < 32; i++) spu.writeRegister(H_Reverb + i * 2, c_room[i]);
        spu.writeRegister(H_SPUReverbAddr, c_roomBase);
        spu.writeRegister(H_SPUrvolL, 0x3fff);
        spu.writeRegister(H_SPUrvolR, 0x3fff);
        loadVoice(spu);
        spu.writeRegister(0x0c00, 0x3fff);  // voice 0 volume left
        spu.writeRegister(0x0c02, 0x1fff);  // voice 0 volume right
        spu.writeRegister(0x0c04, 0x0c00);  // pitch 0.75
        spu.writeRegister(0x0c06, 0x1000 >> 3);
        spu.writeRegister(0x0c08, 0x000f);  // fastest attack, sustain level max
        spu.writeRegister(0x0c0a, 0x0000);
        spu.writeRegister(H_RVBon1, reverb ? 0x0001 : 0x0000);
        m_emulator->m_cpu->m_regs.cycle = (c_keyOnSample - 1) * c_cyclesPerSample;
        spu.writeRegister(H_SPUon1, 0x0001);
        m_emulator->m_cpu->m_regs.cycle = 0;
    }

    std::unique_ptr<PCSX::SPU::impl> open(bool reverb = true) {
        auto spu = std::make_unique<PCSX::SPU::impl>();
        spu->init();
        EXPECT_TRUE(spu->openSynchronous());
        setUp(*spu, reverb);
        return spu;
    }

    std::vector<int16_t> render(bool reverb = true) {
        auto spu = open(reverb);
        std::vector<int16_t> out(c_frames * 2);
        for (size_t done = 0; done < c_frames; done += c_chunk) {
            spu->mixSynchronous(out.data() + done * 2, std::min(c_chunk, c_frames - done));
        }
        spu->close();
        return out;
    }

    // How many samples differ, and where the first one is, so a failure does not print 17640
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

// Guards the premise of the test below: the stream does go through the reverb.
TEST_F(SPUReverbState, ReverbIsAudible) { EXPECT_FALSE(same(render(true), render(false))); }

TEST_F(SPUReverbState, TwoInstancesInterleavedMatchOne) {
    const auto expected = render();

    auto a = open();
    auto b = open();
    std::vector<int16_t> outA(c_frames * 2), outB(c_frames * 2);
    for (size_t done = 0; done < c_frames; done += c_chunk) {
        const size_t n = std::min(c_chunk, c_frames - done);
        a->mixSynchronous(outA.data() + done * 2, n);
        b->mixSynchronous(outB.data() + done * 2, n);
    }
    a->close();
    b->close();

    EXPECT_TRUE(same(outA, expected));
    EXPECT_TRUE(same(outB, expected));
}

// Saved mid-reverb, serialised and loaded into a fresh SPU, the rest of the render has to match a
// run that was never interrupted. Saved once while the voice feeds the reverb and once in the
// tail; key on, the last queued write, is applied by then.
TEST_F(SPUReverbState, SavestateRoundTripMatchesUninterrupted) {
    const auto expected = render();

    for (size_t saveFrame : {130, 3000}) {
        SCOPED_TRACE(saveFrame);
        std::vector<int16_t> out(c_frames * 2);
        std::string serialised;
        {
            auto spu = open();
            for (size_t done = 0; done < saveFrame; done += c_chunk) {
                spu->mixSynchronous(out.data() + done * 2, std::min(c_chunk, saveFrame - done));
            }
            m_emulator->m_cpu->m_regs.cycle = (saveFrame - 1) * c_cyclesPerSample;
            auto state = std::make_unique<PCSX::SaveStates::SPU>();
            spu->save(*state);
            PCSX::Protobuf::OutSlice slice;
            state->serialize(&slice);
            serialised = slice.finalize();
            spu->close();
        }

        auto spu = std::make_unique<PCSX::SPU::impl>();
        spu->init();
        ASSERT_TRUE(spu->openSynchronous());
        {
            auto state = std::make_unique<PCSX::SaveStates::SPU>();
            PCSX::Protobuf::InSlice slice(reinterpret_cast<const uint8_t*>(serialised.data()), serialised.size());
            state->deserialize(&slice, 0);
            spu->load(*state);
        }
        for (size_t done = saveFrame; done < c_frames; done += c_chunk) {
            spu->mixSynchronous(out.data() + done * 2, std::min(c_chunk, c_frames - done));
        }
        spu->close();

        EXPECT_TRUE(same(out, expected));
    }
}
