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

#include "supportpsx/rootkey.h"

#include <stdint.h>

#include <cmath>
#include <vector>

#include "gtest/gtest.h"

using namespace PCSX::RootKey;
using PCSX::Fundamental::c_maxInput;
using PCSX::Fundamental::Estimate;

namespace {

// The pitch register value that plays a sample sounding `note` at 1000h on `target`.
uint16_t pitchFor(double note, double target) {
    return uint16_t(std::lround(0x1000 * std::exp2((target - note) / 12.0)));
}

}  // namespace

TEST(RootKey, NoteNumbering) {
    EXPECT_DOUBLE_EQ(noteOf(440.0), 69.0);
    EXPECT_NEAR(noteOf(261.6256), 60.0, 1e-4);
    EXPECT_NEAR(noteOf(27.5), 21.0, 1e-9);
}

TEST(RootKey, FromFundamental) {
    auto r = fromFundamental({true, 440.0});
    EXPECT_EQ(r.status, Status::Unconfirmed);
    EXPECT_EQ(r.key, 69);
    EXPECT_EQ(r.correction, 0);

    // 263.8 Hz sits 14 cents above C4: the correction lowers it back onto 60.
    r = fromFundamental({true, 263.8});
    EXPECT_EQ(r.key, 60);
    EXPECT_EQ(r.correction, -14);
    EXPECT_NEAR(r.note, 60.1435, 1e-3);

    // 30 cents below A4 rounds to A4 and is corrected upwards.
    r = fromFundamental({true, 440.0 * std::exp2(-0.3 / 12.0)});
    EXPECT_EQ(r.key, 69);
    EXPECT_EQ(r.correction, 30);
}

TEST(RootKey, UnpitchedStaysOnTheNativeFrame) {
    for (Estimate e : {Estimate{false, 0.0}, Estimate{true, 0.0}, Estimate{false, 440.0}}) {
        auto r = combine(e, {0x1000, 0x2000});
        EXPECT_EQ(r.status, Status::Unpitched);
        EXPECT_EQ(r.key, c_unpitched);
        EXPECT_EQ(r.correction, 0);
        EXPECT_EQ(r.note, 60.0);
        EXPECT_EQ(r.keyOns, 0u);
    }
    // Outside the MIDI range.
    EXPECT_EQ(fromFundamental({true, 5.0}).status, Status::Unpitched);
    EXPECT_EQ(fromFundamental({true, 20000.0}).status, Status::Unpitched);
}

TEST(RootKey, KeyOnsOnTheGridAgree) {
    const Estimate f{true, 263.8};
    const double note = noteOf(263.8);
    std::vector<uint16_t> pitches;
    for (int target : {48, 55, 60, 62, 64, 67, 72, 79}) pitches.push_back(pitchFor(note, target));
    auto r = combine(f, pitches);
    EXPECT_EQ(r.status, Status::Agrees);
    EXPECT_EQ(r.keyOns, pitches.size());
    EXPECT_NEAR(r.gridCents, 0.0, 1.0);
    EXPECT_GT(r.coherence, 0.99);
    EXPECT_EQ(r.key, 60);
    EXPECT_EQ(r.correction, -14);
}

TEST(RootKey, KeyOnsOffTheGridDisagree) {
    // The game tunes as if the sample sounded 69.4, so with a fundamental of 69 every
    // played note comes out 40 cents flat.
    const Estimate f{true, 440.0};
    std::vector<uint16_t> pitches;
    for (int target : {60, 64, 67, 69}) pitches.push_back(pitchFor(69.4, target));
    auto r = combine(f, pitches);
    EXPECT_EQ(r.status, Status::Disagrees);
    EXPECT_NEAR(r.gridCents, -40.0, 1.0);
    // The root key still comes from the fundamental.
    EXPECT_EQ(r.key, 69);
    EXPECT_EQ(r.correction, 0);
}

TEST(RootKey, KeyOnsThatDisagreeWithEachOther) {
    // Half 20 cents sharp, half 20 cents flat: each within the agreement bound on its own,
    // and the mean is on the grid, but the spread is not. (At 40 either way the circular
    // mean would sit at the half-semitone instead, which is nearer to both.)
    const Estimate f{true, 440.0};
    std::vector<uint16_t> pitches = {pitchFor(69.2, 69), pitchFor(68.8, 69), pitchFor(69.2, 72), pitchFor(68.8, 72)};
    auto r = combine(f, pitches);
    EXPECT_NEAR(r.gridCents, 0.0, 1.0);
    EXPECT_LT(r.coherence, c_minCoherence);
    EXPECT_EQ(r.status, Status::Disagrees);
}

TEST(RootKey, GridDistanceWrapsAroundTheSemitone) {
    // 49 cents sharp and 49 cents flat are 2 cents apart, not 98.
    const Estimate f{true, 440.0};
    auto r = combine(f, {pitchFor(69.49, 69), pitchFor(68.51, 69)});
    EXPECT_NEAR(std::abs(r.gridCents), 50.0, 1.5);
    EXPECT_GT(r.coherence, 0.99);
    EXPECT_EQ(r.status, Status::Disagrees);
}

TEST(RootKey, MutedKeyOnsAreSkipped) {
    auto r = combine({true, 440.0}, {0, 0});
    EXPECT_EQ(r.status, Status::Unconfirmed);
    EXPECT_EQ(r.keyOns, 0u);
}

TEST(RootKey, LoopIsRepeatedToTheCap) {
    std::vector<int16_t> pcm;
    for (int i = 0; i < 100; i++) pcm.push_back(int16_t(1000 + i));  // attack
    for (int i = 0; i < 56; i++) pcm.push_back(int16_t(i));          // loop
    auto w = analysisWindow(pcm, true, 100);
    ASSERT_EQ(w.size(), c_maxInput);
    for (size_t i = 0; i < w.size(); i++) ASSERT_EQ(w[i], int16_t(i % 56)) << i;
}

TEST(RootKey, LongLoopIsCut) {
    std::vector<int16_t> pcm(40000);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = int16_t(i);
    auto w = analysisWindow(pcm, true, 28);
    ASSERT_EQ(w.size(), c_maxInput);
    EXPECT_EQ(w.front(), 28);
}

TEST(RootKey, OneShotSkipsItsAttack) {
    std::vector<int16_t> pcm(40000);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = int16_t(i);
    auto w = analysisWindow(pcm, false, 0);
    ASSERT_EQ(w.size(), c_maxInput);
    EXPECT_EQ(w.front(), 2048);

    // A short one only loses an eighth.
    std::vector<int16_t> shortOne(800, 7);
    shortOne[100] = 1;
    w = analysisWindow(shortOne, false, 0);
    ASSERT_EQ(w.size(), 700u);
    EXPECT_EQ(w.front(), 1);

    // A loop flag pointing past the end is ignored.
    EXPECT_EQ(analysisWindow(shortOne, true, 800).size(), 700u);
}

TEST(RootKey, NoteNames) {
    EXPECT_EQ(noteName(60), "C4");
    EXPECT_EQ(noteName(69), "A4");
    EXPECT_EQ(noteName(21), "A0");
    EXPECT_EQ(noteName(0), "C-1");
    EXPECT_EQ(noteName(127), "G9");
    EXPECT_EQ(noteName(61), "C#4");
}
