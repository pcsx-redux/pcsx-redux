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

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <memory>
#include <string_view>
#include <vector>

#include "args/args.hxx"
#include "fmt/format.h"
#include "support/file.h"
#include "support/tool-args.h"
#include "supportpsx/adpcm.h"
#include "supportpsx/spu-sample-layout.h"

// Converts a FastTracker 2 .xm module (or a .mod, read as the Amiga-period subset of XM) into the two files used by nugget's xmplayer: a song file with the
// patterns, instruments and sample headers, and a sample file with the SPU ADPCM data to upload. The song
// file format is described in xmplayer.h in nugget, and is written here field by field. Version 1.

namespace {

constexpr unsigned c_formatVersion = 1;
constexpr unsigned c_headerSize = 64;
constexpr unsigned c_instrumentSize = 208;
constexpr unsigned c_sampleSize = 32;
constexpr unsigned c_spuMemory = 512 * 1024 - 0x1010;
constexpr unsigned c_maxVoices = 24;
// The SPU pitch register saturates at 0x3fff, a little under 4 times 44100 Hz.
constexpr double c_maxFrequency = 0x3fff * 44100.0 / 4096.0;

struct Cell {
    uint8_t note = 0, instrument = 0, volume = 0, effect = 0, param = 0;
    bool empty() const { return !note && !instrument && !volume && !effect && !param; }
};

struct Pattern {
    unsigned rows = 64;
    std::vector<Cell> cells;  // rows * channels
};

struct Envelope {
    uint16_t points[12][2] = {};
    uint8_t count = 0, sustain = 0, loopStart = 0, loopEnd = 0, flags = 0;
};

struct Sample {
    std::string name;
    std::vector<int16_t> pcm;
    unsigned loopStart = 0, loopLength = 0;
    unsigned loopType = 0;  // 0 none, 1 forward, 2 ping-pong
    uint8_t volume = 0, pan = 128;
    int8_t finetune = 0, relnote = 0;
    // filled during conversion
    unsigned rateShift = 0;
    int maxRealNote = -1;
    PCSX::SPUSampleLayout::Layout layout;
    unsigned spuOffset = 0;
    unsigned encodedLoopLength = 0;  // source samples, after rate shift; ping-pong period when unrolled
    unsigned encodedLoopStart = 0;
};

struct Instrument {
    std::string name;
    uint8_t keymap[96] = {};
    Envelope vol, pan;
    uint8_t vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0;
    uint16_t fadeout = 0;
    unsigned firstSample = 0, numSamples = 0;
};

// Halves the sample rate with a windowed-sinc half-band low-pass, keeping even samples.
std::vector<int16_t> decimate(const std::vector<int16_t>& in) {
    constexpr int taps = 15;
    static std::array<double, 2 * taps + 1> kernel = [] {
        std::array<double, 2 * taps + 1> k;
        double sum = 0;
        for (int i = -taps; i <= taps; i++) {
            double x = i * 0.5;
            constexpr double pi = std::numbers::pi;
            double sinc = i == 0 ? 1.0 : std::sin(pi * x) / (pi * x);
            double window = 0.42 + 0.5 * std::cos(pi * i / (taps + 1)) + 0.08 * std::cos(2 * pi * i / (taps + 1));
            k[i + taps] = sinc * window;
            sum += k[i + taps];
        }
        for (auto& v : k) v /= sum;
        return k;
    }();
    std::vector<int16_t> out((in.size() + 1) / 2);
    for (size_t o = 0; o < out.size(); o++) {
        double acc = 0;
        for (int i = -taps; i <= taps; i++) {
            ssize_t p = ssize_t(o * 2) + i;
            if (p < 0 || p >= ssize_t(in.size())) continue;
            acc += in[p] * kernel[i + taps];
        }
        out[o] = int16_t(std::clamp(std::lround(acc), -32768l, 32767l));
    }
    return out;
}

// The sample as the SPU will play it: rate shift applied, ping-pong loop unfolded into a forward loop.
// Stretches a sample by `factor` (more samples, so lower pitch at the same playback rate) with a
// Blackman-windowed sinc. Inside a loop, source positions wrap so the seam stays continuous.
std::vector<int16_t> stretch(const std::vector<int16_t>& in, double factor, bool looped, unsigned loopStart,
                             unsigned loopLength, size_t outLength) {
    constexpr int taps = 8;
    constexpr double pi = std::numbers::pi;
    auto at = [&](ssize_t p) -> double {
        if (looped && p >= ssize_t(loopStart + loopLength)) p = loopStart + (p - loopStart) % loopLength;
        if (p < 0 || p >= ssize_t(in.size())) return 0.0;
        return in[p];
    };
    std::vector<int16_t> out(outLength);
    for (size_t o = 0; o < outLength; o++) {
        double src = o / factor;
        if (looped && src >= loopStart + loopLength) src = loopStart + std::fmod(src - loopStart, double(loopLength));
        const ssize_t base = ssize_t(std::floor(src));
        const double frac = src - base;
        double acc = 0, wsum = 0;
        for (int i = -taps + 1; i <= taps; i++) {
            const double x = i - frac;
            const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(pi * x) / (pi * x);
            const double t = (x + taps) / (2.0 * taps);
            const double window = (t <= 0 || t >= 1) ? 0.0 : 0.42 - 0.5 * std::cos(2 * pi * t) + 0.08 * std::cos(4 * pi * t);
            const double w = sinc * window;
            acc += at(base + i) * w;
            wsum += w;
        }
        out[o] = int16_t(std::clamp(std::lround(wsum != 0 ? acc / wsum : acc), -32768l, 32767l));
    }
    return out;
}

struct Prepared {
    std::vector<int16_t> pcm;
    bool hasLoop = false;
    unsigned loopStart = 0, loopLength = 0;
};

Prepared prepare(const Sample& s) {
    Prepared p;
    p.pcm = s.pcm;
    unsigned loopStart = s.loopStart, loopLength = s.loopLength;
    for (unsigned i = 0; i < s.rateShift; i++) {
        p.pcm = decimate(p.pcm);
        loopStart /= 2;
        loopLength = std::max(1u, (loopLength + 1) / 2);
    }
    p.hasLoop = s.loopType != 0 && loopLength > 0 && loopStart < p.pcm.size();
    if (!p.hasLoop) return p;
    loopLength = std::min<unsigned>(loopLength, p.pcm.size() - loopStart);
    p.pcm.resize(loopStart + loopLength);
    if (s.loopType == 2) {
        // The SPU only loops forward. A ping-pong loop over [a, b) plays a .. b-1, then b-1 .. a, then
        // again from a: both end points repeat, so the period is 2 * (b - a). It is written out as the
        // forward part followed by the reversed part.
        for (unsigned i = loopLength; i-- > 0;) p.pcm.push_back(p.pcm[loopStart + i]);
        loopLength = loopLength * 2;
    }
    p.loopStart = loopStart;
    p.loopLength = loopLength;
    return p;
}


struct Module {
    std::string title;
    unsigned channels = 0, songLength = 0, restart = 0, numInstruments = 0, flags = 0, speed = 6, bpm = 125;
    std::vector<uint8_t> orders;
    std::vector<Pattern> patterns;
    std::vector<Instrument> instruments;
    std::vector<Sample> samples;
    bool protracker = false;  // ProTracker periods and limits instead of FastTracker 2's Amiga table
};

// ProTracker's octave 1 periods, C-1 to B-1, one row per finetune -8..7.
static const uint16_t c_ptOctave1[16][12] = {
    {907, 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480}, {900, 850, 802, 757, 715, 675, 636, 601, 567, 535, 505, 477},
    {894, 844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474}, {887, 838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470},
    {881, 832, 785, 741, 699, 660, 623, 588, 555, 524, 494, 467}, {875, 826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463},
    {868, 820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460}, {862, 814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457},
    {856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453}, {850, 802, 757, 715, 674, 637, 601, 567, 535, 505, 477, 450},
    {844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474, 447}, {838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470, 444},
    {832, 785, 741, 699, 660, 623, 588, 555, 524, 495, 467, 441}, {826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463, 437},
    {820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460, 434}, {814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457, 431},
};

// ProTracker's finetune 0 periods for octaves 2 and 3, which are not exact halves of octave 1.
static const uint16_t c_ptFinetune0[24] = {428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
                                           214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};

// The player's period table for a MOD, times 4, as the reference (libopenmpt) renders it: finetune 0
// takes the ProTracker table for octaves 2 and 3 except B-3, which it plays at 113.25 (a quarter of
// 453); other finetunes halve their octave 1 row.
static uint16_t ptPeriod(unsigned row, unsigned index) {
    if (row == 8 && index == 35) return 453;
    if (row == 8 && index >= 12) return c_ptFinetune0[index - 12] * 4;
    return (c_ptOctave1[row][index % 12] * 4) >> (index / 12);
}

// Reads a FastTracker 2 .xm module.
static bool loadXM(PCSX::IO<PCSX::File>& file, std::string_view name, unsigned amplification, Module& m) {
    const size_t fileSize = file->size();
    auto readString = [&](unsigned size) {
        auto slice = file->read(size);
        std::string s(reinterpret_cast<const char*>(slice.data()), slice.size());
        s.resize(strnlen(s.c_str(), size));
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };

    // The signature ends with a space, which readString would trim.
    auto signature = file->read(17);
    if ((signature.size() != 17) ||
        (std::string_view(reinterpret_cast<const char*>(signature.data()), 17) != "Extended Module: ")) {
        fmt::print("{} is not an XM file.\n", name);
        return false;
    }
    const std::string title = readString(20);
    file->skip<uint8_t>();
    const std::string tracker = readString(20);
    const unsigned version = file->read<uint16_t>();
    if (version != 0x0104) {
        fmt::print("XM version {:04x} is not supported, only 0104 is.\n", version);
        return false;
    }
    const unsigned headerSize = file->read<uint32_t>();
    const unsigned songLength = file->read<uint16_t>();
    const unsigned restart = file->read<uint16_t>();
    const unsigned channels = file->read<uint16_t>();
    const unsigned numPatterns = file->read<uint16_t>();
    const unsigned numInstruments = file->read<uint16_t>();
    const unsigned flags = file->read<uint16_t>();
    const unsigned speed = file->read<uint16_t>();
    const unsigned bpm = file->read<uint16_t>();
    if (songLength == 0 || songLength > 256 || channels == 0 || channels > 32 || numPatterns > 256 ||
        numInstruments > 128) {
        fmt::print("Invalid XM header.\n");
        return false;
    }
    std::vector<uint8_t> orders(songLength);
    for (auto& o : orders) o = file->read<uint8_t>();

    fmt::print("Title:       {}\n", title);
    fmt::print("Tracker:     {}\n", tracker);
    fmt::print("Channels:    {}\n", channels);
    fmt::print("Positions:   {}\n", songLength);
    fmt::print("Patterns:    {}\n", numPatterns);
    fmt::print("Instruments: {}\n", numInstruments);
    fmt::print("Frequencies: {}\n", flags & 1 ? "linear" : "amiga");

    file->rSeek(60 + headerSize);
    std::vector<Pattern> patterns(numPatterns);
    for (auto& pattern : patterns) {
        const size_t start = file->rTell();
        const unsigned patternHeaderSize = file->read<uint32_t>();
        file->skip<uint8_t>();
        pattern.rows = file->read<uint16_t>();
        const unsigned packedSize = file->read<uint16_t>();
        if (pattern.rows == 0 || pattern.rows > 256) {
            fmt::print("Invalid pattern row count {}.\n", pattern.rows);
            return false;
        }
        file->rSeek(start + patternHeaderSize);
        pattern.cells.resize(pattern.rows * channels);
        const size_t end = file->rTell() + packedSize;
        for (unsigned i = 0; (i < pattern.cells.size()) && (size_t(file->rTell()) < end); i++) {
            Cell& c = pattern.cells[i];
            uint8_t b = file->read<uint8_t>();
            if (b & 0x80) {
                if (b & 1) c.note = file->read<uint8_t>();
                if (b & 2) c.instrument = file->read<uint8_t>();
                if (b & 4) c.volume = file->read<uint8_t>();
                if (b & 8) c.effect = file->read<uint8_t>();
                if (b & 16) c.param = file->read<uint8_t>();
            } else {
                c.note = b;
                c.instrument = file->read<uint8_t>();
                c.volume = file->read<uint8_t>();
                c.effect = file->read<uint8_t>();
                c.param = file->read<uint8_t>();
            }
            if (c.note > 97) c.note = 0;
            if (c.instrument > numInstruments) c.instrument = 0;
            if (c.effect > 33) c.effect = c.param = 0;
        }
        file->rSeek(end);
    }

    // FT2 plays an order past the pattern table as an empty 64-row pattern. Give those orders one.
    if (std::any_of(orders.begin(), orders.end(), [&](uint8_t o) { return o >= numPatterns; })) {
        if (numPatterns >= 256) {
            fmt::print("Orders point past a full pattern table.\n");
            return false;
        }
        for (auto& o : orders) {
            if (o >= numPatterns) o = numPatterns;
        }
        patterns.push_back(Pattern{64, std::vector<Cell>(64 * channels)});
    }

    std::vector<Instrument> instruments(numInstruments);
    std::vector<Sample> samples;
    for (auto& ins : instruments) {
        const size_t start = file->rTell();
        const unsigned instrumentSize = file->read<uint32_t>();
        ins.name = readString(22);
        file->skip<uint8_t>();
        const unsigned numSamples = file->read<uint16_t>();
        std::fill(std::begin(ins.keymap), std::end(ins.keymap), 0xff);
        ins.firstSample = samples.size();
        if (numSamples == 0) {
            file->rSeek(start + instrumentSize);
            continue;
        }
        const unsigned sampleHeaderSize = file->read<uint32_t>();
        uint8_t keymap[96];
        for (auto& k : keymap) k = file->read<uint8_t>();
        for (auto& p : ins.vol.points) p[0] = file->read<uint16_t>(), p[1] = file->read<uint16_t>();
        for (auto& p : ins.pan.points) p[0] = file->read<uint16_t>(), p[1] = file->read<uint16_t>();
        ins.vol.count = file->read<uint8_t>();
        ins.pan.count = file->read<uint8_t>();
        ins.vol.sustain = file->read<uint8_t>();
        ins.vol.loopStart = file->read<uint8_t>();
        ins.vol.loopEnd = file->read<uint8_t>();
        ins.pan.sustain = file->read<uint8_t>();
        ins.pan.loopStart = file->read<uint8_t>();
        ins.pan.loopEnd = file->read<uint8_t>();
        ins.vol.flags = file->read<uint8_t>();
        ins.pan.flags = file->read<uint8_t>();
        ins.vibType = file->read<uint8_t>();
        ins.vibSweep = file->read<uint8_t>();
        ins.vibDepth = file->read<uint8_t>();
        ins.vibRate = file->read<uint8_t>();
        ins.fadeout = file->read<uint16_t>();
        for (Envelope* e : {&ins.vol, &ins.pan}) {
            if (e->count == 0 || e->count > 12) e->flags = 0, e->count = 0;
            if (e->sustain >= e->count) e->flags &= ~2;
            if (e->loopStart >= e->count || e->loopEnd >= e->count || e->loopStart > e->loopEnd) e->flags &= ~4;
        }
        file->rSeek(start + instrumentSize);
        std::vector<unsigned> lengths(numSamples);
        std::vector<bool> is16(numSamples);
        for (unsigned i = 0; i < numSamples; i++) {
            const size_t sampleStart = file->rTell();
            Sample s;
            lengths[i] = file->read<uint32_t>();
            s.loopStart = file->read<uint32_t>();
            s.loopLength = file->read<uint32_t>();
            s.volume = std::min<uint8_t>(64, file->read<uint8_t>());
            s.finetune = file->read<int8_t>();
            const uint8_t type = file->read<uint8_t>();
            s.pan = file->read<uint8_t>();
            s.relnote = file->read<int8_t>();
            file->skip<uint8_t>();
            s.name = readString(22);
            is16[i] = type & 0x10;
            s.loopType = (type & 3) == 2 ? 2 : (type & 3) ? 1 : 0;
            if (is16[i]) s.loopStart /= 2, s.loopLength /= 2;
            if (s.loopLength == 0) s.loopType = 0;
            samples.push_back(std::move(s));
            file->rSeek(sampleStart + sampleHeaderSize);
        }
        uint64_t dataSize = 0;
        for (auto l : lengths) dataSize += l;
        const size_t dataStart = std::min<size_t>(fileSize, file->rTell());
        if (dataSize > fileSize - dataStart) {
            fmt::print("Truncated XM file.\n");
            return false;
        }
        for (unsigned i = 0; i < numSamples; i++) {
            Sample& s = samples[ins.firstSample + i];
            if (is16[i]) {
                s.pcm.resize(lengths[i] / 2);
                int16_t acc = 0;
                for (auto& v : s.pcm) v = acc += file->read<int16_t>();
                file->skip(lengths[i] & 1);
            } else {
                s.pcm.resize(lengths[i]);
                int8_t acc = 0;
                for (auto& v : s.pcm) v = int16_t(acc += file->read<int8_t>()) * 256;
            }
            for (auto& v : s.pcm) v = int16_t(std::clamp(int32_t(v) * int32_t(amplification) / 256, -32768, 32767));
            if (s.loopStart >= s.pcm.size()) s.loopType = 0;
            if (s.loopType) s.loopLength = std::min<unsigned>(s.loopLength, s.pcm.size() - s.loopStart);
        }
        ins.numSamples = numSamples;
        for (unsigned n = 0; n < 96; n++) {
            if (keymap[n] < numSamples) ins.keymap[n] = ins.firstSample + keymap[n];
        }
        if (size_t(file->rTell()) > fileSize) {
            fmt::print("Truncated XM file.\n");
            return false;
        }
    }
    if (samples.size() > 254) {
        fmt::print("Too many samples ({}), the maximum is 254.\n", samples.size());
        return false;
    }
    m.title = title;
    m.channels = channels;
    m.songLength = songLength;
    m.restart = restart;
    m.numInstruments = numInstruments;
    m.flags = flags;
    m.speed = speed;
    m.bpm = bpm;
    m.orders = std::move(orders);
    m.patterns = std::move(patterns);
    m.instruments = std::move(instruments);
    m.samples = std::move(samples);
    return true;
}

// ProTracker-family .mod files are read as the 4-channel (or N-channel) Amiga-period subset of XM: one
// instrument per sample, no envelopes, the same effect numbers. Returns the channel count, or 0 if the
// tag at offset 1080 is not one this reader knows (15-sample modules included).
static unsigned modChannels(PCSX::IO<PCSX::File>& file) {
    if (file->size() < 1084) return 0;
    file->rSeek(1080);
    auto slice = file->read(4);
    file->rSeek(0);
    const std::string tag(reinterpret_cast<const char*>(slice.data()), 4);
    if (tag == "M.K." || tag == "M!K!" || tag == "FLT4" || tag == "4CHN") return 4;
    if (tag == "FLT8" || tag == "OKTA" || tag == "CD81") return 8;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (digit(tag[0]) && tag.substr(1) == "CHN") return tag[0] - '0';
    if (digit(tag[0]) && digit(tag[1]) && (tag.substr(2) == "CH" || tag.substr(2) == "CN")) {
        return (tag[0] - '0') * 10 + (tag[1] - '0');
    }
    return 0;
}

static bool loadMOD(PCSX::IO<PCSX::File>& file, std::string_view name, unsigned amplification, Module& m) {
    const unsigned channels = modChannels(file);
    if (channels == 0 || channels > 32) {
        fmt::print("{}: unsupported module.\n", name);
        return false;
    }
    auto readString = [&](unsigned size) {
        auto slice = file->read(size);
        std::string s(reinterpret_cast<const char*>(slice.data()), slice.size());
        s.resize(strnlen(s.c_str(), size));
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    auto readBE16 = [&]() {
        unsigned hi = file->read<uint8_t>();
        return (hi << 8) | file->read<uint8_t>();
    };
    m.title = readString(20);
    m.channels = channels;
    m.flags = 0;  // Amiga periods
    m.protracker = true;
    m.speed = 6;
    m.bpm = 125;
    std::vector<unsigned> lengths(31);
    for (unsigned i = 0; i < 31; i++) {
        Sample s;
        s.name = readString(22);
        lengths[i] = readBE16() * 2;
        const int ft = file->read<uint8_t>() & 15;
        s.finetune = int8_t((ft >= 8 ? ft - 16 : ft) * 16);  // ProTracker finetune is 1/8 semitone
        s.volume = std::min<uint8_t>(64, file->read<uint8_t>());
        s.loopStart = readBE16() * 2;
        s.loopLength = readBE16() * 2;
        s.loopType = s.loopLength > 2 ? 1 : 0;
        m.samples.push_back(std::move(s));
    }
    m.songLength = file->read<uint8_t>();
    const unsigned restart = file->read<uint8_t>();
    m.restart = restart < m.songLength ? restart : 0;
    if (m.songLength == 0 || m.songLength > 128) {
        fmt::print("Invalid MOD song length {}.\n", m.songLength);
        return false;
    }
    std::vector<uint8_t> orders(128);
    unsigned numPatterns = 0;
    for (auto& o : orders) {
        o = file->read<uint8_t>();
        numPatterns = std::max(numPatterns, o + 1u);
    }
    orders.resize(m.songLength);
    m.orders = std::move(orders);
    file->skip(4);  // the tag

    // Periods to XM notes, at finetune 0: ProTracker period times 4 against the Amiga table the player uses.
    static const uint16_t c_octave3[12] = {3424, 3232, 3048, 2880, 2712, 2560, 2416, 2280, 2152, 2032, 1920, 1812};
    auto periodToNote = [](unsigned period) -> uint8_t {
        if (period == 0) return 0;
        const unsigned p = period * 4;
        unsigned best = 0, bestDiff = ~0u;
        for (unsigned n = 0; n < 96; n++) {
            const unsigned octave = n / 12;
            const unsigned v = octave >= 3 ? c_octave3[n % 12] >> (octave - 3) : c_octave3[n % 12] << (3 - octave);
            const unsigned d = v > p ? v - p : p - v;
            if (d < bestDiff) bestDiff = d, best = n;
        }
        return best + 1;
    };
    m.patterns.resize(numPatterns);
    for (auto& pattern : m.patterns) {
        pattern.rows = 64;
        pattern.cells.resize(64 * channels);
        for (auto& c : pattern.cells) {
            const uint8_t b0 = file->read<uint8_t>(), b1 = file->read<uint8_t>();
            const uint8_t b2 = file->read<uint8_t>(), b3 = file->read<uint8_t>();
            c.instrument = (b0 & 0xf0) | (b2 >> 4);
            c.note = periodToNote(((b0 & 0x0f) << 8) | b1);
            c.effect = b2 & 0x0f;
            c.param = b3;
            if (c.instrument > 31) c.instrument = 0;
            // ProTracker has no effect memory for these; FT2 would reuse the last parameter on 00.
            if (c.param == 0 && (c.effect == 1 || c.effect == 2 || c.effect == 10)) c.effect = 0;
            if (c.param == 0 && c.effect == 5) c.effect = 3;
            if (c.param == 0 && c.effect == 6) c.effect = 4;
        }
    }
    for (unsigned i = 0; i < 31; i++) {
        Sample& s = m.samples[i];
        s.pcm.resize(lengths[i]);
        for (auto& v : s.pcm) v = int16_t(file->read<int8_t>()) * 256;
        for (auto& v : s.pcm) v = int16_t(std::clamp(int32_t(v) * int32_t(amplification) / 256, -32768, 32767));
        if (s.loopStart >= s.pcm.size()) s.loopType = 0;
        if (s.loopType) s.loopLength = std::min<unsigned>(s.loopLength, s.pcm.size() - s.loopStart);
        Instrument ins;
        ins.name = s.name;
        ins.firstSample = i;
        ins.numSamples = 1;
        std::fill(std::begin(ins.keymap), std::end(ins.keymap), uint8_t(i));
        m.instruments.push_back(std::move(ins));
    }
    m.numInstruments = 31;
    // XM's Amiga periods run on 8363 * 1712 Hz (the NTSC Amiga clock, times 4), ProTracker modules are
    // reference-rendered on the PAL clock, 0.92% lower. Stretch each sample by that ratio. For a looped
    // sample the loop must stay a whole number of samples, so the stretch is the rounded loop length
    // over the old one, and what that misses goes into the finetune, in the 1/16-semitone steps the
    // Amiga table has.
    constexpr double c_clockRatio = (3579545.0 * 4) / (7093789.2 * 2);
    for (auto& s : m.samples) {
        if (s.pcm.empty()) continue;
        double factor = c_clockRatio;
        unsigned loopStart = s.loopStart, loopLength = s.loopLength;
        if (s.loopType) {
            loopLength = std::max(1u, unsigned(std::lround(s.loopLength * c_clockRatio)));
            factor = double(loopLength) / s.loopLength;
            loopStart = unsigned(std::lround(s.loopStart * factor));
        }
        const size_t length = s.loopType ? loopStart + loopLength : size_t(std::lround(s.pcm.size() * factor));
        s.pcm = stretch(s.pcm, factor, s.loopType != 0, s.loopStart, s.loopLength, length);
        s.loopStart = loopStart;
        s.loopLength = loopLength;
        // positive cents: stretched more than the clock needs, so the sample plays low; raise the finetune
        const double cents = 1200.0 * std::log2(factor / c_clockRatio);
        int finetune = s.finetune + int(std::lround(cents / 6.25)) * 8;
        while (finetune > 127) finetune -= 128, s.relnote++;
        while (finetune < -128) finetune += 128, s.relnote--;
        s.finetune = int8_t(finetune);
    }
    fmt::print("Title:       {}\n", m.title);
    fmt::print("Tracker:     ProTracker-family MOD\n");
    fmt::print("Channels:    {}\n", channels);
    fmt::print("Positions:   {}\n", m.songLength);
    fmt::print("Patterns:    {}\n", numPatterns);
    fmt::print("Frequencies: amiga\n");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> samplesFlag(parser, "samples", "", {"s"});
    args::ValueFlag<unsigned> amplificationFlag(parser, "amplification", "", {"a"});
    args::Flag fitFlag(parser, "fit", "", {"f"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
    const auto output = PCSX::ToolArgs::get(outputFlag);
    const auto samplesFile = PCSX::ToolArgs::get(samplesFlag);

    fmt::print(R"(
xmconv
https://github.com/grumpycoders/pcsx-redux/tree/main/tools/xmconv/

)");

    const auto inputs = args::get(inputsList);
    const bool asksForHelp = args::get(helpFlag);
    const bool fit = args::get(fitFlag);
    const auto amplification = PCSX::ToolArgs::get(amplificationFlag).value_or(224);
    if (asksForHelp || inputs.size() != 1 || !output.has_value() || !samplesFile.has_value()) {
        fmt::print(R"(
Usage: {} input.xm|input.mod [-h] [-a amp] [-f] -o output.xmp -s output.smp
  input.xm          mandatory: the input FastTracker 2 module, or a ProTracker-family .mod,
                    which plays with FastTracker 2 effect semantics
  -o output.xmp     mandatory: the song file (patterns, instruments, sample headers)
  -s output.smp     mandatory: the sample file, to upload to SPU memory
  -a amp            optional: sample amplification in 1/256ths. Defaults to 224, which
                    leaves headroom for ADPCM overshoot on full-scale samples.
  -f                optional: if the samples don't fit in SPU memory, halve the
                    sample rate of the largest samples until they do.
  -h                displays this help information and exit.
)",
                   argv[0]);
        return -1;
    }

    PCSX::IO<PCSX::File> file(new PCSX::PosixFile(inputs[0]));
    if (file->failed()) {
        fmt::print("Unable to open file: {}\n", inputs[0]);
        return -1;
    }
    Module m;
    if (!(modChannels(file) ? loadMOD(file, inputs[0], amplification, m) : loadXM(file, inputs[0], amplification, m))) {
        return -1;
    }
    const std::string& title = m.title;
    const unsigned channels = m.channels, songLength = m.songLength, restart = m.restart;
    const unsigned numInstruments = m.numInstruments, flags = m.flags, speed = m.speed, bpm = m.bpm;
    auto& orders = m.orders;
    auto& patterns = m.patterns;
    auto& instruments = m.instruments;
    auto& samples = m.samples;

    // Channels that never carry anything are dropped, and the rest are packed down.
    std::vector<bool> used(channels, false);
    for (auto& p : patterns) {
        for (unsigned i = 0; i < p.cells.size(); i++) {
            if (!p.cells[i].empty()) used[i % channels] = true;
        }
    }
    std::vector<unsigned> channelMap;
    for (unsigned c = 0; c < channels; c++) {
        if (used[c]) channelMap.push_back(c);
    }
    if (channelMap.empty()) channelMap.push_back(0);
    if (channelMap.size() != channels) {
        fmt::print("Dropping {} empty channels, {} remain.\n", channels - channelMap.size(), channelMap.size());
    }
    if (channelMap.size() > c_maxVoices) {
        fmt::print("{} channels are in use, the SPU has {} voices.\n", channelMap.size(), c_maxVoices);
        return -1;
    }
    const unsigned outChannels = channelMap.size();

    // Highest note each sample is played at, to decide how many octaves its rate has to drop so that the
    // pitch register stays under its ceiling. Effects can push higher still; those clamp in the player.
    for (unsigned o : orders) {
        if (o >= patterns.size()) continue;
        auto& p = patterns[o];
        std::vector<unsigned> current(channels, 0);
        for (unsigned r = 0; r < p.rows; r++) {
            for (unsigned c = 0; c < channels; c++) {
                const Cell& cell = p.cells[r * channels + c];
                if (cell.instrument) current[c] = cell.instrument;
                if (cell.note == 0 || cell.note > 96 || current[c] == 0) continue;
                const unsigned s = instruments[current[c] - 1].keymap[cell.note - 1];
                if (s >= samples.size()) continue;
                samples[s].maxRealNote = std::max(samples[s].maxRealNote, int(cell.note) - 1 + samples[s].relnote);
            }
        }
    }
    auto frequency = [](const Sample& s) {
        return 8363.0 * std::pow(2.0, (s.maxRealNote - 48 + s.finetune / 128.0) / 12.0);
    };
    for (auto& s : samples) {
        if (s.maxRealNote < 0 || s.pcm.empty()) continue;
        while ((frequency(s) / (1 << s.rateShift)) > c_maxFrequency && s.rateShift < 4) s.rateShift++;
    }

    auto layoutAll = [&]() {
        unsigned total = 0;
        for (auto& s : samples) {
            if (s.pcm.empty()) continue;
            auto p = prepare(s);
            s.layout = PCSX::SPUSampleLayout::compute(p.pcm.size(), p.hasLoop, p.loopStart, p.loopLength, 0, true,
                                                      c_spuMemory, true);
            s.encodedLoopStart = p.loopStart;
            s.encodedLoopLength = p.loopLength;
            total += s.layout.encodedLength;
        }
        return total;
    };
    unsigned total = layoutAll();
    while (fit && total >= c_spuMemory) {
        Sample* biggest = nullptr;
        for (auto& s : samples) {
            if (s.pcm.empty() || s.rateShift >= 4) continue;
            if (!biggest || s.layout.encodedLength > biggest->layout.encodedLength) biggest = &s;
        }
        if (!biggest) break;
        biggest->rateShift++;
        total = layoutAll();
    }

    fmt::print("Converting samples...\n");
    PCSX::IO<PCSX::File> encoded(new PCSX::PosixFile(samplesFile.value().c_str(), PCSX::FileOps::TRUNCATE));
    std::unique_ptr<PCSX::ADPCM::Encoder> encoder(new PCSX::ADPCM::Encoder);
    unsigned spuOffset = 0;
    for (unsigned i = 0; i < samples.size(); i++) {
        auto& s = samples[i];
        fmt::print("Sample {:3} [{:22}] - ", i, s.name);
        if (s.pcm.empty()) {
            fmt::print("Empty\n");
            continue;
        }
        encoder->reset();
        auto p = prepare(s);
        s.spuOffset = spuOffset;
        const unsigned written = PCSX::SPUSampleLayout::encode(*encoder, p.pcm, s.layout, encoded);
        spuOffset += written;
        fmt::print("{} samples -> {} bytes", s.pcm.size(), written);
        if (s.rateShift) fmt::print(", rate / {}", 1 << s.rateShift);
        if (s.layout.hasLoop) {
            fmt::print(", {} loop {}+{}, rotate {}, unroll {}{}", s.loopType == 2 ? "ping-pong" : "forward",
                       s.layout.loopStart, s.layout.loopLength, s.layout.rotate, s.layout.unroll,
                       s.layout.exact ? "" : ", loop period rounded to a whole block");
        }
        fmt::print("\n");
    }
    encoded->close();
    if (spuOffset >= c_spuMemory) {
        fmt::print("Not enough SPU memory to store all samples; {} bytes required but only {} available.{}\n",
                   spuOffset, c_spuMemory, fit ? "" : " Try -f.");
        return -1;
    }
    fmt::print("Used {} bytes of SPU memory, {} still available.\n", spuOffset, c_spuMemory - spuOffset);

    // Song file.
    PCSX::IO<PCSX::File> out(new PCSX::BufferFile(PCSX::FileOps::READWRITE));
    auto align = [&]() {
        while (out->wTell() & 3) out->write<uint8_t>(0);
    };
    out->write("PSXM", 4);
    out->write<uint16_t>(c_formatVersion);
    out->write<uint16_t>(c_headerSize);
    out->write<uint8_t>(outChannels);
    out->write<uint8_t>(flags & 1);
    out->write<uint8_t>(std::clamp(speed, 1u, 31u));
    out->write<uint8_t>(std::clamp(bpm, 32u, 255u));
    out->write<uint16_t>(songLength);
    out->write<uint16_t>(restart < songLength ? restart : 0);
    out->write<uint16_t>(patterns.size());
    out->write<uint16_t>(numInstruments);
    out->write<uint16_t>(samples.size());
    const size_t periodTableAt = out->wTell();
    out->write<uint16_t>(0);
    out->write<uint32_t>(spuOffset);
    const size_t offsetsAt = out->wTell();
    for (unsigned i = 0; i < 4; i++) out->write<uint32_t>(0);
    {
        char t[20] = {};
        std::copy_n(title.begin(), std::min<size_t>(title.size(), 19), t);
        out->write(t, 20);
    }
    if (m.protracker) {
        out->writeAt<uint16_t>(out->wTell(), periodTableAt);
        for (unsigned row = 0; row < 16; row++) {
            for (unsigned i = 0; i < 36; i++) out->write<uint16_t>(ptPeriod(row, i));
        }
    }
    const unsigned ordersOffset = out->wTell();
    for (auto o : orders) out->write<uint8_t>(o);
    align();
    const unsigned patternTableOffset = out->wTell();
    for (unsigned i = 0; i < patterns.size() * 2; i++) out->write<uint32_t>(0);
    for (unsigned i = 0; i < patterns.size(); i++) {
        auto& p = patterns[i];
        align();
        const unsigned start = out->wTell();
        for (unsigned r = 0; r < p.rows; r++) {
            for (unsigned c : channelMap) {
                const Cell& cell = p.cells[r * channels + c];
                uint8_t mask = (cell.note ? 1 : 0) | (cell.instrument ? 2 : 0) | (cell.volume ? 4 : 0) |
                               (cell.effect ? 8 : 0) | (cell.param ? 16 : 0);
                if (mask == 31) {
                    out->write<uint8_t>(cell.note);
                } else {
                    out->write<uint8_t>(0x80 | mask);
                    if (cell.note) out->write<uint8_t>(cell.note);
                }
                if (cell.instrument) out->write<uint8_t>(cell.instrument);
                if (cell.volume) out->write<uint8_t>(cell.volume);
                if (cell.effect) out->write<uint8_t>(cell.effect);
                if (cell.param) out->write<uint8_t>(cell.param);
            }
        }
        const unsigned size = out->wTell() - start;
        out->writeAt<uint32_t>(start, patternTableOffset + i * 8);
        out->writeAt<uint16_t>(p.rows, patternTableOffset + i * 8 + 4);
        out->writeAt<uint16_t>(size, patternTableOffset + i * 8 + 6);
    }
    align();
    const unsigned instrumentsOffset = out->wTell();
    for (auto& ins : instruments) {
        const size_t start = out->wTell();
        out->write(ins.keymap, 96);
        for (auto* e : {&ins.vol, &ins.pan}) {
            for (auto& p : e->points) out->write<uint16_t>(p[0]), out->write<uint16_t>(p[1]);
        }
        for (auto* e : {&ins.vol, &ins.pan}) {
            out->write<uint8_t>(e->count);
            out->write<uint8_t>(e->sustain);
            out->write<uint8_t>(e->loopStart);
            out->write<uint8_t>(e->loopEnd);
            out->write<uint8_t>(e->flags & 7);
        }
        out->write<uint8_t>(ins.vibType);
        out->write<uint8_t>(ins.vibSweep);
        out->write<uint8_t>(ins.vibDepth);
        out->write<uint8_t>(ins.vibRate);
        out->write<uint16_t>(ins.fadeout);
        if (out->wTell() - start != c_instrumentSize) throw std::runtime_error("instrument record size");
    }
    const unsigned samplesOffset = out->wTell();
    for (auto& s : samples) {
        const size_t start = out->wTell();
        const bool hasLoop = !s.pcm.empty() && s.layout.hasLoop;
        out->write<uint32_t>(s.spuOffset);
        out->write<uint32_t>(s.pcm.empty() ? 0 : s.layout.encodedLength);
        out->write<uint32_t>(s.pcm.empty() ? 0 : (s.pcm.size() + (1 << s.rateShift) - 1) >> s.rateShift);
        out->write<uint32_t>(hasLoop ? s.encodedLoopStart : 0);
        out->write<uint32_t>(hasLoop ? s.encodedLoopLength : 0);
        out->write<uint16_t>(s.layout.pad);
        out->write<uint8_t>((hasLoop ? 1 : 0) | (hasLoop && s.loopType == 2 ? 2 : 0));
        out->write<uint8_t>(s.rateShift);
        out->write<uint8_t>(s.volume);
        out->write<int8_t>(s.finetune);
        out->write<uint8_t>(s.pan);
        out->write<int8_t>(s.relnote);
        out->write<uint32_t>(0);
        if (out->wTell() - start != c_sampleSize) throw std::runtime_error("sample record size");
    }
    out->writeAt<uint32_t>(ordersOffset, offsetsAt);
    out->writeAt<uint32_t>(patternTableOffset, offsetsAt + 4);
    out->writeAt<uint32_t>(instrumentsOffset, offsetsAt + 8);
    out->writeAt<uint32_t>(samplesOffset, offsetsAt + 12);
    if (offsetsAt + 16 + 20 != c_headerSize) throw std::runtime_error("header size");

    PCSX::IO<PCSX::File> song(new PCSX::PosixFile(output.value().c_str(), PCSX::FileOps::TRUNCATE));
    song->write(std::move(out.asA<PCSX::BufferFile>()->borrow()));
    song->close();
    fmt::print("Song file is {} bytes.\n", out->size());
    fmt::print("All done, files {} and {} written out.\n", output.value(), samplesFile.value());
    return 0;
}
