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

#include "spu/types.h"
#include "support/protobuf.h"

namespace PCSX {

namespace SPU {

// The SPU's single global reverb unit. Holds the register-mapped reverb work
// state (REVERBInfo) and the reverb mixing buffer pointers. The unit was
// extracted from the Pete/Neill implementation in the SPU impl, but the DSP is
// no longer that code: it implements the reverb formula psx-spx documents. The
// formerly-implicit impl state (the reverb mode setting, SPUCTRL, sound RAM,
// NSSIZE) is passed in as parameters so the unit owns only reverb state.
//
// Like the noise generator this is a single per-SPU instance and lives in the
// impl rather than in SPUCHAN. The `rvb`, `sRVB*` and `iReverb*` members keep
// their original names.
class ReverbUnit {
  public:
    // SPUCTRL bit 7: reverb master enable (was impl::ControlFlags::ReverbMasterEnable).
    static constexpr uint16_t kReverbMasterEnable = 1 << 7;

    // Reverb work state, written directly by the register handlers.
    REVERBInfo rvb;

    // Reverb mixing buffer (allocated/freed by the impl, which stores the pointer
    // here). One interleaved stereo pair per sample of the batch.
    int *mixStart = 0;

    // Zero the reverb work state (was memset(&rvb, 0, sizeof(REVERBInfo))).
    void reset();

    // Per-voice reverb arm on key-on (was impl::StartREVERB).
    void start(SPUCHAN *voice, uint16_t spuCtrl);
    // Re-init the mixing buffer each block (was impl::InitREVERB).
    void init(int nssize);
    // Mix one active reverb voice into the buffer (was impl::StoreREVERB).
    void store(SPUCHAN *voice, int ns);
    // Produce the left/right wet-out sample (was impl::MixREVERBLeft / MixREVERBRight).
    int mixLeft(int ns, uint16_t *spuMem, uint16_t spuCtrl);
    int mixRight();

    // Savestate bridge (freeze.cc only): mirror the resampling filter state to/from the
    // SPU-level savestate fields. A state saved without them carries zeroes, which is a
    // reset filter.
    template <typename InL, typename InR, typename OutL, typename OutR>
    void saveTo(InL &inL, InR &inR, OutL &outL, OutR &outR, Protobuf::Bool &altLeftTick,
                Protobuf::Int32 &callCount) const {
        saveHistory(m_h44L, inL);
        saveHistory(m_h44R, inR);
        saveHistory(m_h22L, outL);
        saveHistory(m_h22R, outR);
        altLeftTick.value = m_altLeftTick;
        callCount.value = m_callCount;
    }
    template <typename InL, typename InR, typename OutL, typename OutR>
    void loadFrom(const InL &inL, const InR &inR, const OutL &outL, const OutR &outR,
                  const Protobuf::Bool &altLeftTick, const Protobuf::Int32 &callCount) {
        loadHistory(m_h44L, inL);
        loadHistory(m_h44R, inR);
        loadHistory(m_h22L, outL);
        loadHistory(m_h22R, outR);
        m_altLeftTick = altLeftTick.value;
        m_callCount = callCount.value;
    }

  private:
    static constexpr int kFirTaps = 39;
    static constexpr int kFirOutTaps = (kFirTaps + 1) / 2;

    template <int N, typename Field>
    static void saveHistory(const int (&history)[N], Field &field) {
        for (int i = 0; i < N; i++) field.value[i].value = history[i];
        field.count = N;
    }
    template <int N, typename Field>
    static void loadHistory(int (&history)[N], const Field &field) {
        for (int i = 0; i < N; i++) history[i] = field.value[i].value;
    }

    // The input/output resampling FIR (psx-spx "Reverb Buffer Resampling").
    // m_h44[k] = the 44.1kHz reverb input k samples ago; m_h22[k] = the 22.05kHz wet output
    // k ticks ago.
    int m_h44L[kFirTaps] = {};
    int m_h44R[kFirTaps] = {};
    int m_h22L[kFirOutTaps] = {};
    int m_h22R[kFirOutTaps] = {};
    bool m_altLeftTick = false;  // which half of the 22.05kHz iteration this 44.1kHz cycle runs
    int m_callCount = 0;         // 44.1kHz cycles run with reverb on; its parity picks m_altLeftTick

    void resetFilterState();
    void firPushIn(int l, int r);

    // The reverb work area is a circular window in sound RAM. Offsets are counted
    // in 32-bit units from CurrAddr, wrap at the top of RAM back to StartAddr, and
    // are the one piece of arithmetic every work-area access shares. ExtraSample is
    // the half-sample stagger the IIR destinations write at; it is a template
    // parameter because it is always 0 since the reverb moved to the psx-spx IIR offsets and it selects the whole
    // access.
    int wrapOffset(int offset, int extraSample) const;
    int getBuffer(int offset, uint16_t *spuMem) const;
    template <int ExtraSample>
    void setBuffer(int offset, int value, uint16_t *spuMem);
};

}  // namespace SPU

}  // namespace PCSX
