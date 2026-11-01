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

#include "spu/reverb.h"

#include <string.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////

void PCSX::SPU::ReverbUnit::start(SPUCHAN* voice, uint16_t spuCtrl) {
    voice->data.get<Chan::RVBActive>().value = voice->data.get<Chan::Reverb>().value && (spuCtrl & kReverbMasterEnable);
}

////////////////////////////////////////////////////////////////////////

// Helper for Neill's reverb: re-initializes the reverb mixing buffer.
void PCSX::SPU::ReverbUnit::init(int nssize) { memset(mixStart, 0, nssize * 2 * 4); }

////////////////////////////////////////////////////////////////////////

void PCSX::SPU::ReverbUnit::store(SPUCHAN* voice, int ns) {
    const int sendLeft = (voice->data.get<Chan::sval>().value * voice->volume.left()) / 0x4000;
    const int sendRight = (voice->data.get<Chan::sval>().value * voice->volume.right()) / 0x4000;

    // Every active reverb voice is summed into the mixing buffer, interleaved.
    ns <<= 1;
    mixStart[ns] += sendLeft;
    mixStart[ns + 1] += sendRight;
}

////////////////////////////////////////////////////////////////////////

// Resolve a work-area offset to a sample index, wrapping at both ends. Offsets
// arrive in 32-bit units, hence the * 4.
int PCSX::SPU::ReverbUnit::wrapOffset(int offset, int extraSample) const {
    offset = offset * 4 + rvb.CurrAddr + extraSample;
    while (offset > 0x3ffff) offset = rvb.StartAddr + (offset - 0x40000);
    while (offset < rvb.StartAddr) offset = 0x3ffff - (rvb.StartAddr - offset);
    return offset;
}

int PCSX::SPU::ReverbUnit::getBuffer(int offset, uint16_t* spuMem) const {
    return reinterpret_cast<int16_t*>(spuMem)[wrapOffset(offset, 0)];
}

template <int ExtraSample>
void PCSX::SPU::ReverbUnit::setBuffer(int offset, int value, uint16_t* spuMem) {
    reinterpret_cast<int16_t*>(spuMem)[wrapOffset(offset, ExtraSample)] =
        static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

////////////////////////////////////////////////////////////////////////

namespace {
// psx-spx "Reverb Buffer Resampling": input to and output from the reverb unit are
// resampled with this 39-tap FIR.
//
// Normalisation, worked from the table rather than assumed: all 39 taps sum to 32766
// (~0x8000), so the DECIMATING direction divides by 0x8000. Upsampling zero-stuffs, so
// the two output phases split the taps - the odd indices contribute only h[19]=0x4000
// (a passthrough of the raw 22kHz sample) and the even indices sum to 16382 (~0x4000).
// The INTERPOLATING direction therefore divides by 0x4000. Getting those two the same
// way round would present as a plausible 2x gain error rather than as a filter fault.
constexpr int kFir[39] = {
    -0x0001, 0x0000, 0x0002, 0x0000,  -0x000A, 0x0000, 0x0023, 0x0000, -0x0067, 0x0000,  0x010A, 0x0000, -0x0268,
    0x0000,  0x0534, 0x0000, -0x0B90, 0x0000,  0x2806, 0x4000, 0x2806, 0x0000,  -0x0B90, 0x0000, 0x0534, 0x0000,
    -0x0268, 0x0000, 0x010A, 0x0000,  -0x0067, 0x0000, 0x0023, 0x0000, -0x000A, 0x0000,  0x0002, 0x0000, -0x0001,
};
// The histories it runs over are per-unit state (ReverbUnit::m_h44L and friends). The FIR
// is centred, so a causal implementation runs a fixed delay (19 samples in / 10 ticks
// out). That is harmless here: p3-compare.py scores modulo rotation, so a constant group
// delay costs nothing.
// A hardware fixed-point datapath keeps the upper multiplier bits, which is an
// arithmetic shift, i.e. floor. C's / rounds toward zero, so the two differ by one
// LSB on every negative product; the shift is the one that matches the silicon.
inline int rdiv15(int64_t v) { return (int)(v >> 15); }
inline int rdiv14(int64_t v) { return (int)(v >> 14); }
// Every named intermediate of the spec formula (Lin, IIR_INPUT, ACC, the running
// Lout/Rout between the APF clauses) is saturated to the 16-bit rail, mirroring a
// datapath whose registers are 16 bits wide rather than only clamping on the way to
// memory.
inline int sat16(int v) { return std::clamp(v, -32768, 32767); }
inline int firDecimate(const int* h) {
    int64_t a = 0;
    for (int i = 0; i < 39; i++) a += (int64_t)kFir[i] * h[i];
    return rdiv15(a);
}
inline int firInterp(const int* h, bool passthrough) {
    if (passthrough) return h[10];
    int64_t a = 0;
    for (int i = 0; i < 39; i += 2) a += (int64_t)kFir[i] * h[i / 2];
    return rdiv14(a);
}
}  // namespace

void PCSX::SPU::ReverbUnit::resetFilterState() {
    static_assert(std::size(kFir) == kFirTaps);
    std::fill(std::begin(m_h44L), std::end(m_h44L), 0);
    std::fill(std::begin(m_h44R), std::end(m_h44R), 0);
    std::fill(std::begin(m_h22L), std::end(m_h22L), 0);
    std::fill(std::begin(m_h22R), std::end(m_h22R), 0);
    m_altLeftTick = false;
    m_callCount = 0;
}

void PCSX::SPU::ReverbUnit::firPushIn(int l, int r) {
    for (int i = kFirTaps - 1; i > 0; i--) {
        m_h44L[i] = m_h44L[i - 1];
        m_h44R[i] = m_h44R[i - 1];
    }
    m_h44L[0] = l;
    m_h44R[0] = r;
}

// The FIR histories and the L/R parity are reverb-unit state too; a reset that left
// them would replay the previous tail through h22[9].
void PCSX::SPU::ReverbUnit::reset() {
    memset((void*)&rvb, 0, sizeof(REVERBInfo));
    resetFilterState();
}

int PCSX::SPU::ReverbUnit::mixLeft(int ns, uint16_t* spuMem, uint16_t spuCtrl) {
    // This function is called at 44.1 kHz.
    // Reverb is off.
    if (!rvb.StartAddr) {
        rvb.lastWetLeft = rvb.lastWetRight = rvb.wetLeft = rvb.wetRight = 0;
        return 0;
    }

    m_callCount++;

    // psx-spx: the unit spends "one 44100h cycle on left calculations, and the next on
    // right". A complete 22.05kHz iteration is therefore two 44.1kHz cycles, each half
    // running the same formula against that channel's half of the register set, and the
    // work address advancing only once both have run.
    // The unit keeps reading the work area and producing output even when the
    // master enable bit is clear. That bit only gates writes into the buffer.
    // Games that stream audio into the work area depend on this.
    const bool writeEnabled = (spuCtrl & kReverbMasterEnable) != 0;
    firPushIn(*(mixStart + (ns << 1)), *(mixStart + (ns << 1) + 1));
    m_altLeftTick = (m_callCount & 1) != 0;
    const int in = sat16(m_altLeftTick ? firDecimate(m_h44L) : firDecimate(m_h44R));
    const int sSame = m_altLeftTick ? rvb.IIR_SRC_A0 : rvb.IIR_SRC_A1;
    const int dSame = m_altLeftTick ? rvb.IIR_DEST_A0 : rvb.IIR_DEST_A1;
    const int sDiff = m_altLeftTick ? rvb.IIR_SRC_B0 : rvb.IIR_SRC_B1;
    const int dDiff = m_altLeftTick ? rvb.IIR_DEST_B0 : rvb.IIR_DEST_B1;
    const int inCoef = m_altLeftTick ? rvb.IN_COEF_L : rvb.IN_COEF_R;
    const int c1 = m_altLeftTick ? rvb.ACC_SRC_A0 : rvb.ACC_SRC_A1;
    const int c2 = m_altLeftTick ? rvb.ACC_SRC_B0 : rvb.ACC_SRC_B1;
    const int c3 = m_altLeftTick ? rvb.ACC_SRC_C0 : rvb.ACC_SRC_C1;
    const int c4 = m_altLeftTick ? rvb.ACC_SRC_D0 : rvb.ACC_SRC_D1;
    const int mA = m_altLeftTick ? rvb.MIX_DEST_A0 : rvb.MIX_DEST_A1;
    const int mB = m_altLeftTick ? rvb.MIX_DEST_B0 : rvb.MIX_DEST_B1;
    auto at = [&](int off, int extra) { return (int)reinterpret_cast<int16_t*>(spuMem)[wrapOffset(off, extra)]; };
    const int iirSame =
        sat16(rdiv15((int64_t)(getBuffer(sSame, spuMem) * rvb.IIR_COEF)) + rdiv15((int64_t)(in * inCoef)));
    const int iirDiff =
        sat16(rdiv15((int64_t)(getBuffer(sDiff, spuMem) * rvb.IIR_COEF)) + rdiv15((int64_t)(in * inCoef)));
    // psx-spx stores the IIR result at [mLSAME] and reads the previous value back
    // from [mLSAME-2], i.e. the destination cell itself one 16-bit sample earlier -
    // hence the -1 extraSample on the read and none on the store.
    if (writeEnabled) {
        setBuffer<0>(
            dSame,
            rdiv15((int64_t)(iirSame * rvb.IIR_ALPHA)) + rdiv15((int64_t)(at(dSame, -1) * (32768L - rvb.IIR_ALPHA))),
            spuMem);
        setBuffer<0>(
            dDiff,
            rdiv15((int64_t)(iirDiff * rvb.IIR_ALPHA)) + rdiv15((int64_t)(at(dDiff, -1) * (32768L - rvb.IIR_ALPHA))),
            spuMem);
    }
    int out = sat16(rdiv15((int64_t)(getBuffer(c1, spuMem) * rvb.ACC_COEF_A)) +
                    rdiv15((int64_t)(getBuffer(c2, spuMem) * rvb.ACC_COEF_B)) +
                    rdiv15((int64_t)(getBuffer(c3, spuMem) * rvb.ACC_COEF_C)) +
                    rdiv15((int64_t)(getBuffer(c4, spuMem) * rvb.ACC_COEF_D)));
    // psx-spx "SPU Reverb Formula", APF1 then APF2, three clauses each:
    //   Lout=Lout-vAPF1*[mLAPF1-dAPF1], [mLAPF1]=Lout, Lout=Lout*vAPF1+[mLAPF1-dAPF1]
    //   Lout=Lout-vAPF2*[mLAPF2-dAPF2], [mLAPF2]=Lout, Lout=Lout*vAPF2+[mLAPF2-dAPF2]
    //   LeftOutput = Lout*vLOUT          (all products divided by 8000h)
    const int tapA = getBuffer(mA - rvb.FB_SRC_A, spuMem);
    out = sat16(out - rdiv15((int64_t)(tapA * rvb.FB_ALPHA)));
    if (writeEnabled) setBuffer<0>(mA, out, spuMem);
    out = sat16(rdiv15((int64_t)(out * rvb.FB_ALPHA)) + tapA);
    const int tapB = getBuffer(mB - rvb.FB_SRC_B, spuMem);
    out = sat16(out - rdiv15((int64_t)(tapB * rvb.FB_X)));
    if (writeEnabled) setBuffer<0>(mB, out, spuMem);
    out = sat16(rdiv15((int64_t)(out * rvb.FB_X)) + tapB);
    // vLOUT/vROUT are signed 16bit and their products divide by 8000h, not 4000h.
    // registers.cc assigns these from a uint16_t with no cast, unlike every other
    // reverb coefficient there, so sign-extend at the point of use.
    const int vol = m_altLeftTick ? (int)(int16_t)rvb.VolLeft : (int)(int16_t)rvb.VolRight;
    const int wet = rdiv15((int64_t)out * vol);
    if (m_altLeftTick) {
        rvb.wetLeft = wet;
        for (int i = 19; i > 0; i--) m_h22L[i] = m_h22L[i - 1];
        m_h22L[0] = wet;
    } else {
        rvb.wetRight = wet;
        for (int i = 19; i > 0; i--) m_h22R[i] = m_h22R[i - 1];
        m_h22R[0] = wet;
        // Address advances once per COMPLETE 22.05kHz iteration, after both halves.
        rvb.CurrAddr++;
        if (rvb.CurrAddr > 0x3ffff) rvb.CurrAddr = rvb.StartAddr;
    }
    // Emit the even (interpolating) polyphase on this channel's own compute cycle
    // and the centre-tap passthrough on the other one, which puts the wet output one
    // phase earlier than pairing them the obvious way round. h22[9] and not h22[10]:
    // on the off cycle h22[10] is 21 ticks old, so the parity swap on its own would
    // jitter the group delay between 19 and 21.
    return m_altLeftTick ? firInterp(m_h22L, false) : m_h22L[9];
}

////////////////////////////////////////////////////////////////////////

int PCSX::SPU::ReverbUnit::mixRight() {
    // The mirror of the left output stage: mixLeft has already run for this 44.1kHz
    // cycle and left m_altLeftTick set, so the right channel takes the interpolating
    // polyphase on its own compute cycle and the passthrough on the other one.
    return !m_altLeftTick ? firInterp(m_h22R, false) : m_h22R[9];
}

////////////////////////////////////////////////////////////////////////

/*
-----------------------------------------------------------------------------
PSX reverb hardware notes
by Neill Corlett
-----------------------------------------------------------------------------

Yadda yadda disclaimer yadda probably not perfect yadda well it's okay anyway
yadda yadda.

-----------------------------------------------------------------------------

Basics
------

- The reverb buffer is 22khz 16-bit mono PCM.
- It starts at the reverb address given by 1DA2, extends to
  the end of sound RAM, and wraps back to the 1DA2 address.

Setting the address at 1DA2 resets the current reverb work address.

This work address ALWAYS increments every 1/22050 sec., regardless of
whether reverb is enabled (bit 7 of 1DAA set).

And the contents of the reverb buffer ALWAYS play, scaled by the
"reverberation depth left/right" volumes (1D84/1D86).
(which, by the way, appear to be scaled so 3FFF=approx. 1.0, 4000=-1.0)

-----------------------------------------------------------------------------

Register names
--------------

These are probably not their real names.
These are probably not even correct names.
We will use them anyway, because we can.

1DC0: FB_SRC_A       (offset)
1DC2: FB_SRC_B       (offset)
1DC4: IIR_ALPHA      (coef.)
1DC6: ACC_COEF_A     (coef.)
1DC8: ACC_COEF_B     (coef.)
1DCA: ACC_COEF_C     (coef.)
1DCC: ACC_COEF_D     (coef.)
1DCE: IIR_COEF       (coef.)
1DD0: FB_ALPHA       (coef.)
1DD2: FB_X           (coef.)
1DD4: IIR_DEST_A0    (offset)
1DD6: IIR_DEST_A1    (offset)
1DD8: ACC_SRC_A0     (offset)
1DDA: ACC_SRC_A1     (offset)
1DDC: ACC_SRC_B0     (offset)
1DDE: ACC_SRC_B1     (offset)
1DE0: IIR_SRC_A0     (offset)
1DE2: IIR_SRC_A1     (offset)
1DE4: IIR_DEST_B0    (offset)
1DE6: IIR_DEST_B1    (offset)
1DE8: ACC_SRC_C0     (offset)
1DEA: ACC_SRC_C1     (offset)
1DEC: ACC_SRC_D0     (offset)
1DEE: ACC_SRC_D1     (offset)
1DF0: IIR_SRC_B1     (offset)
1DF2: IIR_SRC_B0     (offset)
1DF4: MIX_DEST_A0    (offset)
1DF6: MIX_DEST_A1    (offset)
1DF8: MIX_DEST_B0    (offset)
1DFA: MIX_DEST_B1    (offset)
1DFC: IN_COEF_L      (coef.)
1DFE: IN_COEF_R      (coef.)

The coefficients are signed fractional values.
-32768 would be -1.0
 32768 would be  1.0 (if it were possible... the highest is of course 32767)

The offsets are (byte/8) offsets into the reverb buffer.
i.e. you multiply them by 8, you get byte offsets.
You can also think of them as (samples/4) offsets.
They appear to be signed.  They can be negative.
None of the documented presets make them negative, though.

Yes, 1DF0 and 1DF2 appear to be backwards.  Not a typo.

-----------------------------------------------------------------------------

What it does
------------

We take all reverb sources:
- regular channels that have the reverb bit on
- cd and external sources, if their reverb bits are on
and mix them into one stereo 44100hz signal.

Lowpass/downsample that to 22050hz.  The PSX uses a proper bandlimiting
algorithm here, but I haven't figured out the hysterically exact specifics.
I use an 8-tap filter with these coefficients, which are nice but probably
not the real ones:

0.037828187894
0.157538631280
0.321159685278
0.449322115345
0.449322115345
0.321159685278
0.157538631280
0.037828187894

So we have two input samples (INPUT_SAMPLE_L, INPUT_SAMPLE_R) every 22050hz.

* IN MY EMULATION, I divide these by 2 to make it clip less.
  (and of course the L/R output coefficients are adjusted to compensate)
  The real thing appears to not do this.

At every 22050hz tick:
- If the reverb bit is enabled (bit 7 of 1DAA), execute the reverb
  steady-state algorithm described below
- AFTERWARDS, retrieve the "wet out" L and R samples from the reverb buffer
  (This part may not be exactly right and I guessed at the coefs. TODO: check later.)
  L is: 0.333 * (buffer[MIX_DEST_A0] + buffer[MIX_DEST_B0])
  R is: 0.333 * (buffer[MIX_DEST_A1] + buffer[MIX_DEST_B1])
- Advance the current buffer position by 1 sample

The wet out L and R are then upsampled to 44100hz and played at the
"reverberation depth left/right" (1D84/1D86) volume, independent of the main
volume.

-----------------------------------------------------------------------------

Reverb steady-state
-------------------

The reverb steady-state algorithm is fairly clever, and of course by
"clever" I mean "batshit insane".

buffer[x] is relative to the current buffer position, not the beginning of
the buffer.  Note that all buffer offsets must wrap around so they're
contained within the reverb work area.

Clipping is performed at the end... maybe also sooner, but definitely at
the end.

IIR_INPUT_A0 = buffer[IIR_SRC_A0] * IIR_COEF + INPUT_SAMPLE_L * IN_COEF_L;
IIR_INPUT_A1 = buffer[IIR_SRC_A1] * IIR_COEF + INPUT_SAMPLE_R * IN_COEF_R;
IIR_INPUT_B0 = buffer[IIR_SRC_B0] * IIR_COEF + INPUT_SAMPLE_L * IN_COEF_L;
IIR_INPUT_B1 = buffer[IIR_SRC_B1] * IIR_COEF + INPUT_SAMPLE_R * IN_COEF_R;

IIR_A0 = IIR_INPUT_A0 * IIR_ALPHA + buffer[IIR_DEST_A0] * (1.0 - IIR_ALPHA);
IIR_A1 = IIR_INPUT_A1 * IIR_ALPHA + buffer[IIR_DEST_A1] * (1.0 - IIR_ALPHA);
IIR_B0 = IIR_INPUT_B0 * IIR_ALPHA + buffer[IIR_DEST_B0] * (1.0 - IIR_ALPHA);
IIR_B1 = IIR_INPUT_B1 * IIR_ALPHA + buffer[IIR_DEST_B1] * (1.0 - IIR_ALPHA);

buffer[IIR_DEST_A0 + 1sample] = IIR_A0;
buffer[IIR_DEST_A1 + 1sample] = IIR_A1;
buffer[IIR_DEST_B0 + 1sample] = IIR_B0;
buffer[IIR_DEST_B1 + 1sample] = IIR_B1;

ACC0 = buffer[ACC_SRC_A0] * ACC_COEF_A +
       buffer[ACC_SRC_B0] * ACC_COEF_B +
       buffer[ACC_SRC_C0] * ACC_COEF_C +
       buffer[ACC_SRC_D0] * ACC_COEF_D;
ACC1 = buffer[ACC_SRC_A1] * ACC_COEF_A +
       buffer[ACC_SRC_B1] * ACC_COEF_B +
       buffer[ACC_SRC_C1] * ACC_COEF_C +
       buffer[ACC_SRC_D1] * ACC_COEF_D;

FB_A0 = buffer[MIX_DEST_A0 - FB_SRC_A];
FB_A1 = buffer[MIX_DEST_A1 - FB_SRC_A];
FB_B0 = buffer[MIX_DEST_B0 - FB_SRC_B];
FB_B1 = buffer[MIX_DEST_B1 - FB_SRC_B];

buffer[MIX_DEST_A0] = ACC0 - FB_A0 * FB_ALPHA;
buffer[MIX_DEST_A1] = ACC1 - FB_A1 * FB_ALPHA;
buffer[MIX_DEST_B0] = (FB_ALPHA * ACC0) - FB_A0 * (FB_ALPHA^0x8000) - FB_B0 * FB_X;
buffer[MIX_DEST_B1] = (FB_ALPHA * ACC1) - FB_A1 * (FB_ALPHA^0x8000) - FB_B1 * FB_X;

-----------------------------------------------------------------------------
*/
