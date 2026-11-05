/*
 * Tests for modconv's --stretch: resampling a looped sample so its loop is a
 * whole number of SPU blocks, and compensating the pitch in the patterns.
 */

#include <cmath>
#include <vector>

#include "gtest/gtest.h"
#include "tools/modconv/stretch.h"

using namespace PCSX::ModConv;

namespace {

// One pattern cell: sample number, period, effect, parameter.
void setCell(std::vector<uint8_t>& p, unsigned channels, unsigned row, unsigned ch, unsigned smp, unsigned period,
             unsigned fx, unsigned param) {
    uint8_t* c = p.data() + (row * channels + ch) * 4;
    c[0] = (smp & 0xf0) | (period >> 8);
    c[1] = period & 0xff;
    c[2] = ((smp & 0x0f) << 4) | fx;
    c[3] = param;
}

unsigned cellPeriod(const std::vector<uint8_t>& p, unsigned channels, unsigned row, unsigned ch) {
    const uint8_t* c = p.data() + (row * channels + ch) * 4;
    return ((c[0] & 0x0f) << 8) | c[1];
}

uint8_t cellParam(const std::vector<uint8_t>& p, unsigned channels, unsigned row, unsigned ch) {
    return p[(row * channels + ch) * 4 + 3];
}

}  // namespace

// The 182-sample loop that rounding detunes by 128 cents.
TEST(ModConvStretch, ChoosesWholeBlocksCloserThanRounding) {
    std::vector<int> notes;
    for (int n = 0; n < 24; n++) notes.push_back(n);
    auto s = chooseStretch(182, 0, notes);
    EXPECT_EQ(s.newLoopLength % 28, 0u);
    EXPECT_GT(s.newLoopLength, 182u);
    EXPECT_LE(s.newLoopLength, 364u);
    EXPECT_GE(s.finetune, -8);
    EXPECT_LE(s.finetune, 7);
    EXPECT_EQ(s.offTable, 0u);
    EXPECT_GT(s.roundedCents, 100.0);
    // Periods are integers, about 4 cents apart at C-2: the transposed notes can't do much better.
    EXPECT_LT(s.worstCents, 8.0);
    // Check the reported error against the table directly, on the lowest note.
    const double err = loopPitchError(periodFor(0, 0), 182, periodFor(s.semitones, s.finetune), s.newLoopLength);
    EXPECT_LE(std::abs(err), s.worstCents + 1e-9);
}

// An existing finetune is part of the compensation.
TEST(ModConvStretch, KeepsExistingFinetune) {
    auto s = chooseStretch(182, -3, {12});
    const double err = loopPitchError(periodFor(12, -3), 182, periodFor(12 + s.semitones, s.finetune), s.newLoopLength);
    EXPECT_LT(std::abs(err), 8.0);
    EXPECT_EQ(s.oldFinetune, -3);
}

// Notes near the top of the table limit how far the sample can be transposed.
TEST(ModConvStretch, KeepsNotesOnTheTable) {
    auto s = chooseStretch(182, 0, {33});
    EXPECT_EQ(s.offTable, 0u);
    EXPECT_LE(33 + s.semitones, c_maxNote);
}

// A sine with 7 periods in a 182-sample loop must come out as 7 periods in the new loop, including
// across the wrap from the last sample of the loop to the first one.
TEST(ModConvStretch, ResampledLoopIsSeamless) {
    const unsigned loopStart = 100, loopLength = 182, newLoop = 252;
    // Silence before the loop, so that only a loop read as periodic gets the first samples right.
    std::vector<int16_t> source(loopStart + loopLength, 0);
    for (unsigned i = loopStart; i < source.size(); i++) {
        source[i] = int16_t(std::lround(16000 * std::sin(2 * c_pi * 7 * (double(i) - loopStart) / loopLength)));
    }
    unsigned newLoopStart;
    auto out = resampleLoop(source, loopStart, loopLength, newLoop, newLoopStart);
    EXPECT_EQ(newLoopStart % 2, 0u);
    EXPECT_EQ(out.size(), newLoopStart + newLoop);
    double worst = 0;
    for (unsigned j = 0; j < newLoop; j++) {
        const double expected = 16000 * std::sin(2 * c_pi * 7 * double(j) / newLoop);
        worst = std::max(worst, std::abs(out[newLoopStart + j] - expected));
    }
    EXPECT_LT(worst, 40.0);
    // The step across the wrap is a regular step of the sine.
    const double wrapStep = out[newLoopStart] - out[newLoopStart + newLoop - 1];
    const double expectedStep = 16000 * (std::sin(0.0) - std::sin(-2 * c_pi * 7 / newLoop));
    EXPECT_NEAR(wrapStep, expectedStep, 40.0);
}

TEST(ModConvStretch, RewritesPatterns) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(channels * 256, 0);
    const unsigned c2 = c_periodTable[12];
    setCell(p, channels, 0, 0, 2, c2, 0x3, 0x10);                 // stretched sample, tone portamento
    setCell(p, channels, 1, 0, 0, c2, 0x9, 0x10);                 // no sample number: still sample 2, 9xx
    setCell(p, channels, 2, 0, 2, 0, 0xe, 0x53);                  // E5x on the stretched sample
    setCell(p, channels, 3, 0, 0, c_periodTable[33], 0x1, 8);     // goes past the table, portamento up
    setCell(p, channels, 4, 0, 0, c_periodTable[28], 0x0, 0x47);  // arpeggio
    setCell(p, channels, 0, 1, 1, c2, 0x3, 0x10);                 // sample 1, not stretched
    setCell(p, channels, 1, 1, 1, 0, 0xe, 0x53);                  // E5x on sample 1
    const uint8_t orders[1] = {0};
    auto map = mapCells(p, channels, orders, 1);
    EXPECT_EQ(map.sample[(1 * channels) + 0], 2);
    EXPECT_EQ(map.sample[(2 * channels) + 1], 1);

    Stretch s;
    s.loopLength = 182;
    s.newLoopLength = 252;
    s.semitones = 5;
    s.finetune = 5;
    s.oldFinetune = 0;
    std::vector<std::optional<Stretch>> stretches(32);
    stretches[2] = s;
    auto warnings = rewritePatterns(p, map, stretches);

    EXPECT_EQ(cellPeriod(p, channels, 0, 0), c_periodTable[17]);
    EXPECT_EQ(cellParam(p, channels, 0, 0), 12);  // 0x10 / (252 / 182)
    EXPECT_EQ(cellPeriod(p, channels, 1, 0), c_periodTable[17]);
    EXPECT_EQ(cellParam(p, channels, 1, 0), 22);    // 0x10 * (252 / 182)
    EXPECT_EQ(cellParam(p, channels, 2, 0), 0x57);  // 3 + 5 = 8 is past 7: clamped
    EXPECT_EQ(cellPeriod(p, channels, 3, 0), c_periodTable[c_maxNote]);
    EXPECT_EQ(cellParam(p, channels, 3, 0), 6);  // 8 / (252 / 182)
    EXPECT_EQ(cellPeriod(p, channels, 0, 1), c2);
    EXPECT_EQ(cellParam(p, channels, 0, 1), 0x10);
    EXPECT_EQ(cellParam(p, channels, 1, 1), 0x53);

    ASSERT_EQ(warnings.size(), 3u);
    EXPECT_EQ(warnings[0], "sample 2: 1 notes transposed past the period table, clamped");
    EXPECT_EQ(warnings[1], "sample 2: 1 arpeggios reach past the period table");
    EXPECT_EQ(warnings[2], "sample 2: 1 E5x finetunes out of range after the shift, clamped");
}

// A pattern reached once with sample 1 and once with sample 2 current, at a cell without a sample.
TEST(ModConvStretch, MapsConflicts) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(3 * channels * 256, 0);
    setCell(p, channels, 0, 0, 0, c_periodTable[12], 0, 0);        // pattern 0: note, no sample
    setCell(p, channels, 64 + 0, 0, 1, c_periodTable[12], 0, 0);   // pattern 1: sample 1
    setCell(p, channels, 64 + 1, 0, 0, 0, 0xd, 0x00);              // break
    setCell(p, channels, 128 + 0, 0, 2, c_periodTable[12], 0, 0);  // pattern 2: sample 2
    setCell(p, channels, 128 + 1, 0, 0, 0, 0xd, 0x00);             // break

    const uint8_t once[2] = {1, 0};
    auto map = mapCells(p, channels, once, 2);
    EXPECT_EQ(map.sample[0], 1);
    EXPECT_FALSE(map.conflict[0]);
    // The rest of pattern 1 after the break is never reached.
    EXPECT_EQ(map.sample[(64 + 2) * channels], 0);

    const uint8_t twice[4] = {1, 0, 2, 0};
    map = mapCells(p, channels, twice, 4);
    EXPECT_EQ(map.sample[0], 1);
    EXPECT_TRUE(map.conflict[0]);
}

// Bxx skips a pattern that would otherwise switch the channel to another sample, and the song loops
// back to the start with B00, which has to end the walk.
TEST(ModConvStretch, MapsFollowBxx) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(3 * channels * 256, 0);
    setCell(p, channels, 0, 0, 1, c_periodTable[12], 0xb, 0x02);   // pattern 0: sample 1, jump to position 2
    setCell(p, channels, 64 + 0, 0, 2, c_periodTable[12], 0, 0);   // pattern 1: sample 2, skipped
    setCell(p, channels, 128 + 0, 0, 0, c_periodTable[12], 0, 0);  // pattern 2: note, no sample
    setCell(p, channels, 128 + 1, 0, 0, 0, 0xb, 0x00);             // back to position 0

    const uint8_t orders[3] = {0, 1, 2};
    auto map = mapCells(p, channels, orders, 3);
    EXPECT_EQ(map.sample[128 * channels], 1);
    EXPECT_FALSE(map.conflict[128 * channels]);
    EXPECT_EQ(map.sample[64 * channels], 0);
}

// Bxx and Dxx on one row land on the position after the Bxx target, at the Dxx row, and a Dxx past
// row 63 starts the next pattern at row 0, as the modplayer does.
TEST(ModConvStretch, MapsCombinedBxxDxx) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(4 * channels * 256, 0);
    setCell(p, channels, 0, 0, 0, 0, 0xb, 0x02);                    // pattern 0: jump to position 2
    setCell(p, channels, 0, 1, 0, 0, 0xd, 0x10);                    // and break to row 10
    setCell(p, channels, 128 + 10, 2, 0, c_periodTable[12], 0, 0);  // pattern 2, skipped
    setCell(p, channels, 192 + 10, 2, 3, c_periodTable[12], 0, 0);  // pattern 3: sample 3
    const uint8_t orders[4] = {0, 1, 2, 3};
    auto map = mapCells(p, channels, orders, 4);
    EXPECT_EQ(map.sample[(192 + 10) * channels + 2], 3);
    EXPECT_EQ(map.sample[(128 + 10) * channels + 2], 0);

    std::vector<uint8_t> q(2 * channels * 256, 0);
    setCell(q, channels, 0, 0, 0, 0, 0xd, 0x64);                  // pattern 0: break to row 64
    setCell(q, channels, 64 + 0, 0, 1, c_periodTable[12], 0, 0);  // pattern 1, row 0: sample 1
    map = mapCells(q, channels, orders, 2);
    EXPECT_EQ(map.sample[64 * channels], 1);
}

// The song wraps from its last position to the first, carrying the channel's current sample.
TEST(ModConvStretch, MapsAcrossTheSongWrap) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(2 * channels * 256, 0);
    setCell(p, channels, 5, 0, 0, c_periodTable[12], 0, 0);       // pattern 0: note, no sample
    setCell(p, channels, 64 + 0, 0, 2, c_periodTable[12], 0, 0);  // pattern 1: sample 2
    const uint8_t orders[2] = {0, 1};
    auto map = mapCells(p, channels, orders, 2);
    EXPECT_EQ(map.sample[5 * channels], 2);
}

namespace {

std::vector<std::optional<Stretch>> stretchSample2() {
    Stretch s;
    s.loopLength = 182;
    s.newLoopLength = 252;
    s.semitones = 5;
    s.finetune = 5;
    s.oldFinetune = 0;
    std::vector<std::optional<Stretch>> stretches(32);
    stretches[2] = s;
    return stretches;
}

}  // namespace

// A cell first reached with an unstretched sample, then with a stretched one, is left alone and
// reported for the stretched one.
TEST(ModConvStretch, ReportsCellsFirstPlayedWithAnotherSample) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(3 * channels * 256, 0);
    setCell(p, channels, 0, 0, 0, c_periodTable[12], 0, 0);        // pattern 0: note, no sample
    setCell(p, channels, 64 + 0, 0, 1, c_periodTable[12], 0, 0);   // pattern 1: sample 1
    setCell(p, channels, 64 + 1, 0, 0, 0, 0xd, 0x00);              // break
    setCell(p, channels, 128 + 0, 0, 2, c_periodTable[12], 0, 0);  // pattern 2: sample 2
    setCell(p, channels, 128 + 1, 0, 0, 0, 0xd, 0x00);             // break
    const uint8_t orders[4] = {1, 0, 2, 0};
    auto map = mapCells(p, channels, orders, 4);
    ASSERT_EQ(map.sample[0], 1);
    auto warnings = rewritePatterns(p, map, stretchSample2());
    EXPECT_EQ(cellPeriod(p, channels, 0, 0), c_periodTable[12]);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "sample 2: 1 cells first played with another sample, left as they were");
}

// An arpeggio without a note plays from the channel's last note, transposed.
TEST(ModConvStretch, ChecksArpeggiosWithoutANote) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(channels * 256, 0);
    setCell(p, channels, 0, 0, 2, c_periodTable[28], 0, 0);
    setCell(p, channels, 1, 0, 0, 0, 0x0, 0x07);  // 28 + 5 + 7 = 40, past the table
    setCell(p, channels, 2, 0, 0, 0, 0x0, 0x01);  // 28 + 5 + 1 = 34, fine
    const uint8_t orders[1] = {0};
    auto map = mapCells(p, channels, orders, 1);
    ASSERT_EQ(map.lastNotes[1 * channels].size(), 1u);
    EXPECT_EQ(map.lastNotes[1 * channels][0].first, 28);
    auto warnings = rewritePatterns(p, map, stretchSample2());
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "sample 2: 1 arpeggios reach past the period table");
}

// 300 on a stretched sample reuses a speed set with another sample: the compensated speed is written
// in. 5xy has no speed to write, and is reported.
TEST(ModConvStretch, CompensatesReusedPortamentoSpeed) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(channels * 256, 0);
    const unsigned c2 = c_periodTable[12];
    setCell(p, channels, 0, 0, 1, c2, 0x3, 0x10);  // speed 16, set with sample 1
    setCell(p, channels, 1, 0, 2, c2, 0x3, 0x00);  // sample 2 slides at it
    setCell(p, channels, 2, 0, 0, 0, 0x5, 0x01);   // and again, with a volume slide
    setCell(p, channels, 3, 0, 0, c2, 0x3, 0x08);  // a speed set with sample 2
    setCell(p, channels, 4, 0, 0, 0, 0x3, 0x00);   // reused on sample 2, already scaled
    const uint8_t orders[1] = {0};
    auto map = mapCells(p, channels, orders, 1);
    auto warnings = rewritePatterns(p, map, stretchSample2());
    EXPECT_EQ(cellParam(p, channels, 1, 0), 12);  // 16 / (252 / 182)
    EXPECT_EQ(cellParam(p, channels, 3, 0), 6);   // 8 / (252 / 182)
    EXPECT_EQ(cellParam(p, channels, 4, 0), 0);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "sample 2: 1 tone portamentos reuse a speed set with another sample, left as they were");
}

// The same position reached twice with the same samples but different slide speeds: the walk has to
// see both, so the 300 there is reported instead of being written for the first speed only.
TEST(ModConvStretch, WalksAgainWithAnotherSlideSpeed) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(3 * channels * 256, 0);
    setCell(p, channels, 0, 0, 0, 0, 0x3, 0x00);        // pattern 0: 300, no sample
    setCell(p, channels, 64 + 0, 0, 1, 0, 0x3, 0x10);   // pattern 1: speed 16 with sample 1
    setCell(p, channels, 64 + 1, 0, 2, 0, 0, 0);        // then sample 2
    setCell(p, channels, 128 + 0, 0, 1, 0, 0x3, 0x20);  // pattern 2: speed 32 with sample 1
    setCell(p, channels, 128 + 1, 0, 2, 0, 0xb, 0x01);  // then sample 2, back to position 1
    const uint8_t orders[3] = {1, 0, 2};
    auto map = mapCells(p, channels, orders, 3);
    EXPECT_EQ(map.speedFrom[0], 0xff);
    auto warnings = rewritePatterns(p, map, stretchSample2());
    EXPECT_EQ(cellParam(p, channels, 0, 0), 0);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "sample 2: 1 tone portamentos reuse a speed set with another sample, left as they were");
}

// An arpeggio without a note reached after a high note on one path and a low note on another is
// checked against the high one.
TEST(ModConvStretch, ChecksArpeggiosAgainstEveryLastNote) {
    const unsigned channels = 4;
    std::vector<uint8_t> p(3 * channels * 256, 0);
    setCell(p, channels, 0, 0, 0, 0, 0x0, 0x07);                   // pattern 0: arpeggio, no note
    setCell(p, channels, 64 + 0, 0, 2, c_periodTable[28], 0, 0);   // pattern 1: high note
    setCell(p, channels, 64 + 1, 0, 0, 0, 0xd, 0x00);              // break
    setCell(p, channels, 128 + 0, 0, 2, c_periodTable[12], 0, 0);  // pattern 2: low note
    setCell(p, channels, 128 + 1, 0, 0, 0, 0xd, 0x00);             // break
    const uint8_t orders[4] = {2, 0, 1, 0};
    auto map = mapCells(p, channels, orders, 4);
    EXPECT_EQ(map.lastNotes[0].size(), 2u);
    auto warnings = rewritePatterns(p, map, stretchSample2());
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "sample 2: 1 arpeggios reach past the period table");
}
