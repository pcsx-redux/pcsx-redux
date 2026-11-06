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

#include <stddef.h>
#include <stdint.h>

namespace PCSX {

namespace Fundamental {

// The longest input estimate() is asked to look at.
constexpr size_t c_maxInput = 16384;
// The search range: A0 to C8.
constexpr double c_lowestHz = 27.5;
constexpr double c_highestHz = 4186.01;

struct Estimate {
    bool pitched = false;  // false for noise, percussion and silence
    double hz = 0.0;       // the fundamental, meaningful only when pitched
};

// Fundamental frequency of a mono sample, `count` points (at most c_maxInput) at `rate` Hz.
// The caller picks the stretch to analyse, normally the sustained or looped part. Runs on
// the SPU mixer thread, once per replaced sample.
Estimate estimate(const int16_t* pcm, size_t count, uint32_t rate);

}  // namespace Fundamental

}  // namespace PCSX
