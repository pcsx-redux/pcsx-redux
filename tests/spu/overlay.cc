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

#include "spu/overlay.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "gtest/gtest.h"
#include "supportpsx/sf2.h"
#include "spu/adpcm.h"
#include "spu/samplepitch.h"
#include "supportpsx/rootkey.h"
#include "supportpsx/spusample.h"

namespace {

std::shared_ptr<const PCSX::SPU::Overlay> pack(std::vector<PCSX::SF2::Sample> samples, unsigned* skipped = nullptr) {
    auto data = PCSX::SF2::write(samples, "pack", "");
    return PCSX::SPU::Overlay::fromSF2(data.data(), data.size(), skipped);
}

PCSX::SF2::Sample ramp(const char* name, int count) {
    PCSX::SF2::Sample s;
    s.name = name;
    for (int i = 0; i < count; i++) s.pcm.push_back(int16_t(i * 100));
    return s;
}

}  // namespace

TEST(SPUOverlay, KeysOnSignatureNames) {
    unsigned skipped = 0;
    auto overlay =
        pack({ramp("00000000000000ff", 10), ramp("not a signature", 10), ramp("ABCDEF0123456789", 10)}, &skipped);
    ASSERT_NE(overlay, nullptr);
    EXPECT_EQ(overlay->size(), 2u);
    EXPECT_EQ(skipped, 1u);
    EXPECT_NE(overlay->find(0xff), nullptr);
    EXPECT_NE(overlay->find(0xabcdef0123456789), nullptr);
    EXPECT_EQ(overlay->find(0xfe), nullptr);
}

TEST(SPUOverlay, RateAndRootKeyScaleThePitch) {
    // At pitch 1000h a 44100 Hz sample on note 60 advances one sample per output sample.
    // Half the rate, or a root key an octave up, halves it.
    auto a = ramp("0000000000000001", 100);
    auto b = ramp("0000000000000002", 100);
    b.sampleRate = 22050;
    auto c = ramp("0000000000000003", 100);
    c.rootKey = 72;
    auto overlay = pack({a, b, c});
    ASSERT_NE(overlay, nullptr);
    const int32_t unity = 0x10000;
    for (uint64_t sig : {1, 2, 3}) {
        PCSX::SPU::OverlayVoice voice;
        voice.start(overlay, overlay->find(sig));
        std::vector<int32_t> out;
        for (int i = 0; i < 4; i++) out.push_back(voice.next(unity));
        if (sig == 1) {
            EXPECT_EQ(out, (std::vector<int32_t>{0, 100, 200, 300}));
        } else {
            EXPECT_EQ(out, (std::vector<int32_t>{0, 50, 100, 150})) << "signature " << sig;
        }
    }
    // A positive pitch correction raises the pitch: +100 cents is one semitone up.
    auto d = ramp("0000000000000004", 100);
    d.rootKey = 72;
    d.correction = 100;
    auto corrected = pack({d});
    PCSX::SPU::OverlayVoice up;
    up.start(corrected, corrected->find(4));
    up.next(unity);
    EXPECT_NEAR(up.next(unity), 100 * 0.5 * std::exp2(100.0 / 1200.0), 1);

    // Twice the pitch, twice the speed.
    PCSX::SPU::OverlayVoice voice;
    voice.start(overlay, overlay->find(1));
    voice.next(unity * 2);
    EXPECT_EQ(voice.next(unity * 2), 200);
}

TEST(SPUOverlay, OneShotEndsInSilenceAndStaysActive) {
    auto overlay = pack({ramp("0000000000000001", 3)});
    PCSX::SPU::OverlayVoice voice;
    voice.start(overlay, overlay->find(1));
    EXPECT_EQ(voice.next(0x10000), 0);
    EXPECT_EQ(voice.next(0x10000), 100);
    EXPECT_EQ(voice.next(0x10000), 200);
    // The original sample must not come back through: the voice stays overlaid, silent.
    EXPECT_EQ(voice.next(0x10000), 0);
    EXPECT_TRUE(voice.active());
    voice.reset();
    EXPECT_FALSE(voice.active());
}

TEST(SPUOverlay, LoopWraps) {
    auto s = ramp("0000000000000001", 6);
    s.loops = true;
    s.loopStart = 2;
    s.loopEnd = 6;
    auto overlay = pack({s});
    PCSX::SPU::OverlayVoice voice;
    voice.start(overlay, overlay->find(1));
    std::vector<int32_t> out;
    for (int i = 0; i < 12; i++) out.push_back(voice.next(0x10000));
    EXPECT_EQ(out, (std::vector<int32_t>{0, 100, 200, 300, 400, 500, 200, 300, 400, 500, 200, 300}));
}

TEST(SPUOverlay, ReplacementIsTunedAgainstTheOriginal) {
    // An exported sample comes back with the root key and correction the export measured, and
    // the original measures the same note, so it plays unchanged: 300 Hz is note 62.37,
    // written as key 62 with -37 cents. Tuned against note 60 it would play at 0.87.
    auto exported = ramp("0000000000000001", 100);
    const auto measured = PCSX::RootKey::fromFundamental(300.0);
    exported.rootKey = measured.key;
    exported.correction = measured.correction;
    auto overlay = pack({exported});
    PCSX::SPU::OverlayVoice same;
    same.start(overlay, overlay->find(1), measured.note);
    same.next(0x10000);
    EXPECT_NEAR(same.next(0x10000), 100, 1);

    // A replacement on A4 standing in for an original that sounds A3 plays an octave down.
    auto a4 = ramp("0000000000000002", 100);
    a4.rootKey = 69;
    overlay = pack({a4});
    PCSX::SPU::OverlayVoice down;
    down.start(overlay, overlay->find(2), 57.0);
    down.next(0x10000);
    EXPECT_EQ(down.next(0x10000), 50);
}

TEST(SPUSamplePitch, LoopDecodeKeepsTheFilterHistory) {
    // Block 0 is the attack, blocks 1-2 loop. Filter 1 feeds each block the previous one's
    // last samples, so the second pass round the loop differs from the first.
    using namespace PCSX::SPUSample;
    std::vector<uint8_t> ram(c_ramSize);
    const uint32_t start = 0x1000;
    for (unsigned b = 0; b < 3; b++) {
        uint8_t* block = ram.data() + start + b * c_blockSize;
        block[0] = 0x14;  // filter 1, shift 4
        for (unsigned i = 2; i < c_blockSize; i++) block[i] = uint8_t(0x11 * (b + i));
    }
    ram[start + 1 * c_blockSize + 1] = c_flagLoopStart;
    ram[start + 2 * c_blockSize + 1] = c_flagLoopEnd | c_flagRepeat;
    const auto extent = walk(ram.data(), start);
    ASSERT_TRUE(extent.repeats && extent.hasLoop);

    const auto pcm = PCSX::SPU::decodeSample(ram.data(), extent, 200);
    ASSERT_GE(pcm.size(), 200u);
    EXPECT_EQ(pcm.size() % c_samplesPerBlock, 0u);

    // Reference: one decoder walking blocks 0, 1, 2, 1, 2, ...
    PCSX::SPU::AdpcmDecoder decoder;
    decoder.reset();
    std::vector<int16_t> expected;
    for (unsigned b = 0; expected.size() < pcm.size(); b = b == 2 ? 1 : b + 1) {
        decoder.decodeBlock(ram.data() + start + b * c_blockSize);
        while (!decoder.bufferExhausted()) expected.push_back(int16_t(std::clamp(decoder.takeSample(), -32768, 32767)));
    }
    EXPECT_EQ(pcm, expected);

    // And the loop's second pass is not a copy of its first.
    const size_t loopStart = c_samplesPerBlock, loopLength = 2 * c_samplesPerBlock;
    EXPECT_NE(std::vector<int16_t>(pcm.begin() + loopStart, pcm.begin() + loopStart + loopLength),
              std::vector<int16_t>(pcm.begin() + loopStart + loopLength, pcm.begin() + loopStart + 2 * loopLength));

    // Without a minimum, one pass.
    EXPECT_EQ(PCSX::SPU::decodeSample(ram.data(), extent).size(), 3 * c_samplesPerBlock);
}
