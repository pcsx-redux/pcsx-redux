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

#include <map>
#include <unordered_map>
#include <utility>

#include "core/callprofiler.h"
#include "fmt/format.h"

// callgrind format: https://valgrind.org/docs/manual/cl-format.html
std::string PCSX::Profiler::serializeCallgrind() const {
    struct FunctionData {
        std::map<uint32_t, uint64_t> self;
        // (call site, callee) -> (calls, inclusive instructions)
        std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>> callees;
    };
    std::map<uint32_t, FunctionData> functions;
    std::unordered_map<uint32_t, uint32_t> startCache;
    std::unordered_map<uint32_t, std::string> names;
    auto functionOf = [&](uint32_t addr) {
        auto cached = startCache.find(addr);
        if (cached != startCache.end()) return cached->second;
        uint32_t start;
        std::string name = symbolize(addr, &start);
        startCache.emplace(addr, start);
        names.try_emplace(start, std::move(name));
        return start;
    };

    uint64_t total = 0;
    std::vector<uint32_t> owners;
    for (auto& s : samples()) {
        auto addrs = frames(s.path, s.pc);
        owners.clear();
        for (auto addr : addrs) owners.push_back(functionOf(addr));
        uint64_t instructions = s.counters[Instructions];
        uint64_t calls = s.counters[Calls];
        total += instructions;
        auto& leaf = functions[owners[0]];
        if (instructions) leaf.self[s.pc] += instructions;
        for (size_t i = 0; i + 1 < addrs.size(); i++) {
            auto& edge = functions[owners[i + 1]].callees[{addrs[i + 1], owners[i]}];
            if (i == 0) edge.first += calls;
            edge.second += instructions;
        }
    }

    std::string out = "# callgrind format\nversion: 1\ncreator: pcsx-redux\npositions: instr\nevents: Instructions\n";
    out += fmt::format("totals: {}\n", total);

    std::unordered_map<uint32_t, unsigned> ids;
    auto reference = [&](uint32_t start) {
        auto [it, inserted] = ids.try_emplace(start, unsigned(ids.size() + 1));
        if (inserted) return fmt::format("({}) {}", it->second, names[start]);
        return fmt::format("({})", it->second);
    };

    for (auto& [start, function] : functions) {
        out += fmt::format("\nfn={}\n", reference(start));
        for (auto& [pc, cost] : function.self) out += fmt::format("0x{:08x} {}\n", pc, cost);
        for (auto& [key, edge] : function.callees) {
            out += fmt::format("cfn={}\ncalls={} 0x{:08x}\n0x{:08x} {}\n", reference(key.second), edge.first,
                               key.second, key.first, edge.second);
        }
    }
    return out;
}
