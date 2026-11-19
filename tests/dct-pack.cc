/*
 * Tests for supportpsx/dct's run-level packing and rate control.
 *
 * Each test below is written so it CAN fail: where the assertion is an equality
 * the other arm is constructed to differ, and where it is a bound the input is
 * built to cross it. A test that passes because nothing ran is the failure this
 * file exists to avoid.
 */

#include <stdint.h>

#include <vector>

#include "gtest/gtest.h"
#include "supportpsx/dct.h"

namespace {

// A 32x32 frame with real structure, so blocks differ from each other and a
// change in quantization is visible in the packed size. A flat frame would make
// every quantization setting produce the same stream and every test below vacuous.
struct TestFrame {
    std::vector<uint8_t> y, cb, cr;
    PCSX::DCT::Frame frame;
    TestFrame() : y(32 * 32), cb(16 * 16), cr(16 * 16) {
        for (int j = 0; j < 32; j++) {
            for (int i = 0; i < 32; i++) {
                y[j * 32 + i] = static_cast<uint8_t>((i * 7 + j * 3) ^ (i * j));
            }
        }
        for (int j = 0; j < 16; j++) {
            for (int i = 0; i < 16; i++) {
                cb[j * 16 + i] = static_cast<uint8_t>(128 + ((i - 8) * 5));
                cr[j * 16 + i] = static_cast<uint8_t>(128 - ((j - 8) * 5));
            }
        }
        frame.y = y.data();
        frame.cb = cb.data();
        frame.cr = cr.data();
        frame.width = 32;
        frame.height = 32;
        frame.yStride = 32;
        frame.cStride = 16;
    }
};

struct Transformed {
    std::vector<int16_t> coeffs;
    PCSX::DCT::Result shape;
    Transformed() : coeffs(PCSX::DCT::requiredCoefficientCount(32, 32)) {
        TestFrame tf;
        PCSX::DCT::Encoder enc(1);
        auto p = enc.submit(tf.frame, coeffs);
        shape = p.get();
    }
};

}  // namespace

TEST(DctPack, transformSucceededAndFrameIsNotDegenerate) {
    Transformed t;
    ASSERT_FALSE(t.shape.failed);
    EXPECT_EQ(t.shape.blockCount, 4u * 6u);  // 2x2 macroblocks, 6 blocks each
    // The frame must actually carry AC energy, or every test below is vacuous.
    int nonZeroAc = 0;
    for (uint32_t b = 0; b < t.shape.blockCount; b++) {
        for (int k = 1; k < 64; k++) {
            if (t.coeffs[b * 64 + k] != 0) nonZeroAc++;
        }
    }
    EXPECT_GT(nonZeroAc, 100);
}

TEST(DctPack, noRateControlEqualsAnAlwaysAcceptingFunctor) {
    Transformed t;
    std::vector<uint16_t> plain, viaFunctor;
    auto a = PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, plain);
    unsigned calls = 0;
    auto b = PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, viaFunctor,
                             [&](const PCSX::DCT::PackAttempt &info) -> std::optional<int> {
                                 calls++;
                                 EXPECT_EQ(info.attempt, 0u);
                                 EXPECT_EQ(info.qScale, 8);
                                 return std::nullopt;
                             });
    EXPECT_FALSE(a.failed);
    EXPECT_FALSE(b.failed);
    EXPECT_EQ(plain, viaFunctor);
    EXPECT_EQ(calls, t.shape.blockCount / 6);  // once per macroblock
    EXPECT_EQ(a.attempts, b.attempts);
}

TEST(DctPack, aRetryActuallyRepacksAtTheNewQScale) {
    Transformed t;
    std::vector<uint16_t> low, raised;
    PCSX::DCT::pack(t.coeffs, t.shape, {}, 2, low);
    // Ask once for a much coarser q_scale, then accept. If the retry did not run,
    // or ran and ignored the value, this comes out the same size as `low`.
    auto r =
        PCSX::DCT::pack(t.coeffs, t.shape, {}, 2, raised, [](const PCSX::DCT::PackAttempt &info) -> std::optional<int> {
            if (info.attempt == 0) return 63;
            return std::nullopt;
        });
    EXPECT_FALSE(r.failed);
    EXPECT_LT(raised.size(), low.size());
    EXPECT_EQ(r.attempts, 2u * (t.shape.blockCount / 6));
    // And the q_scale the decoder will read is the raised one, not the initial.
    EXPECT_EQ((raised[0] >> 10) & 0x3f, 63);
    EXPECT_EQ((low[0] >> 10) & 0x3f, 2);
}

TEST(DctPack, alternatingFunctorTerminates) {
    Transformed t;
    std::vector<uint16_t> out;
    // Never repeats a value consecutively, so the converged-test alone cannot stop
    // it. Only the hard attempt cap can. Without the cap this hangs forever.
    auto r = PCSX::DCT::pack(
        t.coeffs, t.shape, {}, 8, out,
        [](const PCSX::DCT::PackAttempt &info) -> std::optional<int> { return (info.attempt % 2) ? 10 : 20; });
    EXPECT_FALSE(r.failed);
    EXPECT_GT(out.size(), 0u);
    EXPECT_EQ(r.attempts, 64u * (t.shape.blockCount / 6));
}

TEST(DctPack, outOfRangeQScaleIsClamped) {
    Transformed t;
    std::vector<uint16_t> lo, hi;
    PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, lo, [](const PCSX::DCT::PackAttempt &i) -> std::optional<int> {
        return i.attempt == 0 ? std::optional<int>(-5) : std::nullopt;
    });
    PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, hi, [](const PCSX::DCT::PackAttempt &i) -> std::optional<int> {
        return i.attempt == 0 ? std::optional<int>(9999) : std::nullopt;
    });
    EXPECT_EQ((lo[0] >> 10) & 0x3f, 1);
    EXPECT_EQ((hi[0] >> 10) & 0x3f, 63);
}

TEST(DctPack, reportedSizesAgreeWithTheStream) {
    Transformed t;
    std::vector<uint16_t> out;
    size_t lastTotal = 0;
    std::vector<size_t> perBlock;
    auto r =
        PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, out, [&](const PCSX::DCT::PackAttempt &info) -> std::optional<int> {
            EXPECT_EQ(info.totalHalfwords, lastTotal + info.sizeHalfwords);
            lastTotal = info.totalHalfwords;
            perBlock.push_back(info.sizeHalfwords);
            return std::nullopt;
        });
    size_t sum = 0;
    for (size_t v : perBlock) sum += v;
    EXPECT_EQ(sum, lastTotal);
    // The reported total is before the DMA padding, so the final stream is at
    // least that and is a whole number of 40h-halfword blocks.
    EXPECT_GE(r.halfwords, sum);
    EXPECT_EQ(r.halfwords % 64, 0u);
    EXPECT_EQ(out.size(), r.halfwords);
}

TEST(DctPack, shortCoefficientSpanFailsRatherThanReadingPastTheEnd) {
    Transformed t;
    std::vector<uint16_t> out;
    auto r = PCSX::DCT::pack(std::span<const int16_t>(t.coeffs.data(), 64), t.shape, {}, 8, out);
    EXPECT_TRUE(r.failed);
    EXPECT_EQ(out.size(), 0u);
}

// The bytes retail executables carry for the standard table, in the order they
// upload it with MDEC(2). Every round-trip test here is blind to the order,
// since the encoder and decoder share the table, so this pins it.
TEST(ScaleQuantTable, standardTableIsInUploadOrder) {
    static constexpr uint8_t c_retail[64] = {
        2,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
        27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
        37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
    };
    const uint8_t *std8 = PCSX::DCT::standardQuantTable();
    for (int i = 0; i < 64; i++) EXPECT_EQ(std8[i], c_retail[i]) << "entry " << i;
}

TEST(ScaleQuantTable, fiftyIsTheIdentityAndTheCurveIsMonotonic) {
    const uint8_t *std8 = PCSX::DCT::standardQuantTable();
    uint8_t at50[64], at1[64], at100[64];
    PCSX::DCT::scaleQuantTable(std8, at50, 50);
    PCSX::DCT::scaleQuantTable(std8, at1, 1);
    PCSX::DCT::scaleQuantTable(std8, at100, 100);
    for (int i = 0; i < 64; i++) {
        EXPECT_EQ(at50[i], std8[i]) << "quality 50 must leave entry " << i << " alone";
        EXPECT_GE(at1[i], at50[i]) << "quality 1 must not be finer at entry " << i;
        EXPECT_LE(at100[i], at50[i]) << "quality 100 must not be coarser at entry " << i;
        EXPECT_GE(at1[i], 1);
        EXPECT_GE(at100[i], 1) << "a zero divisor would be a division by zero downstream";
    }
    // And the extremes must actually differ, or the curve is flat and useless.
    int differ = 0;
    for (int i = 0; i < 64; i++) {
        if (at1[i] != at100[i]) differ++;
    }
    EXPECT_GT(differ, 50);
}

TEST(ScaleQuantTable, aCoarserTableProducesASmallerStream) {
    Transformed t;
    uint8_t fine[64], coarse[64];
    PCSX::DCT::scaleQuantTable(nullptr, fine, 95);
    PCSX::DCT::scaleQuantTable(nullptr, coarse, 5);
    PCSX::DCT::QuantTables tf{fine, fine}, tc{coarse, coarse};
    std::vector<uint16_t> a, b;
    PCSX::DCT::pack(t.coeffs, t.shape, tf, 8, a);
    PCSX::DCT::pack(t.coeffs, t.shape, tc, 8, b);
    EXPECT_LT(b.size(), a.size());
}

TEST(DctPack, dcAndAcClippingAreCountedSeparately) {
    Transformed t;
    // A table fine enough to clip the DC: qt[0] of 1 makes the DC divisor 2, and
    // the forward transform puts 16*luma in blk[0], so a bright block overruns
    // the signed 10 bit field. Raising q_scale cannot reach it, which is the
    // whole reason the two counters are separate.
    uint8_t fine[64];
    PCSX::DCT::scaleQuantTable(nullptr, fine, 100);
    PCSX::DCT::QuantTables tabs{fine, fine};
    std::vector<uint16_t> a, b;
    auto lowQ = PCSX::DCT::pack(t.coeffs, t.shape, tabs, 1, a);
    auto highQ = PCSX::DCT::pack(t.coeffs, t.shape, tabs, 63, b);
    // Without real DC clipping the equality below is 0 == 0 and proves nothing.
    ASSERT_GT(lowQ.clippedDc, 0u) << "the test frame does not overrun the DC field";
    // q_scale 63 against 1 must cut AC clipping hard and leave DC clipping alone.
    EXPECT_LT(highQ.clippedAc, lowQ.clippedAc);
    EXPECT_EQ(highQ.clippedDc, lowQ.clippedDc);
    // And the standard table at quality 50 must clip no DC at all, or the test
    // above is measuring nothing.
    std::vector<uint16_t> c;
    auto std50 = PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, c);
    EXPECT_EQ(std50.clippedDc, 0u);
}

// The AC divisor is NOT uniform across the 63 slots. psx-spx's scale matrix
// carries the orthonormal DCT's DC term and the forward basis does not, and that
// term is PER AXIS, so it lands once for every axis sitting at DC: one-axis slots
// get composite sqrt(2) (8/sqrt(2) = 5793/1024), two-axis slots get composite 1
// (8 = 8192/1024). Measured with the calibration harness: one-axis arms read
// Gfwd 8.000 * Ginv 0.1768 = 1.4139, two-axis arms read 4.000 * 0.2500 = 1.0000.
//
// This exists because the original calibration swept a vertically uniform
// cosine, so every one of its seven frequencies was a one-axis slot and the
// sqrt(2) got applied to all 63. Nothing in this suite ranged over the other
// class, so the whole suite stayed green while 49 of 63 coefficients came back
// at 1/sqrt(2) amplitude.
TEST(DctPack, theAcDivisorSplitsOnHowManyAxesSitAtDc) {
    Transformed t;
    std::vector<int16_t> coeffs(t.coeffs.size(), 0);
    // Raster 1 is row 0 col 1, so one axis is at DC. Raster 9 is row 1 col 1, so
    // neither is. They are zigzag positions 1 and 4, hence runs of 0 and 2.
    coeffs[1] = 1200;
    coeffs[9] = 1200;
    uint8_t flat[64];
    for (auto &v : flat) v = 8;  // flat table, so the frequency class is the only difference
    PCSX::DCT::QuantTables tabs{flat, flat};
    std::vector<uint16_t> out;
    PCSX::DCT::pack(coeffs, t.shape, tabs, 4, out);
    ASSERT_GE(out.size(), 3u);
    // 1200*5793/(8*4*1024) = 212, 1200*8192/(8*4*1024) = 300.
    EXPECT_EQ(out[1], static_cast<uint16_t>((0 << 10) | 212));
    EXPECT_EQ(out[2], static_cast<uint16_t>((2 << 10) | 300));
    // And state the thing the numbers are for: the same coefficient through the
    // same table must come out sqrt(2) larger when neither axis is at DC.
    const int mixed = out[1] & 0x3ff, both = out[2] & 0x3ff;
    EXPECT_NEAR(static_cast<double>(both) / mixed, 1.41421, 0.005);
}

TEST(QualityToQScale, endpointsMidpointAndMonotonicity) {
    EXPECT_EQ(PCSX::DCT::qualityToQScale(100), 1);
    EXPECT_EQ(PCSX::DCT::qualityToQScale(1), 63);
    // 50 must land on 8, the value the tool defaulted to before the dial existed.
    EXPECT_EQ(PCSX::DCT::qualityToQScale(50), 8);
    // Monotonically non-increasing: a higher quality never asks for a coarser
    // q_scale. A linear map would also pass this, so the midpoint above is what
    // pins the shape.
    for (int q = 1; q < 100; q++) {
        EXPECT_GE(PCSX::DCT::qualityToQScale(q), PCSX::DCT::qualityToQScale(q + 1)) << "at quality " << q;
    }
    EXPECT_EQ(PCSX::DCT::qualityToQScale(-5), 63);
    EXPECT_EQ(PCSX::DCT::qualityToQScale(1000), 1);
}

// The BS container: header shape, the q_scale constraint, and a round trip.
//
// ⚠ The round trip below is a CONSISTENCY check, not a correctness one - both
// halves live in the same file and were written from the same reading of
// FileFormat47, so a shared misreading passes it. One did: the escape was encoded
// as a 6-bit run plus a 16-bit level, which is 28 bits where Sony's DecDCTvlc
// reads 16, and a round trip against a matching decoder was perfectly green while
// the stream was garbage to anything else. The real oracle is PSX-Bundle
// psxdev/vlc.c. What this test is for is catching a REGRESSION in one half.
TEST(DctContainer, bsRoundTripsAndRefusesWhatItCannotExpress) {
    Transformed t;
    std::vector<uint16_t> stream;
    auto packed = PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, stream);
    ASSERT_FALSE(packed.failed);
    EXPECT_EQ(packed.minQScale, 8);
    EXPECT_EQ(packed.maxQScale, 8);

    std::vector<uint8_t> bs;
    auto wrapped = PCSX::DCT::toContainer(stream, PCSX::DCT::Container::Bs, bs);
    ASSERT_FALSE(wrapped.failed) << (wrapped.error ? wrapped.error : "");
    EXPECT_EQ(wrapped.blocks, t.shape.blockCount);
    EXPECT_EQ(wrapped.qScale, 8);
    // Word 0 is the MDEC decode command, so the magic sits in its high half and
    // the length is in 32-bit words of PADDED run-level.
    ASSERT_GE(bs.size(), 8u);
    EXPECT_EQ(bs[2] | (bs[3] << 8), 0x3800);
    EXPECT_EQ(bs[4] | (bs[5] << 8), 8);
    EXPECT_EQ(bs[6] | (bs[7] << 8), 2);
    EXPECT_EQ(static_cast<uint32_t>(bs[0] | (bs[1] << 8)), (stream.size() + 1) >> 1);

    std::vector<uint16_t> back;
    auto un = PCSX::DCT::fromContainer(bs, PCSX::DCT::Container::Bs, back);
    ASSERT_FALSE(un.failed) << (un.error ? un.error : "");
    EXPECT_EQ(back, stream) << "bsencode | bsdecode must reproduce the packed stream";

    // A q_scale that moves mid-frame has no representation: BS stores QUANT once.
    // Rate control can produce one, so this must fail rather than write a stream
    // the stock player silently misreads.
    // Odd macroblocks move to 20, even ones keep 8, so the frame really does mix.
    std::vector<uint16_t> varying;
    auto mixed =
        PCSX::DCT::pack(t.coeffs, t.shape, {}, 8, varying, [](const PCSX::DCT::PackAttempt &i) -> std::optional<int> {
            if (i.attempt == 0 && (i.macroblock & 1)) return 20;
            return std::nullopt;
        });
    ASSERT_FALSE(mixed.failed);
    ASSERT_EQ(mixed.minQScale, 8);
    ASSERT_EQ(mixed.maxQScale, 20);
    std::vector<uint8_t> nope;
    auto refused = PCSX::DCT::toContainer(varying, PCSX::DCT::Container::Bs, nope);
    EXPECT_TRUE(refused.failed) << "a varying q_scale is not BS-expressible";
    EXPECT_TRUE(nope.empty());

    // And a corrupt header is refused rather than decoded into plausible noise.
    std::vector<uint8_t> bad = bs;
    bad[3] ^= 0xff;
    std::vector<uint16_t> nothing;
    auto badr = PCSX::DCT::fromContainer(bad, PCSX::DCT::Container::Bs, nothing);
    EXPECT_TRUE(badr.failed);
    EXPECT_TRUE(nothing.empty());

    // ⛔ THE VERSION FIELD IS NOT A v2/v3 BINARY. v1 and v2 both carry a raw 10-bit
    // DC and only v3 delta-codes it, so relabelling a v2 stream as v1 must decode to
    // the identical run-level. Sony's own DecDCTvlc gets this wrong - it tests
    // `type == 2` and sends v1 down the delta path - and a decoder written from that
    // code inherits the bug. Measured on Suikoden II `_KONAMIC.STR`, 168 retail v1
    // frames: the delta path yields 16 blocks of 1800 and then fills with EOB, the
    // raw path yields 1800 and a picture.
    std::vector<uint8_t> asV1 = bs;
    asV1[6] = 1;
    std::vector<uint16_t> v1Back;
    auto v1r = PCSX::DCT::fromContainer(asV1, PCSX::DCT::Container::Bs, v1Back);
    ASSERT_FALSE(v1r.failed) << (v1r.error ? v1r.error : "");
    EXPECT_EQ(v1Back, stream) << "v1 is raw-DC like v2, not delta-coded like v3";
    EXPECT_EQ(v1r.blocks, t.shape.blockCount);

    // v3 takes the delta path, so the same bytes must NOT come back as the v2 stream.
    // This is a dispatch check, not a check of the delta tables - those were graded
    // against DecDCTvlc on 143 retail v3 frames, which no unit test here can carry.
    std::vector<uint8_t> asV3 = bs;
    asV3[6] = 3;
    std::vector<uint16_t> v3Back;
    auto v3r = PCSX::DCT::fromContainer(asV3, PCSX::DCT::Container::Bs, v3Back);
    // A failed or empty decode differs from `stream` too, so rule those out first
    // or the comparison below passes on nothing.
    ASSERT_FALSE(v3r.failed) << (v3r.error ? v3r.error : "");
    ASSERT_GT(v3r.blocks, 0u) << "the delta path decoded no blocks at all";
    ASSERT_EQ(v3Back.size(), stream.size());
    EXPECT_NE(v3Back, stream) << "v3 must not be decoded as raw DC";

    // Versions outside 1..3 are refused rather than guessed at.
    for (uint8_t v : {uint8_t(0), uint8_t(4), uint8_t(0xff)}) {
        std::vector<uint8_t> odd = bs;
        odd[6] = v;
        std::vector<uint16_t> out;
        auto r = PCSX::DCT::fromContainer(odd, PCSX::DCT::Container::Bs, out);
        EXPECT_TRUE(r.failed) << "BS version " << int(v) << " should be refused";
        EXPECT_TRUE(out.empty());
    }
}

// Retail STR walks 16-pixel columns top to bottom, so that is the default, and
// stream macroblock n is column n / rows, row n % rows. On this 2x2 frame the
// two orders agree on macroblocks 0 and 3 and swap 1 and 2, so the comparison
// is between those two, and it is checked first that they differ at all.
TEST(DctOrder, theDefaultIsColumnMajor) {
    EXPECT_EQ(PCSX::DCT::Frame{}.order, PCSX::DCT::MacroblockOrder::Column);

    auto encode = [](PCSX::DCT::MacroblockOrder order) {
        TestFrame tf;
        tf.frame.order = order;
        std::vector<int16_t> coeffs(PCSX::DCT::requiredCoefficientCount(32, 32));
        PCSX::DCT::Encoder enc(1);
        auto shape = enc.submit(tf.frame, coeffs).get();
        EXPECT_FALSE(shape.failed);
        return coeffs;
    };
    auto byDefault = [] {
        TestFrame tf;
        std::vector<int16_t> coeffs(PCSX::DCT::requiredCoefficientCount(32, 32));
        PCSX::DCT::Encoder enc(1);
        auto shape = enc.submit(tf.frame, coeffs).get();
        EXPECT_FALSE(shape.failed);
        return coeffs;
    }();
    const auto raster = encode(PCSX::DCT::MacroblockOrder::Raster);
    const auto column = encode(PCSX::DCT::MacroblockOrder::Column);

    constexpr size_t mb = 6 * 64;  // int16 per macroblock
    auto macroblock = [](const std::vector<int16_t> &c, size_t n) {
        return std::vector<int16_t>(c.begin() + n * mb, c.begin() + (n + 1) * mb);
    };
    ASSERT_NE(macroblock(raster, 1), macroblock(raster, 2));

    // Column macroblock 1 is (col 0, row 1), which raster puts at index 2.
    EXPECT_EQ(macroblock(column, 1), macroblock(raster, 2));
    EXPECT_EQ(macroblock(column, 2), macroblock(raster, 1));
    EXPECT_EQ(macroblock(column, 0), macroblock(raster, 0));
    EXPECT_EQ(macroblock(column, 3), macroblock(raster, 3));
    EXPECT_EQ(byDefault, column);
    EXPECT_NE(byDefault, raster);
}

TEST(DctPack, temporaryFailureForTheSummaryTable) { EXPECT_EQ(1, 2) << "deliberate, reverted before merge"; }
