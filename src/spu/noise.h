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

#include "support/protobuf.h"

namespace PCSX {

namespace SPU {

// The SPU's single global noise generator. One LFSR, shared by every voice
// whose noise-mode bit is set: such a voice outputs the current noise level in
// place of its decoded/interpolated ADPCM sample. The shift+step bits of
// SPUCTRL select the clock that paces the LFSR.
//
// Unlike the per-voice ADPCM/ADSR/interpolation helpers this is a single
// per-SPU instance, so it lives in the impl rather than in SPUCHAN. getVal()
// just reports the level; parking it where the simple interpolation modes look
// for it is the interpolator's business, since that slot is its state.
class NoiseGenerator {
  public:
    // The SPUCTRL noise shift+step selector (register bits 13..8, already
    // shifted down). Recomputed on every SPUCTRL write.
    // Masked to the six bits the register actually supplies. The register path already narrows
    // it, but the savestate path did not, and a clock of 64 or more drives the shift past 15, so
    // the threshold in step() computes to zero and its subtract loop never terminates.
    void setClock(uint32_t clock) { m_clock = clock & kClockMask; }

    // Advance the LFSR by one mixing sample (was impl::NoiseClock()). Called
    // once per output sample, before any voice consumes the level.
    void step();

    // The current noise output sample, sign-extended to 16 bits (was
    // impl::iGetNoiseVal()). In the no/simple interpolation modes the value is
    // also parked in the voice's "current sample" slot SB[29], exactly as
    // before, so the linear resampler sees it.
    int getVal() const;

    // Savestate bridge (freeze.cc only): mirror the LFSR state to/from the three
    // SPU-level savestate fields. The fields are passed in to keep the
    // conversion next to the state it serializes.
    // legacyCount is the 16.16 accumulator older states carry instead of position; it is written
    // as zero, and read only when position is zero.
    void saveTo(Protobuf::UInt32 &clock, Protobuf::UInt32 &legacyCount, Protobuf::UInt32 &value,
                Protobuf::UInt32 &position) const {
        clock.value = m_clock;
        legacyCount.value = 0;
        value.value = m_val;
        position.value = m_count;
    }
    void loadFrom(const Protobuf::UInt32 &clock, const Protobuf::UInt32 &legacyCount, const Protobuf::UInt32 &value,
                  const Protobuf::UInt32 &position) {
        setClock(clock.value);
        m_count = position.value ? position.value : legacyCount.value >> 16;
        m_val = value.value;
    }

  private:
    // SPUCTRL bits 13..8: four shift bits and two step bits.
    static constexpr uint32_t kClockMask = 0x3f;

    uint32_t m_clock = 0;  // shift+step selector from SPUCTRL
    uint32_t m_count = 0;  // position in the noise period, in samples
    uint32_t m_val = 1;    // LFSR state; the noise level lives in the low 16 bits
};

}  // namespace SPU

}  // namespace PCSX
