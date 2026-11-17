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

// Self-contained per-voice resampler. Owns the fractional pitch position and
// step, takes the freshly decoded ADPCM samples, and produces one output at the
// emulator's 44.1kHz mixing rate, in one of four modes (none / "Pete's common
// sense" linear / hardware-accurate gaussian / cubic). Extracted out of the
// MainThread synthesis loop and the SPU impl; the math is unchanged.
//
// The working set is five int32 slots that the two mode families alias
// differently: gauss/cubic pack a four-tap int16 window into m_state[1] and
// m_state[2] and use m_state[0] as the ring index, while the linear modes use
// m_state[0] as the step, m_state[1..3] as delay slots and m_state[4] as a
// recompute flag. That overlap is deliberate, which is why they stay one block
// rather than becoming five named members.
//
// The slots used to live in the tail of the per-voice sample buffer, SB[28..32],
// so that the whole thing serialized as one savestate field. They are members
// now and saveTo()/loadFrom() mirror them back into those same SB indices, so
// the savestate format is unchanged. Nothing else here touches the channel: the
// four-tap window is our own, so storeVal()/getVal() need no buffer passed in
// and this class only mentions Protobuf in the savestate mirrors.
class Interpolator {
  public:
    // Gauss/cubic: the first output sample already has four decoded samples in its window,
    // the newest of them at the pitch counter. Matches silicon captures at pitch 2000h, 1300h
    // and 1500h, which a three-sample start puts one source sample behind.
    static constexpr int32_t kGaussKeyOnPosition = 0x40000;

    // Key-on: clear the interpolation window and seed the fractional pitch
    // position for the active mode, exactly as StartSound used to do inline.
    void keyOn(int interpolationType) {
        // Initialize the interpolation helpers.
        m_state[1] = 0;
        m_state[2] = 0;

        if (interpolationType >= 2) {
            m_spos = kGaussKeyOnPosition;
            m_state[0] = 0;
        } else {
            // No/simple interpolation starts with one 44100Hz decode.
            m_spos = 0x10000L;
            m_state[3] = 0;
        }
    }

    // Pitch stepping. The counter is 16.16 fixed point, so kUnity == one whole
    // source sample consumed; the voice pulls a new decoded sample for as long as
    // it owes one, emits at the current fractional position, then advances.
    static constexpr int32_t kUnity = 0x10000;
    bool owesSample() const { return m_spos >= kUnity; }
    void tookSample() { m_spos -= kUnity; }
    void advance() { m_spos += m_sinc; }
    void setStep(int32_t step) { m_sinc = step ? step : 1; }
    int32_t step() const { return m_sinc; }

    // The noise generator substitutes its own level for the decoded sample. The
    // no/simple modes read their input from the current-sample slot, so it has to
    // land there rather than being returned; gauss/cubic weight their own window
    // and want nothing parked. This used to be a write to SB[29] from inside the
    // noise generator, which reached into state that is now ours.
    void parkExternalSample(int32_t level, int interpolationType) {
        if (interpolationType < 2) m_state[1] = level;
    }

    // A psx-pitch change happened: in simple-interpolation mode, flag that the
    // step must be recomputed on the next pass.
    void onFrequencyChanged(int interpolationType) {
        if (interpolationType == 1) m_state[4] = 1;
    }

    // Store one freshly decoded sample `fa` into the interpolation window so a
    // later getVal() can weight it. A frequency-modulator source voice is not
    // resampled at all - its sample is parked raw and handed straight back - so
    // that mode is the only thing either method ever asked the channel's 3-state
    // FMod flag about. `unmuted` is the SPU-wide unmute control bit.
    void storeVal(int fa, int interpolationType, bool isFModSource, bool unmuted);

    // Produce one resampled output sample at the current fractional pitch
    // position (gauss/cubic) / pitch increment (linear).
    int getVal(int interpolationType, bool isFModSource);

    // Savestate mirrors: the slots ride in SB[28..32] on the wire, where they
    // used to live, so old states keep loading.
    void saveTo(Protobuf::Int32 *sb, Protobuf::Int32 &spos, Protobuf::Int32 &sinc) const {
        for (int i = 0; i < kStateSlots; i++) sb[28 + i].value = m_state[i];
        spos.value = m_spos;
        sinc.value = m_sinc;
    }
    void loadFrom(const Protobuf::Int32 *sb, const Protobuf::Int32 &spos, const Protobuf::Int32 &sinc) {
        for (int i = 0; i < kStateSlots; i++) m_state[i] = sb[28 + i].value;
        m_spos = spos.value;
        m_sinc = sinc.value;
    }

    // Post-load fixup: clear the slot the two mode families disagree about, so a
    // state saved under one interpolation mode cannot feed a garbage ring index
    // or step to another. Was "fix to prevent new interpolations from crashing".
    void resetAfterLoad() { m_state[0] = 0; }

  private:
    static constexpr int kStateSlots = 5;

    int16_t &gaussWindow(int index);
    void interpolateUp();
    void interpolateDown();

    int32_t m_state[kStateSlots] = {};
    int32_t m_spos = 0;  // fractional pitch position, 16.16
    int32_t m_sinc = 0;  // pitch step per output sample, 16.16
};

}  // namespace SPU

}  // namespace PCSX
