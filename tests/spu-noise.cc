/*
 * Tests for the SPU noise generator's clock.
 *
 * Measured on SCPH-1000 and SCPH-1001: with P = 0x20000 >> Shift samples, the
 * LFSR clocks at the start of each quarter of P, and Step adds one clock half
 * way into Step of the four quarters (Step 2 uses opposite quarters), never more
 * than one clock per sample. So the gaps between clocks are only P / 4 and
 * P / 8 samples.
 */

#include <stdint.h>

#include <vector>

#include "gtest/gtest.h"
#include "spu/noise.h"

namespace {

// Gaps, in samples, between successive LFSR clocks over the given number of samples.
std::vector<unsigned> clockGaps(unsigned shift, unsigned step, unsigned samples) {
    PCSX::SPU::NoiseGenerator noise;
    noise.setClock((shift << 2) | step);
    std::vector<unsigned> gaps;
    int last = noise.getVal();
    unsigned lastClock = 0;
    bool seen = false;
    for (unsigned i = 1; i <= samples; i++) {
        noise.step();
        int val = noise.getVal();
        if (val == last) continue;
        last = val;
        if (seen) gaps.push_back(i - lastClock);
        seen = true;
        lastClock = i;
    }
    return gaps;
}

// The gaps must repeat the pattern, starting anywhere in it.
void expectCyclic(const std::vector<unsigned> &gaps, const std::vector<unsigned> &pattern) {
    ASSERT_GE(gaps.size(), pattern.size() * 4);
    size_t rotation = pattern.size();
    for (size_t r = 0; r < pattern.size(); r++) {
        bool match = true;
        for (size_t i = 0; i < gaps.size() && match; i++) match = gaps[i] == pattern[(i + r) % pattern.size()];
        if (match) {
            rotation = r;
            break;
        }
    }
    EXPECT_LT(rotation, pattern.size()) << "gap sequence is not a rotation of the measured pattern";
}

}  // namespace

TEST(SPUNoise, shift13Step3ClocksOnEighthSlots) {
    // P = 16: quarters 4 samples apart, three of them split in half.
    expectCyclic(clockGaps(13, 3, 16 * 64), {2, 2, 2, 2, 2, 2, 4});
}

TEST(SPUNoise, shift10Step1ClocksOnEighthSlots) {
    // P = 128: one quarter split in half.
    expectCyclic(clockGaps(10, 1, 128 * 64), {16, 16, 32, 32, 32});
}

TEST(SPUNoise, shift12Step2SplitsOppositeQuarters) {
    // P = 32: quarters 1 and 3 split.
    expectCyclic(clockGaps(12, 2, 32 * 64), {4, 4, 8, 4, 4, 8});
}

TEST(SPUNoise, step0ClocksEveryQuarter) {
    expectCyclic(clockGaps(13, 0, 16 * 64), {4});
    expectCyclic(clockGaps(10, 0, 128 * 64), {32});
}

TEST(SPUNoise, fastestClocksOncePerSample) {
    expectCyclic(clockGaps(15, 0, 256), {1});
    expectCyclic(clockGaps(15, 3, 256), {1});
    // P = 8: seven of the eight slots, one sample each.
    expectCyclic(clockGaps(14, 3, 256), {1, 1, 1, 1, 1, 1, 2});
}
