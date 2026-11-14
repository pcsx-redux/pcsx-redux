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

static void expectSine(const PCSX::SF2::Sample& s, const char* name) {
    EXPECT_EQ(s.name, name);
    EXPECT_EQ(s.sampleRate, c_rate);
    EXPECT_EQ(s.rootKey, 69);
    ASSERT_EQ(s.pcm.size(), c_points);
    EXPECT_TRUE(s.loops);
    EXPECT_EQ(s.loopStart, c_loopStart);
    EXPECT_EQ(s.loopEnd, c_loopEnd);
    EXPECT_LT(sineError(s.pcm.data(), s.pcm.size()), 0.05);
}

// The region playing `key`, or nullptr.
static const tsf_region* regionFor(const tsf* font, int key) {
    for (int i = 0; i < font->presets[0].regionNum; i++) {
        const tsf_region& r = font->presets[0].regions[i];
        if (key >= r.lokey && key <= r.hikey) return &r;
    }
    return nullptr;
}

static void expectSine(const tsf* font, const tsf_region& r) {
    ASSERT_GE(r.end - r.offset, c_points);
    // tsf's loop_end is inclusive.
    EXPECT_EQ(r.loop_start - r.offset, c_loopStart);
    EXPECT_EQ(r.loop_end - r.offset, c_loopEnd - 1);
    std::vector<int16_t> pcm(c_points);
    for (unsigned i = 0; i < c_points; i++) {
        pcm[i] = int16_t(std::lrint(font->fontSamples[r.offset + i] * 32767.0f));
    }
    EXPECT_LT(sineError(pcm.data(), pcm.size()), 0.05);
}

// The fixture with the byte range of sample `index` moved past the end of smpl.
static std::vector<uint8_t> outOfRangeBank(unsigned index) {
    std::vector<uint8_t> bank(c_bank, c_bank + sizeof(c_bank));
    for (size_t i = 0; i + 8 <= bank.size(); i++) {
        if (memcmp(bank.data() + i, "shdr", 4) != 0) continue;
        uint8_t* h = bank.data() + i + 8 + index * 46;
        const uint32_t start = 0x100000;
        const uint32_t end = start + 0x1000;
        memcpy(h + 20, &start, 4);
        memcpy(h + 24, &end, 4);
        break;
    }
    return bank;
}

TEST(SF3, ReaderDecodesVorbisSamples) {
    const auto samples = PCSX::SF2::read(c_bank, sizeof(c_bank));
    ASSERT_EQ(samples.size(), 2u);
    expectSine(samples[0], "Sine");
    expectSine(samples[1], "Sine2");
}

TEST(SF3, ReaderSkipsOutOfRangeVorbisSamples) {
    const auto bank = outOfRangeBank(1);
    const auto samples = PCSX::SF2::read(bank.data(), bank.size());
    ASSERT_EQ(samples.size(), 1u);
    expectSine(samples[0], "Sine");
}

TEST(SF3, TinySoundFontDecodesVorbisSamples) {
    tsf* font = tsf_load_memory(c_bank, int(sizeof(c_bank)));
    ASSERT_NE(font, nullptr);
    ASSERT_EQ(font->presetNum, 1);
    ASSERT_EQ(font->presets[0].regionNum, 2);
    const tsf_region* low = regionFor(font, 60);
    const tsf_region* high = regionFor(font, 70);
    ASSERT_NE(low, nullptr);
    ASSERT_NE(high, nullptr);
    expectSine(font, *low);
    expectSine(font, *high);
    // The decoded samples are all there is: the Ogg bytes of smpl are not read as PCM after them.
    EXPECT_EQ(high->end - high->offset, c_points);
    // The renderer interpolates one point past a region's end, which tsf keeps at zero.
    EXPECT_EQ(font->fontSamples[high->end], 0.0f);
    tsf_close(font);
}

TEST(SF3, TinySoundFontSkipsOutOfRangeVorbisSamples) {
    // Without bounds checks, tsf reads 1 MiB past smpl here.
    const auto bank = outOfRangeBank(1);
    tsf* font = tsf_load_memory(bank.data(), int(bank.size()));
    ASSERT_NE(font, nullptr);
    const tsf_region* low = regionFor(font, 60);
    const tsf_region* high = regionFor(font, 70);
    ASSERT_NE(low, nullptr);
    ASSERT_NE(high, nullptr);
    expectSine(font, *low);
    EXPECT_EQ(low->end - low->offset, c_points);
    // The rejected sample plays nothing, rather than every sample in the font.
    EXPECT_EQ(high->offset, high->end);
    tsf_close(font);
}

TEST(SF3, TinySoundFontRejectedSampleIgnoresOffsetGenerators) {
    auto bank = outOfRangeBank(1);
    // Turn the second zone's keyRange into startAddrsOffset = -1, pulling its region back into
    // the decoded samples. That zone now covers every key, so read it by position.
    for (size_t i = 0; i + 8 <= bank.size(); i++) {
        if (memcmp(bank.data() + i, "igen", 4) != 0) continue;
        uint8_t* g = bank.data() + i + 8 + 3 * 4;
        ASSERT_EQ(g[0], 43);
        const uint8_t offset[4] = {0, 0, 0xff, 0xff};
        memcpy(g, offset, 4);
        break;
    }
    tsf* font = tsf_load_memory(bank.data(), int(bank.size()));
    ASSERT_NE(font, nullptr);
    ASSERT_EQ(font->presets[0].regionNum, 2);
    const tsf_region& rejected = font->presets[0].regions[1];
    EXPECT_EQ(rejected.offset, rejected.end);
    tsf_close(font);
}
