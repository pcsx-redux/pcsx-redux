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

// The XA decoder's resampler and the cycle the CD last fed audio at are in the SPU's state, so a
// state loads into a fresh SPU as into the one that saved it, and a hard reset clears them.

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/decode_xa.h"
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

class SPUXAState : public ::testing::Test {
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

    static constexpr uint64_t c_cyclesPerSample = PCSX::Emulator::m_psxClockSpeed / 44100;
    // Frames in each sector. Not a multiple of 6 or of 32, so two sectors in, where the state is
    // saved, the resampler is part way to its next output and its ring part way round.
    static constexpr int c_sectorFrames = 1001;
    static constexpr int c_savedSectors = 2;
    static constexpr int c_sectors = 6;
    // Frames mixed with no sector arriving, before the save and after the load: the capture buffer
    // runs dry while the CD is still feeding.
    static constexpr size_t c_dryFrames = 300;
    // Frames mixed after the last sector, past the CD input queue's 2,048-frame cushion.
    static constexpr size_t c_tailFrames = 4096;

    // After the load: sound RAM's CD capture halves, left then right, after every chunk mixed, and
    // the mix. The capture is fed from the resampler's outputs as they are made. The mix plays them
    // through the CD input queue, which is not in the state: a stream loaded mid-way starts the
    // next sector afresh, a cushion later than the stream would have played it.
    struct Played {
        std::vector<uint16_t> capture;
        std::vector<int16_t> mix;
    };

    // Where the rest of the stream plays: on in the SPU that played the first sectors, never saved
    // or loaded; loaded back into that SPU; or loaded into a fresh one.
    enum class Load { None, Same, Fresh };

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

    static std::unique_ptr<PCSX::SaveStates::SPU> parse(const std::string& saved) {
        auto state = std::make_unique<PCSX::SaveStates::SPU>();
        PCSX::Protobuf::InSlice slice(reinterpret_cast<const uint8_t*>(saved.data()), saved.size());
        state->deserialize(&slice, 0);
        return state;
    }

    // The state as a build from before the XA resampler and the CD feed cycle were saved wrote it.
    static void dropXA(PCSX::SaveStates::SPU& state) {
        auto& ringL = state.get<PCSX::SaveStates::SPUXARingL>();
        auto& ringR = state.get<PCSX::SaveStates::SPUXARingR>();
        for (unsigned i = 0; i < 32; i++) ringL.value[i].value = ringR.value[i].value = 0;
        ringL.count = ringR.count = 0;
        state.get<PCSX::SaveStates::SPUXARingPos>().value = 0;
        state.get<PCSX::SaveStates::SPUXAInputsTaken>().value = 0;
        state.get<PCSX::SaveStates::SPUXALastL>().value = 0;
        state.get<PCSX::SaveStates::SPUXALastR>().value = 0;
        state.get<PCSX::SaveStates::SPUCDFeedCycle>().value = 0;
    }

    // Output frames one sector gives: 7 for every 6 inputs, and an 18.9 kHz stream takes each of
    // its samples twice.
    static size_t sectorOutput(int freq) { return size_t(c_sectorFrames) * (freq == 18900 ? 2 : 1) * 7 / 6; }

    // Sector `index` of a stereo stream: a sawtooth on the left, a slower one the other way on the
    // right, continuing from the sector before.
    static void fillSector(xa_decode_t& xa, int freq, int index) {
        xa.freq = freq;
        xa.nbits = 4;
        xa.stereo = 1;
        xa.nsamples = c_sectorFrames;
        for (int i = 0; i < c_sectorFrames; i++) {
            const int n = index * c_sectorFrames + i;
            xa.pcm[i * 2] = int16_t(((n * 731) & 0xffff) - 0x8000) / 2;
            xa.pcm[i * 2 + 1] = int16_t(0x7fff - ((n * 293) & 0xffff)) / 2;
        }
    }

    void mix(PCSX::SPU::impl& spu, size_t frames, uint64_t& frame, Played* keep) {
        std::vector<int16_t> out(64 * 2);
        const auto* ram = reinterpret_cast<const uint16_t*>(spu.getSpuRam());
        for (size_t done = 0; done < frames; done += 64) {
            const size_t n = std::min<size_t>(64, frames - done);
            spu.mixSynchronous(out.data(), n);
            if (!keep) continue;
            keep->mix.insert(keep->mix.end(), out.begin(), out.begin() + n * 2);
            keep->capture.insert(keep->capture.end(), ram, ram + 0x400);
        }
        frame += frames;
    }

    void feed(PCSX::SPU::impl& spu, xa_decode_t& xa, int freq, int index, uint64_t frame) {
        m_emulator->m_cpu->m_regs.cycle = frame * c_cyclesPerSample;
        fillSector(xa, freq, index);
        spu.playADPCMchannel(&xa);
    }

    // Plays c_savedSectors sectors of a stream into an SPU and saves it. Hands the state to
    // `change`, then loads it as `load` says, and plays the rest of the stream.
    Played play(int freq, Load load, void (*change)(PCSX::SaveStates::SPU&) = nullptr) {
        m_emulator->m_cpu->m_regs.cycle = 0;
        auto spu = open();
        EXPECT_NE(spu, nullptr);
        if (!spu) return {};
        auto xa = std::make_unique<xa_decode_t>();
        spu->writeRegister(H_SPUctrl, 0xc001);  // on, unmuted, CD audio in
        spu->writeRegister(H_SPUmvolL, 0x3fff);
        spu->writeRegister(H_SPUmvolR, 0x3fff);
        uint64_t frame = 0;
        mix(*spu, 64, frame, nullptr);
        for (int s = 0; s < c_savedSectors; s++) {
            feed(*spu, *xa, freq, s, frame);
            mix(*spu, sectorOutput(freq), frame, nullptr);
        }
        mix(*spu, c_dryFrames, frame, nullptr);
        // Saved on the cycle the last frame mixed is produced at, so the load starts the mixer at
        // the next.
        const uint64_t saveCycle = (frame - 1) * c_cyclesPerSample;
        m_emulator->m_cpu->m_regs.cycle = saveCycle;
        std::unique_ptr<PCSX::SaveStates::SPU> state;
        if (load != Load::None) {
            state = parse(serialise(*spu));
            if (change) change(*state);
        }
        if (load == Load::Fresh) {
            spu->close();
            spu = open();
            EXPECT_NE(spu, nullptr);
            if (!spu) return {};
            m_emulator->m_cpu->m_regs.cycle = saveCycle;
        }
        if (load != Load::None) spu->load(*state);

        Played played;
        mix(*spu, c_dryFrames, frame, &played);
        for (int s = c_savedSectors; s < c_sectors; s++) {
            feed(*spu, *xa, freq, s, frame);
            mix(*spu, sectorOutput(freq), frame, &played);
        }
        mix(*spu, c_tailFrames, frame, &played);
        spu->close();
        m_emulator->m_cpu->m_regs.cycle = 0;
        return played;
    }

    static size_t differ(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
        if (a.size() != b.size()) return std::max(a.size(), b.size());
        size_t n = 0;
        for (size_t i = 0; i < a.size(); i++) n += a[i] != b[i];
        return n;
    }
    static size_t differ(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
        if (a.size() != b.size()) return std::max(a.size(), b.size());
        size_t n = 0;
        for (size_t i = 0; i < a.size(); i++) n += a[i] != b[i];
        return n;
    }

    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

}  // namespace

// A 37.8 kHz stream and an 18.9 kHz one, which averages each sample with the one before, each
// loaded mid-stream. The SPU that saved the state and a fresh one both capture what the stream
// does with no save and load at all, and the fresh one plays what the one that saved it plays.
TEST_F(SPUXAState, AFreshSPULoadsAStreamAsTheOneThatSavedIt) {
    for (int freq : {37800, 18900}) {
        SCOPED_TRACE(freq);
        const auto played = play(freq, Load::None);
        ASSERT_FALSE(played.mix.empty());
        EXPECT_TRUE(std::any_of(played.mix.begin(), played.mix.end(), [](int16_t s) { return s != 0; }));
        EXPECT_TRUE(std::any_of(played.capture.begin(), played.capture.end(), [](uint16_t s) { return s != 0; }));
        const auto kept = play(freq, Load::Same);
        const auto fresh = play(freq, Load::Fresh);
        EXPECT_EQ(differ(kept.capture, played.capture), 0u);
        EXPECT_EQ(differ(fresh.capture, played.capture), 0u);
        EXPECT_EQ(differ(fresh.mix, kept.mix), 0u);
    }
}

// A state from a build that did not save the resampler loads it as an SPU just opened has it, into
// an SPU that has played a stream as into a fresh one.
TEST_F(SPUXAState, AnOlderStateLoadsTheResamplerAsJustOpened) {
    for (int freq : {37800, 18900}) {
        SCOPED_TRACE(freq);
        const auto kept = play(freq, Load::Same, dropXA);
        const auto fresh = play(freq, Load::Fresh, dropXA);
        EXPECT_EQ(differ(fresh.mix, kept.mix), 0u);
        EXPECT_EQ(differ(fresh.capture, kept.capture), 0u);
    }
}

// The phase is saved as the inputs taken since the last burst of outputs, 0 to 5. A larger value,
// which no build writes, loads as 5.
TEST_F(SPUXAState, AnInputsTakenPastFiveLoadsAsFive) {
    auto spu = open();
    ASSERT_NE(spu, nullptr);
    for (uint32_t taken : {5u, 6u, 0x80000000u, 0xffffffffu}) {
        SCOPED_TRACE(taken);
        auto state = parse(serialise(*spu));
        state->get<PCSX::SaveStates::SPUXAInputsTaken>().value = taken;
        spu->load(*state);
        EXPECT_EQ(parse(serialise(*spu))->get<PCSX::SaveStates::SPUXAInputsTaken>().value, 5u);
    }
    spu->close();
}

// A hard reset after a stream leaves the resampler and the CD feed cycle as an SPU just opened
// has them.
TEST_F(SPUXAState, AHardResetClearsTheResampler) {
    const auto opened = [&] {
        auto spu = open();
        EXPECT_NE(spu, nullptr);
        auto state = spu ? parse(serialise(*spu)) : nullptr;
        if (spu) spu->close();
        return state;
    }();
    ASSERT_NE(opened, nullptr);

    for (int freq : {37800, 18900}) {
        SCOPED_TRACE(freq);
        m_emulator->m_cpu->m_regs.cycle = 0;
        auto spu = open();
        ASSERT_NE(spu, nullptr);
        auto xa = std::make_unique<xa_decode_t>();
        spu->writeRegister(H_SPUctrl, 0xc001);
        uint64_t frame = 0;
        for (int s = 0; s < c_savedSectors; s++) {
            feed(*spu, *xa, freq, s, frame);
            mix(*spu, sectorOutput(freq), frame, nullptr);
        }
        auto before = parse(serialise(*spu));
        EXPECT_NE(before->get<PCSX::SaveStates::SPUCDFeedCycle>().value, 0u);
        EXPECT_NE(before->get<PCSX::SaveStates::SPUXARingPos>().value, 0u);

        m_emulator->m_cpu->m_regs.cycle = 0;
        spu->reset();
        auto after = parse(serialise(*spu));
        for (unsigned i = 0; i < 32; i++) {
            EXPECT_EQ(after->get<PCSX::SaveStates::SPUXARingL>().value[i].value,
                      opened->get<PCSX::SaveStates::SPUXARingL>().value[i].value);
            EXPECT_EQ(after->get<PCSX::SaveStates::SPUXARingR>().value[i].value,
                      opened->get<PCSX::SaveStates::SPUXARingR>().value[i].value);
        }
        EXPECT_EQ(after->get<PCSX::SaveStates::SPUXARingPos>().value,
                  opened->get<PCSX::SaveStates::SPUXARingPos>().value);
        EXPECT_EQ(after->get<PCSX::SaveStates::SPUXAInputsTaken>().value,
                  opened->get<PCSX::SaveStates::SPUXAInputsTaken>().value);
        EXPECT_EQ(after->get<PCSX::SaveStates::SPUXALastL>().value, opened->get<PCSX::SaveStates::SPUXALastL>().value);
        EXPECT_EQ(after->get<PCSX::SaveStates::SPUXALastR>().value, opened->get<PCSX::SaveStates::SPUXALastR>().value);
        EXPECT_EQ(after->get<PCSX::SaveStates::SPUCDFeedCycle>().value, 0u);
        spu->close();
    }
}
