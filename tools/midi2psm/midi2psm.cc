/***************************************************************************
 *   Copyright (C) 2025 PCSX-Redux authors                                 *
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

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "args/args.hxx"
#include "fmt/format.h"
#include "support/binstruct.h"
#include "support/file.h"
#include "support/tool-args.h"
#include "support/typestring-wrapper.h"
#include "supportpsx/midi-converter.h"


using PCSX::MidiConverter::MIDI_CHANNEL_PRESSURE;
using PCSX::MidiConverter::MIDI_CONTROL_CHANGE;
using PCSX::MidiConverter::MIDI_CC_ALL_NOTES_OFF;
using PCSX::MidiConverter::MIDI_CC_ALL_SOUND_OFF;
using PCSX::MidiConverter::MIDI_CC_BANK_LSB;
using PCSX::MidiConverter::MIDI_CC_BANK_MSB;
using PCSX::MidiConverter::MIDI_CC_DATA_ENTRY_LSB;
using PCSX::MidiConverter::MIDI_CC_DATA_ENTRY_MSB;
using PCSX::MidiConverter::MIDI_CC_EXPRESSION;
using PCSX::MidiConverter::MIDI_CC_LOOP_POINT;
using PCSX::MidiConverter::MIDI_CC_MODULATION;
using PCSX::MidiConverter::MIDI_CC_PAN;
using PCSX::MidiConverter::MIDI_CC_RESET_ALL;
using PCSX::MidiConverter::MIDI_CC_REVERB_SEND;
using PCSX::MidiConverter::MIDI_CC_RPN_LSB;
using PCSX::MidiConverter::MIDI_CC_RPN_MSB;
using PCSX::MidiConverter::MIDI_CC_SUSTAIN;
using PCSX::MidiConverter::MIDI_CC_VOLUME;
using PCSX::MidiConverter::MIDI_DRUM_CHANNEL;
using PCSX::MidiConverter::MIDI_META;
using PCSX::MidiConverter::MIDI_META_COPYRIGHT;
using PCSX::MidiConverter::MIDI_META_CUE_POINT;
using PCSX::MidiConverter::MIDI_META_MARKER;
using PCSX::MidiConverter::MIDI_META_TRACK_NAME;
using PCSX::MidiConverter::MIDI_META_TEMPO;
using PCSX::MidiConverter::MIDI_NOTE_OFF;
using PCSX::MidiConverter::MIDI_NOTE_ON;
using PCSX::MidiConverter::MIDI_PITCH_BEND;
using PCSX::MidiConverter::MIDI_PROGRAM_CHANGE;
using PCSX::MidiConverter::MidiFile;
using PCSX::MidiConverter::SampleKey;
using PCSX::MidiConverter::SPU_RAM_BASE;
using PCSX::MidiConverter::SPU_RAM_SIZE;
using PCSX::MidiConverter::SpuSample;
using PCSX::MidiConverter::extractAndEncode;
using PCSX::MidiConverter::extractMidiMetadata;
using PCSX::MidiConverter::findLoopPointTick;
using PCSX::MidiConverter::findRegions;
using PCSX::MidiConverter::midiNoteToFreq;
using PCSX::MidiConverter::sf2RegionToSpuADSR;

// ============================================================================
// PSM Event Types
// ============================================================================

enum PsmEventType : uint8_t {
    PSM_NOTE_ON = 0x00,
    PSM_NOTE_OFF = 0x01,
    PSM_PITCH_BEND = 0x02,
    PSM_CC_VOLUME = 0x03,
    PSM_CC_PAN = 0x04,
    PSM_CC_EXPRESSION = 0x05,
    PSM_CC_SUSTAIN = 0x06,
    PSM_CC_MODULATION = 0x07,
    PSM_CC_REVERB = 0x08,
    PSM_PROGRAM_CHANGE = 0x09,
    PSM_TEMPO_CHANGE = 0x0A,
    PSM_LOOP_POINT = 0x0B,
    PSM_END = 0x0C,
    PSM_USER = 0x0D,
    PSM_LONG_WAIT = 0xFF,
};

namespace bs = PCSX::BinStruct;

// PSM event stream record: a fixed 8-byte entry. The PSM file is a PsmHeader followed by N of these.
typedef bs::Field<bs::UInt16, TYPESTRING("psmDeltaTick")> PsmDeltaTick;
typedef bs::Field<bs::UInt8, TYPESTRING("psmType")> PsmType;
typedef bs::Field<bs::UInt8, TYPESTRING("psmChannel")> PsmChannel;
typedef bs::Field<bs::UInt32, TYPESTRING("psmData")> PsmData;
typedef bs::Struct<TYPESTRING("PsmEvent"), PsmDeltaTick, PsmType, PsmChannel, PsmData> PsmEvent;

// PSM file header (16 bytes): "PSM\0" magic, version, initial tick rate, event count.
typedef bs::Field<bs::CString<4>, TYPESTRING("psmMagic")> PsmMagic;
typedef bs::Field<bs::UInt32, TYPESTRING("psmVersion")> PsmVersion;
typedef bs::Field<bs::UInt32, TYPESTRING("psmTickRate")> PsmTickRate;
typedef bs::Field<bs::UInt32, TYPESTRING("psmEventCount")> PsmEventCount;
typedef bs::Struct<TYPESTRING("PsmHeader"), PsmMagic, PsmVersion, PsmTickRate, PsmEventCount> PsmHeader;

// ============================================================================
// VAB Format Structures (Sony SDK compatible), modeled as binstructs so the wire layout is described
// once and (de)serialized through the typed, little-endian IO<File> API - no packed structs, no
// host-endianness or padding assumptions.
// ============================================================================

// VabHdr (32 bytes)
typedef bs::Field<bs::CString<4>, TYPESTRING("vabMagic")> VabMagic;        // "pBAV"
typedef bs::Field<bs::UInt32, TYPESTRING("vabVersion")> VabVersion;        // format version
typedef bs::Field<bs::UInt32, TYPESTRING("vabId")> VabId;                  // bank ID
typedef bs::Field<bs::UInt32, TYPESTRING("vabFileSize")> VabFileSize;      // total file size in bytes
typedef bs::Field<bs::UInt16, TYPESTRING("vabReserved0")> VabReserved0;    // system reserved
typedef bs::Field<bs::UInt16, TYPESTRING("vabNumPrograms")> VabNumPrograms;  // number of programs (max 128)
typedef bs::Field<bs::UInt16, TYPESTRING("vabNumTones")> VabNumTones;      // total number of tones
typedef bs::Field<bs::UInt16, TYPESTRING("vabNumVags")> VabNumVags;        // number of VAG samples (max 254)
typedef bs::Field<bs::UInt8, TYPESTRING("vabMasterVol")> VabMasterVol;     // master volume (0-127)
typedef bs::Field<bs::UInt8, TYPESTRING("vabMasterPan")> VabMasterPan;     // master pan (0-127, 64 = center)
typedef bs::Field<bs::UInt8, TYPESTRING("vabAttr1")> VabAttr1;             // user-defined
typedef bs::Field<bs::UInt8, TYPESTRING("vabAttr2")> VabAttr2;             // user-defined
typedef bs::Field<bs::UInt32, TYPESTRING("vabReserved1")> VabReserved1;    // system reserved
typedef bs::Struct<TYPESTRING("VabHdr"), VabMagic, VabVersion, VabId, VabFileSize, VabReserved0, VabNumPrograms,
                   VabNumTones, VabNumVags, VabMasterVol, VabMasterPan, VabAttr1, VabAttr2, VabReserved1>
    VabHdr;

// ProgAtr (16 bytes)
typedef bs::Field<bs::UInt8, TYPESTRING("progTones")> ProgTones;        // number of tones in this program (0-16)
typedef bs::Field<bs::UInt8, TYPESTRING("progMvol")> ProgMvol;          // program volume (0-127)
typedef bs::Field<bs::UInt8, TYPESTRING("progPrior")> ProgPrior;        // priority (0-127)
typedef bs::Field<bs::UInt8, TYPESTRING("progMode")> ProgMode;          // mode flags
typedef bs::Field<bs::UInt8, TYPESTRING("progMpan")> ProgMpan;          // program pan (0-127, 64 = center)
typedef bs::Field<bs::UInt8, TYPESTRING("progReserved0")> ProgReserved0;
typedef bs::Field<bs::Int16, TYPESTRING("progAttr")> ProgAttr;          // user-defined attribute
typedef bs::Field<bs::UInt32, TYPESTRING("progReserved1")> ProgReserved1;
typedef bs::Field<bs::UInt32, TYPESTRING("progReserved2")> ProgReserved2;
typedef bs::Struct<TYPESTRING("ProgAtr"), ProgTones, ProgMvol, ProgPrior, ProgMode, ProgMpan, ProgReserved0,
                   ProgAttr, ProgReserved1, ProgReserved2>
    ProgAtr;

// VagAtr (32 bytes)
typedef bs::Field<bs::UInt8, TYPESTRING("vagPrior")> VagPrior;          // priority (0-127)
typedef bs::Field<bs::UInt8, TYPESTRING("vagMode")> VagMode;            // mode flags
typedef bs::Field<bs::UInt8, TYPESTRING("vagVol")> VagVol;              // tone volume (0-127)
typedef bs::Field<bs::UInt8, TYPESTRING("vagPan")> VagPan;              // tone pan (0-127, 64 = center)
typedef bs::Field<bs::UInt8, TYPESTRING("vagCenter")> VagCenter;        // center note (root key, MIDI note number)
typedef bs::Field<bs::UInt8, TYPESTRING("vagShift")> VagShift;          // pitch fine tune (signed cents, as uint8)
typedef bs::Field<bs::UInt8, TYPESTRING("vagMin")> VagMin;              // minimum key range
typedef bs::Field<bs::UInt8, TYPESTRING("vagMax")> VagMax;              // maximum key range
typedef bs::Field<bs::UInt8, TYPESTRING("vagVibW")> VagVibW;            // vibrato width
typedef bs::Field<bs::UInt8, TYPESTRING("vagVibT")> VagVibT;            // vibrato time/frequency
typedef bs::Field<bs::UInt8, TYPESTRING("vagPorW")> VagPorW;            // portamento width
typedef bs::Field<bs::UInt8, TYPESTRING("vagPorT")> VagPorT;            // portamento time
typedef bs::Field<bs::UInt8, TYPESTRING("vagPbmin")> VagPbmin;          // pitch bend min (semitones)
typedef bs::Field<bs::UInt8, TYPESTRING("vagPbmax")> VagPbmax;          // pitch bend max (semitones)
typedef bs::Field<bs::UInt8, TYPESTRING("vagReserved0")> VagReserved0;
typedef bs::Field<bs::UInt8, TYPESTRING("vagReserved1")> VagReserved1;
typedef bs::Field<bs::UInt16, TYPESTRING("vagAdsr1")> VagAdsr1;         // SPU ADSR register (voice +0x08)
typedef bs::Field<bs::UInt16, TYPESTRING("vagAdsr2")> VagAdsr2;         // SPU ADSR register (voice +0x0A)
typedef bs::Field<bs::Int16, TYPESTRING("vagProg")> VagProg;            // program index this tone belongs to
typedef bs::Field<bs::Int16, TYPESTRING("vagVag")> VagVag;              // VAG index (0-based, -1 = unused)
// reserved (reserved2[0] = sampleRate >> 4 for player)
typedef bs::RepeatedField<bs::Int16, TYPESTRING("vagReserved2"), 4> VagReserved2;
typedef bs::Struct<TYPESTRING("VagAtr"), VagPrior, VagMode, VagVol, VagPan, VagCenter, VagShift, VagMin, VagMax,
                   VagVibW, VagVibT, VagPorW, VagPorT, VagPbmin, VagPbmax, VagReserved0, VagReserved1, VagAdsr1,
                   VagAdsr2, VagProg, VagVag, VagReserved2>
    VagAtr;

// On-disk (wire) sizes of the VAB structures, needed for the VabHdr.fileSize total. binstruct describes
// the layout but doesn't expose it as a sizeof, so these are stated explicitly.
static constexpr uint32_t kVabHdrBytes = 32;
static constexpr uint32_t kProgAtrBytes = 16;
static constexpr uint32_t kVagAtrBytes = 32;

// ============================================================================
// Channel State (minimal - player handles volumes/pitch/sustain)
// ============================================================================

struct ChannelState {
    int program;
    uint8_t bankMSB;
    uint8_t bankLSB;

    ChannelState() : program(0), bankMSB(0), bankLSB(0) {}
};

// ============================================================================
// Conversion Context
// ============================================================================

using FilePtr = PCSX::IO<PCSX::File>;

struct ProgramBuildInfo {
    uint8_t toneCount = 0;
    // Map from region index within the tsf_preset to tone index within this program
    std::map<int, uint8_t> regionToTone;
};

struct ConvertContext {
    MidiFile midi;
    tsf* sf2;

    // Sample management
    std::vector<SpuSample> samples;
    std::map<SampleKey, size_t> sampleMap;
    uint32_t nextSpuAddr;

    // Channel state
    ChannelState channels[16];

    // VAB building
    ProgramBuildInfo programBuild[128];
    int numPrograms;
    std::map<int, uint8_t> presetToProgram;  // tsf preset index -> VAB program index
    ProgAtr progAtrs[128];
    VagAtr vagAtrs[128 * 16];  // max possible (compacted on write)

    // PSM events
    std::vector<PsmEvent> psmEvents;
    uint32_t lastTick;

    // Options
    unsigned maxLayers;

    // Metadata and loop
    std::string trackName;
    std::string copyright;
    int32_t loopPointTick;

    // Polyphony tracking
    int activeVoices;
    int peakPolyphony;

    // ---- Methods ----
    void generate();
    // Writes the VAB instrument bank. When vbFile is null the sample body trails the header in vhFile
    // (combined .vab); when non-null, vhFile gets the header only and vbFile gets the raw sample body
    // (split VH/VB), the latter disposable after upload to SPU RAM.
    bool writeVab(const char* vhFile, const char* vbFile);
    bool writePsm(const char* filename);

    void emitEvent(uint8_t type, uint8_t channel, uint32_t data, uint32_t absTick);
    uint8_t getOrAssignProgram(int presetIndex);
    uint8_t getOrAssignTone(uint8_t programIdx, int presetIndex, struct tsf_region* region);
};

void ConvertContext::emitEvent(uint8_t type, uint8_t channel, uint32_t data, uint32_t absTick) {
    uint32_t delta = absTick - lastTick;
    lastTick = absTick;

    // Consume excess delta with LONG_WAIT events (max deltaTick is 65535)
    while (delta > 65535) {
        uint32_t consume = delta - 65535;  // leave up to 65535 for the final event
        if (consume > 0xFFFFFFFF) consume = 0xFFFFFFFF;
        PsmEvent wait;
        wait.get<PsmDeltaTick>().value = 0;
        wait.get<PsmType>().value = PSM_LONG_WAIT;
        wait.get<PsmChannel>().value = 0;
        wait.get<PsmData>().value = consume;
        psmEvents.push_back(wait);
        delta -= consume;
    }

    PsmEvent ev;
    ev.get<PsmDeltaTick>().value = (uint16_t)delta;
    ev.get<PsmType>().value = type;
    ev.get<PsmChannel>().value = channel;
    ev.get<PsmData>().value = data;
    psmEvents.push_back(ev);
}

uint8_t ConvertContext::getOrAssignProgram(int presetIndex) {
    auto it = presetToProgram.find(presetIndex);
    if (it != presetToProgram.end()) return it->second;

    if (numPrograms >= 128) {
        fmt::print(stderr, "Warning: exceeded 128 VAB programs, reusing program 127\n");
        return 127;
    }

    uint8_t idx = (uint8_t)numPrograms++;
    presetToProgram[presetIndex] = idx;

    // Initialize ProgAtr with sensible defaults
    ProgAtr& pa = progAtrs[idx];
    pa.reset();
    pa.get<ProgTones>().value = 0;
    pa.get<ProgMvol>().value = 127;
    pa.get<ProgPrior>().value = 127;
    pa.get<ProgMode>().value = 0;
    pa.get<ProgMpan>().value = 64;

    return idx;
}

uint8_t ConvertContext::getOrAssignTone(uint8_t programIdx, int presetIndex, struct tsf_region* region) {
    // Compute region index within the tsf_preset
    struct tsf_preset* preset = &sf2->presets[presetIndex];
    int regionIdx = (int)(region - &preset->regions[0]);

    auto& build = programBuild[programIdx];
    auto it = build.regionToTone.find(regionIdx);
    if (it != build.regionToTone.end()) return it->second;

    if (build.toneCount >= 16) {
        fmt::print(stderr, "Warning: program {} exceeded 16 tones, reusing tone 15\n", programIdx);
        return 15;
    }

    uint8_t toneIdx = build.toneCount++;
    build.regionToTone[regionIdx] = toneIdx;

    // Update ProgAtr tone count
    progAtrs[programIdx].get<ProgTones>().value = build.toneCount;

    // Extract and encode the sample
    size_t sampleIdx = extractAndEncode(sf2, region, samples, sampleMap, nextSpuAddr);

    // Fill in the VagAtr entry
    VagAtr& va = vagAtrs[programIdx * 16 + toneIdx];
    va.reset();

    va.get<VagPrior>().value = 127;
    va.get<VagMode>().value = 0;

    // Apply SF2 attenuation to tone volume
    float attnGain = 1.0f;
    if (region->attenuation > 0.0f) {
        attnGain = powf(10.0f, -region->attenuation / 200.0f);
        if (attnGain < 0.0f) attnGain = 0.0f;
        if (attnGain > 1.0f) attnGain = 1.0f;
    }
    va.get<VagVol>().value = (uint8_t)(attnGain * 127.0f + 0.5f);

    // Map SF2 pan (-0.5 to +0.5) to VAB pan (0-127, 64 = center)
    int pan = (int)(64.0f + region->pan * 128.0f);
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    va.get<VagPan>().value = (uint8_t)pan;

    // Compute adjusted center note: fold sample rate and transpose into center
    // so the player can assume 44100 Hz and just compute freq(note)/freq(center)*0x1000
    SpuSample& sample = samples[sampleIdx];
    // The player computes 2^((note - center) / 12) * 2^(shift / 1200), so the fractional part of
    // the center goes into the shift with its sign flipped, alongside the tuning.
    double semitonesAdj = 12.0 * log2(44100.0 / (double)sample.sampleRate);
    double adjustedCenter = (double)sample.rootKey - sample.transpose + semitonesAdj - sample.tune / 100.0;
    int centerNote = (int)round(adjustedCenter);
    int fineTuneCents = (int)round((centerNote - adjustedCenter) * 100.0);

    // Clamp center to valid MIDI range
    // Moving the center up by one semitone needs 100 more cents of shift to keep the pitch.
    if (centerNote < 0) {
        fineTuneCents -= centerNote * 100;
        centerNote = 0;
    }
    if (centerNote > 127) {
        fineTuneCents -= (centerNote - 127) * 100;
        centerNote = 127;
    }

    // Clamp fine tune to int8 range (-128 to +127 cents)
    if (fineTuneCents < -128) fineTuneCents = -128;
    if (fineTuneCents > 127) fineTuneCents = 127;

    va.get<VagCenter>().value = (uint8_t)centerNote;
    va.get<VagShift>().value = (uint8_t)(int8_t)fineTuneCents;

    // Key range from SF2 region
    va.get<VagMin>().value = (uint8_t)region->lokey;
    va.get<VagMax>().value = (uint8_t)region->hikey;

    // Vibrato/portamento: zero (player handles CC#1 modulation)
    va.get<VagVibW>().value = 0;
    va.get<VagVibT>().value = 0;
    va.get<VagPorW>().value = 0;
    va.get<VagPorT>().value = 0;

    // Pitch bend range: default 2 semitones
    va.get<VagPbmin>().value = 2;
    va.get<VagPbmax>().value = 2;

    // ADSR
    bool isDrum = false;
    // Check if this program is a drum program by scanning which channels use it
    // (simpler: check if any region in the preset has group != 0, or if bank >= 128)
    int bank = tsf_get_presetindex(sf2, 128, 0);
    if (bank >= 0 && bank == presetIndex) isDrum = true;
    // Also check for drum channel assignment (program >= 128 convention not applicable here,
    // so we rely on the drum bank check)

    sf2RegionToSpuADSR(region, isDrum, va.get<VagAdsr1>().value, va.get<VagAdsr2>().value);

    va.get<VagProg>().value = (int16_t)programIdx;
    va.get<VagVag>().value = (sampleIdx != (size_t)-1) ? (int16_t)sampleIdx : -1;

    // Store sample rate in reserved field for player reference
    // Player can use this for pitch computation if it needs the actual rate
    va.get<VagReserved2>()[0].value = (int16_t)(sample.sampleRate >> 4);  // fits in 16 bits up to ~500 kHz

    return toneIdx;
}

void ConvertContext::generate() {
    lastTick = 0;
    loopPointTick = -1;
    activeVoices = 0;
    peakPolyphony = 0;
    numPrograms = 0;
    nextSpuAddr = SPU_RAM_BASE;

    for (auto& pa : progAtrs) pa.reset();
    for (auto& va : vagAtrs) va.reset();

    for (int i = 0; i < 16; i++) {
        channels[i] = ChannelState();
    }
    // The drum channel's program selects the GM drum kit (preset number under SF2 bank 128);
    // it defaults to 0 (Standard kit) and is updated by program changes like any other channel.

    // Pre-scan for loop markers
    for (auto& mev : midi.events) {
        if (mev.type == MIDI_CONTROL_CHANGE && mev.data1 == MIDI_CC_LOOP_POINT) {
            loopPointTick = (int32_t)mev.absoluteTick;
            break;
        }
        if (mev.type == MIDI_META && mev.data1 == MIDI_META_MARKER) {
            if (mev.textData == "loopStart" || mev.textData == "LoopStart" || mev.textData == "loop") {
                loopPointTick = (int32_t)mev.absoluteTick;
                break;
            }
        }
    }

    // Pre-scan for metadata
    for (auto& mev : midi.events) {
        if (mev.type == MIDI_META) {
            if (mev.data1 == MIDI_META_TRACK_NAME && trackName.empty()) {
                trackName = mev.textData;
            } else if (mev.data1 == MIDI_META_COPYRIGHT && copyright.empty()) {
                copyright = mev.textData;
            }
        }
    }

    // Default MIDI tempo: 120 BPM = 500000 us/beat
    uint32_t currentTempo = 500000;

    auto computeTickRate = [&]() -> uint32_t {
        double ticksPerSec = (double)midi.tpqn * 1000000.0 / (double)currentTempo;
        return (uint32_t)(ticksPerSec * 65536.0);  // 16.16 fixed point
    };

    // Emit initial tick rate as first event
    emitEvent(PSM_TEMPO_CHANGE, 0, computeTickRate(), 0);

    bool loopPointEmitted = false;

    // If no loop marker found, emit loop point at start
    if (loopPointTick < 0) {
        emitEvent(PSM_LOOP_POINT, 0, 0, 0);
        loopPointEmitted = true;
    }

    // Simple polyphony tracking: count active (channel, note) pairs
    // Each NOTE_ON for a layer increments, NOTE_OFF for a note decrements all its layers
    struct ActiveNote {
        uint8_t channel;
        uint8_t note;
        int layers;  // how many PSM NOTE_ONs were emitted for this note
    };
    std::vector<ActiveNote> activeNotes;

    // Process all MIDI events
    for (size_t i = 0; i < midi.events.size(); i++) {
        auto& mev = midi.events[i];

        // Emit loop point if we've reached its tick
        if (!loopPointEmitted && loopPointTick >= 0 && (int32_t)mev.absoluteTick >= loopPointTick) {
            emitEvent(PSM_LOOP_POINT, 0, 0, (uint32_t)loopPointTick);
            loopPointEmitted = true;
        }

        if (mev.type == MIDI_META && mev.data1 == MIDI_META_TEMPO) {
            currentTempo = mev.tempo;
            emitEvent(PSM_TEMPO_CHANGE, 0, computeTickRate(), mev.absoluteTick);
            continue;
        }

        // A cue point whose text is "tag:payload" becomes a USER event for the program playing the song.
        if (mev.type == MIDI_META && mev.data1 == MIDI_META_CUE_POINT) {
            std::string_view text = mev.textData;
            auto colon = text.find(':');
            unsigned tag = 0;
            uint32_t payload = 0;
            if (colon != std::string_view::npos) {
                auto end = text.data() + colon;
                auto [p1, e1] = std::from_chars(text.data(), end, tag);
                auto [p2, e2] = std::from_chars(end + 1, text.data() + text.size(), payload);
                if (e1 == std::errc() && p1 == end && e2 == std::errc() && p2 == text.data() + text.size() &&
                    tag <= 255) {
                    emitEvent(PSM_USER, tag, payload, mev.absoluteTick);
                    continue;
                }
            }
            fmt::print(stderr, "Ignoring cue point \"{}\" at tick {}: expected tag:payload\n", text, mev.absoluteTick);
            continue;
        }

        if (mev.type == MIDI_META) continue;

        if (mev.type == MIDI_PROGRAM_CHANGE) {
            channels[mev.channel].program = mev.data1;

            // Emit program change (the program index will be resolved at NOTE_ON time,
            // but emit the event for the player's state tracking)
            // We'll use the MIDI program number; the player maps it to VAB program
            // Actually, we should emit the VAB program index. But we may not know it yet
            // if no note has been played with this program. Defer: emit with MIDI program number.
            // The player can use this for display/debugging but relies on NOTE_ON's program field.
            emitEvent(PSM_PROGRAM_CHANGE, mev.channel, mev.data1, mev.absoluteTick);
            continue;
        }

        if (mev.type == MIDI_CONTROL_CHANGE) {
            switch (mev.data1) {
                case MIDI_CC_VOLUME:
                    emitEvent(PSM_CC_VOLUME, mev.channel, mev.data2, mev.absoluteTick);
                    break;
                case MIDI_CC_PAN:
                    emitEvent(PSM_CC_PAN, mev.channel, mev.data2, mev.absoluteTick);
                    break;
                case MIDI_CC_EXPRESSION:
                    emitEvent(PSM_CC_EXPRESSION, mev.channel, mev.data2, mev.absoluteTick);
                    break;
                case MIDI_CC_SUSTAIN:
                    emitEvent(PSM_CC_SUSTAIN, mev.channel, mev.data2, mev.absoluteTick);
                    break;
                case MIDI_CC_MODULATION:
                    emitEvent(PSM_CC_MODULATION, mev.channel, mev.data2, mev.absoluteTick);
                    break;
                case MIDI_CC_REVERB_SEND:
                    emitEvent(PSM_CC_REVERB, mev.channel, mev.data2, mev.absoluteTick);
                    break;
                case MIDI_CC_BANK_MSB:
                    channels[mev.channel].bankMSB = mev.data2;
                    break;
                case MIDI_CC_BANK_LSB:
                    channels[mev.channel].bankLSB = mev.data2;
                    break;
                case MIDI_CC_ALL_SOUND_OFF:
                case MIDI_CC_ALL_NOTES_OFF:
                    // Emit as sustain off + let the player handle it
                    // The player sees NOTE_OFF for each active note, but for panic CCs
                    // we can emit a sustain-off to release held voices
                    emitEvent(PSM_CC_SUSTAIN, mev.channel, 0, mev.absoluteTick);
                    // Also emit NOTE_OFF for all tracked active notes on this channel
                    for (auto it2 = activeNotes.begin(); it2 != activeNotes.end();) {
                        if (it2->channel == mev.channel) {
                            emitEvent(PSM_NOTE_OFF, mev.channel, it2->note, mev.absoluteTick);
                            activeVoices -= it2->layers;
                            it2 = activeNotes.erase(it2);
                        } else {
                            ++it2;
                        }
                    }
                    break;
                case MIDI_CC_RESET_ALL:
                    // Reset channel CCs to defaults
                    emitEvent(PSM_CC_VOLUME, mev.channel, 100, mev.absoluteTick);
                    emitEvent(PSM_CC_PAN, mev.channel, 64, mev.absoluteTick);
                    emitEvent(PSM_CC_EXPRESSION, mev.channel, 127, mev.absoluteTick);
                    emitEvent(PSM_CC_SUSTAIN, mev.channel, 0, mev.absoluteTick);
                    emitEvent(PSM_CC_MODULATION, mev.channel, 0, mev.absoluteTick);
                    emitEvent(PSM_CC_REVERB, mev.channel, 40, mev.absoluteTick);
                    emitEvent(PSM_PITCH_BEND, mev.channel, 0, mev.absoluteTick);
                    break;
                case MIDI_CC_LOOP_POINT:
                    // Already handled in pre-scan
                    break;
                default:
                    break;
            }
            continue;
        }

        if (mev.type == MIDI_PITCH_BEND) {
            int bend = ((int)mev.data2 << 7 | mev.data1) - 8192;
            // Pack as signed 16-bit in lower 16 bits of data
            emitEvent(PSM_PITCH_BEND, mev.channel, (uint32_t)(uint16_t)(int16_t)bend, mev.absoluteTick);
            continue;
        }

        if (mev.type == MIDI_NOTE_ON) {
            uint8_t note = mev.data1;
            uint8_t velocity = mev.data2;
            uint8_t ch = mev.channel;
            auto& chanState = channels[ch];
            bool isDrum = (ch == MIDI_DRUM_CHANNEL);

            // Find the SF2 preset
            // Drums live in SF2 bank 128 with the preset number selecting the kit; melodic
            // channels use bank MSB (SF2/GM fonts don't use bank LSB, so it is ignored here).
            int bank = isDrum ? 128 : (int)chanState.bankMSB;
            int prog = chanState.program;
            int presetIndex = tsf_get_presetindex(sf2, bank, prog);
            if (presetIndex < 0) {
                // Fall back to the default bank/kit when the requested one is absent.
                presetIndex = tsf_get_presetindex(sf2, isDrum ? 128 : 0, isDrum ? 0 : prog);
                if (presetIndex < 0) continue;
            }

            auto regions = findRegions(sf2, presetIndex, note, velocity);
            if (regions.empty()) continue;

            if (maxLayers > 0 && regions.size() > maxLayers) {
                regions.resize(maxLayers);
            }

            // Get or assign VAB program
            uint8_t vabProg = getOrAssignProgram(presetIndex);

            int layerCount = 0;
            for (auto* region : regions) {
                // Get or assign tone within the program
                uint8_t toneIdx = getOrAssignTone(vabProg, presetIndex, region);

                // Pack NOTE_ON data: note | velocity | program | toneIndex
                uint32_t data = (uint32_t)note |
                                ((uint32_t)velocity << 8) |
                                ((uint32_t)vabProg << 16) |
                                ((uint32_t)toneIdx << 24);
                emitEvent(PSM_NOTE_ON, ch, data, mev.absoluteTick);
                layerCount++;
            }

            // Track polyphony
            activeVoices += layerCount;
            if (activeVoices > peakPolyphony) peakPolyphony = activeVoices;

            // Record for NOTE_OFF tracking
            activeNotes.push_back({ch, note, layerCount});
            continue;
        }

        if (mev.type == MIDI_NOTE_OFF) {
            emitEvent(PSM_NOTE_OFF, mev.channel, mev.data1, mev.absoluteTick);

            // Update polyphony tracking: release all layers for this (channel, note)
            for (auto it2 = activeNotes.begin(); it2 != activeNotes.end(); ++it2) {
                if (it2->channel == mev.channel && it2->note == mev.data1) {
                    activeVoices -= it2->layers;
                    activeNotes.erase(it2);
                    break;
                }
            }
            continue;
        }
    }

    // Emit loop point if never emitted
    if (!loopPointEmitted) {
        uint32_t endTick = midi.events.empty() ? 0 : midi.events.back().absoluteTick;
        emitEvent(PSM_LOOP_POINT, 0, 0, endTick);
    }

    // End marker
    {
        uint32_t endTick = midi.events.empty() ? 0 : midi.events.back().absoluteTick;
        emitEvent(PSM_END, 0, 0, endTick);
    }
}

bool ConvertContext::writeVab(const char* vhFile, const char* vbFile) {
    const bool split = vbFile != nullptr;

    // Sample body sink: a real file in split mode (the disposable VB), or an in-memory buffer in
    // combined mode whose bytes get appended after the header. Same modconv idiom either way.
    FilePtr body = split ? FilePtr(new PCSX::PosixFile(vbFile, PCSX::FileOps::TRUNCATE))
                         : FilePtr(new PCSX::BufferFile(PCSX::FileOps::READWRITE));
    if (body->failed()) return false;
    for (auto& s : samples) body->write(s.adpcmData.data(), s.adpcmData.size());

    FilePtr out(new PCSX::PosixFile(vhFile, PCSX::FileOps::TRUNCATE));
    if (out->failed()) return false;

    // Compute sizes
    uint16_t totalTones = 0;
    for (int i = 0; i < numPrograms; i++) {
        totalTones += progAtrs[i].get<ProgTones>().value;
    }
    uint32_t headerOnlySize =
        kVabHdrBytes + 128 * kProgAtrBytes + (uint32_t)numPrograms * 16 * kVagAtrBytes + 256 * sizeof(uint16_t);
    uint32_t vagBodySize = 0;
    for (auto& s : samples) vagBodySize += s.adpcmSize;

    // Header (fileSize is the total logical size, identical for combined and split layouts)
    VabHdr hdr;
    hdr.reset();
    hdr.get<VabMagic>().set("pBAV");
    hdr.get<VabVersion>().value = 7;  // standard VAB version
    hdr.get<VabFileSize>().value = headerOnlySize + vagBodySize;
    hdr.get<VabNumPrograms>().value = (uint16_t)numPrograms;
    hdr.get<VabNumTones>().value = totalTones;
    hdr.get<VabNumVags>().value = (uint16_t)samples.size();
    hdr.get<VabMasterVol>().value = 127;
    hdr.get<VabMasterPan>().value = 64;
    hdr.serialize(out);

    // ProgAtr table (always 128 entries)
    for (int i = 0; i < 128; i++) progAtrs[i].serialize(out);

    // VagAtr table (numPrograms * 16 entries)
    for (int i = 0; i < numPrograms * 16; i++) vagAtrs[i].serialize(out);

    // VAG offset table (256 entries, each is size >> 3)
    for (int i = 0; i < 256; i++) {
        uint16_t entry = 0;
        if (i < (int)samples.size()) {
            entry = (uint16_t)(samples[i].adpcmSize >> 3);
        }
        out->write<uint16_t>(entry);
    }

    // Combined mode: the sample body trails the header. Split mode: it already lives in vbFile.
    if (!split) out->write(std::move(body.asA<PCSX::BufferFile>()->borrow()));

    return true;
}

bool ConvertContext::writePsm(const char* filename) {
    FilePtr f(new PCSX::PosixFile(filename, PCSX::FileOps::TRUNCATE));
    if (f->failed()) return false;

    // Find the initial tick rate from the first TEMPO_CHANGE event
    uint32_t initialTickRate = 0;
    for (auto& ev : psmEvents) {
        if (ev.get<PsmType>().value == PSM_TEMPO_CHANGE) {
            initialTickRate = ev.get<PsmData>().value;
            break;
        }
    }

    // Write PSM header (16 bytes)
    PsmHeader hdr;
    hdr.reset();
    hdr.get<PsmMagic>().set("PSM");  // "PSM\0"
    hdr.get<PsmVersion>().value = 1;
    hdr.get<PsmTickRate>().value = initialTickRate;
    hdr.get<PsmEventCount>().value = (uint32_t)psmEvents.size();
    hdr.serialize(f);

    // Write events (8 bytes each)
    for (auto& ev : psmEvents) ev.serialize(f);

    f->close();
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> soundfontFlag(parser, "soundfont", "", {"s"});
    args::ValueFlag<std::string> bankOutputFlag(parser, "bank", "", {"b"});
    args::ValueFlag<std::string> sampleOutputFlag(parser, "samples", "", {"i"});
    args::ValueFlag<unsigned> maxLayersFlag(parser, "maxlayers", "", {"l"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
    const auto output = PCSX::ToolArgs::get(outputFlag);
    const auto soundfont = PCSX::ToolArgs::get(soundfontFlag);
    const auto bankOutput = PCSX::ToolArgs::get(bankOutputFlag);
    const auto sampleOutput = PCSX::ToolArgs::get(sampleOutputFlag);
    const auto maxLayersOpt = PCSX::ToolArgs::get(maxLayersFlag);

    fmt::print(R"(
midi2psm - MIDI to PSM+VAB converter
Part of PCSX-Redux - https://github.com/pcsx-redux/pcsx-redux

)");

    const auto inputs = args::get(inputsList);
    const bool asksForHelp = args::get(helpFlag);
    const bool hasOutput = output.has_value();
    const bool hasSoundfont = soundfont.has_value();
    const bool hasBankOutput = bankOutput.has_value();
    const bool oneInput = inputs.size() == 1;

    if (asksForHelp || !oneInput || !hasOutput || !hasSoundfont || !hasBankOutput) {
        fmt::print(R"(
Usage: {} input.mid -s soundfont.sf2 -o output.psm -b output.vab [-i output.vb] [-l maxlayers] [-h]
  input.mid          mandatory: the input MIDI file.
  -s soundfont.sf2   mandatory: the SoundFont instrument bank.
  -o output.psm      mandatory: the output PSM event stream file.
  -b output.vab      mandatory: the output VAB instrument bank file (VH when -i is used).
  -i output.vb       optional: separate sample body file. When specified,
                     -b writes only the VH (header, programs, tones, offset table)
                     and -i writes the VB (raw sample data). The VB buffer can be
                     freed after uploading to SPU RAM via PSM_LoadBankEx().
  -l maxlayers       optional: max overlapping SF2 regions per note (default 0 = unlimited).
                     Setting to 1 disables layered instruments. Useful when voice
                     count is limited and layers cause excessive voice stealing.
  -h                 display this help and exit.
)",
                   argv[0]);
        return asksForHelp ? 0 : -1;
    }

    // Read input MIDI file
    FilePtr midiFile(new PCSX::PosixFile(std::string(inputs[0]).c_str()));
    if (midiFile->failed()) {
        fmt::print(stderr, "Error: unable to open MIDI file: {}\n", inputs[0]);
        return -1;
    }

    // Read SoundFont file
    FilePtr sf2File(new PCSX::PosixFile(soundfont.value().c_str()));
    if (sf2File->failed()) {
        fmt::print(stderr, "Error: unable to open SoundFont: {}\n", soundfont.value());
        return -1;
    }
    sf2File->rSeek(0, SEEK_END);
    size_t sf2Size = sf2File->rSeek(0, SEEK_CUR);
    sf2File->rSeek(0, SEEK_SET);
    auto sf2Slice = sf2File->read(sf2Size);
    const uint8_t* sf2Data = (const uint8_t*)sf2Slice.data();

    // Parse MIDI
    ConvertContext ctx;
    if (!ctx.midi.parse(midiFile)) {
        fmt::print(stderr, "Error: failed to parse MIDI file\n");
        return -1;
    }
    fmt::print("MIDI: format {}, {} tracks, {} tpqn, {} events\n", ctx.midi.format, ctx.midi.trackCount, ctx.midi.tpqn,
               ctx.midi.events.size());

    // Load SoundFont
    ctx.sf2 = tsf_load_memory(sf2Data, (int)sf2Size);
    if (!ctx.sf2) {
        fmt::print(stderr, "Error: failed to load SoundFont\n");
        return -1;
    }
    fmt::print("SoundFont: {} presets\n", tsf_get_presetcount(ctx.sf2));

    ctx.maxLayers = maxLayersOpt.value_or(0);

    // Generate PSM events and build VAB
    fmt::print("Converting...\n");
    ctx.generate();

    // Compute total tone count
    uint16_t totalTones = 0;
    for (int i = 0; i < ctx.numPrograms; i++) {
        totalTones += ctx.progAtrs[i].get<ProgTones>().value;
    }

    fmt::print("Programs: {}\n", ctx.numPrograms);
    fmt::print("Tones: {}\n", totalTones);
    fmt::print("Samples: {}, SPU RAM used: {} / {} bytes ({:.1f}%)\n", ctx.samples.size(), ctx.nextSpuAddr,
               SPU_RAM_SIZE, ctx.nextSpuAddr * 100.0 / SPU_RAM_SIZE);
    fmt::print("PSM events: {}\n", ctx.psmEvents.size());
    fmt::print("Peak polyphony: {}\n", ctx.peakPolyphony);
    if (ctx.loopPointTick >= 0) {
        fmt::print("Loop point: tick {}\n", ctx.loopPointTick);
    }

    // Write VAB file (combined or split)
    if (sampleOutput.has_value()) {
        // Split mode: VH (header only) + VB (sample body, disposable)
        if (!ctx.writeVab(bankOutput.value().c_str(), sampleOutput.value().c_str())) {
            fmt::print(stderr, "Error: failed to write split VAB files\n");
            tsf_close(ctx.sf2);
            return -1;
        }
        fmt::print("Wrote: {} (VH instrument header) + {} (VB sample body)\n",
                   bankOutput.value(), sampleOutput.value());
    } else {
        // Combined mode: single VAB file
        if (!ctx.writeVab(bankOutput.value().c_str(), nullptr)) {
            fmt::print(stderr, "Error: failed to write VAB file\n");
            tsf_close(ctx.sf2);
            return -1;
        }
        fmt::print("Wrote: {} (VAB instrument bank)\n", bankOutput.value());
    }

    // Write PSM file
    if (!ctx.writePsm(output.value().c_str())) {
        fmt::print(stderr, "Error: failed to write PSM file\n");
        tsf_close(ctx.sf2);
        return -1;
    }
    fmt::print("Wrote: {} (PSM event stream)\n", output.value());

    tsf_close(ctx.sf2);
    return 0;
}
