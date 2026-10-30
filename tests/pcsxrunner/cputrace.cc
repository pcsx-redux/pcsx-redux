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

#include "core/cputrace.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "core/disr3000a.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "gtest/gtest.h"
#include "main/main.h"

// A captured trace record, rendered back through PlaybackValueSource, must be
// byte-identical to the live annotated disassembly taken at the moment of capture.
// The capture observer fires while the CPU still holds the pre-execution state
// the record was built from, so both renders see the same instant.

namespace {

enum Category {
    C_LB,
    C_LBU,
    C_LH,
    C_LHU,
    C_LW,
    C_LWL,
    C_LWR,
    C_SB,
    C_SH,
    C_SW,
    C_SWL,
    C_SWR,
    C_LWC2,
    C_SWC2,
    C_MFHI,
    C_MFLO,
    C_MTHI,
    C_MTLO,
    C_MFC0,
    C_MTC0,
    C_MFC2,
    C_CFC2,
    C_MTC2,
    C_CTC2,
    C_BEQ,
    C_BNE,
    C_BLEZ,
    C_BGTZ,
    C_BCOND,
    C_JR,
    C_JALR,
    C_J,
    C_JAL,
    C_GTE_CMD,
    C_OTHER,
    C_COUNT,
};

const char* const s_categoryNames[C_COUNT] = {
    "LB",   "LBU",  "LH",   "LHU",  "LW",    "LWL",  "LWR",  "SB",   "SH",   "SW",      "SWL",   "SWR",
    "LWC2", "SWC2", "MFHI", "MFLO", "MTHI",  "MTLO", "MFC0", "MTC0", "MFC2", "CFC2",    "MTC2",  "CTC2",
    "BEQ",  "BNE",  "BLEZ", "BGTZ", "BCOND", "JR",   "JALR", "J",    "JAL",  "GTE cmd", "other",
};

Category classify(uint32_t code) {
    uint32_t op = code >> 26;
    uint32_t rs = (code >> 21) & 0x1f;
    uint32_t funct = code & 0x3f;
    switch (op) {
        case 0x00:
            switch (funct) {
                case 0x08:
                    return C_JR;
                case 0x09:
                    return C_JALR;
                case 0x10:
                    return C_MFHI;
                case 0x11:
                    return C_MTHI;
                case 0x12:
                    return C_MFLO;
                case 0x13:
                    return C_MTLO;
            }
            return C_OTHER;
        case 0x01:
            return C_BCOND;
        case 0x02:
            return C_J;
        case 0x03:
            return C_JAL;
        case 0x04:
            return C_BEQ;
        case 0x05:
            return C_BNE;
        case 0x06:
            return C_BLEZ;
        case 0x07:
            return C_BGTZ;
        case 0x10:
            if (rs == 0) return C_MFC0;
            if (rs == 4) return C_MTC0;
            return C_OTHER;
        case 0x12:
            if (rs & 0x10) return C_GTE_CMD;
            if (rs == 0) return C_MFC2;
            if (rs == 2) return C_CFC2;
            if (rs == 4) return C_MTC2;
            if (rs == 6) return C_CTC2;
            return C_OTHER;
        case 0x20:
            return C_LB;
        case 0x21:
            return C_LH;
        case 0x22:
            return C_LWL;
        case 0x23:
            return C_LW;
        case 0x24:
            return C_LBU;
        case 0x25:
            return C_LHU;
        case 0x26:
            return C_LWR;
        case 0x28:
            return C_SB;
        case 0x29:
            return C_SH;
        case 0x2a:
            return C_SWL;
        case 0x2b:
            return C_SW;
        case 0x2e:
            return C_SWR;
        case 0x32:
            return C_LWC2;
        case 0x3a:
            return C_SWC2;
    }
    return C_OTHER;
}

struct Stats {
    // Bounds runtime and trace memory (32 bytes per record); past this the
    // observer turns capture off and the program runs on untraced.
    uint64_t maxChecks = 0;
    uint64_t checked = 0;
    uint64_t mismatches = 0;
    uint64_t perCategory[C_COUNT] = {};
    uint64_t perCategoryMismatch[C_COUNT] = {};
    uint64_t firstSeen[C_COUNT] = {};
    std::vector<std::string> examples;
};

Stats* s_stats = nullptr;

void observer(const PCSX::TraceEntry& e) {
    Stats& s = *s_stats;
    if (s.checked >= s.maxChecks) {
        PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
            .get<PCSX::Emulator::DebugSettings::Trace>()
            .value = false;
        return;
    }
    std::string live = PCSX::Disasm::asString(e.code, 0, e.pc, nullptr, true);
    PCSX::PlaybackValueSource source(e);
    std::string played = PCSX::Disasm::asString(e.code, 0, e.pc, nullptr, true, &source);
    Category c = classify(e.code);
    if (s.perCategory[c] == 0) s.firstSeen[c] = s.checked;
    s.checked++;
    s.perCategory[c]++;
    if (live != played) {
        s.mismatches++;
        s.perCategoryMismatch[c]++;
        if (s.examples.size() < 20) {
            s.examples.push_back("live:     " + live + "\n    playback: " + played);
        }
    }
}

Stats runOne(const char* exe, bool debugger, uint64_t maxChecks) {
    Stats stats;
    stats.maxChecks = maxChecks;
    s_stats = &stats;
    PCSX::CpuTrace::s_captureObserver = observer;
    // -debugger also routes capture through the debug-enabled interpreter path.
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        debugger ? "-debugger" : "-no-debugger", "-trace", "-loadexe", exe);
    int ret = invoker.invoke();
    PCSX::CpuTrace::s_captureObserver = nullptr;
    s_stats = nullptr;
    EXPECT_EQ(ret, 0) << exe;

    fprintf(stderr, "[cputrace] %s: checked %llu instructions, %llu mismatches\n", exe,
            (unsigned long long)stats.checked, (unsigned long long)stats.mismatches);
    for (int i = 0; i < C_COUNT; i++) {
        fprintf(stderr, "[cputrace]   %-8s %10llu checked, %llu mismatched, first at #%llu\n", s_categoryNames[i],
                (unsigned long long)stats.perCategory[i], (unsigned long long)stats.perCategoryMismatch[i],
                (unsigned long long)stats.firstSeen[i]);
    }
    for (auto& ex : stats.examples) {
        fprintf(stderr, "[cputrace]   MISMATCH\n    %s\n", ex.c_str());
    }
    return stats;
}

}  // namespace

TEST(CpuTrace, PlaybackMatchesLive) {
    // Each program is chosen for the operand classes it reaches: cpu (LWL/LWR,
    // HI/LO), cop0 (MFC0, debugger path), gte (MFC2/CFC2), psyqo (LH/LHU,
    // LWC2/SWC2; runs ~15M instructions to completion), memcpy (SWL/SWR). Caps
    // keep the run bounded.
    struct {
        const char* exe;
        bool debugger;
        uint64_t maxChecks;
    } runs[] = {
        {"src/mips/tests/cpu/cpu.ps-exe", false, 8'000'000},
        {"src/mips/tests/cop0/cop0.ps-exe", true, 8'000'000},
        {"src/mips/tests/gte/gte.ps-exe", false, 8'000'000},
        {"src/mips/tests/psyqo/psyqo-tests.ps-exe", false, 16'000'000},
        {"src/mips/tests/memcpy/memcpy.ps-exe", false, 4'000'000},
    };
    uint64_t total = 0, totalMismatches = 0;
    uint64_t coverage[C_COUNT] = {};
    for (auto& run : runs) {
        Stats s = runOne(run.exe, run.debugger, run.maxChecks);
        total += s.checked;
        totalMismatches += s.mismatches;
        for (int i = 0; i < C_COUNT; i++) coverage[i] += s.perCategory[i];
    }
    fprintf(stderr, "[cputrace] TOTAL: checked %llu instructions, %llu mismatches\n", (unsigned long long)total,
            (unsigned long long)totalMismatches);
    EXPECT_GT(total, 0u);
    EXPECT_EQ(totalMismatches, 0u);
    // Every operand-bearing class the playback source has to reconstruct must have
    // been exercised at least once, or the comparison proves less than it claims.
    for (int i = 0; i < C_COUNT; i++) {
        EXPECT_GT(coverage[i], 0u) << "no instruction of class " << s_categoryNames[i] << " was traced";
    }
}

namespace {

// Save, load and text export, checked from inside the run so the emulator that
// owns the live store is still up. Once kRecords records exist, the live store is
// saved, loaded into a second store, saved again, and both files must match byte
// for byte; the loaded store's text export must match the live annotated lines
// collected as each record was captured.
constexpr size_t kRecords = 200'000;

struct RoundTrip {
    std::vector<std::string> liveLines;
    bool done = false;
    bool loaded = false;
    bool sameBytes = false;
    size_t loadedSize = 0;
    size_t textMismatches = 0;
    size_t textLines = 0;
    std::string firstMismatch;
};

RoundTrip* s_roundTrip = nullptr;

void roundTripObserver(const PCSX::TraceEntry& e) {
    RoundTrip& r = *s_roundTrip;
    if (r.done) return;
    r.liveLines.push_back(PCSX::Disasm::asString(e.code, 0, e.pc, nullptr, true));
    if (r.liveLines.size() < kRecords) return;
    r.done = true;
    PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
        .get<PCSX::Emulator::DebugSettings::Trace>()
        .value = false;

    PCSX::IO<PCSX::BufferFile> saved = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
    PCSX::g_emulator->m_cpuTrace->save(saved);
    saved->rSeek(0, SEEK_SET);
    PCSX::CpuTrace copy;
    r.loaded = copy.load(saved);
    r.loadedSize = copy.size();
    PCSX::IO<PCSX::BufferFile> resaved = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
    copy.save(resaved);
    auto a = saved->borrow();
    auto b = resaved->borrow();
    r.sameBytes = a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;

    PCSX::IO<PCSX::BufferFile> text = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
    copy.exportText(text);
    auto t = text->borrow();
    std::string all(reinterpret_cast<const char*>(t.data()), t.size());
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) nl = all.size();
        std::string line = all.substr(pos, nl - pos);
        if (r.textLines >= r.liveLines.size() || line != r.liveLines[r.textLines]) {
            if (r.textMismatches++ == 0) r.firstMismatch = line;
        }
        r.textLines++;
        pos = nl + 1;
    }
}

struct Cycles {
    uint64_t first = PCSX::kUnknownCycle;
    uint64_t last = 0;
    size_t records = 0;
    size_t unknown = 0;
    size_t backwards = 0;
    size_t mismatched = 0;
};
Cycles* s_cycles = nullptr;

void cycleObserver(const PCSX::TraceEntry& e) {
    Cycles& c = *s_cycles;
    if (c.records >= 100'000) {
        PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
            .get<PCSX::Emulator::DebugSettings::Trace>()
            .value = false;
        return;
    }
    if (e.cycle == PCSX::kUnknownCycle) c.unknown++;
    if (e.cycle != PCSX::g_emulator->m_cpu->m_regs.cycle) c.mismatched++;
    if (c.records > 0 && e.cycle < c.last) c.backwards++;
    if (c.records == 0) c.first = e.cycle;
    c.last = e.cycle;
    c.records++;
}

template <typename T>
void put(PCSX::IO<PCSX::BufferFile>& file, T value) {
    file->write(&value, sizeof(value));
}

template <typename T>
T get(PCSX::IO<PCSX::BufferFile>& file) {
    T value = 0;
    file->read(&value, sizeof(value));
    return value;
}

size_t s_limitCalls = 0;
void limitObserver(const PCSX::TraceEntry&) {
    if (s_limitCalls++ == 0) PCSX::g_emulator->m_cpuTrace->setLimit(1000);
}

}  // namespace

TEST(CpuTrace, SaveLoadAndTextExport) {
    RoundTrip r;
    s_roundTrip = &r;
    PCSX::CpuTrace::s_captureObserver = roundTripObserver;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-trace", "-loadexe", "src/mips/tests/cpu/cpu.ps-exe");
    int ret = invoker.invoke();
    PCSX::CpuTrace::s_captureObserver = nullptr;
    s_roundTrip = nullptr;
    EXPECT_EQ(ret, 0);
    ASSERT_TRUE(r.done);
    EXPECT_TRUE(r.loaded);
    EXPECT_EQ(r.loadedSize, kRecords);
    EXPECT_TRUE(r.sameBytes);
    EXPECT_EQ(r.textLines, kRecords);
    EXPECT_EQ(r.textMismatches, 0u) << "first mismatching line: " << r.firstMismatch;
    fprintf(stderr, "[cputrace] round trip: %zu records, %zu text lines, %zu mismatches\n", r.loadedSize, r.textLines,
            r.textMismatches);
}

TEST(CpuTrace, LoadRejectsGarbage) {
    PCSX::IO<PCSX::BufferFile> junk = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
    junk->writeString("this is not a trace file at all");
    junk->rSeek(0, SEEK_SET);
    PCSX::CpuTrace trace;
    EXPECT_FALSE(trace.load(junk));
    EXPECT_EQ(trace.size(), 0u);
}

TEST(CpuTrace, CaptureStopsAtLimit) {
    s_limitCalls = 0;
    PCSX::CpuTrace::s_captureObserver = limitObserver;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-trace", "-loadexe", "src/mips/tests/cpu/cpu.ps-exe");
    int ret = invoker.invoke();
    PCSX::CpuTrace::s_captureObserver = nullptr;
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(s_limitCalls, 1000u);
}

TEST(CpuTrace, RecordsCarryTheCycleCounter) {
    Cycles c;
    s_cycles = &c;
    PCSX::CpuTrace::s_captureObserver = cycleObserver;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-trace", "-loadexe", "src/mips/tests/cpu/cpu.ps-exe");
    int ret = invoker.invoke();
    PCSX::CpuTrace::s_captureObserver = nullptr;
    s_cycles = nullptr;
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(c.records, 100'000u);
    EXPECT_EQ(c.unknown, 0u);
    EXPECT_EQ(c.mismatched, 0u);
    EXPECT_EQ(c.backwards, 0u);
    // At least one cycle per instruction.
    EXPECT_GE(c.last - c.first, c.records - 1);
    fprintf(stderr, "[cputrace] cycles: %zu records spanning %llu cycles\n", c.records,
            static_cast<unsigned long long>(c.last - c.first));
}

TEST(CpuTrace, LoadsVersion1Files) {
    const uint32_t records[2][8] = {
        {0xbfc00000, 0x3c080013, 1, 2, 3, 4, 5, 6},
        {0xbfc00004, 0x3508243f, 7, 8, 9, 10, 11, 12},
    };
    PCSX::IO<PCSX::BufferFile> v1 = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
    v1->write("PCSXTRC", 8);
    put<uint32_t>(v1, 1);
    put<uint32_t>(v1, 32);
    put<uint64_t>(v1, 2);
    v1->write(records, sizeof(records));
    v1->rSeek(0, SEEK_SET);
    PCSX::CpuTrace trace;
    ASSERT_TRUE(trace.load(v1));
    ASSERT_EQ(trace.size(), 2u);
    for (size_t i = 0; i < 2; i++) {
        const PCSX::TraceEntry& e = trace[i];
        EXPECT_EQ(e.pc, records[i][0]);
        EXPECT_EQ(e.code, records[i][1]);
        EXPECT_EQ(e.regRs, records[i][2]);
        EXPECT_EQ(e.memValue, records[i][7]);
        EXPECT_EQ(e.cycle, PCSX::kUnknownCycle);
    }

    // Saving writes the current version, which loads back unchanged.
    PCSX::IO<PCSX::BufferFile> v2 = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
    ASSERT_TRUE(trace.save(v2));
    v2->rSeek(8, SEEK_SET);
    EXPECT_EQ(get<uint32_t>(v2), 2u);
    EXPECT_EQ(get<uint32_t>(v2), sizeof(PCSX::TraceEntry));
    v2->rSeek(0, SEEK_SET);
    PCSX::CpuTrace again;
    ASSERT_TRUE(again.load(v2));
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(again[1].pc, records[1][0]);
    EXPECT_EQ(again[1].cycle, PCSX::kUnknownCycle);

    // A truncated version 1 file is refused and leaves the store empty.
    PCSX::IO<PCSX::BufferFile> cut = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
    cut->write("PCSXTRC", 8);
    put<uint32_t>(cut, 1);
    put<uint32_t>(cut, 32);
    put<uint64_t>(cut, 2);
    cut->write(records, sizeof(records[0]));
    cut->rSeek(0, SEEK_SET);
    EXPECT_FALSE(again.load(cut));
    EXPECT_EQ(again.size(), 0u);
}

TEST(CpuTrace, LoadRefusesATruncatedHeader) {
    // Both versions, cut right after the record size: there is no record count to read.
    for (uint32_t version : {1u, 2u}) {
        PCSX::IO<PCSX::BufferFile> cut = new PCSX::BufferFile(PCSX::FileOps::READWRITE);
        cut->write("PCSXTRC", 8);
        put<uint32_t>(cut, version);
        put<uint32_t>(cut, version == 1 ? 32 : sizeof(PCSX::TraceEntry));
        cut->rSeek(0, SEEK_SET);
        PCSX::CpuTrace trace;
        EXPECT_FALSE(trace.load(cut)) << "version " << version;
        EXPECT_EQ(trace.size(), 0u);
    }
}
