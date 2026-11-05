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

#include "spu/noise.h"

namespace {
// The noise period P = 0x20000 >> shift samples is split in eighths, and the
// LFSR clocks on the selected ones, one bit per eighth: the start of every
// quarter, plus the half-way point of one, two (opposite) or three quarters
// for step 1, 2 or 3. Measured on SCPH-1000 and SCPH-1001.
constexpr uint8_t kSlots[4] = {0x55, 0x57, 0x77, 0x7f};
}  // namespace

int PCSX::SPU::NoiseGenerator::getVal() const {
    // The noise level is the low 16 bits of the LFSR, signed.
    return static_cast<int16_t>(m_val);
}

void PCSX::SPU::NoiseGenerator::step() {
    // kWaveform is the precomputed LFSR feedback bit (bit 15 ^ 12 ^ 11 ^ 10 ^ 1),
    // indexed by bits 15..10.
    static constexpr uint8_t kWaveform[64] = {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1,
                                              1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
                                              1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1};

    const uint32_t shift = m_clock >> 2;
    const uint32_t step = m_clock & 3;
    const uint32_t period = 0x20000u >> shift;

    const uint32_t sample = (m_count + 1) % period;
    m_count = sample;

    // Eighth j starts at sample j * P / 8, rounded down, so this sample holds
    // eighths [first, last). At the fastest rate two share a sample, and the
    // LFSR still clocks only once.
    const uint32_t first = (sample * 8 + period - 1) / period;
    const uint32_t last = ((sample + 1) * 8 + period - 1) / period;
    const uint32_t covered = ((1u << last) - 1) & ~((1u << first) - 1);
    if (kSlots[step] & covered) m_val = (m_val << 1) | kWaveform[(m_val >> 10) & 63];
}
