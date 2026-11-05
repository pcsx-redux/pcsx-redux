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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "fmt/format.h"

namespace PCSX {
namespace ModConv {

// M_PI needs _USE_MATH_DEFINES on MSVC.
constexpr double c_pi = 3.14159265358979323846;

// Stretching a looped sample: the SPU loops on whole 28-sample blocks, so instead of rounding a loop
// period of L samples, the whole sample is resampled by L' / L, where L' is a multiple of 28. The
// sample then plays lower by 1200 * log2(L' / L) cents, which is compensated without changing the
// .hit format: every note played with the sample is transposed up by whole semitones, and the
// remainder goes in the sample's finetune, in 1/8 semitone steps.

// modplayer's MOD_PeriodTable (src/mips/modplayer/modplayer.c): 16 rows of 36 notes, one row per
// finetune, in the order 0 to 7 then -8 to -1. A note is played as MOD_PeriodTable[note + finetune * 36].
inline constexpr uint16_t c_periodTable[36 * 16] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,  // octave 1 tune 0
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,  // octave 2 tune 0
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,  // octave 3 tune 0
    850, 802, 757, 715, 674, 637, 601, 567, 535, 505, 477, 450,  // octave 1 tune 1
    425, 401, 379, 357, 337, 318, 300, 284, 268, 253, 239, 225,  // octave 2 tune 1
    213, 201, 189, 179, 169, 159, 150, 142, 134, 126, 119, 113,  // octave 3 tune 1
    844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474, 447,  // octave 1 tune 2
    422, 398, 376, 355, 335, 316, 298, 282, 266, 251, 237, 224,  // octave 2 tune 2
    211, 199, 188, 177, 167, 158, 149, 141, 133, 125, 118, 112,  // octave 3 tune 2
    838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470, 444,  // octave 1 tune 3
    419, 395, 373, 352, 332, 314, 296, 280, 264, 249, 235, 222,  // octave 2 tune 3
    209, 198, 187, 176, 166, 157, 148, 140, 132, 125, 118, 111,  // octave 3 tune 3
    832, 785, 741, 699, 660, 623, 588, 555, 524, 495, 467, 441,  // octave 1 tune 4
    416, 392, 370, 350, 330, 312, 294, 278, 262, 247, 233, 220,  // octave 2 tune 4
    208, 196, 185, 175, 165, 156, 147, 139, 131, 124, 117, 110,  // octave 3 tune 4
    826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463, 437,  // octave 1 tune 5
    413, 390, 368, 347, 328, 309, 292, 276, 260, 245, 232, 219,  // octave 2 tune 5
    206, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116, 109,  // octave 3 tune 5
    820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460, 434,  // octave 1 tune 6
    410, 387, 365, 345, 325, 307, 290, 274, 258, 244, 230, 217,  // octave 2 tune 6
    205, 193, 183, 172, 163, 154, 145, 137, 129, 122, 115, 109,  // octave 3 tune 6
    814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457, 431,  // octave 1 tune 7
    407, 384, 363, 342, 323, 305, 288, 272, 256, 242, 228, 216,  // octave 2 tune 7
    204, 192, 181, 171, 161, 152, 144, 136, 128, 121, 114, 108,  // octave 3 tune 7
    907, 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480,  // octave 1 tune -8
    453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240,  // octave 2 tune -8
    226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120,  // octave 3 tune -8
    900, 850, 802, 757, 715, 675, 636, 601, 567, 535, 505, 477,  // octave 1 tune -7
    450, 425, 401, 379, 357, 337, 318, 300, 284, 268, 253, 238,  // octave 2 tune -7
    225, 212, 200, 189, 179, 169, 159, 150, 142, 134, 126, 119,  // octave 3 tune -7
    894, 844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474,  // octave 1 tune -6
    447, 422, 398, 376, 355, 335, 316, 298, 282, 266, 251, 237,  // octave 2 tune -6
    223, 211, 199, 188, 177, 167, 158, 149, 141, 133, 125, 118,  // octave 3 tune -6
    887, 838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470,  // octave 1 tune -5
    444, 419, 395, 373, 352, 332, 314, 296, 280, 264, 249, 235,  // octave 2 tune -5
    222, 209, 198, 187, 176, 166, 157, 148, 140, 132, 125, 118,  // octave 3 tune -5
    881, 832, 785, 741, 699, 660, 623, 588, 555, 524, 494, 467,  // octave 1 tune -4
    441, 416, 392, 370, 350, 330, 312, 294, 278, 262, 247, 233,  // octave 2 tune -4
    220, 208, 196, 185, 175, 165, 156, 147, 139, 131, 123, 117,  // octave 3 tune -4
    875, 826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463,  // octave 1 tune -3
    437, 413, 390, 368, 347, 328, 309, 292, 276, 260, 245, 232,  // octave 2 tune -3
    219, 206, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116,  // octave 3 tune -3
    868, 820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460,  // octave 1 tune -2
    434, 410, 387, 365, 345, 325, 307, 290, 274, 258, 244, 230,  // octave 2 tune -2
    217, 205, 193, 183, 172, 163, 154, 145, 137, 129, 122, 115,  // octave 3 tune -2
    862, 814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457,  // octave 1 tune -1
    431, 407, 384, 363, 342, 323, 305, 288, 272, 256, 242, 228,  // octave 2 tune -1
    216, 203, 192, 181, 171, 161, 152, 144, 136, 128, 121, 114,  // octave 3 tune -1
};

// modplayer looks pattern periods up in the finetune 0 row, scanning indices 34 down to 0, so the
// last note of the table, B-3, is never found. Transposed notes have to stay at or below 34.
inline constexpr int c_maxNote = 34;

inline int finetuneFromNibble(uint8_t nibble) {
    nibble &= 15;
    return nibble < 8 ? nibble : int(nibble) - 16;
}
inline uint8_t finetuneToNibble(int finetune) { return uint8_t(finetune) & 15; }
inline unsigned periodFor(int note, int finetune) { return c_periodTable[note + finetuneToNibble(finetune) * 36]; }

// Index of a pattern period in the finetune 0 row, the way modplayer finds it, or -1.
inline int noteFromPeriod(unsigned period) {
    for (int i = c_maxNote; i >= 0; i--) {
        if (c_periodTable[i] == period) return i;
    }
    return -1;
}

// The SPU pitch modplayer sets for a period, with its integer divisions.
inline unsigned spuPitch(unsigned period) { return ((7093789 / (period * 2)) << 12) / 44100; }

// Error in cents of a loop of encodedLoop samples played at period, against a loop of loopLength
// samples played at idealPeriod at the Amiga rate.
inline double loopPitchError(unsigned idealPeriod, unsigned loopLength, unsigned period, unsigned encodedLoop) {
    const double ideal = 7093789.0 / (2.0 * idealPeriod) / loopLength;
    const double played = spuPitch(period) * 44100.0 / 4096.0 / encodedLoop;
    return 1200.0 * std::log2(played / ideal);
}

struct Stretch {
    unsigned loopLength = 0;     // L, samples
    unsigned newLoopLength = 0;  // L', samples, a multiple of 28
    int semitones = 0;
    int oldFinetune = 0;
    int finetune = 0;
    double worstCents = 0;    // worst loop pitch error over the notes played, after stretching
    double roundedCents = 0;  // same, when the loop is rounded to whole blocks instead
    unsigned offTable = 0;    // notes played that would transpose past the table
    double ratio() const { return double(newLoopLength) / loopLength; }
};

// Picks L' among the multiples of 28 from L rounded up to 2L, with its transposition. notes are the
// finetune 0 note indices the sample is played at in the song; if empty, every note is considered.
// The choice keeps every note on the table if it can, then minimizes the worst pitch error over the
// notes. Candidates within 2 cents of the best one count as equal, and the shortest one wins: that is
// about one step of the SPU pitch register at C-2 (pitch 770 for 44100 Hz = 4096), which modplayer
// already rounds every note to, so a smaller gain is not worth the extra SPU memory. That step is
// derived from the register scale, not measured.
inline Stretch chooseStretch(unsigned loopLength, int finetune, std::vector<int> notes) {
    if (notes.empty()) {
        for (int n = 0; n <= c_maxNote; n++) notes.push_back(n);
    }
    const unsigned rounded = std::max(28u, (loopLength + 14) / 28 * 28);
    double roundedCents = 0;
    for (auto n : notes) {
        const unsigned ideal = periodFor(n, finetune);
        roundedCents = std::max(roundedCents, std::abs(loopPitchError(ideal, loopLength, ideal, rounded)));
    }
    std::vector<Stretch> candidates;
    for (unsigned lp = (loopLength + 27) / 28 * 28; lp <= std::max(2 * loopLength, 28u); lp += 28) {
        const double cents = 1200.0 * std::log2(double(lp) / loopLength);
        const int nearest = finetune + int(std::lround(cents / 12.5));
        for (int eighths = nearest - 1; eighths <= nearest + 1; eighths++) {
            Stretch s;
            s.loopLength = loopLength;
            s.newLoopLength = lp;
            s.oldFinetune = finetune;
            s.roundedCents = roundedCents;
            s.semitones = int(std::floor((eighths + 4) / 8.0));
            s.finetune = eighths - 8 * s.semitones;
            for (auto n : notes) {
                const int moved = n + s.semitones;
                if (moved > c_maxNote || moved < 0) {
                    s.offTable++;
                    continue;
                }
                const unsigned ideal = periodFor(n, finetune);
                s.worstCents = std::max(s.worstCents,
                                        std::abs(loopPitchError(ideal, loopLength, periodFor(moved, s.finetune), lp)));
            }
            candidates.push_back(s);
        }
    }
    const auto best = std::min_element(candidates.begin(), candidates.end(), [](const Stretch& a, const Stretch& b) {
        return (a.offTable != b.offTable) ? a.offTable < b.offTable : a.worstCents < b.worstCents;
    });
    for (auto& c : candidates) {
        if ((c.offTable == best->offTable) && (c.worstCents <= best->worstCents + 2.0)) return c;
    }
    return *best;
}

// Resamples a looped sample so that its loop is newLoop samples long. The loop is resampled as a
// periodic signal, so the last sample of the loop leads into the first one the same way the rest of
// the loop does, and the SPU wraps around without a seam. The part before the loop is resampled by
// the same ratio, and newLoopStart is rounded to a whole MOD word. Windowed sinc, 32 taps, Kaiser
// window; this only ever upsamples, so the source band needs no filtering.
inline std::vector<int16_t> resampleLoop(const std::vector<int16_t>& source, unsigned loopStart, unsigned loopLength,
                                         unsigned newLoop, unsigned& newLoopStart) {
    const double ratio = double(newLoop) / loopLength;
    newLoopStart = unsigned(std::lround(loopStart * ratio / 2.0)) * 2;
    constexpr int halfTaps = 16;
    constexpr double beta = 9.0;
    auto i0 = [](double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 32; k++) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
        }
        return sum;
    };
    const double i0beta = i0(beta);
    auto kernel = [&](double d) {
        if (std::abs(d) >= halfTaps) return 0.0;
        const double w = i0(beta * std::sqrt(1.0 - (d / halfTaps) * (d / halfTaps))) / i0beta;
        if (d == 0) return w;
        const double x = c_pi * d;
        return w * std::sin(x) / x;
    };
    // Inside the loop, the source is read as the loop repeating forever in both directions.
    auto at = [&](long i, bool periodic) -> double {
        const long s = loopStart, l = loopLength;
        if (periodic || (i >= s)) {
            long m = (i - s) % l;
            if (m < 0) m += l;
            return source[s + m];
        }
        if (i < 0) return 0;
        return source[i];
    };
    std::vector<int16_t> out(newLoopStart + newLoop);
    for (unsigned j = 0; j < out.size(); j++) {
        const bool inLoop = j >= newLoopStart;
        const double x = double(loopStart) + (double(j) - double(newLoopStart)) / ratio;
        const long base = long(std::floor(x));
        double acc = 0;
        for (long k = base - halfTaps + 1; k <= base + halfTaps; k++) acc += at(k, inLoop) * kernel(x - k);
        out[j] = int16_t(std::clamp(std::lround(acc), -32768l, 32767l));
    }
    return out;
}

// Which sample each pattern cell plays with, following the order table the way the player does: a
// cell with a sample number switches the channel to it, a cell without one keeps the channel's
// current sample. Dxx and Bxx end the pattern: Bxx goes to the position it names, Dxx starts the
// next pattern at its row. The walk ends when it reaches a position and row it has already been at
// with the same samples current. Cells never reached stay at 0.
struct CellMap {
    std::vector<uint8_t> sample;    // per cell: 0 = not played, 1-31 = sample number on the first visit
    std::vector<bool> conflict;     // the cell is reached with two different samples
    std::vector<uint32_t> samples;  // every sample the cell is reached with, bit n = sample n
    // Every last note the channel can have at the cell, as a finetune 0 note index and the sample it
    // was keyed with: what a cell without a period plays from. Unknown notes are left out.
    std::vector<std::vector<std::pair<int8_t, uint8_t>>> lastNotes;
    // The channel's tone portamento speed (the last nonzero 3xx), and the sample current when it was
    // set: what 300 and 5xy slide at. speedFrom is 0 when no speed was set, 0xff when it depends on
    // the path.
    std::vector<uint8_t> speed;
    std::vector<uint8_t> speedFrom;
};

inline CellMap mapCells(const std::vector<uint8_t>& patterns, unsigned channels, const uint8_t* orders,
                        unsigned positions) {
    const unsigned patternCount = patterns.size() / (channels * 256);
    CellMap map;
    map.sample.assign(patternCount * 64 * channels, 0);
    map.conflict.assign(map.sample.size(), false);
    map.samples.assign(map.sample.size(), 0);
    map.lastNotes.assign(map.sample.size(), {});
    map.speed.assign(map.sample.size(), 0);
    map.speedFrom.assign(map.sample.size(), 0);
    std::vector<bool> seen(map.sample.size(), false);
    std::vector<uint8_t> current(channels, 0);
    std::vector<int> lastNote(channels, -1);
    std::vector<uint8_t> lastNoteSample(channels, 0), speed(channels, 0), speedFrom(channels, 0);
    // The walk stops on a position and row already reached with the same channel state: the current
    // samples, and the last notes and slide speeds the cell map records.
    auto channelState = [&]() {
        std::vector<uint8_t> state(current);
        for (unsigned ch = 0; ch < channels; ch++) {
            state.push_back(uint8_t(lastNote[ch]));
            state.push_back(lastNoteSample[ch]);
            state.push_back(speed[ch]);
            state.push_back(speedFrom[ch]);
        }
        return state;
    };
    std::set<std::tuple<unsigned, unsigned, std::vector<uint8_t>>> visited;
    unsigned pos = 0, startRow = 0;
    while ((positions != 0) && visited.emplace(pos, startRow, channelState()).second) {
        const unsigned pattern = orders[pos];
        bool jump = false, brk = false;
        unsigned jumpTo = 0, breakRow = 0;
        for (unsigned row = startRow; (pattern < patternCount) && (row < 64); row++) {
            for (unsigned ch = 0; ch < channels; ch++) {
                const unsigned cell = (pattern * 64 + row) * channels + ch;
                const uint8_t* c = patterns.data() + cell * 4;
                const uint8_t smp = (c[0] & 0xf0) | (c[2] >> 4);
                if (smp != 0 && smp <= 31) current[ch] = smp;
                if (current[ch] != 0) {
                    if (map.sample[cell] == 0) {
                        map.sample[cell] = current[ch];
                    } else if (map.sample[cell] != current[ch]) {
                        map.conflict[cell] = true;
                    }
                    map.samples[cell] |= 1u << current[ch];
                }
                const unsigned fx = c[2] & 0x0f;
                const unsigned period = ((c[0] & 0x0f) << 8) | c[1];
                if (period != 0) {
                    lastNote[ch] = noteFromPeriod(period);
                    lastNoteSample[ch] = current[ch];
                }
                if ((fx == 0x3) && (c[3] != 0)) {
                    speed[ch] = c[3];
                    speedFrom[ch] = current[ch];
                }
                if (lastNote[ch] >= 0) {
                    const std::pair<int8_t, uint8_t> last(int8_t(lastNote[ch]), lastNoteSample[ch]);
                    auto& notes = map.lastNotes[cell];
                    if (std::find(notes.begin(), notes.end(), last) == notes.end()) notes.push_back(last);
                }
                if (!seen[cell]) {
                    seen[cell] = true;
                    map.speed[cell] = speed[ch];
                    map.speedFrom[cell] = speedFrom[ch];
                } else {
                    if ((map.speed[cell] != speed[ch]) || (map.speedFrom[cell] != speedFrom[ch])) {
                        map.speedFrom[cell] = 0xff;
                    }
                }
                // The first channel with each effect wins, as in the player.
                if ((fx == 0xd) && !brk) {
                    brk = true;
                    breakRow = unsigned(c[3] >> 4) * 10 + (c[3] & 0x0f);
                } else if ((fx == 0xb) && !jump) {
                    jump = true;
                    jumpTo = c[3];
                }
            }
            if (jump || brk) break;
        }
        // The player's order and row transitions: positions wrap to 0, a Dxx past row 63 starts at
        // row 0, and Bxx with Dxx on the same row lands on the position after the Bxx target.
        unsigned next = (pos + 1 < positions) ? pos + 1 : 0;
        if (jump) next = (jumpTo < positions) ? jumpTo : 0;
        startRow = 0;
        if (brk) {
            startRow = (breakRow < 64) ? breakRow : 0;
            if (jump && (++next >= positions)) next = 0;
        }
        pos = next;
    }
    return map;
}

// The finetune 0 notes each sample is played at, from a cell map. Indexed by sample number, 1-31.
inline std::vector<std::vector<int>> notesPerSample(const std::vector<uint8_t>& patterns, const CellMap& map) {
    std::vector<std::vector<int>> notes(32);
    for (unsigned cell = 0; cell < map.sample.size(); cell++) {
        if (map.sample[cell] == 0) continue;
        const uint8_t* c = patterns.data() + cell * 4;
        const int note = noteFromPeriod(((c[0] & 0x0f) << 8) | c[1]);
        if (note < 0) continue;
        auto& v = notes[map.sample[cell]];
        if (std::find(v.begin(), v.end(), note) == v.end()) v.push_back(note);
    }
    return notes;
}

// Rewrites the pattern cells played with a stretched sample, and returns warnings, one per line.
// stretches is indexed by sample number, 1-31.
// - notes are transposed by the stretch's semitones; notes moving past the table are clamped;
// - portamento speeds (1xx, 2xx, 3xx, E1x, E2x) and vibrato depth (4xy) are in period units, and the
//   transposed notes have periods smaller by the stretch ratio, so they are divided by it;
// - 9xx offsets are in sample bytes, so they are multiplied by it, clamped to ff;
// - E5x sets the sample's finetune, so it gets the same finetune shift as the sample;
// - 300 slides at the channel's last 3xx speed: when that speed was set on another sample, the
//   compensated speed is written in explicitly, and 5xy, which has no speed to write, is reported;
// - arpeggios reaching past the table are reported, including on cells without a note, which play
//   from the channel's last one: modplayer would read the next finetune's row;
// - a cell reached with a stretched sample and another one can only be right for one of them, and is
//   reported; it is rewritten for the sample it is first reached with.
inline std::vector<std::string> rewritePatterns(std::vector<uint8_t>& patterns, const CellMap& map,
                                                const std::vector<std::optional<Stretch>>& stretches) {
    std::vector<std::string> warnings;
    std::vector<unsigned> clamped(32), unknown(32), conflicts(32), skipped(32), offsets(32), arpeggios(32),
        finetunes(32), slides(32);
    auto scaleDown = [](unsigned v, double ratio) -> unsigned {
        if (v == 0) return 0;
        return std::max(1u, unsigned(std::lround(v / ratio)));
    };
    for (unsigned cell = 0; cell < map.sample.size(); cell++) {
        uint8_t* c = patterns.data() + cell * 4;
        const unsigned active = map.sample[cell];
        const uint8_t rowSample = (c[0] & 0xf0) | (c[2] >> 4);
        const unsigned fx = c[2] & 0x0f;
        // E5x applies to the sample of the cell, or the channel's current one.
        if ((fx == 0xe) && ((c[3] >> 4) == 5)) {
            const unsigned target = rowSample ? rowSample : active;
            if (target != 0 && target <= 31 && stretches[target]) {
                const auto& s = *stretches[target];
                int ft = finetuneFromNibble(c[3]) + s.finetune - s.oldFinetune;
                if (ft < -8 || ft > 7) {
                    finetunes[target]++;
                    ft = std::clamp(ft, -8, 7);
                }
                c[3] = 0x50 | finetuneToNibble(ft);
            }
        }
        // Only a cell with a note or an effect can be wrong for one of its samples.
        const bool shared = map.conflict[cell] && ((c[0] & 0x0f) || c[1] || (c[2] & 0x0f) || c[3]);
        if (shared) {
            for (unsigned i = 1; i <= 31; i++) {
                if ((i != active) && (map.samples[cell] & (1u << i)) && stretches[i]) skipped[i]++;
            }
        }
        if (active == 0 || !stretches[active]) continue;
        const auto& s = *stretches[active];
        if (shared) conflicts[active]++;
        const double ratio = s.ratio();
        const unsigned period = ((c[0] & 0x0f) << 8) | c[1];
        int moved = -1;
        if (period != 0) {
            const int note = noteFromPeriod(period);
            if (note < 0) {
                unknown[active]++;
            } else {
                moved = note + s.semitones;
                if (moved > c_maxNote || moved < 0) {
                    clamped[active]++;
                    moved = std::clamp(moved, 0, c_maxNote);
                }
                const unsigned np = c_periodTable[moved];
                c[0] = (c[0] & 0xf0) | (np >> 8);
                c[1] = np & 0xff;
            }
        }
        switch (fx) {
            case 0x0:
                if (c[3] != 0) {
                    // Without a note, check the highest note the channel can be on.
                    int base = moved;
                    if (period == 0) {
                        for (auto [note, from] : map.lastNotes[cell]) {
                            const int shifted =
                                note + ((from != 0 && stretches[from]) ? stretches[from]->semitones : 0);
                            base = std::max(base, std::clamp(shifted, 0, c_maxNote));
                        }
                    }
                    if ((base >= 0) && ((base + std::max(c[3] >> 4, c[3] & 0x0f)) > 35)) arpeggios[active]++;
                }
                break;
            case 0x3:
                if ((c[3] == 0) && (map.speedFrom[cell] != active) && (map.speedFrom[cell] != 0)) {
                    if (map.speedFrom[cell] == 0xff) {
                        slides[active]++;
                    } else {
                        c[3] = std::min(255u, scaleDown(map.speed[cell], ratio));
                    }
                    break;
                }
                [[fallthrough]];
            case 0x1:
            case 0x2:
                c[3] = std::min(255u, scaleDown(c[3], ratio));
                break;
            case 0x5:
                if ((map.speedFrom[cell] != active) && (map.speedFrom[cell] != 0)) slides[active]++;
                break;
            case 0x4:
                c[3] = (c[3] & 0xf0) | std::min(15u, scaleDown(c[3] & 0x0f, ratio));
                break;
            case 0x9:
                if (c[3] != 0) {
                    long v = std::lround(c[3] * ratio);
                    if (v > 255) {
                        offsets[active]++;
                        v = 255;
                    }
                    c[3] = uint8_t(v);
                }
                break;
            case 0xe:
                if ((c[3] >> 4) == 1 || (c[3] >> 4) == 2) {
                    c[3] = (c[3] & 0xf0) | std::min(15u, scaleDown(c[3] & 0x0f, ratio));
                }
                break;
        }
    }
    for (unsigned i = 1; i <= 31; i++) {
        if (!stretches[i]) continue;
        if (clamped[i])
            warnings.push_back(
                fmt::format("sample {}: {} notes transposed past the period table, clamped", i, clamped[i]));
        if (unknown[i])
            warnings.push_back(
                fmt::format("sample {}: {} notes with a period not in the table, left alone", i, unknown[i]));
        if (conflicts[i])
            warnings.push_back(fmt::format(
                "sample {}: {} cells also played with another sample, rewritten for this one", i, conflicts[i]));
        if (skipped[i])
            warnings.push_back(
                fmt::format("sample {}: {} cells first played with another sample, left as they were", i, skipped[i]));
        if (slides[i])
            warnings.push_back(
                fmt::format("sample {}: {} tone portamentos reuse a speed set with another sample, left as they were",
                            i, slides[i]));
        if (offsets[i])
            warnings.push_back(fmt::format("sample {}: {} 9xx offsets past ff after scaling, clamped", i, offsets[i]));
        if (arpeggios[i])
            warnings.push_back(fmt::format("sample {}: {} arpeggios reach past the period table", i, arpeggios[i]));
        if (finetunes[i])
            warnings.push_back(
                fmt::format("sample {}: {} E5x finetunes out of range after the shift, clamped", i, finetunes[i]));
    }
    return warnings;
}

}  // namespace ModConv
}  // namespace PCSX
