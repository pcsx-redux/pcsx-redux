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

#include "supportpsx/sf2.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "gtest/gtest.h"
#include "supportpsx/spusample.h"

#define TSF_STATIC
#define TSF_NO_STDIO
#define TSF_IMPLEMENTATION
#include "tsf.h"

namespace {

using namespace PCSX::SPUSample;

// Three blocks: loop start on the second, loop end + repeat on the third.
void writeSample(std::vector<uint8_t>& ram, uint32_t at) {
    for (unsigned b = 0; b < 3; b++) {
        uint8_t* block = ram.data() + at + b * c_blockSize;
        block[0] = 0x12;
        block[1] = 0;
        for (unsigned i = 2; i < c_blockSize; i++) block[i] = uint8_t(b * 31 + i * 7);
    }
    ram[at + 1 * c_blockSize + 1] = c_flagLoopStart;
    ram[at + 2 * c_blockSize + 1] = c_flagLoopEnd | c_flagRepeat;
}

}  // namespace

TEST(SPUSample, WalkFindsTheLoop) {
    std::vector<uint8_t> ram(c_ramSize);
    writeSample(ram, 0x1000);
    auto e = walk(ram.data(), 0x1000);
    EXPECT_EQ(e.start, 0x1000u);
    EXPECT_EQ(e.end, 0x1030u);
    EXPECT_EQ(e.blocks(), 3u);
    EXPECT_TRUE(e.hasLoop);
    EXPECT_EQ(e.loop, 0x1010u);
    EXPECT_TRUE(e.repeats);
    EXPECT_FALSE(e.truncated);
}

TEST(SPUSample, WalkWithNoEndIsTruncated) {
    std::vector<uint8_t> ram(c_ramSize);
    auto e = walk(ram.data(), c_ramSize - 2 * c_blockSize);
    EXPECT_TRUE(e.truncated);
    EXPECT_EQ(e.end, c_ramSize);
}

TEST(SPUSample, SignatureFollowsContentNotAddress) {
    std::vector<uint8_t> ram(c_ramSize);
    writeSample(ram, 0x1000);
    writeSample(ram, 0x42000);
    const auto a = walk(ram.data(), 0x1000);
    const auto b = walk(ram.data(), 0x42000);
    EXPECT_EQ(signature(ram.data(), a), signature(ram.data(), b));

    // One nibble of sample data.
    ram[0x42000 + 0x25] ^= 0x10;
    EXPECT_NE(signature(ram.data(), a), signature(ram.data(), b));
    ram[0x42000 + 0x25] ^= 0x10;
    EXPECT_EQ(signature(ram.data(), a), signature(ram.data(), b));

    // Moving the loop start changes the sample, so it changes the signature.
    ram[0x42000 + 1 * c_blockSize + 1] = 0;
    ram[0x42000 + 1] = c_flagLoopStart;
    EXPECT_NE(signature(ram.data(), a), signature(ram.data(), walk(ram.data(), 0x42000)));

    // Bytes past the end block are not part of the sample.
    writeSample(ram, 0x42000);
    ram[0x42030] = 0x55;
    EXPECT_EQ(signature(ram.data(), a), signature(ram.data(), walk(ram.data(), 0x42000)));
}

TEST(SF2, LoadsBackWithTinySoundFont) {
    std::vector<PCSX::SF2::Sample> samples(2);
    samples[0].name = "oneshot";
    for (int i = 0; i < 100; i++) samples[0].pcm.push_back(int16_t(i * 300 - 15000));
    samples[1].name = "0123456789abcdef";
    for (int i = 0; i < 280; i++) samples[1].pcm.push_back(int16_t(std::sin(i * 0.1) * 20000));
    samples[1].loops = true;
    samples[1].loopStart = 56;
    samples[1].loopEnd = 280;
    samples[1].sampleRate = 22050;

    auto data = PCSX::SF2::write(samples, "test bank", "a comment");
    ASSERT_EQ(memcmp(data.data(), "RIFF", 4), 0);
    EXPECT_EQ(data.size() % 2, 0u);
    const uint32_t riffSize = data[4] | data[5] << 8 | data[6] << 16 | uint32_t(data[7]) << 24;
    EXPECT_EQ(riffSize + 8, data.size());

    tsf* font = tsf_load_memory(data.data(), int(data.size()));
    ASSERT_NE(font, nullptr);
    ASSERT_EQ(tsf_get_presetcount(font), 2);
    EXPECT_STREQ(tsf_get_presetname(font, 1), "0123456789abcdef");

    for (int p = 0; p < 2; p++) {
        const auto& s = samples[p];
        ASSERT_EQ(font->presets[p].regionNum, 1);
        const tsf_region& r = font->presets[p].regions[0];
        EXPECT_EQ(r.pitch_keycenter, 60);
        EXPECT_EQ(r.sample_rate, s.sampleRate);
        // tsf adds one to the end it reads; the file stores one past the last sample.
        // A loop ending at the last sample is followed by 8 points copied from the loop start.
        const size_t tail = s.loops ? 8 : 0;
        EXPECT_EQ(r.end - r.offset, s.pcm.size() + tail + 1);
        for (size_t i = 0; i < s.pcm.size() + tail; i++) {
            const int16_t expected = i < s.pcm.size() ? s.pcm[i] : s.pcm[s.loopStart + i - s.pcm.size()];
            ASSERT_EQ(std::lround(font->fontSamples[r.offset + i] * 32767.0), expected) << "preset " << p << " " << i;
        }
        if (s.loops) {
            EXPECT_EQ(r.loop_mode, TSF_LOOPMODE_CONTINUOUS);
            EXPECT_EQ(r.loop_start - r.offset, s.loopStart);
            // tsf keeps the last looped sample, the file stores one past it.
            EXPECT_EQ(r.loop_end - r.offset, s.loopEnd - 1);
        } else {
            EXPECT_EQ(r.loop_mode, TSF_LOOPMODE_NONE);
        }
    }
    tsf_close(font);
}

TEST(SF2, OutOfRangeRootKeyReadsAs60) {
    std::vector<PCSX::SF2::Sample> samples(3);
    const uint8_t keys[] = {127, 128, 255};
    for (int n = 0; n < 3; n++) {
        samples[n].name = std::string("s") + char('0' + n);
        for (int i = 0; i < 20; i++) samples[n].pcm.push_back(int16_t(i));
        samples[n].rootKey = keys[n];
    }
    auto data = PCSX::SF2::write(samples, "bank", "");
    auto back = PCSX::SF2::read(data.data(), data.size());
    ASSERT_EQ(back.size(), 3u);
    EXPECT_EQ(back[0].rootKey, 127);
    EXPECT_EQ(back[1].rootKey, 60);
    EXPECT_EQ(back[2].rootKey, 60);
}

TEST(SF2, ReadsBackWhatItWrites) {
    std::vector<PCSX::SF2::Sample> samples(2);
    samples[0].name = "first";
    for (int i = 0; i < 50; i++) samples[0].pcm.push_back(int16_t(i * 13 - 300));
    samples[0].rootKey = 64;
    samples[0].correction = -12;
    samples[0].sampleRate = 32000;
    samples[1].name = "fedcba9876543210";
    for (int i = 0; i < 90; i++) samples[1].pcm.push_back(int16_t(-i * 200));
    samples[1].loops = true;
    samples[1].loopStart = 28;
    samples[1].loopEnd = 90;

    auto data = PCSX::SF2::write(samples, "bank", "");
    auto back = PCSX::SF2::read(data.data(), data.size());
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].name, "first");
    EXPECT_EQ(back[0].pcm, samples[0].pcm);
    EXPECT_EQ(back[0].rootKey, 64);
    EXPECT_EQ(back[0].correction, -12);
    EXPECT_EQ(back[0].sampleRate, 32000u);
    EXPECT_FALSE(back[0].loops);
    EXPECT_EQ(back[1].name, "fedcba9876543210");
    EXPECT_TRUE(back[1].loops);
    EXPECT_EQ(back[1].loopStart, 28u);
    EXPECT_EQ(back[1].loopEnd, 90u);
    // The 8 points after a loop that ends on the last sample come back with it.
    ASSERT_EQ(back[1].pcm.size(), 98u);
    EXPECT_TRUE(std::equal(samples[1].pcm.begin(), samples[1].pcm.end(), back[1].pcm.begin()));

    EXPECT_TRUE(PCSX::SF2::read(data.data(), 11).empty());
    data[8] = 'x';
    EXPECT_TRUE(PCSX::SF2::read(data.data(), data.size()).empty());
}
