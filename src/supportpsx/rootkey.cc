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

#include "supportpsx/rootkey.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double c_pi = 3.14159265358979323846;
// A one-shot's attack is skipped up to this many points, or an eighth of the sample.
constexpr size_t c_attack = 2048;

}  // namespace

double PCSX::RootKey::noteOf(double hz) { return c_a4Note + 12.0 * std::log2(hz / c_a4Hz); }

PCSX::RootKey::Result PCSX::RootKey::fromFundamental(const Fundamental::Estimate& fundamental) {
    Result r;
    if (!fundamental.pitched || !(fundamental.hz > 0.0)) return r;
    const double note = noteOf(fundamental.hz);
    const double key = std::round(note);
    if (key < 0.0 || key > 127.0) return r;
    r.status = Status::Unconfirmed;
    r.note = note;
    r.key = uint8_t(key);
    r.correction = int8_t(std::clamp(std::lround((key - note) * 100.0), -50l, 50l));
    return r;
}

PCSX::RootKey::Result PCSX::RootKey::combine(const Fundamental::Estimate& fundamental,
                                             const std::vector<uint16_t>& keyOnPitches) {
    Result r = fromFundamental(fundamental);
    if (r.status == Status::Unpitched) return r;
    // Distances to the grid are circular: 0.49 and -0.49 semitones are neighbours.
    double c = 0.0, s = 0.0;
    for (uint16_t p : keyOnPitches) {
        if (p == 0) continue;
        const double played = r.note + 12.0 * std::log2(double(p) / 0x1000);
        const double off = played - std::round(played);
        c += std::cos(2.0 * c_pi * off);
        s += std::sin(2.0 * c_pi * off);
        r.keyOns++;
    }
    if (r.keyOns == 0) return r;
    c /= r.keyOns;
    s /= r.keyOns;
    r.coherence = std::sqrt(c * c + s * s);
    r.gridCents = std::atan2(s, c) / (2.0 * c_pi) * 100.0;
    const bool agrees = std::abs(r.gridCents) <= c_agreeCents && r.coherence >= c_minCoherence;
    r.status = agrees ? Status::Agrees : Status::Disagrees;
    return r;
}

std::vector<int16_t> PCSX::RootKey::analysisWindow(const std::vector<int16_t>& pcm, bool loops, uint32_t loopStart) {
    constexpr size_t cap = Fundamental::c_maxInput;
    if (loops && loopStart < pcm.size()) {
        // The loop is the sustained sound. Repeating it keeps it exactly as periodic as the
        // hardware plays it, and gives a short loop enough periods to measure.
        std::vector<int16_t> out(pcm.begin() + loopStart, pcm.end());
        const size_t length = out.size();
        out.reserve(cap);
        while (out.size() < cap) out.push_back(out[out.size() - length]);
        out.resize(cap);
        return out;
    }
    const size_t skip = std::min(c_attack, pcm.size() / 8);
    const size_t end = std::min(pcm.size(), skip + cap);
    return std::vector<int16_t>(pcm.begin() + skip, pcm.begin() + end);
}

const char* PCSX::RootKey::statusName(Status status) {
    switch (status) {
        case Status::Unpitched:
            return "unpitched";
        case Status::Unconfirmed:
            return "unconfirmed";
        case Status::Agrees:
            return "agrees";
        case Status::Disagrees:
            return "disagrees";
    }
    return "?";
}

std::string PCSX::RootKey::noteName(uint8_t key) {
    static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[key % 12]) + std::to_string(int(key / 12) - 1);
}
