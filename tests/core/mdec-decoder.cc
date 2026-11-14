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

// The shared MDEC decoder against the emulator it was lifted out of.
//
// Every golden below was taken from the emulator BEFORE its decoder moved into
// src/core/mdec-decoder.cc: the block hash from src/core/mdec.cc's own rl2blk, and the pixel
// hashes from a booted guest (src/mips/tests/mdec/roundtrip) pushing each case
// through the emulated MDEC at 24 bits, and at 15 bits with STP set. So these
// pin the shared code to what the emulator's MDEC did, and the emulator's MDEC
// is what the hardware arms were graded against. A mismatch here is a change in
// emulated output, not a test to regenerate.

#include "core/mdec-decoder.h"

#include <stdint.h>
#include <string.h>

#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tests/core/mdec-decoder-cases.h"

namespace {

struct Golden {
    const char *name;
    unsigned consumed;  // halfwords one macroblock reads
    uint64_t blocks;    // the six decoded blocks, as little-endian int32
    uint64_t rgb24;     // 768 bytes
    uint64_t rgb15;     // 512 bytes, STP set, little-endian
};

const Golden c_goldens[] = {
    {"DCONLY", 12, 0x5443de8511a3eca5ull, 0xe688b8230f2f1a25ull, 0xdf36505a170284a5ull},
    {"DCEDGE", 12, 0x47f99715275415a5ull, 0xa505191cc4fb83a5ull, 0xe2f3ec385c0e4025ull},
    {"FULLAC8", 390, 0x76aa2bb0b179263eull, 0x35a5abc94823bc19ull, 0x5becf717ff44a849ull},
    {"FULLAC1", 390, 0x6c526936941ad6eaull, 0x9e65ba8d77448844ull, 0x6b491f3696f32a83ull},
    {"FULLAC63", 390, 0xae933668e4bfc804ull, 0x379e3c562d5fd780ull, 0xd04a453840ffce0eull},
    {"Q0", 18, 0xf66e2321bb061ad4ull, 0xfddf8816962c29e9ull, 0x578f51fea2fb902bull},
    {"Q0FULL", 390, 0x9c98be9121ab3e70ull, 0x958342e34331bc63ull, 0x43952edcd166add1ull},
    {"SATDC", 12, 0xb22e903895b615a5ull, 0x2e1142b0bc0be725ull, 0xe2f3ec385c0e4025ull},
    {"SATAC", 23, 0x01a8dbb601c0c224ull, 0x0ec40f9bbf32ba83ull, 0x745d7acfb4c33a03ull},
    {"NEGSC", 390, 0x4f583010d2b295f1ull, 0xe048c431e9c50ebaull, 0xd3ee2fcde715b275ull},
    {"HALFSC", 390, 0xbe4e4157f710eb2bull, 0xd773298cc1f3f2baull, 0xe169fe9bf0f47a98ull},
    {"RANDSC", 83, 0xeaacd46c1bfcb79aull, 0x98d9cadacdfcfe53ull, 0x167b258d3a2d5dcfull},
    {"NOSCALE", 86, 0x6ab05ef9aa8b9b25ull, 0xdb3681d9568edf25ull, 0x9477a10802415325ull},
    {"YUVRP64", 12, 0x014a971f3f2162a5ull, 0x1cb3f37a2c983d65ull, 0xb9e1f7d29dc558a5ull},
    {"YUVRN64", 12, 0x707520e69e90c625ull, 0x719fd6f92f747f25ull, 0x4b29ec7e802406a5ull},
    {"YUVRP37", 12, 0x878069aa2252c0a5ull, 0xc60789c0e03aeb25ull, 0x98b60ca69a126ba5ull},
    {"YUVRN37", 12, 0xe97f0bdc84150e25ull, 0xa71e6e412ff7e525ull, 0xba3913e408573725ull},
    {"YUVRP101", 12, 0xb45ec0fe11160aa5ull, 0x5f6d6eb058b68265ull, 0x02d65a0af0e44025ull},
    {"YUVRN101", 12, 0x35feb5ebbc9c2625ull, 0x3c4b4a74d8cfb665ull, 0x6c5b358f75df6a25ull},
    {"YUVRP50", 12, 0xcff8078293f074a5ull, 0xae17dc95be2bf565ull, 0x40836429f1b67ea5ull},
    {"YUVRN50", 12, 0xcacdbbf412c2f425ull, 0x65b610be55b48825ull, 0x0ac14600839003a5ull},
    {"YUVRP63", 12, 0x6c5894323e9528a5ull, 0xddbcd4e8f364cce5ull, 0xb9e1f7d29dc558a5ull},
    {"YUVBP64", 12, 0x2520d9b5922962a5ull, 0x9eaf12c5146cb3a5ull, 0xc0472d3d9f876625ull},
    {"YUVBN64", 12, 0xa5e3deeaade2c625ull, 0xf5f2fd685f72a525ull, 0xbdec7dccdef29925ull},
    {"YUVBP37", 12, 0x219cf686a1e2c0a5ull, 0xc751a40695bd1165ull, 0xc09ef9673edcae25ull},
    {"YUVBN37", 12, 0xce4d7f8efa470e25ull, 0xc9eccf6efe8a8425ull, 0x8f0c4e5b03946325ull},
    {"YUVBP101", 12, 0xe9390181a97e0aa5ull, 0x5f54a069609f7525ull, 0x06dd771efb099225ull},
    {"YUVBN101", 12, 0xb88325153e6e2625ull, 0x6befc6065e2e4165ull, 0xbcc6d1e5352a5825ull},
    {"YUVBP50", 12, 0x808ede78c8b074a5ull, 0xcb6ceb4da2583ba5ull, 0x7890df2a5260b825ull},
    {"YUVBN50", 12, 0x4897c260fb5cf425ull, 0xd03a9cbf61dcf1e5ull, 0x0a868cb120ac9d25ull},
    {"YUVBP63", 12, 0x4535afbf0c8528a5ull, 0x55a09cdb63ea0da5ull, 0xc0472d3d9f876625ull},
    {"YUVBOTH", 12, 0x8ea7b40e582a4025ull, 0xdb095cd812c72f65ull, 0x8b46bd20cce8ce25ull},
    {"TIES", 12, 0xb04fd35156c9ef25ull, 0xe421c01141218665ull, 0x038148d77bdbe925ull},
    {"WRAP", 17, 0xb0d442272e35ec84ull, 0x02014cd6cadd67c4ull, 0x9385a719df628bdfull},
    {"RAND10", 85, 0xa30ecb4af32f7863ull, 0x10ae5e2fc176bb80ull, 0xddb21f2892ac57b6ull},
    {"RAND11", 78, 0x4175b1ffef7a984bull, 0x23e2bc7285c6eb4cull, 0xc9be6e4231281465ull},
    {"RAND12", 78, 0x5c135fe2cef9ec0aull, 0x5ef0beb076fb5164ull, 0x0eb402db35ed6837ull},
    {"RAND13", 87, 0x7116ec4df22a3129ull, 0x707977baf7bc6014ull, 0x205487a79cc63012ull},
    {"RANDG20", 76, 0xd5eed3710e004106ull, 0x96b63c71f77525b3ull, 0xfb501df6ba717827ull},
    {"RANDG21", 77, 0xee041e0491218e2dull, 0x151a5e20d18f29bcull, 0x87e536d6d7f93b93ull},
    {"RANDG22", 73, 0x641a5a2f4b25dd6dull, 0x3b24815749afc9c7ull, 0xa3f0abd0205d8915ull},
    {"RANDG23", 94, 0xd1d83b101fda25daull, 0xc3da0e877170c019ull, 0xe40219960fba6685ull},
};

uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t hashBlocks(const int *blk) {
    std::vector<uint8_t> b;
    for (unsigned i = 0; i < PCSX::MDECDecoder::BLOCKS * PCSX::MDECDecoder::DSIZE2; i++) {
        const uint32_t v = static_cast<uint32_t>(blk[i]);
        for (int s = 0; s < 32; s += 8) b.push_back(static_cast<uint8_t>(v >> s));
    }
    return fnv1a(b.data(), b.size());
}

// The chip state the rig leaves behind: MDEC(2), then MDEC(3) only if the case
// uploads one, which leaves the power-on all-zero matrix otherwise.
PCSX::MDECDecoder::Tables tablesFor(const MDECDecoderCases::Case &c) {
    PCSX::MDECDecoder::Tables t;
    t.setQuant(c.quant, c.quant + 64);
    if (c.uploadScale) t.setScale(c.scale);
    return t;
}

const Golden *goldenFor(const std::string &name) {
    for (auto &g : c_goldens) {
        if (name == g.name) return &g;
    }
    return nullptr;
}

}  // namespace

TEST(MDECDecoder, everyCaseDecodesToTheEmulatorsCapture) {
    const auto cases = MDECDecoderCases::all();
    ASSERT_EQ(cases.size(), std::size(c_goldens));
    for (auto &c : cases) {
        SCOPED_TRACE(c.name);
        const Golden *g = goldenFor(c.name);
        ASSERT_NE(g, nullptr);
        const auto t = tablesFor(c);
        int blk[PCSX::MDECDecoder::BLOCKS * PCSX::MDECDecoder::DSIZE2];
        PCSX::MDECDecoder::Status status;
        const uint16_t *end =
            PCSX::MDECDecoder::decodeMacroblock(blk, c.rl.data(), t, c.rl.data() + c.rl.size(), &status);
        EXPECT_EQ(status.anomaly, PCSX::MDECDecoder::Anomaly::None);
        EXPECT_EQ(static_cast<unsigned>(end - c.rl.data()), g->consumed);
        EXPECT_EQ(hashBlocks(blk), g->blocks);

        uint8_t rgb24[16 * 16 * 3];
        PCSX::MDECDecoder::toRGB24(blk, rgb24);
        EXPECT_EQ(fnv1a(rgb24, sizeof(rgb24)), g->rgb24);

        uint16_t rgb15[16 * 16];
        PCSX::MDECDecoder::toRGB15(blk, rgb15, false, true);
        EXPECT_EQ(fnv1a(reinterpret_cast<const uint8_t *>(rgb15), sizeof(rgb15)), g->rgb15);
    }
}

// tools/mdec rawdecode is decodeFrame. Stitch every case that shares the standard
// tables into one stream and check that each macroblock lands, in both orders,
// with exactly the bytes the emulator produced for it alone.
TEST(MDECDecoder, aFrameIsTheEmulatorsMacroblocksInPlace) {
    const auto cases = MDECDecoderCases::all();
    std::vector<const MDECDecoderCases::Case *> same;
    for (auto &c : cases) {
        if (c.uploadScale && memcmp(c.scale, MDECDecoderCases::c_stdScale, sizeof(c.scale)) == 0 &&
            memcmp(c.quant, MDECDecoderCases::c_stdQuant, 64) == 0 &&
            memcmp(c.quant + 64, MDECDecoderCases::c_stdQuant, 64) == 0) {
            same.push_back(&c);
        }
    }
    ASSERT_GE(same.size(), 8u);
    std::vector<uint16_t> stream;
    for (auto *c : same) stream.insert(stream.end(), c->rl.begin(), c->rl.end());
    const auto t = tablesFor(*same[0]);
    const unsigned n = static_cast<unsigned>(same.size());

    for (bool column : {false, true}) {
        SCOPED_TRACE(column ? "column" : "raster");
        // One row of macroblocks raster, one column of them column-major: either
        // way the stream order is the case order.
        const unsigned w = column ? 16 : 16 * n, h = column ? 16 * n : 16;
        std::vector<uint8_t> image(w * h * 3);
        const auto frame = PCSX::MDECDecoder::decodeFrame(stream.data(), static_cast<unsigned>(stream.size()), t, w,
                                                          h, column, image.data());
        EXPECT_EQ(frame.status.anomaly, PCSX::MDECDecoder::Anomaly::None);
        EXPECT_EQ(frame.consumed, stream.size());
        for (unsigned i = 0; i < n; i++) {
            SCOPED_TRACE(same[i]->name);
            uint8_t mb[16 * 16 * 3];
            for (unsigned py = 0; py < 16; py++) {
                const unsigned x = column ? 0 : i * 16, y = (column ? i * 16 : 0) + py;
                memcpy(mb + py * 48, image.data() + (y * w + x) * 3, 48);
            }
            EXPECT_EQ(fnv1a(mb, sizeof(mb)), goldenFor(same[i]->name)->rgb24);
        }
    }
}

// The emulator reads unbounded and never sees these; a file decode must stop.
TEST(MDECDecoder, aBoundedDecodeReportsWhereTheStreamWentWrong) {
    const auto cases = MDECDecoderCases::all();
    const auto &c = cases[0];
    const auto t = tablesFor(c);
    int blk[PCSX::MDECDecoder::BLOCKS * PCSX::MDECDecoder::DSIZE2];

    // DCONLY is header, FE00h, six times. Cut it before block 3's header, then
    // between block 3's header and its terminator.
    PCSX::MDECDecoder::Status atHeader;
    PCSX::MDECDecoder::decodeMacroblock(blk, c.rl.data(), t, c.rl.data() + 6, &atHeader);
    EXPECT_EQ(atHeader.anomaly, PCSX::MDECDecoder::Anomaly::Exhausted);
    EXPECT_EQ(atHeader.block, 3u);
    EXPECT_TRUE(atHeader.atHeader);

    PCSX::MDECDecoder::Status inside;
    PCSX::MDECDecoder::decodeMacroblock(blk, c.rl.data(), t, c.rl.data() + 7, &inside);
    EXPECT_EQ(inside.anomaly, PCSX::MDECDecoder::Anomaly::Exhausted);
    EXPECT_EQ(inside.block, 3u);
    EXPECT_FALSE(inside.atHeader);

    // A run of 63 from k = 1 goes past coefficient 63. Like the emulator, the
    // block ends there and the walk carries on.
    std::vector<uint16_t> rl = {MDECDecoderCases::header(8, 1), MDECDecoderCases::ac(0, 5),
                                MDECDecoderCases::ac(63, 5)};
    for (int i = 0; i < 5; i++) {
        rl.push_back(MDECDecoderCases::header(8, 1));
        rl.push_back(0xfe00);
    }
    PCSX::MDECDecoder::Status past;
    const uint16_t *end = PCSX::MDECDecoder::decodeMacroblock(blk, rl.data(), t, rl.data() + rl.size(), &past);
    EXPECT_EQ(past.anomaly, PCSX::MDECDecoder::Anomaly::RunPast63);
    EXPECT_EQ(past.block, 0u);
    EXPECT_EQ(end, rl.data() + rl.size());
}
