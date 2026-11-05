/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include "spu/adpcm.h"
#include "spu/adsr.h"
#include "spu/interpolation.h"
#include "spu/overlay.h"
#include "spu/volume.h"
#include "support/protobuf.h"
#include "support/settings.h"

namespace PCSX {

namespace SPU {

// Main channel struct.

namespace Chan {
// Field numbers 26 (rvb_offset), 27 (rvb_repeat) and 30 (rvb_num) were the
// fake-reverb timing state. That mode is gone; do not reuse the numbers.
// Start flag.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("new"), 1> New;
// Mixing state.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("sb_pos"), 2> SBPos;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("s_pos"), 3> spos;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("s_inc"), 4> sinc;
typedef Protobuf::RepeatedField<Protobuf::Int32, 64, TYPESTRING("sb"), 5> SB;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("sval"), 6> sval;

// Start pointer into sound memory.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("start"), 7> StartPtr;
// Current position in sound memory.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("current"), 8> CurrPtr;
// Loop pointer in sound memory.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("loop"), 9> LoopPtr;

// Is the channel active, that is, is a sample playing?
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("on"), 10> On;
// Is the channel stopped? A sample can still be playing, in the ADSR release phase.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("stop"), 11> Stop;
// Can reverb be applied to this channel? The control register bit must be set for it to become
// active.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("reverb"), 12> Reverb;
// Current PSX pitch.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("act_freq"), 13> ActFreq;
// Current PC pitch.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("used_freq"), 14> UsedFreq;
// Left volume (savestate mirror of VoiceVolume; runtime state lives in SPUCHAN::volume).
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("left_volume"), 15> LeftVolume;
// Left PSX volume value (savestate mirror of VoiceVolume).
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("left_vol_raw"), 16> LeftVolRaw;
// Ignore the loop bit if an external loop address is used.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("ignore_loop"), 17> IgnoreLoop;
// Mute mode.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("mute"), 18> Mute;
// Right volume (savestate mirror of VoiceVolume).
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("right_volume"), 19> RightVolume;
// Right PSX volume value (savestate mirror of VoiceVolume).
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("right_vol_raw"), 20> RightVolRaw;
// Raw pitch (0...3fff).
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("raw_pitch"), 21> RawPitch;
// Debug IRQ-done flag.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("irq_done"), 22> IrqDone;
// Last decoding info.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("s_1"), 23> s_1;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("s_2"), 24> s_2;
// Reverb active flag.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("rvb_active"), 25> RVBActive;
// Reverb offset.
// Reverb repeat.
// Noise active flag.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("noise"), 28> Noise;
// Frequency modulation (0=off, 1=sound channel, 2=freq channel).
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("fmod"), 29> FMod;
// Another reverb helper.
// Solo mode.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("solo"), 31> Solo;
// Skip ID 32.
typedef Protobuf::Message<TYPESTRING("ChannelData"), New, SBPos, spos, sinc, SB, sval, StartPtr, CurrPtr, LoopPtr, On,
                          Stop, Reverb, ActFreq, UsedFreq, LeftVolume, LeftVolRaw, IgnoreLoop, Mute, RightVolume,
                          RightVolRaw, RawPitch, IrqDone, s_1, s_2, RVBActive, Noise, FMod, Solo>
    Data;
}  // namespace Chan

struct SPUCHAN {
    Chan::Data data;
    AdpcmDecoder adpcm;   // per-voice ADPCM decoder: cursor + IIR history + block decode
    AdsrEnvelope adsr;    // per-voice ADSR envelope: state + four-phase machine
    Interpolator interp;  // per-voice resampler: none/simple/gauss/cubic interpolation
    VoiceVolume volume;   // per-voice left/right output volume: raw register + decoded level
    OverlayVoice overlay;  // per-voice replacement playhead, not part of the savestate
};

struct REVERBInfo {
    int StartAddr;  // reverb area start address in samples
    int CurrAddr;   // reverb area current address in samples

    int VolLeft;
    int VolRight;
    int lastWetLeft;
    int lastWetRight;
    int wetLeft;
    int wetRight;

    int FB_SRC_A;     // (offset)
    int FB_SRC_B;     // (offset)
    int IIR_ALPHA;    // (coef.)
    int ACC_COEF_A;   // (coef.)
    int ACC_COEF_B;   // (coef.)
    int ACC_COEF_C;   // (coef.)
    int ACC_COEF_D;   // (coef.)
    int IIR_COEF;     // (coef.)
    int FB_ALPHA;     // (coef.)
    int FB_X;         // (coef.)
    int IIR_DEST_A0;  // (offset)
    int IIR_DEST_A1;  // (offset)
    int ACC_SRC_A0;   // (offset)
    int ACC_SRC_A1;   // (offset)
    int ACC_SRC_B0;   // (offset)
    int ACC_SRC_B1;   // (offset)
    int IIR_SRC_A0;   // (offset)
    int IIR_SRC_A1;   // (offset)
    int IIR_DEST_B0;  // (offset)
    int IIR_DEST_B1;  // (offset)
    int ACC_SRC_C0;   // (offset)
    int ACC_SRC_C1;   // (offset)
    int ACC_SRC_D0;   // (offset)
    int ACC_SRC_D1;   // (offset)
    int IIR_SRC_B1;   // (offset)
    int IIR_SRC_B0;   // (offset)
    int MIX_DEST_A0;  // (offset)
    int MIX_DEST_A1;  // (offset)
    int MIX_DEST_B0;  // (offset)
    int MIX_DEST_B1;  // (offset)
    int IN_COEF_L;    // (coef.)
    int IN_COEF_R;    // (coef.)
};

}  // namespace SPU

}  // namespace PCSX
