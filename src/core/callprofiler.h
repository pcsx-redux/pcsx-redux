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

#pragma once

#include <stdint.h>

#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace PCSX {

// An exact call-path profiler. The interpreter counts every instruction it executes into a
// bucket keyed by (call path, pc), with the call path interned by CallStacks, so the result
// is what a sampling profiler would converge to with no sampling noise. Each bucket carries
// a small array of counters, so more events can be added as columns without reshaping it.
//
// CallStacks is fed by the interpreter only, so profiling needs the interpreter.
class Profiler {
  public:
    enum Counter : unsigned {
        // Instructions executed with this pc on this path.
        Instructions,
        // Calls made to this pc, on the path the callee runs on.
        Calls,
        CounterCount,
    };
    typedef std::array<uint64_t, CounterCount> Counters;
    static const char* counterName(unsigned counter);

    struct Sample {
        uint32_t path;
        uint32_t pc;
        Counters counters;
    };

    struct Function {
        std::string name;
        uint32_t address;
        Counters self = {};
        Counters inclusive = {};
    };

    // Returns false, and does nothing, if the cpu is the dynarec.
    bool start();
    void stop();
    void clear();
    bool running() const { return m_running; }

    void countInstruction(uint32_t pc);
    void countCall(uint32_t path, uint32_t target) {
        if (target) m_samples[key(path, target)][Calls]++;
    }

    std::vector<Sample> samples() const;
    Counters totals() const;
    // The code addresses of a sample, leaf first: the pc, then the call site of each frame
    // up to the root.
    std::vector<uint32_t> frames(uint32_t path, uint32_t pc) const;
    // Self and inclusive counters per function, by containing symbol. Recursion counts a
    // function's inclusive cost once per sample.
    std::vector<Function> functions() const;

    // The google/pprof profile.proto format, uncompressed.
    std::string serializePprof() const;
    // The callgrind format KCachegrind reads.
    std::string serializeCallgrind() const;

    // Names code addresses after the symbol containing them, against one symbol table. A
    // name the table holds at more than one address gets "@0x<start>" appended, so functions
    // sharing a name stay apart in every export; a unique name stays bare. An address no
    // symbol covers is named by its own hex value, and is its own start.
    class SymbolNamer {
      public:
        explicit SymbolNamer(const std::map<uint32_t, std::string>& symbols);
        std::string operator()(uint32_t addr, uint32_t* start = nullptr) const;

      private:
        const std::map<uint32_t, std::string>& m_symbols;
        std::unordered_set<std::string> m_shared;
    };
    // A namer over the cpu's current symbol table. Build one per export: it indexes the
    // whole table.
    static SymbolNamer symbolNamer();

    bool m_running = false;

  private:
    static uint64_t key(uint32_t path, uint32_t pc) { return (uint64_t(path) << 32) | pc; }
    std::unordered_map<uint64_t, Counters> m_samples;
};

}  // namespace PCSX
