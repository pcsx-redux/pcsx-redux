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

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/callprofiler.h"
#include "gtest/gtest.h"
#include "main/main.h"

// src/mips/tests/profiler is a call tree whose instruction counts are written down in its
// source. Profiling it from boot and keeping only what runs under main has to give those
// counts exactly, on every call path. The profile files are read back with parsers written
// here, independent of the serializers that produced them.

namespace {

struct Counts {
    uint64_t instructions = 0;
    uint64_t calls = 0;
    bool operator==(const Counts& other) const { return instructions == other.instructions && calls == other.calls; }
};

std::ostream& operator<<(std::ostream& os, const Counts& c) {
    return os << "{instructions " << c.instructions << ", calls " << c.calls << "}";
}

std::string runControl(const char* option, const char* fileName) {
    auto path = (std::filesystem::temp_directory_path() / fileName).string();
    std::filesystem::remove(path);
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        option, path.c_str(), "-loadexe", "src/mips/tests/profiler/profiler.elf");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
    std::ifstream in(path, std::ios::binary);
    std::stringstream data;
    data << in.rdbuf();
    return data.str();
}

// Minimal protobuf wire reader.
struct Wire {
    // Owns its bytes: sub-messages are read from the temporaries bytes() returns.
    std::string data;
    const uint8_t* ptr;
    const uint8_t* end;
    bool ok = true;
    explicit Wire(std::string s)
        : data(std::move(s)), ptr(reinterpret_cast<const uint8_t*>(data.data())), end(ptr + data.size()) {}
    Wire(const Wire&) = delete;
    bool done() const { return !ok || ptr >= end; }
    uint64_t varint() {
        uint64_t v = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (ptr >= end) {
                ok = false;
                return 0;
            }
            uint8_t b = *ptr++;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    std::string bytes() {
        uint64_t size = varint();
        if (size > uint64_t(end - ptr)) {
            ok = false;
            return {};
        }
        std::string r(reinterpret_cast<const char*>(ptr), size);
        ptr += size;
        return r;
    }
    void skip(unsigned type) {
        switch (type) {
            case 0:
                varint();
                break;
            case 1:
                ptr += 8;
                break;
            case 2:
                bytes();
                break;
            case 5:
                ptr += 4;
                break;
            default:
                ok = false;
        }
    }
};

// A repeated integer field, packed or not.
void readInts(Wire& w, unsigned type, std::vector<uint64_t>& out) {
    if (type == 2) {
        Wire packed(w.bytes());
        while (!packed.done()) out.push_back(packed.varint());
    } else {
        out.push_back(w.varint());
    }
}

// Function name per call path under main, joined with '/', from a pprof profile.
std::map<std::string, Counts> readPprof(const std::string& data) {
    std::vector<std::string> strings;
    std::vector<std::string> sampleTypes;
    std::map<uint64_t, uint64_t> functionNames;
    std::map<uint64_t, uint64_t> locationFunctions;
    std::vector<std::pair<std::vector<uint64_t>, std::vector<uint64_t>>> samples;
    std::vector<uint64_t> sampleTypeNames;

    Wire w(data);
    while (!w.done()) {
        uint64_t tag = w.varint();
        unsigned type = tag & 7;
        switch (tag >> 3) {
            case 1: {
                Wire vt(w.bytes());
                while (!vt.done()) {
                    uint64_t t = vt.varint();
                    if ((t >> 3) == 1) {
                        sampleTypeNames.push_back(vt.varint());
                    } else {
                        vt.skip(t & 7);
                    }
                }
                break;
            }
            case 2: {
                Wire s(w.bytes());
                std::vector<uint64_t> locations, values;
                while (!s.done()) {
                    uint64_t t = s.varint();
                    if ((t >> 3) == 1) {
                        readInts(s, t & 7, locations);
                    } else if ((t >> 3) == 2) {
                        readInts(s, t & 7, values);
                    } else {
                        s.skip(t & 7);
                    }
                }
                samples.emplace_back(std::move(locations), std::move(values));
                break;
            }
            case 4: {
                Wire l(w.bytes());
                uint64_t id = 0, function = 0;
                while (!l.done()) {
                    uint64_t t = l.varint();
                    if ((t >> 3) == 1) {
                        id = l.varint();
                    } else if ((t >> 3) == 4) {
                        Wire line(l.bytes());
                        while (!line.done()) {
                            uint64_t lt = line.varint();
                            if ((lt >> 3) == 1) {
                                function = line.varint();
                            } else {
                                line.skip(lt & 7);
                            }
                        }
                    } else {
                        l.skip(t & 7);
                    }
                }
                locationFunctions[id] = function;
                break;
            }
            case 5: {
                Wire f(w.bytes());
                uint64_t id = 0, name = 0;
                while (!f.done()) {
                    uint64_t t = f.varint();
                    if ((t >> 3) == 1) {
                        id = f.varint();
                    } else if ((t >> 3) == 2) {
                        name = f.varint();
                    } else {
                        f.skip(t & 7);
                    }
                }
                functionNames[id] = name;
                break;
            }
            case 6:
                strings.push_back(w.bytes());
                break;
            default:
                w.skip(type);
        }
    }
    EXPECT_TRUE(w.ok) << "malformed pprof";
    EXPECT_FALSE(strings.empty());
    EXPECT_TRUE(strings.empty() || strings[0].empty()) << "string_table[0] must be empty";

    auto str = [&](uint64_t i) -> std::string { return i < strings.size() ? strings[i] : "<bad string>"; };
    int instructionsIndex = -1, callsIndex = -1;
    for (unsigned i = 0; i < sampleTypeNames.size(); i++) {
        if (str(sampleTypeNames[i]) == "instructions") instructionsIndex = i;
        if (str(sampleTypeNames[i]) == "calls") callsIndex = i;
    }
    EXPECT_GE(instructionsIndex, 0);
    EXPECT_GE(callsIndex, 0);

    std::map<std::string, Counts> ret;
    for (auto& [locations, values] : samples) {
        // Leaf first; keep from the outermost main down to the leaf.
        std::vector<std::string> names;
        for (auto l : locations) names.push_back(str(functionNames[locationFunctions[l]]));
        int mainIndex = -1;
        for (int i = int(names.size()) - 1; i >= 0; i--) {
            if (names[i] == "main") {
                mainIndex = i;
                break;
            }
        }
        if (mainIndex < 0) continue;
        std::string path;
        for (int i = mainIndex; i >= 0; i--) path += (path.empty() ? "" : "/") + names[i];
        auto& c = ret[path];
        if (instructionsIndex >= 0 && unsigned(instructionsIndex) < values.size()) {
            c.instructions += values[instructionsIndex];
        }
        if (callsIndex >= 0 && unsigned(callsIndex) < values.size()) c.calls += values[callsIndex];
    }
    return ret;
}

// What callgrind says about the control's functions: the self cost of each, and the calls
// and inclusive cost of each caller -> callee edge.
struct Callgrind {
    std::map<std::string, uint64_t> self;
    std::map<std::pair<std::string, std::string>, std::pair<uint64_t, uint64_t>> edges;
    bool hasEvents = false;
};

Callgrind readCallgrind(const std::string& data) {
    Callgrind ret;
    std::istringstream in(data);
    std::string line;
    unsigned positions = 1;
    int eventIndex = -1;
    std::map<std::string, std::string> names;
    auto resolve = [&](std::string spec) {
        // "(id) name" defines a compressed name, "(id)" uses one.
        if (!spec.empty() && spec[0] == '(') {
            auto close = spec.find(')');
            std::string id = spec.substr(0, close + 1);
            std::string rest = close + 1 < spec.size() ? spec.substr(close + 1) : "";
            auto start = rest.find_first_not_of(' ');
            if (start == std::string::npos) return names[id];
            names[id] = rest.substr(start);
            return names[id];
        }
        return spec;
    };
    auto costOf = [&](const std::string& costLine) -> uint64_t {
        std::istringstream tokens(costLine);
        std::string token;
        std::vector<std::string> all;
        while (tokens >> token) all.push_back(token);
        unsigned index = positions + eventIndex;
        if (eventIndex < 0 || index >= all.size()) return 0;
        return std::stoull(all[index]);
    };
    std::string fn, cfn;
    uint64_t pendingCalls = 0;
    bool afterCalls = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (afterCalls) {
            afterCalls = false;
            auto& edge = ret.edges[{fn, cfn}];
            edge.first += pendingCalls;
            edge.second += costOf(line);
            continue;
        }
        if (line.rfind("positions:", 0) == 0) {
            std::istringstream tokens(line.substr(10));
            std::string token;
            positions = 0;
            while (tokens >> token) positions++;
        } else if (line.rfind("events:", 0) == 0) {
            ret.hasEvents = true;
            std::istringstream tokens(line.substr(7));
            std::string token;
            for (int i = 0; tokens >> token; i++) {
                for (auto& c : token) c = std::tolower(c);
                if (token == "instructions") eventIndex = i;
            }
        } else if (line.rfind("fn=", 0) == 0) {
            fn = resolve(line.substr(3));
        } else if (line.rfind("cfn=", 0) == 0) {
            cfn = resolve(line.substr(4));
        } else if (line.rfind("calls=", 0) == 0) {
            pendingCalls = std::stoull(line.substr(6));
            afterCalls = true;
        } else if ((line[0] >= '0' && line[0] <= '9') || line[0] == '+' || line[0] == '-' || line[0] == '*') {
            ret.self[fn] += costOf(line);
        }
        // Everything else (headers, ob=, fl=, fi=, fe=, cob=, cfi=, totals:) carries no cost.
    }
    EXPECT_GE(eventIndex, 0) << "no event named Instructions";
    return ret;
}

}  // namespace

TEST(Profiler, PprofMatchesControl) {
    auto data = runControl("-profile", "pcsx-redux-profiler-test.pb");
    ASSERT_FALSE(data.empty());
    std::map<std::string, Counts> expected = {
        {"main", {22, 1}},
        {"main/mid", {20, 2}},
        {"main/mid/leaf", {72, 4}},
        {"main/mid2", {8, 1}},
        {"main/mid2/leaf", {18, 1}},
        {"main/leaf", {18, 1}},
        {"main/rec", {10, 1}},
        {"main/rec/rec", {10, 1}},
        {"main/rec/rec/rec", {10, 1}},
        {"main/rec/rec/rec/rec", {8, 1}},
        {"main/ping", {10, 1}},
        {"main/ping/pong", {10, 1}},
        {"main/ping/pong/ping", {10, 1}},
        {"main/ping/pong/ping/pong", {8, 1}},
    };
    auto got = readPprof(data);
    for (auto& [path, counts] : expected) {
        EXPECT_EQ(got[path], counts) << path;
    }
    for (auto& [path, counts] : got) {
        EXPECT_TRUE(expected.count(path)) << "unexpected path " << path << " " << counts;
    }
}

// The callgrind serializer is not written yet; this pins what it must produce. Inclusive
// cost is per call: a recursive edge like rec -> rec adds up the inclusive cost of each of
// its calls, so nested calls count once for every level they sit under, as valgrind's own
// callgrind does.
TEST(Profiler, CallgrindMatchesControl) {
    auto data = runControl("-callgrind", "pcsx-redux-profiler-test.callgrind");
    ASSERT_FALSE(data.empty()) << "no callgrind output";
    auto got = readCallgrind(data);
    EXPECT_TRUE(got.hasEvents);
    std::map<std::string, uint64_t> self = {
        {"main", 22}, {"mid", 20}, {"mid2", 8}, {"leaf", 108}, {"rec", 38}, {"ping", 20}, {"pong", 18},
    };
    for (auto& [fn, cost] : self) EXPECT_EQ(got.self[fn], cost) << "self cost of " << fn;
    std::map<std::pair<std::string, std::string>, std::pair<uint64_t, uint64_t>> edges = {
        {{"main", "mid"}, {2, 92}},  {{"mid", "leaf"}, {4, 72}},  {{"main", "mid2"}, {1, 26}},
        {{"mid2", "leaf"}, {1, 18}}, {{"main", "leaf"}, {1, 18}}, {{"main", "rec"}, {1, 38}},
        {{"rec", "rec"}, {3, 54}},   {{"main", "ping"}, {1, 38}}, {{"ping", "pong"}, {2, 36}},
        {{"pong", "ping"}, {1, 18}},
    };
    for (auto& [edge, value] : edges) {
        auto& g = got.edges[edge];
        EXPECT_EQ(g.first, value.first) << "calls " << edge.first << " -> " << edge.second;
        EXPECT_EQ(g.second, value.second) << "inclusive " << edge.first << " -> " << edge.second;
    }
    for (auto& [edge, value] : got.edges) {
        if (self.count(edge.first) && edge.second != "main") {
            EXPECT_TRUE(edges.count(edge)) << "unexpected edge " << edge.first << " -> " << edge.second;
        }
    }
}

// Two functions can share a name: static functions in different files, or a symbol
// table that lists one name twice. Every export keys functions by start address but
// shows them by name, so a shared name has to carry its address or readers merge them.
TEST(Profiler, SharedSymbolNamesCarryTheirAddress) {
    std::map<uint32_t, std::string> symbols = {
        {0x80010000, "helper"},
        {0x80010100, "main"},
        {0x80020000, "helper"},
        {0x80030000, "other"},
    };
    PCSX::Profiler::SymbolNamer name(symbols);
    uint32_t start = 0;
    EXPECT_EQ(name(0x80010010, &start), "helper@0x80010000");
    EXPECT_EQ(start, 0x80010000);
    EXPECT_EQ(name(0x80020000, &start), "helper@0x80020000");
    EXPECT_EQ(start, 0x80020000);
    EXPECT_EQ(name(0x80020044, &start), "helper@0x80020000");
    EXPECT_EQ(name(0x80010104, &start), "main");
    EXPECT_EQ(start, 0x80010100);
    EXPECT_EQ(name(0x80030008), "other");
    EXPECT_EQ(name(0x00001000, &start), "0x00001000");
    EXPECT_EQ(start, 0x00001000);
}
