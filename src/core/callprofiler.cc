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

#include "core/callprofiler.h"

#include <algorithm>
#include <map>
#include <set>

#include "core/callstacks.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "fmt/format.h"
#include "support/protobuf.h"

const char* PCSX::Profiler::counterName(unsigned counter) {
    switch (counter) {
        case Instructions:
            return "instructions";
        case Calls:
            return "calls";
    }
    return "unknown";
}

bool PCSX::Profiler::start() {
    if (g_emulator->m_cpu->isDynarec()) return false;
    g_emulator->m_callStacks->setPathTracking(true);
    m_running = true;
    return true;
}

void PCSX::Profiler::stop() {
    m_running = false;
    g_emulator->m_callStacks->setPathTracking(false);
}

void PCSX::Profiler::clear() {
    m_samples.clear();
    g_emulator->m_callStacks->clearPaths();
}

void PCSX::Profiler::countInstruction(uint32_t pc) {
    m_samples[key(g_emulator->m_callStacks->pathAt(pc), pc)][Instructions]++;
}

std::vector<PCSX::Profiler::Sample> PCSX::Profiler::samples() const {
    std::vector<Sample> ret;
    ret.reserve(m_samples.size());
    for (auto& [k, counters] : m_samples) ret.push_back({uint32_t(k >> 32), uint32_t(k), counters});
    std::sort(ret.begin(), ret.end(),
              [](const Sample& a, const Sample& b) { return a.path != b.path ? a.path < b.path : a.pc < b.pc; });
    return ret;
}

PCSX::Profiler::Counters PCSX::Profiler::totals() const {
    Counters ret = {};
    for (auto& [k, counters] : m_samples) {
        for (unsigned i = 0; i < CounterCount; i++) ret[i] += counters[i];
    }
    return ret;
}

std::vector<uint32_t> PCSX::Profiler::frames(uint32_t path, uint32_t pc) const {
    auto& nodes = g_emulator->m_callStacks->getPathNodes();
    std::vector<uint32_t> ret;
    ret.push_back(pc);
    while (path && (path < nodes.size())) {
        // The return address sits after the jal and its delay slot.
        ret.push_back(nodes[path].ra - 8);
        path = nodes[path].parent;
    }
    return ret;
}

PCSX::Profiler::SymbolNamer::SymbolNamer(const std::map<uint32_t, std::string>& symbols) : m_symbols(symbols) {
    std::unordered_set<std::string> seen;
    for (auto& [address, name] : symbols) {
        if (!seen.insert(name).second) m_shared.insert(name);
    }
}

std::string PCSX::Profiler::SymbolNamer::operator()(uint32_t addr, uint32_t* start) const {
    auto symbol = R3000Acpu::findContainingSymbol(m_symbols, addr);
    if (!symbol) {
        if (start) *start = addr;
        return fmt::format("0x{:08x}", addr);
    }
    if (start) *start = symbol->first;
    if (m_shared.contains(symbol->second)) return fmt::format("{}@0x{:08x}", symbol->second, symbol->first);
    return symbol->second;
}

PCSX::Profiler::SymbolNamer PCSX::Profiler::symbolNamer() { return SymbolNamer(g_emulator->m_cpu->m_symbols); }

std::vector<PCSX::Profiler::Function> PCSX::Profiler::functions() const {
    std::map<uint32_t, Function> byAddress;
    std::unordered_map<uint32_t, uint32_t> addressCache;
    auto symbolize = symbolNamer();
    auto functionOf = [&](uint32_t addr) -> Function& {
        auto cached = addressCache.find(addr);
        uint32_t start;
        if (cached != addressCache.end()) {
            start = cached->second;
        } else {
            std::string name = symbolize(addr, &start);
            addressCache.emplace(addr, start);
            auto& f = byAddress[start];
            if (f.name.empty()) {
                f.name = std::move(name);
                f.address = start;
            }
        }
        return byAddress[start];
    };
    std::set<uint32_t> seen;
    for (auto& [k, counters] : m_samples) {
        auto addrs = frames(uint32_t(k >> 32), uint32_t(k));
        auto& leaf = functionOf(addrs[0]);
        for (unsigned i = 0; i < CounterCount; i++) leaf.self[i] += counters[i];
        seen.clear();
        for (auto addr : addrs) {
            auto& f = functionOf(addr);
            if (!seen.insert(f.address).second) continue;
            for (unsigned i = 0; i < CounterCount; i++) f.inclusive[i] += counters[i];
        }
    }
    std::vector<Function> ret;
    ret.reserve(byAddress.size());
    for (auto& [addr, f] : byAddress) ret.push_back(std::move(f));
    return ret;
}

namespace PCSX {
namespace Pprof {

// The subset of github.com/google/pprof/proto/profile.proto this writes.
typedef Protobuf::Field<Protobuf::Int64, TYPESTRING("type"), 1> ValueTypeType;
typedef Protobuf::Field<Protobuf::Int64, TYPESTRING("unit"), 2> ValueTypeUnit;
typedef Protobuf::Message<TYPESTRING("ValueType"), ValueTypeType, ValueTypeUnit> ValueType;

typedef Protobuf::RepeatedVariableField<Protobuf::UInt64, TYPESTRING("location_id"), 1> SampleLocationId;
typedef Protobuf::RepeatedVariableField<Protobuf::Int64, TYPESTRING("value"), 2> SampleValue;
typedef Protobuf::Message<TYPESTRING("Sample"), SampleLocationId, SampleValue> Sample;

typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("id"), 1> MappingId;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("memory_start"), 2> MappingMemoryStart;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("memory_limit"), 3> MappingMemoryLimit;
typedef Protobuf::Field<Protobuf::Int64, TYPESTRING("filename"), 5> MappingFilename;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("has_functions"), 7> MappingHasFunctions;
typedef Protobuf::Message<TYPESTRING("Mapping"), MappingId, MappingMemoryStart, MappingMemoryLimit, MappingFilename,
                          MappingHasFunctions>
    Mapping;

typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("function_id"), 1> LineFunctionId;
typedef Protobuf::Message<TYPESTRING("Line"), LineFunctionId> Line;

typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("id"), 1> LocationId;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("mapping_id"), 2> LocationMappingId;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("address"), 3> LocationAddress;
typedef Protobuf::RepeatedVariableField<Line, TYPESTRING("line"), 4> LocationLine;
typedef Protobuf::Message<TYPESTRING("Location"), LocationId, LocationMappingId, LocationAddress, LocationLine>
    Location;

typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("id"), 1> FunctionId;
typedef Protobuf::Field<Protobuf::Int64, TYPESTRING("name"), 2> FunctionName;
typedef Protobuf::Field<Protobuf::Int64, TYPESTRING("system_name"), 3> FunctionSystemName;
typedef Protobuf::Message<TYPESTRING("Function"), FunctionId, FunctionName, FunctionSystemName> Function;

typedef Protobuf::RepeatedVariableField<ValueType, TYPESTRING("sample_type"), 1> ProfileSampleType;
typedef Protobuf::RepeatedVariableField<Sample, TYPESTRING("sample"), 2> ProfileSample;
typedef Protobuf::RepeatedVariableField<Mapping, TYPESTRING("mapping"), 3> ProfileMapping;
typedef Protobuf::RepeatedVariableField<Location, TYPESTRING("location"), 4> ProfileLocation;
typedef Protobuf::RepeatedVariableField<Function, TYPESTRING("function"), 5> ProfileFunction;
// string_table is field 6, appended by hand: RepeatedVariableField only frames messages.
static constexpr uint64_t StringTableField = 6;
typedef Protobuf::Field<Protobuf::Int64, TYPESTRING("period"), 12> ProfilePeriod;
typedef Protobuf::Field<Protobuf::Int64, TYPESTRING("default_sample_type"), 14> ProfileDefaultSampleType;
typedef Protobuf::Message<TYPESTRING("Profile"), ProfileSampleType, ProfileSample, ProfileMapping, ProfileLocation,
                          ProfileFunction, ProfilePeriod, ProfileDefaultSampleType>
    Profile;

}  // namespace Pprof
}  // namespace PCSX

std::string PCSX::Profiler::serializePprof() const {
    using namespace Pprof;
    Profile profile;

    std::vector<std::string> strings;
    std::unordered_map<std::string, int64_t> stringIds;
    auto stringId = [&](const std::string& str) -> int64_t {
        auto [it, inserted] = stringIds.try_emplace(str, int64_t(strings.size()));
        if (inserted) strings.push_back(str);
        return it->second;
    };
    stringId("");

    auto& sampleTypes = profile.get<ProfileSampleType>().value;
    for (unsigned i = 0; i < CounterCount; i++) {
        ValueType type;
        type.get<ValueTypeType>().value = stringId(counterName(i));
        type.get<ValueTypeUnit>().value = stringId("count");
        sampleTypes.push_back(std::move(type));
    }

    Mapping mapping;
    mapping.get<MappingId>().value = 1;
    mapping.get<MappingMemoryStart>().value = 0;
    mapping.get<MappingMemoryLimit>().value = 0x100000000ULL;
    mapping.get<MappingFilename>().value = stringId("psx");
    mapping.get<MappingHasFunctions>().value = true;
    profile.get<ProfileMapping>().value.push_back(std::move(mapping));

    auto& functions = profile.get<ProfileFunction>().value;
    std::unordered_map<uint32_t, uint64_t> functionIds;
    auto& locations = profile.get<ProfileLocation>().value;
    std::unordered_map<uint32_t, uint64_t> locationIds;
    auto symbolize = symbolNamer();
    auto locationId = [&](uint32_t addr) -> uint64_t {
        auto found = locationIds.find(addr);
        if (found != locationIds.end()) return found->second;
        uint32_t start;
        std::string name = symbolize(addr, &start);
        auto [fit, newFunction] = functionIds.try_emplace(start, uint64_t(functions.size() + 1));
        if (newFunction) {
            Pprof::Function function;
            function.get<FunctionId>().value = fit->second;
            function.get<FunctionName>().value = stringId(name);
            function.get<FunctionSystemName>().value = stringId(name);
            functions.push_back(std::move(function));
        }
        uint64_t id = locations.size() + 1;
        Location location;
        location.get<LocationId>().value = id;
        location.get<LocationMappingId>().value = 1;
        location.get<LocationAddress>().value = addr;
        Line line;
        line.get<LineFunctionId>().value = fit->second;
        location.get<LocationLine>().value.push_back(std::move(line));
        locations.push_back(std::move(location));
        locationIds.emplace(addr, id);
        return id;
    };

    auto& samples = profile.get<ProfileSample>().value;
    for (auto& s : this->samples()) {
        Pprof::Sample sample;
        auto& ids = sample.get<SampleLocationId>().value;
        for (auto addr : frames(s.path, s.pc)) {
            ids.emplace_back();
            ids.back().value = locationId(addr);
        }
        auto& values = sample.get<SampleValue>().value;
        for (auto v : s.counters) {
            values.emplace_back();
            values.back().value = int64_t(v);
        }
        samples.push_back(std::move(sample));
    }

    profile.get<ProfilePeriod>().value = 1;
    profile.get<ProfileDefaultSampleType>().value = stringId(counterName(Instructions));

    Protobuf::OutSlice slice;
    profile.serialize(&slice);
    for (auto& str : strings) {
        slice.putVarInt((StringTableField << 3) | 2);
        slice.putVarInt(str.size());
        slice.putBytes(str);
    }
    return slice.finalize();
}
