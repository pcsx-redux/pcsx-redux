/*
 * Tests for the SPU voice volume sweep's lazy CPU-side reader.
 *
 * The reader jumps over the gap between two accesses in one call. It has to
 * land on the same level as the SPU thread's track, which steps once per
 * sample, however long the gap: the slowest sweep rate steps every 1 << 20
 * samples, so a gap of several of those must still count every step.
 */

#include <stdint.h>

#include "gtest/gtest.h"
#include "spu/volume.h"

namespace {

struct Case {
    int16_t start;  // fixed-mode value written first, to set the starting level
    uint16_t sweep;
};

constexpr Case c_cases[] = {
    {0, 0x807c},                       // linear increase, shift 31 step 0: one step per 1 << 20 samples
    {0, 0x8000 | (20 << 2)},           // linear increase
    {0, 0xc000 | (14 << 2) | 1},       // exponential increase, crosses the knee
    {0x3fff, 0xe000 | (13 << 2) | 2},  // exponential decrease from the top
    {0x3fff, 0xa000 | (16 << 2)},      // linear decrease
    {0, 0x9000 | (12 << 2)},           // linear increase, negative phase
    {0x4001, 0xf000 | (15 << 2)},      // exponential decrease from a negative level
};

}  // namespace

TEST(SPUVoiceVolume, readerMatchesPerSampleSteppingAcrossLongGaps) {
    constexpr uint64_t c_end = (9u << 20) + 12345;
    for (const auto &c : c_cases) {
        PCSX::SPU::VoiceVolume once, every, mix;
        for (auto *v : {&once, &every}) {
            v->setLeft(c.start, 0);
            v->setLeft(static_cast<int16_t>(c.sweep), 0);
        }
        mix.applyLeft(c.start);
        mix.applyLeft(static_cast<int16_t>(c.sweep));
        uint16_t first = every.currentLeft(0);
        bool moved = false;
        for (uint64_t n = 1; n <= c_end; n++) {
            mix.step();
            const uint16_t v = every.currentLeft(n);
            if (v != first) moved = true;
            ASSERT_EQ(static_cast<int16_t>(v) >> 1, mix.left()) << "sweep " << std::hex << c.sweep << " sample " << n;
        }
        EXPECT_TRUE(moved) << "sweep " << std::hex << c.sweep << " never moved, the case tests nothing";
        EXPECT_EQ(once.currentLeft(c_end), every.currentLeft(c_end)) << "sweep " << std::hex << c.sweep;
    }
}
