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

// A state load keeps each voice's envelope as the state saved it, without decoding the ADSR
// registers again. A sustain level saved on the old envelope scale loads as the maximum, and a
// value outside the range the registers' decode gives loads inside it.

#include <stdint.h>
#include <string.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/sstate.h"
#include "core/system.h"
#include "gtest/gtest.h"
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

class SPUEnvelopeLoad : public ::testing::Test {
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
    }
    void TearDown() override {
        if (m_spu) m_spu->close();
        m_spu.reset();
        m_emulator.reset();
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }

    static auto& envelope(PCSX::SaveStates::SPU& state, unsigned voice) {
        return state.get<PCSX::SaveStates::Channels>().value[voice].get<PCSX::SaveStates::ADSRInfoEx>();
    }

    // Saves the SPU, puts `value` in one field of voice 0's saved envelope, loads that and saves
    // again: the value the second state holds.
    template <typename Field>
    int32_t loaded(int32_t value) {
        auto state = std::make_unique<PCSX::SaveStates::SPU>();
        m_spu->save(*state);
        envelope(*state, 0).get<Field>().value = value;
        m_spu->load(*state);
        auto again = std::make_unique<PCSX::SaveStates::SPU>();
        m_spu->save(*again);
        return envelope(*again, 0).get<Field>().value;
    }
    int32_t loadedSustainLevel(int32_t level) { return loaded<PCSX::SPU::exSustainLevel>(level); }

    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    std::unique_ptr<PCSX::SPU::impl> m_spu;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

}  // namespace

// Before f4d80dcb an SPU just opened set each voice's sustain level to 0xf << 27, the old
// envelope scale. Such a state loads it as 15, the level an SPU opened now has.
TEST_F(SPUEnvelopeLoad, AnOldScaleSustainLevelLoadsAsTheMaximum) { EXPECT_EQ(loadedSustainLevel(0xf << 27), 15); }

// Every level the register can hold loads as saved.
TEST_F(SPUEnvelopeLoad, ARegisterSustainLevelLoadsAsSaved) {
    for (int32_t level = 0; level <= 15; level++) {
        SCOPED_TRACE(level);
        EXPECT_EQ(loadedSustainLevel(level), level);
    }
}

// The first levels past each end of the register's range load at that end.
TEST_F(SPUEnvelopeLoad, ASustainLevelOutsideTheRegisterLoadsInsideIt) {
    EXPECT_EQ(loadedSustainLevel(16), 15);
    EXPECT_EQ(loadedSustainLevel(-1), 0);
}

// adsr.cc indexes its envelope tables with the rates. Each rate the registers can give loads as
// saved at both ends of its range, and one past its range, or negative, loads inside it.
TEST_F(SPUEnvelopeLoad, ARateOutsideTheRegisterLoadsInsideIt) {
    EXPECT_EQ(loaded<PCSX::SPU::exAttackRate>(0x7f), 0x7f);
    EXPECT_EQ(loaded<PCSX::SPU::exDecayRate>(0xf), 0xf);
    EXPECT_EQ(loaded<PCSX::SPU::exSustainRate>(0x7f), 0x7f);
    EXPECT_EQ(loaded<PCSX::SPU::exReleaseRate>(0x1f), 0x1f);
    EXPECT_LE(loaded<PCSX::SPU::exAttackRate>(0x80), 0x7f);
    EXPECT_LE(loaded<PCSX::SPU::exDecayRate>(0x10), 0xf);
    EXPECT_LE(loaded<PCSX::SPU::exSustainRate>(0x80), 0x7f);
    EXPECT_LE(loaded<PCSX::SPU::exReleaseRate>(0x20), 0x1f);
    for (int32_t rate : {loaded<PCSX::SPU::exAttackRate>(-1), loaded<PCSX::SPU::exDecayRate>(-1),
                         loaded<PCSX::SPU::exSustainRate>(-1), loaded<PCSX::SPU::exReleaseRate>(-1)}) {
        EXPECT_GE(rate, 0);
    }
}

// The rest of a voice's envelope settings load as saved too, even ones its saved ADSR registers do
// not decode to: nothing replays the registers over them.
TEST_F(SPUEnvelopeLoad, AnEnvelopeLoadsAsSavedNotAsItsRegistersDecode) {
    auto state = std::make_unique<PCSX::SaveStates::SPU>();
    m_spu->save(*state);
    // Voice 3's two ADSR registers, 1F801C38h and 1F801C3Ah: halfwords 28 and 29 of the saved ports.
    uint8_t* ports = state->get<PCSX::SaveStates::SPUPorts>().value;
    ASSERT_NE(ports, nullptr);
    const uint16_t regs[2] = {0x80ff, 0x5fc0};
    memcpy(ports + 28 * 2, regs, sizeof(regs));
    auto& saved = envelope(*state, 3);
    // 80FFh decodes to an exponential attack, 5FC0h to a falling sustain: the saved fields say
    // otherwise.
    saved.get<PCSX::SPU::exAttackModeExp>().value = 0;
    saved.get<PCSX::SPU::exAttackRate>().value = 0x55;
    saved.get<PCSX::SPU::exDecayRate>().value = 0x9;
    saved.get<PCSX::SPU::exSustainIncrease>().value = 1;
    saved.get<PCSX::SPU::exSustainRate>().value = 0x3c;
    saved.get<PCSX::SPU::exReleaseRate>().value = 0x11;
    m_spu->load(*state);
    auto again = std::make_unique<PCSX::SaveStates::SPU>();
    m_spu->save(*again);
    const auto& kept = envelope(*again, 3);
    EXPECT_EQ(kept.get<PCSX::SPU::exAttackModeExp>().value, 0);
    EXPECT_EQ(kept.get<PCSX::SPU::exAttackRate>().value, 0x55);
    EXPECT_EQ(kept.get<PCSX::SPU::exDecayRate>().value, 0x9);
    EXPECT_EQ(kept.get<PCSX::SPU::exSustainIncrease>().value, 1);
    EXPECT_EQ(kept.get<PCSX::SPU::exSustainRate>().value, 0x3c);
    EXPECT_EQ(kept.get<PCSX::SPU::exReleaseRate>().value, 0x11);
}
