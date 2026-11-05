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

// Envelope traces against silicon (SCPH-1001 and SCPH-5501, ENVX polled every ~10us):
// key-on with a linear attack at rate 0 (+0x3800 per sample, peak in three samples),
// decay rate D, sustain level SL, a frozen sustain rate (0x7f) so whatever level the
// decay leaves is held. The decay compares the level against SL only on samples
// where a step lands, so a slow decay always takes its first step. Steps land on
// samples where the SPU-wide sample count is a multiple of the rate's period, so
// two voices on one rate step together whenever they were keyed on.

#include <stdint.h>

#include <vector>

#include "gtest/gtest.h"
#include "spu/adsr.h"

namespace {

using PCSX::SPU::AdsrEnvelope;

constexpr int c_peakSample = 2;  // 0x3800, 0x7000, 0x7fff
constexpr int c_holdSamples = 2000;

void program(AdsrEnvelope &env, int sustainLevel, int decayRate, int attackRate = 0) {
    auto &ex = env.ex();
    ex.get<PCSX::SPU::exAttackModeExp>().value = 0;
    ex.get<PCSX::SPU::exAttackRate>().value = attackRate;
    ex.get<PCSX::SPU::exDecayRate>().value = decayRate;
    ex.get<PCSX::SPU::exSustainLevel>().value = sustainLevel;
    ex.get<PCSX::SPU::exSustainModeExp>().value = 0;
    ex.get<PCSX::SPU::exSustainIncrease>().value = 0;
    ex.get<PCSX::SPU::exSustainRate>().value = 0x7f;
    ex.get<PCSX::SPU::exReleaseModeExp>().value = 0;
    ex.get<PCSX::SPU::exReleaseRate>().value = 0;
    env.keyOn();
}

// One ENVX value per sample from key-on, the first one being SPU-wide sample `start`.
std::vector<int> trace(int sustainLevel, int decayRate, uint64_t start = 0) {
    AdsrEnvelope env;
    program(env, sustainLevel, decayRate);
    std::vector<int> out;
    bool channelOn = true;
    for (int i = 0; i <= c_peakSample + c_holdSamples; i++) out.push_back(env.step(false, channelOn, start + i));
    return out;
}

// Run-length view: the distinct successive values.
std::vector<int> changes(const std::vector<int> &t) {
    std::vector<int> out;
    for (int v : t) {
        if (out.empty() || out.back() != v) out.push_back(v);
    }
    return out;
}

int lastChange(const std::vector<int> &t) {
    int last = 0;
    for (int i = 1; i < static_cast<int>(t.size()); i++) {
        if (t[i] != t[i - 1]) last = i;
    }
    return last;
}

}  // namespace

TEST(SPUAdsr, FastDecayToSustain15) {
    const auto t = trace(15, 0);
    EXPECT_EQ(changes(t), (std::vector<int>{0x3800, 0x7000, 0x7fff, 0x3fff}));
    EXPECT_EQ(lastChange(t), c_peakSample + 1);
}

TEST(SPUAdsr, FastDecayToSustain14) {
    const auto t = trace(14, 0);
    EXPECT_EQ(changes(t), (std::vector<int>{0x3800, 0x7000, 0x7fff, 0x3fff}));
    EXPECT_EQ(lastChange(t), c_peakSample + 1);
}

TEST(SPUAdsr, SlowestDecayStepsOnceAtSustain15) {
    // Decay rate 15 steps every 16 samples on a grid shared by all voices; silicon holds
    // 0x7fff until the next grid sample, takes one step and stops at 0x7ff7. Whatever the
    // key-on sample, the step lands on the same sample modulo 16.
    int64_t gridPhase = -1;
    for (uint64_t start = 0; start < 16; start++) {
        const auto t = trace(15, 15, start);
        EXPECT_EQ(changes(t), (std::vector<int>{0x3800, 0x7000, 0x7fff, 0x7ff7})) << "start " << start;
        const int step = lastChange(t);
        EXPECT_GT(step, c_peakSample) << "start " << start;
        EXPECT_LE(step, c_peakSample + 16) << "start " << start;
        const int64_t phase = static_cast<int64_t>((start + step) % 16);
        if (gridPhase < 0) gridPhase = phase;
        EXPECT_EQ(phase, gridPhase) << "start " << start;
    }
}

TEST(SPUAdsr, VoicesShareTheStepGrid) {
    // Linear attack at shift 16 steps every 32 samples. Silicon (voices 0 and 23, the
    // second keyed on 2 to 21 samples after the first) steps both voices on the same samples.
    constexpr int kAttackShift16 = 16 << 2;
    constexpr uint64_t kPeriod = 32;
    for (uint64_t offset : {2u, 5u, 11u, 21u}) {
        AdsrEnvelope a, b;
        program(a, 15, 0, kAttackShift16);
        program(b, 15, 0, kAttackShift16);
        bool onA = true, onB = true;
        int lastA = 0, lastB = 0, stepsB = 0;
        int64_t gridPhase = -1;
        for (uint64_t s = 0; s < 64 * kPeriod; s++) {
            const int va = a.step(false, onA, s);
            bool stepA = va != lastA;
            lastA = va;
            if (stepA) {
                const int64_t phase = static_cast<int64_t>(s % kPeriod);
                if (gridPhase < 0) gridPhase = phase;
                EXPECT_EQ(phase, gridPhase) << "offset " << offset;
            }
            if (s < offset) continue;
            const int vb = b.step(false, onB, s);
            const bool stepB = vb != lastB;
            lastB = vb;
            EXPECT_EQ(stepA, stepB) << "offset " << offset << " sample " << s;
            if (stepB) stepsB++;
        }
        // 64 * 32 samples hold 64 grid steps and voice B is keyed on inside the first period, so it
        // takes every step after its key-on: 63 or 64 depending on where the first one falls.
        EXPECT_GE(stepsB, 63) << "offset " << offset;
    }
}

TEST(SPUAdsr, MidDecaySettlesAtSustain14) {
    // Silicon ENVX sequence, one step per sample, settling ~0.75ms after the peak.
    const std::vector<int> silicon = {0x3800, 0x7000, 0x7fff, 0x7fbf, 0x7f7f, 0x7f3f, 0x7eff, 0x7ebf, 0x7e7f,
                                      0x7e3f, 0x7dff, 0x7dc0, 0x7d81, 0x7d42, 0x7d03, 0x7cc4, 0x7c85, 0x7c46,
                                      0x7c07, 0x7bc8, 0x7b8a, 0x7b4c, 0x7b0e, 0x7ad0, 0x7a92, 0x7a54, 0x7a16,
                                      0x79d8, 0x799b, 0x795e, 0x7921, 0x78e4, 0x78a7, 0x786a, 0x782d, 0x77f0};
    const auto t = trace(14, 8);
    EXPECT_EQ(changes(t), silicon);
    EXPECT_EQ(lastChange(t), c_peakSample + 33);
}

TEST(SPUAdsr, MidDecayStepsOnceAtSustain15) {
    const auto t = trace(15, 8);
    EXPECT_EQ(changes(t), (std::vector<int>{0x3800, 0x7000, 0x7fff, 0x7fbf}));
    EXPECT_EQ(lastChange(t), c_peakSample + 1);
}
