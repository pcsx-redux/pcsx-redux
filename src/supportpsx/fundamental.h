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

#include <optional>
#include <span>

namespace PCSX {

namespace Fundamental {

// The longest input estimate() is asked to look at.
constexpr size_t c_maxInput = 16384;
// The search range: A0 to C8.
constexpr double c_lowestHz = 27.5;
constexpr double c_highestHz = 4186.01;

// Fundamental frequency in Hz of a mono sample, at most c_maxInput points at `rate` Hz, or
// nullopt when it has none: noise, percussion, silence. The caller picks the stretch to
// analyse, normally the sustained or looped part. Runs on the SPU mixer thread, once per
// replaced sample.
std::optional<double> estimate(std::span<const int16_t> pcm, uint32_t rate);

}  // namespace Fundamental

}  // namespace PCSX
