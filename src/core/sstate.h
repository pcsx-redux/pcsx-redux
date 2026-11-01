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

#include <string_view>

#include "spu/types.h"
#include "support/protobuf.h"
#include "support/settings.h"

namespace PCSX {

namespace SaveStates {

typedef Protobuf::Field<Protobuf::String, TYPESTRING("version_string"), 1> VersionString;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("version"), 2> Version;
typedef Protobuf::Message<TYPESTRING("SaveStateInfo"), VersionString, Version> SaveStateInfo;
typedef Protobuf::MessageField<SaveStateInfo, TYPESTRING("save_state_info"), 1> SaveStateInfoField;

typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("width"), 1> Width;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("height"), 2> Height;
typedef Protobuf::Field<Protobuf::Bytes, TYPESTRING("red"), 3> Red;
typedef Protobuf::Field<Protobuf::Bytes, TYPESTRING("green"), 4> Green;
typedef Protobuf::Field<Protobuf::Bytes, TYPESTRING("blue"), 5> Blue;
typedef Protobuf::Message<TYPESTRING("Thumbnail"), Width, Height, Red, Green, Blue> Thumbnail;
typedef Protobuf::MessageField<Thumbnail, TYPESTRING("thumbnail"), 2> ThumbnailField;

typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x00800000>, TYPESTRING("ram"), 1> RAM;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x00080000>, TYPESTRING("rom"), 2> ROM;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x00800000>, TYPESTRING("exp1"), 3> EXP1;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x00010000>, TYPESTRING("hardware"), 4> HardwareMemory;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x00200000>, TYPESTRING("sram"), 5> SRAM;
// The BIU cache control register at 0xfffe0130, which decides whether stores reach RAM. States from
// before it was saved don't have has_biu, and leave it as it is.
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("biu"), 6> BIU;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("has_biu"), 7> HasBIU;
typedef Protobuf::Message<TYPESTRING("Memory"), RAM, ROM, EXP1, HardwareMemory, SRAM, BIU, HasBIU> Memory;
typedef Protobuf::MessageField<Memory, TYPESTRING("memory"), 3> MemoryField;

typedef Protobuf::RepeatedFieldRef<Protobuf::UInt32, 34, TYPESTRING("gpr"), 1> GPR;
typedef Protobuf::RepeatedFieldRef<Protobuf::UInt32, 32, TYPESTRING("cp0"), 2> CP0;
typedef Protobuf::RepeatedFieldRef<Protobuf::UInt32, 32, TYPESTRING("cp2d"), 3> CP2D;
typedef Protobuf::RepeatedFieldRef<Protobuf::UInt32, 32, TYPESTRING("cp2c"), 4> CP2C;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("pc"), 5> PC;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("code"), 6> Code;
typedef Protobuf::FieldRef<Protobuf::UInt64, TYPESTRING("cycle"), 7> Cycle;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("schedule_mask"), 8> ScheduleMask;
// skip id 9
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x1000>, TYPESTRING("icache_addr"), 10> ICacheAddr;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x1000>, TYPESTRING("icache_code"), 11> ICacheCode;
// skip id 12
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("next_is_delay_slot"), 13> NextIsDelaySlot;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("index"), 1> DelaySlotIndex;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mask"), 6> DelaySlotMask;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("value"), 2> DelaySlotValue;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("pc_value"), 3> DelaySlotPcValue;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("active"), 4> DelaySlotActive;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("pc_active"), 5> DelaySlotPcActive;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("from_link"), 7> DelaySlotFromLink;
typedef Protobuf::Message<TYPESTRING("DelaySlotInfo"), DelaySlotIndex, DelaySlotValue, DelaySlotMask, DelaySlotPcValue,
                          DelaySlotActive, DelaySlotPcActive, DelaySlotFromLink>
    DelaySlotInfo;
typedef Protobuf::MessageField<DelaySlotInfo, TYPESTRING("delay_slot_info_1"), 14> DelaySlotInfo1;
typedef Protobuf::MessageField<DelaySlotInfo, TYPESTRING("delay_slot_info_2"), 15> DelaySlotInfo2;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("current_delayed_load"), 16> CurrentDelayedLoad;
typedef Protobuf::RepeatedFieldRef<Protobuf::UInt64, 32, TYPESTRING("schedule_targets"), 17> ScheduleTargetsField;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("in_isr"), 18> InISR;
typedef Protobuf::FieldRef<Protobuf::UInt64, TYPESTRING("muldiv_ready"), 19> MulDivReady;
typedef Protobuf::FieldRef<Protobuf::UInt64, TYPESTRING("gte_ready"), 20> GteReady;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("reg"), 1> GtePendingReg;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("value"), 2> GtePendingValue;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("ctrl"), 3> GtePendingCtrl;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("countdown"), 4> GtePendingCountdown;
typedef Protobuf::Message<TYPESTRING("GtePendingWrite"), GtePendingReg, GtePendingValue, GtePendingCtrl,
                          GtePendingCountdown>
    GtePendingWriteInfo;
typedef Protobuf::MessageField<GtePendingWriteInfo, TYPESTRING("gte_pending_1"), 21> GtePending1;
typedef Protobuf::MessageField<GtePendingWriteInfo, TYPESTRING("gte_pending_2"), 22> GtePending2;
typedef Protobuf::MessageField<GtePendingWriteInfo, TYPESTRING("gte_pending_3"), 23> GtePending3;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("gte_pending_count"), 24> GtePendingCount;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("next_is_branch_slot"), 25> NextIsBranchSlot;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("next_slot_branch_pc"), 26> NextSlotBranchPC;
typedef Protobuf::Message<TYPESTRING("Registers"), GPR, CP0, CP2D, CP2C, PC, Code, Cycle, ScheduleMask, ICacheAddr,
                          ICacheCode, NextIsDelaySlot, DelaySlotInfo1, DelaySlotInfo2, CurrentDelayedLoad,
                          ScheduleTargetsField, InISR, MulDivReady, GteReady, GtePending1, GtePending2, GtePending3,
                          GtePendingCount, NextIsBranchSlot, NextSlotBranchPC>
    Registers;
typedef Protobuf::MessageField<Registers, TYPESTRING("registers"), 4> RegistersField;

typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("status"), 1> GPUStatus;
typedef Protobuf::Field<Protobuf::FixedBytes<0x400>, TYPESTRING("control"), 2> GPUControl;
typedef Protobuf::Field<Protobuf::FixedBytes<0x00200000>, TYPESTRING("vram"), 3> GPUVRam;
typedef Protobuf::Field<Protobuf::FixedBytes<24>, TYPESTRING("environment"), 4> GPUEnvironment;
typedef Protobuf::Message<TYPESTRING("GPU"), GPUStatus, GPUControl, GPUVRam, GPUEnvironment> GPU;
typedef Protobuf::MessageField<GPU, TYPESTRING("gpu"), 5> GPUField;

typedef Protobuf::Field<Protobuf::FixedBytes<0x80000>, TYPESTRING("ram"), 1> SPURam;
typedef Protobuf::Field<Protobuf::FixedBytes<0x200>, TYPESTRING("ports"), 2> SPUPorts;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("frequency"), 1> XAFrequency;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("nbits"), 2> XANBits;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("stereo"), 3> XAStereo;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("nsamples"), 4> XANSamples;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("y0"), 1> ADPCMDecodeY0;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("y1"), 2> ADPCMDecodeY1;
typedef Protobuf::Message<TYPESTRING("ADPCMDecode"), ADPCMDecodeY0, ADPCMDecodeY1> ADPCMDecode;
typedef Protobuf::MessageField<ADPCMDecode, TYPESTRING("left"), 5> XAADPCMLeft;
typedef Protobuf::MessageField<ADPCMDecode, TYPESTRING("right"), 6> XAADPCMRight;
typedef Protobuf::Field<Protobuf::FixedBytes<32768>, TYPESTRING("pcm"), 7> XAPCM;
typedef Protobuf::Message<TYPESTRING("XA"), XAFrequency, XANBits, XAStereo, XANSamples, XAADPCMLeft, XAADPCMRight,
                          XAPCM>
    XA;
typedef Protobuf::MessageField<XA, TYPESTRING("xa"), 3> XAField;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("irq"), 4> SPUIrq;

typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("irqptr"), 5> SPUIrqPtr;
typedef Protobuf::MessageField<::PCSX::SPU::Chan::Data, TYPESTRING("data"), 1> Data;
typedef Protobuf::MessageField<::PCSX::SPU::ADSRInfo, TYPESTRING("adsr"), 2> ADSRInfo;
typedef Protobuf::MessageField<::PCSX::SPU::ADSRInfoEx, TYPESTRING("adsr_ex"), 3> ADSRInfoEx;
typedef Protobuf::Message<TYPESTRING("Channel"), Data, ADSRInfo, ADSRInfoEx> Channel;
typedef Protobuf::RepeatedField<Channel, 24, TYPESTRING("channel"), 6> Channels;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("addr"), 7> SPUAddr;
typedef Protobuf::Field<Protobuf::UInt16, TYPESTRING("ctrl"), 8> SPUCtrl;
typedef Protobuf::Field<Protobuf::UInt16, TYPESTRING("stat"), 9> SPUStat;
// Capture buffer
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("cbStartInd"), 10> CBStartIndex;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("cbCurrInd"), 11> CBCurrIndex;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("cbEndInd"), 12> CBEndIndex;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("cbVoiceInd"), 13> CBVoiceIndex;

typedef Protobuf::Field<Protobuf::FixedBytes<1024 * 16 * 2>, TYPESTRING("CBLeft"), 14> CBCDLeft;
typedef Protobuf::Field<Protobuf::FixedBytes<1024 * 16 * 2>, TYPESTRING("CBRight"), 15> CBCDRight;

// global noise generator
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("noiseClock"), 16> SPUNoiseClock;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("noiseCount"), 17> SPUNoiseCount;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("noiseVal"), 18> SPUNoiseVal;

// ENDX latch, one bit per voice
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("endx"), 19> SPUEndx;

// main volume sides written by software: bit 0 left, bit 1 right; bit 2 marks the field present
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("mainVolWritten"), 20> SPUMainVolWritten;

// The cpu-side ENVX walk (SPU::impl::EnvelopeCheckpoint), one per voice. ENVX reads are answered
// from it, so a state restored without it answers them from whatever the walk held before.
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("keyOnCycle"), 1> ECPKeyOnCycle;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("serviceSample"), 2> ECPServiceSample;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("keyOffCycle"), 3> ECPKeyOffCycle;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("keyedOn"), 4> ECPKeyedOn;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("cachedSample"), 5> ECPCachedSample;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("cachedState"), 6> ECPCachedState;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("cachedVol"), 7> ECPCachedVol;
// Unused since envelope steps follow the SPU-wide sample count; kept so older savestates load.
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("cachedFraction"), 8> ECPCachedFraction;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("cachedOn"), 9> ECPCachedOn;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("block"), 10> ECPBlock;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("loop"), 11> ECPLoop;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("ignoreLoop"), 12> ECPIgnoreLoop;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("left"), 13> ECPLeft;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("pos"), 14> ECPPos;
typedef Protobuf::Field<Protobuf::Int32, TYPESTRING("pitchStep"), 15> ECPPitchStep;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("ended"), 16> ECPEnded;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("untracked"), 17> ECPUntracked;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("keyOnBlock"), 18> ECPKeyOnBlock;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("keyOnLoop"), 19> ECPKeyOnLoop;
typedef Protobuf::Message<TYPESTRING("EnvelopeCheckpoint"), ECPKeyOnCycle, ECPServiceSample, ECPKeyOffCycle, ECPKeyedOn,
                          ECPCachedSample, ECPCachedState, ECPCachedVol, ECPCachedFraction, ECPCachedOn, ECPBlock,
                          ECPLoop, ECPIgnoreLoop, ECPLeft, ECPPos, ECPPitchStep, ECPEnded, ECPUntracked, ECPKeyOnBlock,
                          ECPKeyOnLoop>
    EnvelopeCheckpoint;
typedef Protobuf::RepeatedField<EnvelopeCheckpoint, 24, TYPESTRING("envelopeCheckpoint"), 21> EnvelopeCheckpoints;

typedef Protobuf::Message<TYPESTRING("SPU"), SPURam, SPUPorts, XAField, SPUIrq, SPUIrqPtr, Channels, SPUAddr, SPUCtrl,
                          SPUStat, CBStartIndex, CBCurrIndex, CBEndIndex, CBVoiceIndex, CBCDLeft, CBCDRight,
                          SPUNoiseClock, SPUNoiseCount, SPUNoiseVal, SPUEndx, SPUMainVolWritten, EnvelopeCheckpoints>
    SPU;
typedef Protobuf::MessageField<SPU, TYPESTRING("spu"), 6> SPUField;

typedef Protobuf::FieldPtr<Protobuf::FixedBytes<0x1010>, TYPESTRING("buffer"), 1> SIOBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("status_reg"), 2> SIOStatusReg;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mode_reg"), 3> SIOModeReg;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("ctrl_reg"), 4> SIOCtrlReg;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("baud_reg"), 5> SIOBaudReg;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("buffer_max_index"), 6> SIOBufferMaxIndex;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("buffer_index"), 7> SIOBufferIndex;
// skip id 8
// skip id 9
// skip id 10
// skip id 11
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("pad_state"), 12> SIOPadState;
// skip id 13
// skip id 14
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("currentdevice"), 15> SIOCurrentDevice;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("pad_address"), 32> SIOPadAddress;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd1_tempbuffer"), 16> SIOMCD1TempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1_directoryflag"), 17> SIOMCD1DirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1_checksumin"), 18> SIOMCD1ChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1_checksumout"), 19> SIOMCD1ChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1_commandticks"), 20> SIOMCD1CommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1_currentcommand"), 21> SIOMCD1CurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1_sector"), 22> SIOMCD1Sector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd1_dataoffset"), 23> SIOMCD1DataOffset;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd2_tempbuffer"), 24> SIOMCD2TempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2_directoryflag"), 25> SIOMCD2DirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2_checksumin"), 26> SIOMCD2ChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2_checksumout"), 27> SIOMCD2ChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2_commandticks"), 28> SIOMCD2CommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2_currentcommand"), 29> SIOMCD2CurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2_sector"), 30> SIOMCD2Sector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd2_dataoffset"), 31> SIOMCD2DataOffset;
// The memory cards in a multitap's slots B to D, SIO::m_tapMemoryCard[0] to [5].
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd1b_tempbuffer"), 33> SIOMCD1BTempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1b_directoryflag"), 34> SIOMCD1BDirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1b_checksumin"), 35> SIOMCD1BChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1b_checksumout"), 36> SIOMCD1BChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1b_commandticks"), 37> SIOMCD1BCommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1b_currentcommand"), 38> SIOMCD1BCurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1b_sector"), 39> SIOMCD1BSector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd1b_dataoffset"), 40> SIOMCD1BDataOffset;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd1c_tempbuffer"), 41> SIOMCD1CTempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1c_directoryflag"), 42> SIOMCD1CDirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1c_checksumin"), 43> SIOMCD1CChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1c_checksumout"), 44> SIOMCD1CChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1c_commandticks"), 45> SIOMCD1CCommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1c_currentcommand"), 46> SIOMCD1CCurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1c_sector"), 47> SIOMCD1CSector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd1c_dataoffset"), 48> SIOMCD1CDataOffset;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd1d_tempbuffer"), 49> SIOMCD1DTempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1d_directoryflag"), 50> SIOMCD1DDirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1d_checksumin"), 51> SIOMCD1DChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1d_checksumout"), 52> SIOMCD1DChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1d_commandticks"), 53> SIOMCD1DCommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd1d_currentcommand"), 54> SIOMCD1DCurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd1d_sector"), 55> SIOMCD1DSector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd1d_dataoffset"), 56> SIOMCD1DDataOffset;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd2b_tempbuffer"), 57> SIOMCD2BTempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2b_directoryflag"), 58> SIOMCD2BDirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2b_checksumin"), 59> SIOMCD2BChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2b_checksumout"), 60> SIOMCD2BChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2b_commandticks"), 61> SIOMCD2BCommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2b_currentcommand"), 62> SIOMCD2BCurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2b_sector"), 63> SIOMCD2BSector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd2b_dataoffset"), 64> SIOMCD2BDataOffset;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd2c_tempbuffer"), 65> SIOMCD2CTempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2c_directoryflag"), 66> SIOMCD2CDirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2c_checksumin"), 67> SIOMCD2CChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2c_checksumout"), 68> SIOMCD2CChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2c_commandticks"), 69> SIOMCD2CCommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2c_currentcommand"), 70> SIOMCD2CCurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2c_sector"), 71> SIOMCD2CSector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd2c_dataoffset"), 72> SIOMCD2CDataOffset;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<128>, TYPESTRING("mcd2d_tempbuffer"), 73> SIOMCD2DTempBuffer;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2d_directoryflag"), 74> SIOMCD2DDirectoryFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2d_checksumin"), 75> SIOMCD2DChecksumIn;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2d_checksumout"), 76> SIOMCD2DChecksumOut;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2d_commandticks"), 77> SIOMCD2DCommandTicks;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mcd2d_currentcommand"), 78> SIOMCD2DCurrentCommand;
typedef Protobuf::FieldRef<Protobuf::UInt16, TYPESTRING("mcd2d_sector"), 79> SIOMCD2DSector;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("mcd2d_dataoffset"), 80> SIOMCD2DDataOffset;

// The pad's own side of a transfer, one per port. Absent from states made before this existed;
// "present" tells a real all-defaults pad from a state that never carried one.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("present"), 1> PadPresent;
typedef Protobuf::Field<Protobuf::UInt8, TYPESTRING("cmd"), 2> PadCmd;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("config_mode"), 3> PadConfigMode;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("analog_mode"), 4> PadAnalogMode;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("buffer_len"), 5> PadBufferLen;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("current_byte"), 6> PadCurrentByte;
typedef Protobuf::Field<Protobuf::FixedBytes<256>, TYPESTRING("buffer"), 7> PadBuffer;
typedef Protobuf::Field<Protobuf::FixedBytes<6>, TYPESTRING("motor_mapping"), 8> PadMotorMapping;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("config_commands_used"), 9> PadConfigCommandsUsed;
typedef Protobuf::Field<Protobuf::UInt8, TYPESTRING("old_method_latch"), 10> PadOldMethodLatch;
typedef Protobuf::Message<TYPESTRING("PadTransfer"), PadPresent, PadCmd, PadConfigMode, PadAnalogMode, PadBufferLen,
                          PadCurrentByte, PadBuffer, PadMotorMapping, PadConfigCommandsUsed, PadOldMethodLatch>
    PadTransfer;
typedef Protobuf::MessageField<PadTransfer, TYPESTRING("pad1"), 81> SIOPad1;
typedef Protobuf::MessageField<PadTransfer, TYPESTRING("pad2"), 82> SIOPad2;

typedef Protobuf::Message<
    TYPESTRING("SIO"), SIOBuffer, SIOStatusReg, SIOModeReg, SIOCtrlReg, SIOBaudReg, SIOBufferMaxIndex, SIOBufferIndex,
    SIOPadState, SIOCurrentDevice, SIOPadAddress, SIOMCD1TempBuffer, SIOMCD1DirectoryFlag, SIOMCD1ChecksumIn,
    SIOMCD1ChecksumOut, SIOMCD1CommandTicks, SIOMCD1CurrentCommand, SIOMCD1Sector, SIOMCD1DataOffset, SIOMCD2TempBuffer,
    SIOMCD2DirectoryFlag, SIOMCD2ChecksumIn, SIOMCD2ChecksumOut, SIOMCD2CommandTicks, SIOMCD2CurrentCommand,
    SIOMCD2Sector, SIOMCD2DataOffset, SIOMCD1BTempBuffer, SIOMCD1BDirectoryFlag, SIOMCD1BChecksumIn,
    SIOMCD1BChecksumOut, SIOMCD1BCommandTicks, SIOMCD1BCurrentCommand, SIOMCD1BSector, SIOMCD1BDataOffset,
    SIOMCD1CTempBuffer, SIOMCD1CDirectoryFlag, SIOMCD1CChecksumIn, SIOMCD1CChecksumOut, SIOMCD1CCommandTicks,
    SIOMCD1CCurrentCommand, SIOMCD1CSector, SIOMCD1CDataOffset, SIOMCD1DTempBuffer, SIOMCD1DDirectoryFlag,
    SIOMCD1DChecksumIn, SIOMCD1DChecksumOut, SIOMCD1DCommandTicks, SIOMCD1DCurrentCommand, SIOMCD1DSector,
    SIOMCD1DDataOffset, SIOMCD2BTempBuffer, SIOMCD2BDirectoryFlag, SIOMCD2BChecksumIn, SIOMCD2BChecksumOut,
    SIOMCD2BCommandTicks, SIOMCD2BCurrentCommand, SIOMCD2BSector, SIOMCD2BDataOffset, SIOMCD2CTempBuffer,
    SIOMCD2CDirectoryFlag, SIOMCD2CChecksumIn, SIOMCD2CChecksumOut, SIOMCD2CCommandTicks, SIOMCD2CCurrentCommand,
    SIOMCD2CSector, SIOMCD2CDataOffset, SIOMCD2DTempBuffer, SIOMCD2DDirectoryFlag, SIOMCD2DChecksumIn,
    SIOMCD2DChecksumOut, SIOMCD2DCommandTicks, SIOMCD2DCurrentCommand, SIOMCD2DSector, SIOMCD2DDataOffset, SIOPad1,
    SIOPad2>
    SIO;
typedef Protobuf::MessageField<SIO, TYPESTRING("sio"), 7> SIOField;
// skip id 8
typedef Protobuf::EmptyMessage<TYPESTRING("Hardware")> Hardware;
typedef Protobuf::MessageField<Hardware, TYPESTRING("hardware"), 9> HardwareField;

typedef Protobuf::Field<Protobuf::UInt16, TYPESTRING("mode"), 1> RcntMode;
typedef Protobuf::Field<Protobuf::UInt16, TYPESTRING("target"), 2> RcntTarget;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("rate"), 3> RcntRate;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("irq"), 4> RcntIRQ;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("counter_state"), 5> RcntCounterState;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("irq_state"), 6> RcntIRQState;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("cycle"), 7> RcntCycle;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("cycle_start"), 8> RcntCycleStart;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("gate_started"), 9> RcntGateStarted;
typedef Protobuf::Message<TYPESTRING("Rcnt"), RcntMode, RcntTarget, RcntRate, RcntIRQ, RcntCounterState, RcntIRQState,
                          RcntCycle, RcntCycleStart, RcntGateStarted>
    Rcnt;
typedef Protobuf::RepeatedField<Rcnt, 4, TYPESTRING("rcnts"), 1> Rcnts;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("hsync_count"), 2> HSyncCount;
// Field 3 was spu_sync_countdown, dropped when the SPU stopped being pumped on the
// CPU's scanline schedule. DO NOT REUSE FIELD NUMBER 3 in Counters.
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("psx_next_counter"), 4> PSXNextCounter;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("field_count"), 5> FieldCount;
typedef Protobuf::Message<TYPESTRING("Counters"), Rcnts, HSyncCount, PSXNextCounter, FieldCount> Counters;
typedef Protobuf::MessageField<Counters, TYPESTRING("counters"), 10> CountersField;

typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("reg0"), 1> MDECReg0;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("reg1"), 2> MDECReg1;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("rl"), 3> MDECRl;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("rl_end"), 4> MDECRlEnd;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("block_buffer_pos"), 5> MDECBlockBufferPos;
typedef Protobuf::Field<Protobuf::FixedBytes<768>, TYPESTRING("block_buffer"), 6> MDECBlockBuffer;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("adr"), 7> MDECDMAADR;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("bcr"), 8> MDECDMABCR;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("chcr"), 9> MDECDMACHCR;
typedef Protobuf::RepeatedField<Protobuf::Int32, 64, TYPESTRING("iq_y"), 10> MDECIQY;
typedef Protobuf::RepeatedField<Protobuf::Int32, 64, TYPESTRING("iq_uv"), 11> MDECIQUV;
// Added with MDEC(3) support. The raw quant tables are needed because the general
// IDCT path must not use the AAN-prescaled ones, and the scale matrix is needed
// because the hardware does not initialise it: a state restored without it would
// decode every subsequent macroblock against a zero matrix.
typedef Protobuf::RepeatedField<Protobuf::Int32, 64, TYPESTRING("qt_y"), 12> MDECQTY;
typedef Protobuf::RepeatedField<Protobuf::Int32, 64, TYPESTRING("qt_uv"), 13> MDECQTUV;
typedef Protobuf::RepeatedField<Protobuf::Int32, 64, TYPESTRING("scaletable"), 14> MDECScaleTable;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("custom_scale"), 15> MDECCustomScale;
typedef Protobuf::Message<TYPESTRING("MDEC"), MDECReg0, MDECReg1, MDECRl, MDECRlEnd, MDECBlockBufferPos,
                          MDECBlockBuffer, MDECDMAADR, MDECDMABCR, MDECDMACHCR, MDECIQY, MDECIQUV, MDECQTY, MDECQTUV,
                          MDECScaleTable, MDECCustomScale>
    MDEC;
typedef Protobuf::MessageField<MDEC, TYPESTRING("mdec"), 11> MDECField;

// skip id 12

typedef Protobuf::Field<Protobuf::UInt16, TYPESTRING("fd"), 1> PCdrvFD;
typedef Protobuf::Field<Protobuf::String, TYPESTRING("filename"), 2> PCdrvFilename;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("create"), 3> PCdrvCreate;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("read_position"), 4> PCdrvReadPosition;
typedef Protobuf::Field<Protobuf::UInt64, TYPESTRING("write_position"), 5> PCdrvWritePosition;
// Older saves have neither position: their files resume reading at 0 and writing at the end.
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("has_positions"), 6> PCdrvHasPositions;
typedef Protobuf::Message<TYPESTRING("PCdrvFile"), PCdrvFD, PCdrvFilename, PCdrvCreate, PCdrvReadPosition,
                          PCdrvWritePosition, PCdrvHasPositions>
    PCdrvFile;
typedef Protobuf::RepeatedVariableField<PCdrvFile, TYPESTRING("files"), 13> PCdrvFilesField;

typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("ra"), 1> CallRA;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("sp"), 2> CallSP;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("fp"), 3> CallFP;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("shadow"), 4> Shadow;
typedef Protobuf::Message<TYPESTRING("Call"), CallRA, CallSP, CallFP, Shadow> Call;

typedef Protobuf::RepeatedVariableField<Call, TYPESTRING("calls"), 1> Calls;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("low"), 2> LowSP;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("high"), 3> HighSP;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("ra"), 4> PresumedRA;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("fp"), 5> PresumedFP;
typedef Protobuf::Field<Protobuf::Bool, TYPESTRING("iscurrent"), 6> CallstackIsCurrent;
typedef Protobuf::Message<TYPESTRING("Calls"), Calls, LowSP, HighSP, PresumedRA, PresumedFP, CallstackIsCurrent>
    CallStack;

typedef Protobuf::RepeatedVariableField<CallStack, TYPESTRING("CallStacks"), 1> CallStacksMessageField;
typedef Protobuf::Field<Protobuf::UInt32, TYPESTRING("currentSP"), 2> CallStacksCurrentSP;
typedef Protobuf::Message<TYPESTRING("CallStacks"), CallStacksMessageField, CallStacksCurrentSP> CallStacks;
typedef Protobuf::MessageField<CallStacks, TYPESTRING("callstacks"), 14> CallStacksField;

typedef Protobuf::FieldPtr<Protobuf::FixedBytes<16>, TYPESTRING("payload"), 1> CDQueuePayload;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("value"), 2> CDQueueValue;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("value_read"), 3> CDQueueValueRead;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("has_value"), 4> CDQueueHasValue;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("hit_max"), 5> CDQueueHitMax;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("payload_size"), 6> CDQueuePayloadSize;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("payload_index"), 7> CDQueuePayloadIndex;
typedef Protobuf::Message<TYPESTRING("CDQueueElement"), CDQueuePayload, CDQueueValue, CDQueueValueRead, CDQueueHasValue,
                          CDQueueHitMax, CDQueuePayloadSize, CDQueuePayloadIndex>
    CDQueueElement;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("m"), 1> CDMSFMinute;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("s"), 2> CDMSFSecond;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("f"), 3> CDMSFFrame;
typedef Protobuf::Message<TYPESTRING("CDMSF"), CDMSFMinute, CDMSFSecond, CDMSFFrame> CDMSF;
typedef Protobuf::FieldRef<Protobuf::Int32, TYPESTRING("y0"), 1> CDADPCMY0;
typedef Protobuf::FieldRef<Protobuf::Int32, TYPESTRING("y1"), 2> CDADPCMY1;
typedef Protobuf::Message<TYPESTRING("CDADPCM"), CDADPCMY0, CDADPCMY1> CDADPCM;

typedef Protobuf::FieldPtr<Protobuf::FixedBytes<2352>, TYPESTRING("dataFIFO"), 1> CDDataFIFO;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("dataFIFOIndex"), 2> CDDataFIFOIndex;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("dataFIFOSize"), 3> CDDataFIFOSize;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("dataFIFOPending"), 4> CDDataFIFOPending;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("registerAddress"), 5> CDRegisterAddress;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("motorOn"), 6> CDMotorOn;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("speedChanged"), 7> CDSpeedChanged;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("invalidLocL"), 8> CDInvalidLocL;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("dataRequested"), 9> CDDataRequested;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("subheaderFilter"), 10> CDSubheaderFilter;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("realtime"), 11> CDRealtime;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("readingState"), 12> CDReadingState;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("startPlaying"), 13> CDStartPlaying;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("readingType"), 14> CDReadingType;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("autoPause"), 15> CDAutoPause;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("report"), 16> CDReport;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("setLocPending"), 17> CDSetLocPending;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("muted"), 18> CDMuted;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("peakFlag"), 19> CDPeakFlag;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("playTrack"), 20> CDPlayTrack;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("playStartCycle"), 21> CDPlayStartCycle;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("status"), 22> CDStatus;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("speed"), 23> CDSpeed;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("readSpan"), 24> CDReadSpan;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("interruptCauseMask"), 25> CDInterruptCauseMask;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<4>, TYPESTRING("atv"), 26> CDATV;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<4>, TYPESTRING("atvPending"), 27> CDATVPending;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("adpcmMuted"), 28> CDADPCMMuted;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("mode"), 29> CDMode;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("filterFile"), 30> CDFilterFile;
typedef Protobuf::FieldRef<Protobuf::UInt8, TYPESTRING("filterChannel"), 31> CDFilterChannel;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("xaEnded"), 32> CDXAEnded;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("xaFirstSector"), 33> CDXAFirstSector;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("soundMapEnabled"), 34> CDSoundMapEnabled;
typedef Protobuf::MessageField<CDMSF, TYPESTRING("currentPosition"), 35> CDCurrentPosition;
typedef Protobuf::MessageField<CDMSF, TYPESTRING("seekPosition"), 36> CDSeekPosition;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<8>, TYPESTRING("lastLocP"), 37> CDLastLocP;
typedef Protobuf::FieldPtr<Protobuf::FixedBytes<8>, TYPESTRING("lastLocL"), 38> CDLastLocL;
typedef Protobuf::FieldRef<Protobuf::UInt64, TYPESTRING("seed"), 39> CDSeed;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("lidOpen"), 40> CDLidOpen;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("wasLidOpened"), 41> CDWasLidOpened;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("lidCloseScheduled"), 42> CDLidCloseScheduled;
typedef Protobuf::FieldRef<Protobuf::UInt32, TYPESTRING("lidCloseAtCycles"), 43> CDLidCloseAtCycles;
typedef Protobuf::MessageField<CDQueueElement, TYPESTRING("commandFifo"), 44> CDCommandFifo;
typedef Protobuf::MessageField<CDQueueElement, TYPESTRING("commandExecuting"), 45> CDCommandExecuting;
typedef Protobuf::MessageField<CDQueueElement, TYPESTRING("responseFifo0"), 46> CDResponseFifo0;
typedef Protobuf::MessageField<CDQueueElement, TYPESTRING("responseFifo1"), 47> CDResponseFifo1;
typedef Protobuf::MessageField<CDADPCM, TYPESTRING("xaLeft"), 48> CDXALeft;
typedef Protobuf::MessageField<CDADPCM, TYPESTRING("xaRight"), 49> CDXARight;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("sectorReadSinceSeek"), 50> CDSectorReadSinceSeek;
typedef Protobuf::FieldRef<Protobuf::Bool, TYPESTRING("scexCountersReset"), 51> CDScexCountersReset;
typedef Protobuf::Message<
    TYPESTRING("CDRom"), CDDataFIFO, CDDataFIFOIndex, CDDataFIFOSize, CDDataFIFOPending, CDRegisterAddress, CDMotorOn,
    CDSpeedChanged, CDInvalidLocL, CDDataRequested, CDSubheaderFilter, CDRealtime, CDReadingState, CDStartPlaying,
    CDReadingType, CDAutoPause, CDReport, CDSetLocPending, CDMuted, CDPeakFlag, CDPlayTrack, CDPlayStartCycle, CDStatus,
    CDSpeed, CDReadSpan, CDInterruptCauseMask, CDATV, CDATVPending, CDADPCMMuted, CDMode, CDFilterFile, CDFilterChannel,
    CDXAEnded, CDXAFirstSector, CDSoundMapEnabled, CDCurrentPosition, CDSeekPosition, CDLastLocP, CDLastLocL, CDSeed,
    CDLidOpen, CDWasLidOpened, CDLidCloseScheduled, CDLidCloseAtCycles, CDCommandFifo, CDCommandExecuting,
    CDResponseFifo0, CDResponseFifo1, CDXALeft, CDXARight, CDSectorReadSinceSeek, CDScexCountersReset>
    CDRom;
typedef Protobuf::MessageField<CDRom, TYPESTRING("cdrom"), 15> CDRomField;

typedef Protobuf::Message<TYPESTRING("SaveState"), SaveStateInfoField, ThumbnailField, MemoryField, RegistersField,
                          GPUField, SPUField, SIOField, HardwareField, CountersField, MDECField, PCdrvFilesField,
                          CallStacksField, CDRomField>
    SaveState;

typedef Protobuf::ProtoFile<SaveStateInfo, Thumbnail, Memory, DelaySlotInfo, GtePendingWriteInfo, Registers, GPU,
                            ADPCMDecode, XA, ::PCSX::SPU::Chan::Data, ::PCSX::SPU::ADSRInfo, ::PCSX::SPU::ADSRInfoEx,
                            Channel, EnvelopeCheckpoint, SPU, PadTransfer, SIO, CDQueueElement, CDMSF, CDADPCM, CDRom,
                            Hardware, Rcnt, Counters, MDEC, PCdrvFile, Call, CallStack, CallStacks, SaveState>
    ProtoFile;

SaveState constructSaveState();

// The classes whose private state the save state reads befriend this instead of
// constructSaveState() itself, so their headers only need a forward declaration of it
// rather than this whole schema.
struct Access {
    static SaveState constructSaveState();
};

std::string save();
// Replaces the emulator state right now. Only safe with nothing of the
// emulation left on the stack; use loadSafe() from anywhere else.
bool load(std::string_view data);
// Loads if it's safe to do so, queues it for the main loop otherwise. This is
// what every user-facing entry point wants: the Lua bindings, the GUI menu and
// hotkey, and the web server all reach for a load from callbacks that run in
// the middle of Counters::update() and R3000Acpu::branchTest(). Returns false
// if the data does not parse as a save state, or if a load done right away
// failed. A queued load that parsed fine is applied by the main loop.
bool loadSafe(std::string &&data);

/* The in-memory pair. capture()/restore() are save()/load() with the protobuf encode and
   decode taken out: the message already owns a copy of every field, and commit() has always
   pushed those copies live, so the only missing direction was pulling live state into them.
   restore() deliberately does NOT call m_cpu->Reset() the way load() does, so a caller that
   wants a transparent snapshot keeps its icache and delayed-load state - which also means it
   owes the code cache an invalidation for anything it rewound. */
void capture(SaveState &state);
void restore(SaveState &state);
}  // namespace SaveStates

}  // namespace PCSX
