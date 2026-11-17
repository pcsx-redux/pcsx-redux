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

// The sound RAM transfer address of an SPU just opened is inside sound RAM, so the data port and a
// DMA used before the guest sets the address move sound RAM from its start. A state loads and saves
// back the same bytes, and a voice plays the same after one is loaded. The address is read with
// getTransferAddr(), the SPU's own: what 1F801DA6h reads back is not under test here.

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

class SPUTransferAddress : public ::testing::Test {
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
        m_spu = open();
        ASSERT_NE(m_spu, nullptr);
        m_ram = reinterpret_cast<uint16_t*>(m_spu->getSpuRam());
    }
    void TearDown() override {
        if (m_spu) m_spu->close();
        m_spu.reset();
        m_emulator.reset();
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }

    // The first four halfwords of sound RAM, and four other values.
    static constexpr uint16_t c_old[4] = {0x1111, 0x2222, 0x3333, 0x4444};
    static constexpr uint16_t c_new[4] = {0xaaaa, 0xbbbb, 0xcccc, 0xdddd};
    // Frames a voice plays after its key on, and the CPU cycles in one 44.1 kHz frame.
    static constexpr size_t c_playFrames = 2048;
    static constexpr uint64_t c_cyclesPerSample = PCSX::Emulator::m_psxClockSpeed / 44100;

    static std::unique_ptr<PCSX::SPU::impl> open() {
        auto spu = std::make_unique<PCSX::SPU::impl>();
        spu->init();
        if (!spu->openSynchronous()) return nullptr;
        return spu;
    }

    static std::string serialise(PCSX::SPU::impl& spu) {
        auto state = std::make_unique<PCSX::SaveStates::SPU>();
        spu.save(*state);
        PCSX::Protobuf::OutSlice slice;
        state->serialize(&slice);
        return slice.finalize();
    }

    static void deserialise(PCSX::SPU::impl& spu, const std::string& saved) {
        auto state = std::make_unique<PCSX::SaveStates::SPU>();
        PCSX::Protobuf::InSlice slice(reinterpret_cast<const uint8_t*>(saved.data()), saved.size());
        state->deserialize(&slice, 0);
        spu.load(*state);
    }

    // Mixes 64 frames, so the register writes queued for the mixer are applied (save() waits for them
    // only while a mixer thread runs), then saves the SPU, loads that state back and saves again: the
    // two must be the same bytes.
    ::testing::AssertionResult roundTrips() {
        int16_t out[64 * 2];
        m_spu->mixSynchronous(out, 64);
        // Saved at frame 63, the last one mixed, where mixSynchronous() leaves the CPU side.
        m_emulator->m_cpu->m_regs.cycle = 63 * c_cyclesPerSample;
        const std::string saved = serialise(*m_spu);
        deserialise(*m_spu, saved);
        const std::string again = serialise(*m_spu);
        if (again.size() != saved.size()) {
            return ::testing::AssertionFailure() << "sizes " << saved.size() << " then " << again.size();
        }
        size_t diffs = 0, first = 0;
        for (size_t i = 0; i < saved.size(); i++) {
            if (saved[i] == again[i]) continue;
            if (!diffs) first = i;
            diffs++;
        }
        if (!diffs) return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << diffs << " bytes differ, first at " << first;
    }

    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    std::unique_ptr<PCSX::SPU::impl> m_spu;
    uint16_t* m_ram = nullptr;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

}  // namespace

TEST_F(SPUTransferAddress, StartsAtZero) { EXPECT_EQ(m_spu->getTransferAddr(), 0u); }

// Each of the next four checks the address before it moves sound RAM, so an address outside it
// fails the test instead of indexing past spuMem.
TEST_F(SPUTransferAddress, DataPortReadsFromTheStart) {
    ASSERT_EQ(m_spu->getTransferAddr(), 0u);
    memcpy(m_ram, c_old, sizeof(c_old));
    for (uint16_t v : c_old) EXPECT_EQ(m_spu->readRegister(H_SPUdata), v);
    // Four halfwords on.
    EXPECT_EQ(m_spu->getTransferAddr(), 8u);
}

TEST_F(SPUTransferAddress, DataPortWritesFromTheStart) {
    ASSERT_EQ(m_spu->getTransferAddr(), 0u);
    memcpy(m_ram, c_old, sizeof(c_old));
    for (uint16_t v : c_new) m_spu->writeRegister(H_SPUdata, v);
    EXPECT_EQ(memcmp(m_ram, c_new, sizeof(c_new)), 0);
    EXPECT_EQ(m_spu->getTransferAddr(), 8u);
}

TEST_F(SPUTransferAddress, DMAReadsFromTheStart) {
    ASSERT_EQ(m_spu->getTransferAddr(), 0u);
    memcpy(m_ram, c_old, sizeof(c_old));
    uint16_t out[4] = {};
    m_spu->readDMAMem(out, 4);
    EXPECT_EQ(memcmp(out, c_old, sizeof(c_old)), 0);
    EXPECT_EQ(m_spu->getTransferAddr(), 8u);
}

TEST_F(SPUTransferAddress, DMAWritesFromTheStart) {
    ASSERT_EQ(m_spu->getTransferAddr(), 0u);
    memcpy(m_ram, c_old, sizeof(c_old));
    uint16_t in[4];
    memcpy(in, c_new, sizeof(c_new));
    m_spu->writeDMAMem(in, 4);
    EXPECT_EQ(memcmp(m_ram, c_new, sizeof(c_new)), 0);
    EXPECT_EQ(m_spu->getTransferAddr(), 8u);
}

// An older build's SPU, its address never set, saved 0xffffffff. That loads as an SPU opened now
// starts, at 0.
TEST_F(SPUTransferAddress, AnOldStateLoadsAtZero) {
    auto state = std::make_unique<PCSX::SaveStates::SPU>();
    m_spu->save(*state);
    state->get<PCSX::SaveStates::SPUAddr>().value = 0xffffffff;
    m_spu->load(*state);
    ASSERT_EQ(m_spu->getTransferAddr(), 0u);
    memcpy(m_ram, c_old, sizeof(c_old));
    EXPECT_EQ(m_spu->readRegister(H_SPUdata), c_old[0]);
}

// A saved address inside sound RAM loads as it was saved, up to the last one the register can set.
TEST_F(SPUTransferAddress, AStateKeepsItsAddress) {
    for (uint16_t addr : {uint16_t(0x1000 >> 3), uint16_t(0xffff)}) {
        SCOPED_TRACE(addr);
        m_spu->writeRegister(H_SPUaddr, addr);
        auto state = std::make_unique<PCSX::SaveStates::SPU>();
        m_spu->save(*state);
        m_spu->writeRegister(H_SPUaddr, 0);
        m_spu->load(*state);
        EXPECT_EQ(m_spu->getTransferAddr(), uint32_t(addr) * 8);
    }
}

// The data port moves the address on past the last one the register can set, up to 7FFFEh, and a
// state saved there loads as saved too.
TEST_F(SPUTransferAddress, AStateKeepsADataPortAddress) {
    m_spu->writeRegister(H_SPUaddr, 0xffff);
    for (int i = 0; i < 3; i++) m_spu->readRegister(H_SPUdata);
    ASSERT_EQ(m_spu->getTransferAddr(), 0x7fffeu);
    auto state = std::make_unique<PCSX::SaveStates::SPU>();
    m_spu->save(*state);
    m_spu->writeRegister(H_SPUaddr, 0);
    m_spu->load(*state);
    EXPECT_EQ(m_spu->getTransferAddr(), 0x7fffeu);
}

// A state saved by an SPU just opened loads and saves back the same bytes.
TEST_F(SPUTransferAddress, AFreshStateRoundTrips) { EXPECT_TRUE(roundTrips()); }

// So does one saved after the guest set the address, its voices' ADSR still never written.
TEST_F(SPUTransferAddress, AStateOfUnwrittenVoicesRoundTrips) {
    m_spu->writeRegister(H_SPUaddr, 0x1000 >> 3);
    EXPECT_TRUE(roundTrips());
}

// And one whose voices' ADSR the guest wrote.
TEST_F(SPUTransferAddress, AStateOfWrittenVoicesRoundTrips) {
    for (uint32_t ch = 0; ch < 24; ch++) {
        m_spu->writeRegister(0x0c08 + ch * 16, 0x80ff);
        m_spu->writeRegister(0x0c0a + ch * 16, 0x5fc0);
    }
    m_spu->writeRegister(H_SPUaddr, 0x1000 >> 3);
    EXPECT_TRUE(roundTrips());
}

// And one whose voices' ADSR the guest wrote, then wrote 0.
TEST_F(SPUTransferAddress, AStateOfVoicesWrittenZeroRoundTrips) {
    for (uint32_t ch = 0; ch < 24; ch++) {
        m_spu->writeRegister(0x0c08 + ch * 16, 0x80ff);
        m_spu->writeRegister(0x0c0a + ch * 16, 0x5fc0);
        m_spu->writeRegister(0x0c08 + ch * 16, 0);
        m_spu->writeRegister(0x0c0a + ch * 16, 0);
    }
    m_spu->writeRegister(H_SPUaddr, 0x1000 >> 3);
    EXPECT_TRUE(roundTrips());
}

// A voice whose ADSR the guest never wrote, keyed on after a state saved before is loaded into a
// fresh SPU, plays what it plays without the load.
TEST_F(SPUTransferAddress, AnUnwrittenVoicePlaysTheSameAfterALoad) {
    m_spu->close();
    m_spu.reset();
    auto play = [this](bool load) {
        m_emulator->m_cpu->m_regs.cycle = 0;
        auto spu = open();
        EXPECT_NE(spu, nullptr);
        if (!spu) return std::vector<int16_t>();
        spu->writeRegister(H_SPUctrl, 0xc000);  // on, unmuted
        spu->writeRegister(H_SPUmvolL, 0x3fff);
        spu->writeRegister(H_SPUmvolR, 0x3fff);
        // One square wave period in two blocks: the first latches the loop address (flag 4), the second
        // repeats (flags 3, the only end flags decodeNextBlock() repeats on).
        uint8_t blocks[32] = {0x00, 0x04};
        blocks[17] = 0x03;
        for (int b = 0; b < 32; b += 16) {
            for (int i = 2; i < 9; i++) blocks[b + i] = 0x77;
            for (int i = 9; i < 16; i++) blocks[b + i] = 0x99;
        }
        uint16_t words[16];
        memcpy(words, blocks, sizeof(blocks));
        spu->writeRegister(H_SPUaddr, 0x1000 >> 3);
        spu->writeDMAMem(words, 16);
        spu->writeRegister(0x0c00, 0x2000);  // voice 0 volume left
        spu->writeRegister(0x0c02, 0x2000);  // voice 0 volume right
        spu->writeRegister(0x0c04, 0x1000);  // pitch 1.0
        spu->writeRegister(0x0c06, 0x1000 >> 3);
        std::vector<int16_t> out(c_playFrames * 2);
        spu->mixSynchronous(out.data(), 64);
        if (load) {
            // Saved at frame 63, the last one mixed (where mixSynchronous() leaves the CPU side), so the
            // load starts the mixer at frame 64, where the SPU that saved it is.
            m_emulator->m_cpu->m_regs.cycle = 63 * c_cyclesPerSample;
            const std::string saved = serialise(*spu);
            spu->close();
            spu = open();
            EXPECT_NE(spu, nullptr);
            if (!spu) return std::vector<int16_t>();
            deserialise(*spu, saved);
        }
        m_emulator->m_cpu->m_regs.cycle = 64 * c_cyclesPerSample;
        spu->writeRegister(H_SPUon1, 0x0001);
        for (size_t done = 0; done < c_playFrames; done += 64) spu->mixSynchronous(out.data() + done * 2, 64);
        spu->close();
        m_emulator->m_cpu->m_regs.cycle = 0;
        return out;
    };
    const auto expected = play(false);
    ASSERT_EQ(expected.size(), c_playFrames * 2);
    EXPECT_TRUE(std::any_of(expected.begin(), expected.end(), [](int16_t s) { return s != 0; }));
    const auto loaded = play(true);
    ASSERT_EQ(loaded.size(), c_playFrames * 2);
    size_t diffs = 0;
    for (size_t i = 0; i < expected.size(); i++) diffs += expected[i] != loaded[i];
    EXPECT_EQ(diffs, 0u);
}
