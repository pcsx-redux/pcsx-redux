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

// SF3 banks store each sample as Ogg Vorbis. Both SoundFont readers have to decode them: the
// one in supportpsx/sf2.cc (SPU sample overlays), and TinySoundFont as midi-converter.h
// configures it (midi2psm, midi2spd).

#include <stdint.h>
#include <string.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "gtest/gtest.h"
#include "supportpsx/midi-converter.h"
#include "supportpsx/sf2.h"
#include "tests/supportpsx/sf3-fixture.h"

namespace {

using namespace SF3Fixture;

// Root mean square distance to the sine the fixture was encoded from, relative to its amplitude.
double sineError(const int16_t* pcm, size_t count) {
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        const double ref = std::trunc(c_amplitude * std::sin(2.0 * std::numbers::pi * double(i) / double(c_period)));
        const double d = double(pcm[i]) - ref;
        sum += d * d;
    }
    return std::sqrt(sum / double(count)) / c_amplitude;
}

}  // namespace

TEST(SF3, ReaderDecodesVorbisSamples) {
    const auto samples = PCSX::SF2::read(c_bank, sizeof(c_bank));
    ASSERT_EQ(samples.size(), 1u);
    const auto& s = samples[0];
    EXPECT_EQ(s.name, "Sine");
    EXPECT_EQ(s.sampleRate, c_rate);
    EXPECT_EQ(s.rootKey, 69);
    ASSERT_EQ(s.pcm.size(), c_points);
    EXPECT_TRUE(s.loops);
    EXPECT_EQ(s.loopStart, c_loopStart);
    EXPECT_EQ(s.loopEnd, c_loopEnd);
    EXPECT_LT(sineError(s.pcm.data(), s.pcm.size()), 0.05);
}

// The fixture with its sample's byte range moved past the end of smpl.
static std::vector<uint8_t> outOfRangeBank() {
    std::vector<uint8_t> bank(c_bank, c_bank + sizeof(c_bank));
    for (size_t i = 0; i + 8 <= bank.size(); i++) {
        if (memcmp(bank.data() + i, "shdr", 4) != 0) continue;
        uint8_t* h = bank.data() + i + 8;
        const uint32_t start = 0x100000;
        const uint32_t end = start + 0x1000;
        memcpy(h + 20, &start, 4);
        memcpy(h + 24, &end, 4);
        break;
    }
    return bank;
}

TEST(SF3, ReaderSkipsOutOfRangeVorbisSamples) {
    const auto bank = outOfRangeBank();
    EXPECT_TRUE(PCSX::SF2::read(bank.data(), bank.size()).empty());
}

TEST(SF3, TinySoundFontSkipsOutOfRangeVorbisSamples) {
    const auto bank = outOfRangeBank();
    // tsf drops the sample and loads the rest, instead of reading 1 MiB past smpl.
    tsf* font = tsf_load_memory(bank.data(), int(bank.size()));
    ASSERT_NE(font, nullptr);
    tsf_close(font);
}

TEST(SF3, TinySoundFontDecodesVorbisSamples) {
    tsf* font = tsf_load_memory(c_bank, int(sizeof(c_bank)));
    ASSERT_NE(font, nullptr);
    ASSERT_EQ(font->presetNum, 1);
    ASSERT_EQ(font->presets[0].regionNum, 1);
    const tsf_region& r = font->presets[0].regions[0];
    ASSERT_LT(r.offset, r.end);
    // tsf runs a region one point past the end of its sample when the font has one to spare.
    ASSERT_GE(r.end - r.offset, c_points);
    ASSERT_LE(r.end - r.offset, c_points + 1);
    // tsf's loop_end is inclusive.
    EXPECT_EQ(r.loop_start - r.offset, c_loopStart);
    EXPECT_EQ(r.loop_end - r.offset, c_loopEnd - 1);

    std::vector<int16_t> pcm(c_points);
    for (unsigned i = 0; i < c_points; i++) {
        pcm[i] = int16_t(std::lrint(font->fontSamples[r.offset + i] * 32767.0f));
    }
    EXPECT_LT(sineError(pcm.data(), pcm.size()), 0.05);
    tsf_close(font);
}
