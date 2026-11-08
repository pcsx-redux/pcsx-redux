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

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "supportpsx/fundamental.h"

namespace PCSX {

namespace RootKey {

// MIDI note numbering: note 69 is 440 Hz, which is A4 in scientific pitch notation (A3 in
// the Yamaha convention, where middle C is C3).
constexpr double c_a4Hz = 440.0;
constexpr int c_a4Note = 69;
// The SoundFont root key of an unpitched sample.
constexpr uint8_t c_unpitched = 255;
// How far the played notes may sit from the equal-tempered grid, on average, and how tightly
// they must agree with each other, for the voice log to confirm a fundamental.
constexpr double c_agreeCents = 25.0;
constexpr double c_minCoherence = 0.9;

// Fractional MIDI note of a frequency.
double noteOf(double hz);

enum class Status {
    Unpitched,    // no fundamental: noise, percussion, silence
    Unconfirmed,  // a fundamental, and no key-on pitch to check it against
    Agrees,       // the key-on pitches land on the A440 grid with this fundamental
    Disagrees,    // they do not: needs a human
};

struct Result {
    Status status = Status::Unpitched;
    // The note the sample sounds at SPU pitch 1000h (44100 Hz), fractional. 60 when unpitched,
    // which is the SPU Samples export's frame for a sample with no known pitch.
    double note = 60.0;
    uint8_t key = c_unpitched;  // SoundFont root key: the nearest note, or c_unpitched
    int8_t correction = 0;      // SoundFont pitch correction in cents, bringing note onto key
    // Mean distance of the played notes from the nearest equal-tempered note, in cents, and
    // how consistent they are (1 = all at the same distance). Only set from key-ons.
    double gridCents = 0.0;
    double coherence = 0.0;
    uint64_t keyOns = 0;
};

// From the fundamental of a sample decoded at its stored rate, which SPU pitch 1000h plays
// unchanged.
Result fromFundamental(std::optional<double> fundamental);

// Same, then checked against the pitch registers the game keyed the sample on with. Each
// pitch p plays the sample at note + 12 log2(p / 1000h); a game tuned to A440 equal
// temperament puts those on whole notes. An octave error in the fundamental (or a pick of the
// third harmonic, two cents off a twelfth) is invisible to this check. Pitch 0 is skipped.
// The offset it measures is the sample's own detune plus the game's fine tuning.
Result combine(std::optional<double> fundamental, const std::vector<uint16_t>& keyOnPitches);

// Key-on count by pitch register value.
using PitchCounts = std::map<uint16_t, uint32_t>;
// Same as combine(), from counts.
Result combineCounts(std::optional<double> fundamental, const PitchCounts& keyOnPitches);

// The stretch of a decoded sample to estimate the fundamental from: a loop repeated up to
// Fundamental::c_maxInput points, or a one-shot past its attack. `loopStart` is in points,
// and only read when `loops`.
std::vector<int16_t> analysisWindow(const std::vector<int16_t>& pcm, bool loops, uint32_t loopStart);

const char* statusName(Status status);

// Scientific pitch notation of a MIDI note: 60 is "C4", 69 is "A4".
std::string noteName(uint8_t key);

}  // namespace RootKey

}  // namespace PCSX
