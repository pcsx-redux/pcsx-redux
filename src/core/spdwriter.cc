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

#include "core/spdwriter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace {

enum : uint8_t {
    PKT_REG_WRITE = 0x00,
    PKT_END_PATTERN = 0x02,
    PKT_TRACE_BEGIN = 0x04,
    PKT_ORDER_TABLE = 0x10,
    PKT_PATTERN_HEADER = 0x11,
    PKT_SAMPLE_DATA = 0x21,
    PKT_TICK_RATE = 0x30,
    PKT_COMMENT = 0x43,
    PKT_VOICE_COUNT = 0x45,
};

constexpr uint16_t c_inlineWait = 0xefff;
constexpr uint16_t c_keyOnLo = 0x188;
constexpr uint16_t c_keyOnHi = 0x18a;
constexpr uint16_t c_keyOffLo = 0x18c;
constexpr uint16_t c_keyOffHi = 0x18e;
constexpr uint16_t c_spuCnt = 0x1aa;
constexpr uint32_t c_captureBuffers = 0x1000;

class Stream {
  public:
    void packet(uint8_t type, std::span<const uint32_t> payload) {
        m_words.push_back((uint32_t(type) << 24) | uint32_t(payload.size()));
        m_words.insert(m_words.end(), payload.begin(), payload.end());
    }
    void string(uint8_t type, std::string_view str) {
        std::vector<uint32_t> payload((str.size() + 4) / 4, 0);
        std::copy(str.begin(), str.end(), reinterpret_cast<char*>(payload.data()));
        packet(type, payload);
    }
    size_t bytes() const { return m_words.size() * 4 + 16; }
    std::vector<uint32_t>& words() { return m_words; }

  private:
    std::vector<uint32_t> m_words;
};

uint32_t regWord(uint16_t reg, uint16_t value) { return (uint32_t(reg) << 16) | value; }

void appendWait(std::vector<uint32_t>& words, uint64_t ticks) {
    while (ticks > 0) {
        uint64_t chunk = std::min<uint64_t>(ticks, 0xffff);
        words.push_back(regWord(c_inlineWait, uint16_t(chunk)));
        ticks -= chunk;
    }
}

// Walks 16-byte ADPCM blocks from start until one carries the end flag, returning the offset
// just past it. Stops at the end of sound RAM if no block does.
uint32_t sampleEnd(const uint8_t* spuRam, uint32_t start) {
    uint32_t addr = start & ~0xfu;
    while (addr < PCSX::SPD::c_spuRamSize) {
        uint8_t flags = spuRam[addr + 1];
        addr += 16;
        if (flags & 1) break;
    }
    return addr;
}

}  // namespace

bool PCSX::SPD::isDropped(uint16_t reg) {
    if (reg < 0x180) return (reg & 0xf) == 0xc;  // ENVX, the live envelope level
    switch (reg) {
        case 0x19c:  // ENDX
        case 0x19e:
        case 0x1a6:  // transfer address
        case 0x1a8:  // transfer FIFO
        case 0x1ae:  // SPUSTAT
        case 0x1b8:  // current main volume
        case 0x1ba:
            return true;
    }
    return false;
}

uint16_t PCSX::SPD::filterValue(uint16_t reg, uint16_t value) {
    // Bits 4-5 are the transfer mode, bit 6 the IRQ enable.
    if (reg == c_spuCnt) return value & ~0x0070;
    return value;
}

namespace {

struct Span {
    uint32_t lo = PCSX::SPD::c_spuRamSize;
    uint32_t hi = 0;
};

// Widens span to cover the sample at addr, out to that sample's end flag.
void addSample(Span& span, uint32_t addr, const uint8_t* spuRam) {
    if (addr < c_captureBuffers) return;
    span.lo = std::min(span.lo, addr & ~0xfu);
    span.hi = std::max(span.hi, sampleEnd(spuRam, addr));
}

// Each voice in mask plays from its start address and can jump to its repeat address.
void addKeyedSamples(Span& span, const std::array<uint16_t, 0x100>& regs, uint32_t mask, unsigned first,
                     uint32_t& keyed, const uint8_t* spuRam) {
    for (unsigned i = 0; i < 16; i++) {
        unsigned voice = first + i;
        if (!(mask & (1u << i)) || voice >= 24) continue;
        keyed |= 1u << voice;
        addSample(span, uint32_t(regs[(voice * 0x10 + 6) / 2]) << 3, spuRam);
        addSample(span, uint32_t(regs[(voice * 0x10 + 0xe) / 2]) << 3, spuRam);
    }
}

// The sample span: every sample a keyed voice starts from or repeats to, out to its end flag.
// Voices never keyed during the capture point at whatever the game left there, and walking
// those would drag arbitrary RAM into the file.
Span sampleSpan(std::span<const uint16_t, 0x100> baseline, std::span<const PCSX::SPD::RegWrite> writes,
                const uint8_t* spuRam, bool fullRam) {
    Span span;
    if (fullRam) return {c_captureBuffers, PCSX::SPD::c_spuRamSize};
    std::array<uint16_t, 0x100> regs;
    std::copy(baseline.begin(), baseline.end(), regs.begin());
    uint32_t keyed = 0;
    for (auto& w : writes) {
        if (w.reg >= 0x200) continue;
        regs[w.reg / 2] = w.value;
        if (w.reg == c_keyOnLo) addKeyedSamples(span, regs, w.value, 0, keyed, spuRam);
        if (w.reg == c_keyOnHi) addKeyedSamples(span, regs, w.value, 16, keyed, spuRam);
        // A repeat address written while the voice plays is where it jumps at the next loop end.
        if (w.reg < 0x180 && (w.reg & 0xf) == 0xe && (keyed & (1u << (w.reg / 0x10)))) {
            addSample(span, uint32_t(w.value) << 3, spuRam);
        }
    }
    return span;
}

bool isRestored(uint16_t reg) {
    if (PCSX::SPD::isDropped(reg)) return false;
    if (reg < 0x180) return true;
    return reg != c_spuCnt && (reg < c_keyOnLo || reg > c_keyOffHi);
}

// State restoration: every voice's settings, then the globals with SPUCNT last. Key on/off
// are edge-triggered and stay out, so a voice already sounding when the capture started is
// silent until the game keys it again.
std::vector<uint32_t> restorationWrites(std::span<const uint16_t, 0x100> baseline) {
    std::vector<uint32_t> words;
    for (uint16_t reg = 0; reg < 0x200; reg += 2) {
        if (isRestored(reg)) words.push_back(regWord(reg, baseline[reg / 2]));
    }
    words.push_back(regWord(c_spuCnt, PCSX::SPD::filterValue(c_spuCnt, baseline[c_spuCnt / 2])));
    return words;
}

// Ticks are rounded from the absolute cycle distance to the first write, so rounding never
// accumulates across the stream. The cycle counter is not monotonic: a Lua guest call
// restores it on return, so writes after the call can carry smaller stamps than writes made
// during it. Such a write keeps its order and lands on the current tick.
void traceWrites(Stream& stream, std::span<const PCSX::SPD::RegWrite> writes, uint32_t tickRate, uint32_t clock,
                 PCSX::SPD::Result& result) {
    const double ticksPerCycle = double(tickRate) / 65536.0 / double(clock);
    const uint64_t origin = writes[0].cycle;
    std::vector<uint32_t> words;
    uint64_t current = 0;
    for (auto& w : writes) {
        uint64_t tick = w.cycle <= origin ? 0 : uint64_t(std::llround(double(w.cycle - origin) * ticksPerCycle));
        if (tick > current) {
            appendWait(words, tick - current);
            stream.packet(PKT_REG_WRITE, words);
            words.clear();
            current = tick;
        }
        if (w.reg >= 0x200 || PCSX::SPD::isDropped(w.reg)) continue;
        words.push_back(regWord(w.reg, PCSX::SPD::filterValue(w.reg, w.value)));
        result.writes++;
    }
    if (!words.empty()) stream.packet(PKT_REG_WRITE, words);
    result.ticks = current;
}

void serialize(Stream& stream, PCSX::SPD::Result& result) {
    static const char magic[16] = "PSXSPUDUMPv1r1";
    result.data.resize(stream.bytes());
    std::copy(magic, magic + 16, result.data.begin());
    uint8_t* out = result.data.data() + 16;
    for (uint32_t word : stream.words()) {
        *out++ = word & 0xff;
        *out++ = (word >> 8) & 0xff;
        *out++ = (word >> 16) & 0xff;
        *out++ = word >> 24;
    }
}

}  // namespace

PCSX::SPD::Result PCSX::SPD::writeCapture(std::span<const uint16_t, 0x100> baseline, std::span<const RegWrite> writes,
                                          const uint8_t* spuRam, const Options& options) {
    Result result;
    if (writes.empty()) {
        result.error = "no register writes to export";
        return result;
    }

    const uint32_t tickRate = options.tickRate ? options.tickRate : (c_ntscHblanks << 16) / 16;
    const Span span = sampleSpan(baseline, writes, spuRam, options.fullRam);

    Stream stream;
    stream.string(PKT_COMMENT, options.comment);
    const uint32_t voiceCount = 24;
    stream.packet(PKT_VOICE_COUNT, std::span(&voiceCount, 1));
    stream.packet(PKT_TICK_RATE, std::span(&tickRate, 1));

    if (span.hi > span.lo) {
        result.sampleBase = span.lo;
        result.sampleSize = span.hi - span.lo;
        std::vector<uint32_t> payload(1 + (span.hi - span.lo + 3) / 4, 0);
        payload[0] = span.lo >> 3;
        std::copy(spuRam + span.lo, spuRam + span.hi, reinterpret_cast<uint8_t*>(payload.data() + 1));
        stream.packet(PKT_SAMPLE_DATA, payload);
    }

    const uint32_t order[] = {0, 0, 0};  // no loop, one entry, pattern 0
    stream.packet(PKT_ORDER_TABLE, order);
    const size_t patternHeader = stream.words().size() + 1;
    const uint32_t placeholder = 0;
    stream.packet(PKT_PATTERN_HEADER, std::span(&placeholder, 1));
    stream.words()[patternHeader] = uint32_t(stream.bytes());

    stream.packet(PKT_REG_WRITE, restorationWrites(baseline));
    stream.packet(PKT_TRACE_BEGIN, {});
    traceWrites(stream, writes, tickRate, options.clock, result);
    stream.packet(PKT_END_PATTERN, {});
    serialize(stream, result);
    return result;
}
