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

#include <stdint.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/system.h"
#include "fmt/format.h"
#include "gtest/gtest.h"
#include "gui/widgets/spusamples.h"
#include "supportpsx/sf2.h"
#include "supportpsx/spusample.h"

namespace {

using Samples = std::map<uint32_t, PCSX::Widgets::SPUSamples::Sample>;

// Writes `blocks` ADPCM blocks at `start`. Every data byte of block i is `seed + i`, bytes 2 and 3
// of each block also carry `tag`, and the last block's flags are `endFlags`.
void put(std::vector<uint8_t>& ram, uint32_t start, int blocks, uint8_t endFlags, uint8_t seed, uint16_t tag = 0) {
    for (int i = 0; i < blocks; i++) {
        uint8_t* b = ram.data() + start + i * 16;
        b[0] = 0;
        b[1] = i == blocks - 1 ? endFlags : 0;
        for (int j = 2; j < 16; j++) b[j] = uint8_t(seed + i);
        b[2] = uint8_t(tag);
        b[3] = uint8_t(tag >> 8);
    }
}

// A sample as the window holds it after key-ons at `start` with these loop register values.
void add(Samples& samples, const std::vector<uint8_t>& ram, uint32_t start, std::map<uint32_t, uint32_t> loops) {
    auto& s = samples[start];
    static_cast<PCSX::SPUSample::Extent&>(s) = PCSX::SPUSample::walk(ram.data(), start);
    s.signature = PCSX::SPUSample::signature(ram.data(), s);
    s.pitchesSignature = s.signature;
    s.keyOnLoops = std::move(loops);
    s.keyOns = 1;
}

// A bank Overlay::fromSF2 reads the way the export meant it: one sample per signature, each
// with its record.
::testing::AssertionResult readsBackWhole(const std::vector<uint8_t>& bank) {
    const auto samples = PCSX::SF2::read(bank.data(), bank.size());
    const auto records = PCSX::SPUSample::records(PCSX::SF2::readComment(bank.data(), bank.size()));
    std::set<std::string> names;
    for (auto& s : samples) {
        if (!names.insert(s.name).second) return ::testing::AssertionFailure() << "two samples named " << s.name;
    }
    if (records.size() != samples.size()) {
        return ::testing::AssertionFailure() << samples.size() << " samples, " << records.size() << " records";
    }
    return ::testing::AssertionSuccess();
}

class TestSystem final : public PCSX::System {
  public:
    void softReset() override {}
    void hardReset() override {}
    void biosPutc(int) override {}
    const PCSX::Arguments& getArgs() const override { throw std::logic_error("unused"); }
    void printf(std::string&&) override {}
    void log(PCSX::LogClass, std::string&&) override {}
    void message(std::string&&) override {}
    void luaMessage(const std::string&, bool) override {}
    void update(bool) override {}
    void close() override {}
    void purgeAllEvents() override {}
    void testQuit(int) override {}
};

// The export's messages go through the translation table, which lives on g_system.
class SPUSamplesExport : public ::testing::Test {
  protected:
    void SetUp() override {
        m_oldSystem = PCSX::g_system;
        PCSX::g_system = &m_system;
    }
    void TearDown() override { PCSX::g_system = m_oldSystem; }

    TestSystem m_system;
    PCSX::System* m_oldSystem = nullptr;
};

}  // namespace

// The same data at two addresses, keyed on with the loop register at different offsets into it:
// a pack keyed by signature can only hold one of the two loops.
TEST_F(SPUSamplesExport, TwoLoopsForOneSignatureAreNotBothExported) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    put(ram, 0x1000, 4, 3, 0x10);
    put(ram, 0x2000, 4, 3, 0x10);
    Samples samples;
    add(samples, ram, 0x1000, {{0x1010, 1}});
    add(samples, ram, 0x2000, {{0x2020, 1}});
    const auto bank = PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples);
    if (!bank.empty()) EXPECT_TRUE(readsBackWhole(bank));
}

// More samples than the comment field has room for records.
TEST_F(SPUSamplesExport, RecordsThatDoNotFitFailTheExport) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    Samples samples;
    for (uint16_t i = 0; i < 2000; i++) {
        const uint32_t start = 0x1000 + i * 16;
        put(ram, start, 1, 1, 0x20, i);
        add(samples, ram, start, {});
    }
    const auto bank = PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples);
    if (!bank.empty()) EXPECT_TRUE(readsBackWhole(bank));
}

// Control for the one above: a bank whose records fit loses none of them.
TEST_F(SPUSamplesExport, RecordsThatFitAreAllWritten) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    Samples samples;
    for (uint16_t i = 0; i < 1000; i++) {
        const uint32_t start = 0x1000 + i * 16;
        put(ram, start, 1, 1, 0x20, i);
        add(samples, ram, start, {});
    }
    const auto bank = PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples);
    ASSERT_FALSE(bank.empty());
    EXPECT_TRUE(readsBackWhole(bank));
    EXPECT_EQ(PCSX::SF2::read(bank.data(), bank.size()).size(), 1000u);
}

TEST_F(SPUSamplesExport, TwoLoopsForOneSignatureSayWhy) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    put(ram, 0x1000, 4, 3, 0x10);
    put(ram, 0x2000, 4, 3, 0x10);
    Samples samples;
    add(samples, ram, 0x1000, {{0x1010, 1}});
    add(samples, ram, 0x2000, {{0x2020, 1}});
    std::string error;
    EXPECT_TRUE(PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples, &error).empty());
    EXPECT_NE(error.find("01000 (loop at 01010)"), std::string::npos) << error;
    EXPECT_NE(error.find("02000 (loop at 02020)"), std::string::npos) << error;
}

// The same loop offset at both addresses is one sample, with that loop.
TEST_F(SPUSamplesExport, CopiesWithTheSameLoopAreOneSample) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    put(ram, 0x1000, 4, 3, 0x10);
    put(ram, 0x2000, 4, 3, 0x10);
    Samples samples;
    add(samples, ram, 0x1000, {{0x1010, 1}});
    add(samples, ram, 0x2000, {{0x2010, 2}});
    std::string error;
    const auto bank = PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples, &error);
    ASSERT_TRUE(error.empty()) << error;
    const auto read = PCSX::SF2::read(bank.data(), bank.size());
    ASSERT_EQ(read.size(), 1u);
    EXPECT_TRUE(read[0].loops);
    EXPECT_EQ(read[0].loopStart, 28u);
}

// A copy keyed on with no loop register logged says nothing about the loop; the other copy's
// loop is exported, whichever address comes first.
TEST_F(SPUSamplesExport, ACopyWithNoLoggedLoopTakesTheOther) {
    for (bool firstKnown : {false, true}) {
        SCOPED_TRACE(firstKnown);
        std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
        put(ram, 0x1000, 4, 3, 0x10);
        put(ram, 0x2000, 4, 3, 0x10);
        Samples samples;
        add(samples, ram, 0x1000,
            firstKnown ? std::map<uint32_t, uint32_t>{{0x1020, 1}} : std::map<uint32_t, uint32_t>{});
        add(samples, ram, 0x2000,
            firstKnown ? std::map<uint32_t, uint32_t>{} : std::map<uint32_t, uint32_t>{{0x2020, 1}});
        std::string error;
        const auto bank = PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples, &error);
        ASSERT_TRUE(error.empty()) << error;
        const auto read = PCSX::SF2::read(bank.data(), bank.size());
        ASSERT_EQ(read.size(), 1u);
        EXPECT_TRUE(read[0].loops);
        EXPECT_EQ(read[0].loopStart, 56u);
    }
}

// A copy whose logged loops all fall outside it is a one-shot, which conflicts with a loop.
TEST_F(SPUSamplesExport, AOneShotCopyConflictsWithALoopedOne) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    put(ram, 0x1000, 4, 3, 0x10);
    put(ram, 0x2000, 4, 3, 0x10);
    Samples samples;
    add(samples, ram, 0x1000, {{0x8000, 1}});
    add(samples, ram, 0x2000, {{0x2020, 1}});
    std::string error;
    EXPECT_TRUE(PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples, &error).empty());
    EXPECT_NE(error.find("01000 (no loop)"), std::string::npos) << error;
}

TEST_F(SPUSamplesExport, RecordsThatDoNotFitSayWhy) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    Samples samples;
    for (uint16_t i = 0; i < 2000; i++) {
        const uint32_t start = 0x1000 + i * 16;
        put(ram, start, 1, 1, 0x20, i);
        add(samples, ram, start, {});
    }
    std::string error;
    EXPECT_TRUE(PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples, &error).empty());
    EXPECT_NE(error.find("2000 samples"), std::string::npos) << error;
}

// The conflict names the copies the two loops were logged at, not a copy that took one of them.
TEST_F(SPUSamplesExport, AConflictNamesTheCopiesTheLoopsCameFrom) {
    std::vector<uint8_t> ram(PCSX::SPUSample::c_ramSize);
    put(ram, 0x1000, 4, 3, 0x10);
    put(ram, 0x2000, 4, 3, 0x10);
    put(ram, 0x3000, 4, 3, 0x10);
    Samples samples;
    add(samples, ram, 0x1000, {});
    add(samples, ram, 0x2000, {{0x2010, 1}});
    add(samples, ram, 0x3000, {{0x3020, 1}});
    std::string error;
    EXPECT_TRUE(PCSX::Widgets::SPUSamples::exportSF2(ram.data(), samples, &error).empty());
    EXPECT_NE(error.find("02000 (loop at 02010)"), std::string::npos) << error;
    EXPECT_NE(error.find("03000 (loop at 03020)"), std::string::npos) << error;
}
