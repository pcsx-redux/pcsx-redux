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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "args/args.hxx"
#include "fmt/format.h"
#include "support/file.h"
#include "support/tool-args.h"
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
// Voice Allocator
// ============================================================================

struct VoiceSlot {
    bool active;
    bool sustainHeld;  // note-off received but sustain pedal is down
    uint8_t midiChannel;
    uint8_t midiNote;
    uint8_t velocity;
    uint32_t startTick;
    int exclusiveGroup;  // SF2 exclusive group (0 = none)
    // Per-voice pitch state for pitch bend
    int baseMidiNote;      // the MIDI note being played
    uint32_t sampleRate;   // sample rate of the assigned sample
    int rootKey;           // root key of the assigned sample
    int transpose;         // SF2 transpose
    int tuneCents;         // SF2 fine tune
    float sf2Pan;          // SF2 region pan (-0.5 to +0.5)
    float sf2Attenuation;  // SF2 initial attenuation (centibels)
};

struct VoiceAllocator {
    VoiceSlot voices[24];
    unsigned maxVoices;
    unsigned peakVoices;
    unsigned voiceSteals;

    void init(unsigned max) {
        maxVoices = max;
        peakVoices = 0;
        voiceSteals = 0;
        memset(voices, 0, sizeof(voices));
    }

    int allocate(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t tick) {
        // Find a free voice
        for (unsigned i = 0; i < maxVoices; i++) {
            if (!voices[i].active) {
                voices[i] = {};
                voices[i].active = true;
                voices[i].midiChannel = channel;
                voices[i].midiNote = note;
                voices[i].velocity = velocity;
                voices[i].startTick = tick;
                unsigned used = 0;
                for (unsigned j = 0; j < maxVoices; j++) {
                    if (voices[j].active) used++;
                }
                if (used > peakVoices) peakVoices = used;
                return (int)i;
            }
        }
        // Voice stealing priority:
        // 1. Prefer sustain-held voices (already logically released)
        // 2. Prefer lowest velocity (least audible)
        // 3. Fall back to oldest note
        int bestVoice = -1;
        int bestScore = -1;  // higher = better candidate for stealing
        for (unsigned i = 0; i < maxVoices; i++) {
            int score = 0;
            if (voices[i].sustainHeld) score += 10000;    // strongly prefer sustain-held
            score += (127 - voices[i].velocity) * 10;     // prefer low velocity
            score += (tick - voices[i].startTick) / 100;  // slight preference for older
            if (score > bestScore) {
                bestScore = score;
                bestVoice = (int)i;
            }
        }
        if (bestVoice < 0) bestVoice = 0;
        voiceSteals++;
        voices[bestVoice] = {};
        voices[bestVoice].active = true;
        voices[bestVoice].midiChannel = channel;
        voices[bestVoice].midiNote = note;
        voices[bestVoice].velocity = velocity;
        voices[bestVoice].startTick = tick;
        return bestVoice;
    }

    // Kill all active voices in a given exclusive group, returning bitmask of killed voices
    uint32_t killExclusiveGroup(int group) {
        if (group == 0) return 0;
        uint32_t mask = 0;
        for (unsigned i = 0; i < maxVoices; i++) {
            if (voices[i].active && voices[i].exclusiveGroup == group) {
                voices[i].active = false;
                voices[i].sustainHeld = false;
                mask |= (1 << i);
            }
        }
        return mask;
    }

    int find(uint8_t channel, uint8_t note) {
        for (unsigned i = 0; i < maxVoices; i++) {
            if (voices[i].active && voices[i].midiChannel == channel && voices[i].midiNote == note) {
                return (int)i;
            }
        }
        return -1;
    }

    void release(int voice) {
        if (voice >= 0 && voice < 24) {
            voices[voice].active = false;
            voices[voice].sustainHeld = false;
        }
    }

    // Mark a voice as sustain-held instead of releasing it
    void holdSustain(int voice) {
        if (voice >= 0 && voice < 24) {
            voices[voice].sustainHeld = true;
        }
    }

    // Release all sustain-held voices on a channel, returning bitmask
    uint32_t releaseSustainedVoices(uint8_t channel) {
        uint32_t mask = 0;
        for (unsigned i = 0; i < maxVoices; i++) {
            if (voices[i].active && voices[i].sustainHeld && voices[i].midiChannel == channel) {
                voices[i].active = false;
                voices[i].sustainHeld = false;
                mask |= (1 << i);
            }
        }
        return mask;
    }
};

// ============================================================================
// SPUDUMP Writer
// ============================================================================

// SPU register offsets from base 0x1F801C00
#define SPU_VOICE_OFF(v, r) ((v) * 0x10 + (r))
#define SPU_VOL_LEFT(v) SPU_VOICE_OFF(v, 0x00)
#define SPU_VOL_RIGHT(v) SPU_VOICE_OFF(v, 0x02)
#define SPU_PITCH(v) SPU_VOICE_OFF(v, 0x04)
#define SPU_SAMPLE_START(v) SPU_VOICE_OFF(v, 0x06)
#define SPU_ADSR_LO(v) SPU_VOICE_OFF(v, 0x08)
#define SPU_ADSR_HI(v) SPU_VOICE_OFF(v, 0x0A)
#define SPU_REPEAT_ADDR(v) SPU_VOICE_OFF(v, 0x0E)
#define SPU_KEY_ON_LO 0x188
#define SPU_KEY_ON_HI 0x18A
#define SPU_KEY_OFF_LO 0x18C
#define SPU_KEY_OFF_HI 0x18E
#define SPU_MAIN_VOL_L 0x180
#define SPU_MAIN_VOL_R 0x182
#define SPU_NOISE_LO 0x194
#define SPU_NOISE_HI 0x196
#define SPU_REVERB_LO 0x198
#define SPU_REVERB_HI 0x19A
#define SPU_REVERB_OUT_L 0x184
#define SPU_REVERB_OUT_R 0x186
#define SPU_REVERB_BASE 0x1A2  // mBASE: reverb work area start address (in 8-byte units)
#define SPU_SPUCNT 0x1AA       // SPU control register (bit 7 = reverb master enable)
#define SPU_REVERB_CFG 0x1C0   // Start of 32 reverb configuration registers (0x1C0-0x1FE)

// ============================================================================
// SPU Reverb Presets
// ============================================================================

// Each preset has 32 configuration register values and a buffer size requirement.
// The reverb buffer occupies the top of SPU RAM: mBASE*8 through 0x7FFFE.
struct ReverbPreset {
    const char* name;
    uint32_t bufferSize;  // bytes needed at top of SPU RAM
    uint16_t regs[32];    // dAPF1 through vRIN
    uint16_t outVolL;     // reverb output volume left
    uint16_t outVolR;     // reverb output volume right
};

// Presets from PS1 BIOS / psx-spx documentation
static const ReverbPreset REVERB_PRESETS[] = {
    {"off",
     0x10,
     {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
      0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001,
      0x0001, 0x0001, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0000, 0x0000},
     0x0000,
     0x0000},
    {"room",
     0x1F40,
     {// Studio Small
      0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4,
      0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135,
      0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
     0x3800,
     0x3800},
    {"studio",
     0x4840,
     {// Studio Medium
      0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904,
      0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305,
      0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
     0x3800,
     0x3800},
    {"hall",
     0xADE0,
     {// Hall
      0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA,
      0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1,
      0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
     0x3000,
     0x3000},
    {"space",
     0xF6C0,
     {// Space Echo
      0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6,
      0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D,
      0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
     0x2800,
     0x2800},
};
static constexpr int REVERB_PRESET_COUNT = sizeof(REVERB_PRESETS) / sizeof(REVERB_PRESETS[0]);
static constexpr int REVERB_DEFAULT_PRESET = 3;  // hall

// SPUDUMP packet types
enum SpdPacket : uint8_t {
    SPD_PKT_REG_WRITE = 0x00,
    SPD_PKT_WAIT = 0x01,
    SPD_PKT_END_PATTERN = 0x02,
    SPD_PKT_LOOP_POINT = 0x03,
    SPD_PKT_ORDER_TABLE = 0x10,
    SPD_PKT_PATTERN_HDR = 0x11,
    SPD_PKT_MACRO_DEF = 0x13,
    SPD_PKT_SAMPLE_DIR = 0x20,
    SPD_PKT_SAMPLE_DATA = 0x21,
    SPD_PKT_TICK_RATE = 0x30,
    SPD_PKT_TITLE = 0x40,
    SPD_PKT_AUTHOR = 0x41,
    SPD_PKT_COMMENT = 0x43,
    SPD_PKT_VOICE_COUNT = 0x45,
};

struct RegWrite {
    uint16_t offset;
    uint16_t value;
};

struct StreamEvent {
    enum Type { REG_WRITES, WAIT, END_PATTERN, LOOP_POINT, TICK_RATE };
    Type type;
    std::vector<RegWrite> writes;  // includes both real reg writes and macro invokes
    uint32_t waitTicks;
    uint32_t tickRate;
};

// Virtual register addresses within register write packets.
// The player checks the offset and dispatches accordingly.

// Macro invocation: offset = 0xF000 | macroIndex, value = voice number.
#define MACRO_INVOKE_BASE 0xF000

// Inline wait: offset = 0xEFFF, value = tick count.
// Collapses a wait into the preceding register write packet.
#define INLINE_WAIT_ADDR 0xEFFF

// A macro captures voice-0-relative register writes that are constant
// for a given instrument/zone combination (ADSR + sample start).
struct MacroDef {
    std::vector<RegWrite> writes;  // offsets are voice-0-relative
};

// Key for deduplicating macros: the set of register values they write
struct MacroKey {
    uint16_t adsrLo;
    uint16_t adsrHi;
    uint16_t sampleStart;  // in 8-byte units
    bool operator<(const MacroKey& o) const {
        if (adsrLo != o.adsrLo) return adsrLo < o.adsrLo;
        if (adsrHi != o.adsrHi) return adsrHi < o.adsrHi;
        return sampleStart < o.sampleStart;
    }
};

using FilePtr = PCSX::IO<PCSX::File>;

static void writeMagic(FilePtr& f);
static void writeSampleDir(FilePtr& f, const std::vector<SpuSample>& samples);
static void writeSamplePackets(FilePtr& f, const std::vector<SpuSample>& samples);
static void writeMacroDefs(FilePtr& f, const std::vector<MacroDef>& macroDefs);
static void writeStreamPackets(FilePtr& f, const std::vector<StreamEvent>& stream);

static void writePacket(FilePtr& f, uint8_t type, const uint32_t* payload, uint32_t words) {
    uint32_t header = (words & 0x00FFFFFF) | ((uint32_t)type << 24);
    f->write<uint32_t>(header);
    for (uint32_t i = 0; i < words; i++) {
        f->write<uint32_t>(payload[i]);
    }
}

static void writePacketEmpty(FilePtr& f, uint8_t type) {
    uint32_t header = (uint32_t)type << 24;
    f->write<uint32_t>(header);
}

static void writePacketString(FilePtr& f, uint8_t type, const char* str) {
    size_t len = strlen(str) + 1;  // include the NUL terminator
    size_t padded = (len + 3) & ~3;
    uint32_t words = (uint32_t)(padded / 4);
    uint32_t header = (words & 0x00FFFFFF) | ((uint32_t)type << 24);
    f->write<uint32_t>(header);
    f->write(str, len);
    for (size_t i = len; i < padded; i++) f->write<uint8_t>(0);
}

// GM-standard velocity curve: attempt to approximate the roughly quadratic
// relationship between velocity and perceived loudness. The MIDI spec doesn't
// mandate a specific curve, but most GM implementations use something close to
// velocity squared. Returns a value in the range [0, 127].
static int velocityCurve(int velocity) {
    // Quadratic: out = vel^2 / 127
    return (velocity * velocity + 63) / 127;  // +63 for rounding
}

// Combine MIDI pan (0-127, CC#10) with SF2 region pan (-0.5 to +0.5).
// Returns effective pan in 0-127 range.
static int combinePan(int midiPan, float sf2Pan) {
    // SF2 pan: -0.5 = hard left, 0 = center, +0.5 = hard right
    // Convert to 0-127 range: center = 64
    int sf2PanMidi = (int)(64.0f + sf2Pan * 128.0f);
    if (sf2PanMidi < 0) sf2PanMidi = 0;
    if (sf2PanMidi > 127) sf2PanMidi = 127;
    // Combine: both offsets from center, sum the deviations
    int combined = 64 + (midiPan - 64) + (sf2PanMidi - 64);
    if (combined < 0) combined = 0;
    if (combined > 127) combined = 127;
    return combined;
}

// Compute left and right volume from velocity, channel state, and optional SF2 region pan.
// sf2Pan is the region's pan value (-0.5 to +0.5), sf2Attenuation is in centibels (0 = none).
static void computeVolumes(int velocity, uint8_t chanVolume, uint8_t chanExpression, int pan, float sf2Pan,
                           float sf2Attenuation, uint16_t& volL, uint16_t& volR) {
    // Apply GM velocity curve
    int vel = velocityCurve(velocity);

    // Apply SF2 initial attenuation (centibels, 0 = full volume, higher = quieter)
    // Convert to a gain multiplier: gain = 10^(-attenuation_cB / 200)
    float attnGain = 1.0f;
    if (sf2Attenuation > 0.0f) {
        attnGain = powf(10.0f, -sf2Attenuation / 200.0f);
        if (attnGain < 0.0f) attnGain = 0.0f;
    }

    int vol = (int)(vel * chanVolume * chanExpression * attnGain) / (127 * 127);

    // Combine MIDI pan with SF2 region pan
    int effectivePan = combinePan(pan, sf2Pan);
    int panL = (effectivePan <= 64) ? 127 : (127 - effectivePan) * 2;
    int panR = (effectivePan >= 64) ? 127 : effectivePan * 2;
    if (panL > 127) panL = 127;
    if (panR > 127) panR = 127;

    volL = (uint16_t)((vol * panL * 0x3FFF) / (127 * 127));
    volR = (uint16_t)((vol * panR * 0x3FFF) / (127 * 127));
}

// Compute SPU pitch register value.
// pitchBend: -8192 to 8191, bendRange: in cents (e.g., 200 = 2 semitones)
// modCents: additional pitch offset in cents from modulation/vibrato LFO
static uint16_t computeSpuPitch(int midiNote, uint32_t sampleRate, int rootKey, int transpose, int tuneCents,
                                int16_t pitchBend = 0, uint16_t bendRangeCents = 200, double modCents = 0.0) {
    // Effective note = midiNote + transpose + tune/100
    double effectiveNote = midiNote + transpose + tuneCents / 100.0;
    // Apply pitch bend: bend range is in cents, bend value is -8192..8191 mapping to -range..+range
    if (pitchBend != 0 && bendRangeCents > 0) {
        double bendSemitones = (pitchBend / 8192.0) * (bendRangeCents / 100.0);
        effectiveNote += bendSemitones;
    }
    // Apply modulation/vibrato offset (in cents -> semitones)
    if (modCents != 0.0) {
        effectiveNote += modCents / 100.0;
    }
    double noteFreq = 440.0 * pow(2.0, (effectiveNote - 69) / 12.0);
    double rootFreq = midiNoteToFreq(rootKey);
    // SPU pitch register: 0x1000 = 44100 Hz playback rate.
    double pitch = (noteFreq / rootFreq) * ((double)sampleRate / 44100.0) * 0x1000;
    if (pitch < 0) pitch = 0;
    if (pitch > 0x3FFF) pitch = 0x3FFF;
    return (uint16_t)pitch;
}

// ============================================================================
// Conversion Pipeline
// ============================================================================

struct ChannelState {
    int program;
    uint8_t volume;
    uint8_t pan;
    uint8_t expression;
    int16_t pitchBend;        // -8192 to 8191
    bool sustainOn;           // CC#64 state
    uint8_t bankMSB;          // CC#0
    uint8_t bankLSB;          // CC#32
    uint8_t modulation;       // CC#1 modulation wheel (0-127)
    uint8_t reverbSend;       // CC#91 reverb send (0-127, default 40 per GM)
    uint16_t pitchBendRange;  // in semitones * 100 + cents (default 200 = 2 semitones)
    // RPN state machine
    uint8_t rpnMSB;
    uint8_t rpnLSB;

    ChannelState()
        : program(0),
          volume(100),
          pan(64),
          expression(127),
          pitchBend(0),
          sustainOn(false),
          bankMSB(0),
          bankLSB(0),
          modulation(0),
          reverbSend(40),
          pitchBendRange(200),
          rpnMSB(0x7F),
          rpnLSB(0x7F) {}

    void reset() {
        volume = 100;
        pan = 64;
        expression = 127;
        pitchBend = 0;
        sustainOn = false;
        modulation = 0;
        reverbSend = 40;
        rpnMSB = 0x7F;
        rpnLSB = 0x7F;
        // Note: program and bank are NOT reset by CC#121
    }
};

struct ConvertContext {
    MidiFile midi;
    tsf* sf2;
    std::vector<SpuSample> samples;
    std::map<SampleKey, size_t> sampleMap;  // key -> index in samples
    VoiceAllocator allocator;
    ChannelState channels[16];
    std::vector<StreamEvent> stream;
    unsigned maxVoices;
    unsigned maxLayers;  // max overlapping regions per note (0 = unlimited)

    // SPU RAM tracking
    uint32_t nextSpuAddr;  // next free SPU RAM address in bytes
    uint32_t maxSpuAddr;   // max usable address (SPU_RAM_SIZE minus reverb buffer)

    // Macro system
    std::vector<MacroDef> macroDefs;
    std::map<MacroKey, uint16_t> macroMap;  // key -> macro index

    uint16_t getOrCreateMacro(uint16_t adsrLo, uint16_t adsrHi, uint16_t sampleStart8) {
        MacroKey key = {adsrLo, adsrHi, sampleStart8};
        auto it = macroMap.find(key);
        if (it != macroMap.end()) return it->second;

        uint16_t idx = (uint16_t)macroDefs.size();
        MacroDef macro;
        // Voice-0-relative offsets
        macro.writes.push_back({(uint16_t)SPU_SAMPLE_START(0), sampleStart8});
        macro.writes.push_back({(uint16_t)SPU_ADSR_LO(0), adsrLo});
        macro.writes.push_back({(uint16_t)SPU_ADSR_HI(0), adsrHi});
        macroDefs.push_back(std::move(macro));
        macroMap[key] = idx;
        return idx;
    }

    // Register cache: track last value written to each SPU register offset.
    // Only emit a write if the value has actually changed.
    // SPU register space is 0x200 bytes = 0x100 16-bit registers.
    uint16_t regCache[0x100];
    bool regCacheValid[0x100];

    void initRegCache() { memset(regCacheValid, 0, sizeof(regCacheValid)); }

    // Add a register write to the event only if the value changed.
    // KEY_ON and KEY_OFF are never cached (they're edge-triggered).
    void cachedWrite(StreamEvent& ev, uint16_t offset, uint16_t value) {
        // Never cache key-on/key-off - they're triggers, not state
        if (offset == SPU_KEY_ON_LO || offset == SPU_KEY_ON_HI || offset == SPU_KEY_OFF_LO ||
            offset == SPU_KEY_OFF_HI) {
            ev.writes.push_back({offset, value});
            return;
        }
        uint16_t idx = offset / 2;
        if (idx < 0x100 && regCacheValid[idx] && regCache[idx] == value) return;
        if (idx < 0x100) {
            regCache[idx] = value;
            regCacheValid[idx] = true;
        }
        ev.writes.push_back({offset, value});
    }

    // Reverb
    int reverbPreset;  // index into REVERB_PRESETS, 0 = off

    // Loop point detection
    int32_t loopPointTick;  // -1 = no loop point detected, use start

    // Metadata extracted from MIDI
    std::string trackName;
    std::string copyright;

    void generateStream();
    bool writeSpd(const char* filename, const char* title);
    bool writeSplit(const char* musicFile, const char* sampleFile, const char* title);

  private:
    // Shared SPUDUMP sections, used by both the combined and split writers. The head is everything
    // up to (but not including) the sample section; the tail is everything after it. Only the sample
    // section differs between the two layouts.
    void writeSpdHead(FilePtr& f, const char* title);
    void writeSpdTail(FilePtr& f);
};

void ConvertContext::generateStream() {
    allocator.init(maxVoices);
    initRegCache();
    loopPointTick = -1;

    // Initialize all channel states
    for (int i = 0; i < 16; i++) {
        channels[i] = ChannelState();
    }
    // Channel 10 is drums (0-indexed as channel 9). Its program selects the GM drum kit
    // (preset number under SF2 bank 128), defaulting to 0 (Standard) and updated by program
    // changes like any other channel.

    // Pre-scan for loop markers (CC#111 and text markers)
    for (auto& mev : midi.events) {
        if (mev.type == MIDI_CONTROL_CHANGE && mev.data1 == MIDI_CC_LOOP_POINT) {
            loopPointTick = (int32_t)mev.absoluteTick;
            break;
        }
        if (mev.type == MIDI_META && mev.data1 == MIDI_META_MARKER) {
            // Check for common loop marker text
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

    uint32_t lastTick = 0;
    bool loopPointEmitted = false;

    // Default MIDI tempo is 120 BPM = 500000 us/beat
    uint32_t currentTempo = 500000;

    // Vibrato LFO state. The LFO runs at ~6 Hz (standard GM vibrato rate).
    // Phase is tracked as a global tick counter; the actual sine lookup converts
    // this to a phase based on the current tick rate (ticks/sec).
    // Modulation depth: CC#1 value 127 = +/- 50 cents (quarter semitone) at full depth.
    // This is a typical GM default; some implementations use more or less.
    static constexpr double VIBRATO_RATE_HZ = 6.0;
    static constexpr double VIBRATO_MAX_CENTS = 50.0;  // max depth at CC#1 = 127
    uint32_t globalTick = 0;                           // absolute tick counter for LFO phase
    // Real-time accumulator for the vibrato LFO phase. Converting an absolute tick position to
    // seconds with only the current tempo would make the phase jump at every tempo change, so we
    // bank the seconds elapsed up to the last tempo change and add the current segment on top.
    double accumulatedSeconds = 0.0;
    uint32_t lastTempoTick = 0;

    auto computeTickRate = [&]() -> uint32_t {
        double ticksPerSec = (double)midi.tpqn * 1000000.0 / (double)currentTempo;
        return (uint32_t)(ticksPerSec * 65536.0);  // 16.16 fixed point
    };

    // Emit initial tick rate
    {
        StreamEvent ev;
        ev.type = StreamEvent::TICK_RATE;
        ev.tickRate = computeTickRate();
        stream.push_back(ev);
    }

    // Emit reverb setup registers if a reverb preset is active
    uint32_t reverbOnMask = 0;  // per-voice reverb enable bitmask
    if (reverbPreset > 0 && reverbPreset < REVERB_PRESET_COUNT) {
        auto& preset = REVERB_PRESETS[reverbPreset];

        StreamEvent ev;
        ev.type = StreamEvent::REG_WRITES;

        // Set reverb work area base address (top of SPU RAM minus buffer size)
        uint16_t mBase = (uint16_t)((SPU_RAM_SIZE - preset.bufferSize) >> 3);
        ev.writes.push_back({(uint16_t)SPU_REVERB_BASE, mBase});

        // Write all 32 reverb configuration registers
        for (int i = 0; i < 32; i++) {
            ev.writes.push_back({(uint16_t)(SPU_REVERB_CFG + i * 2), preset.regs[i]});
        }

        // Set reverb output volumes
        ev.writes.push_back({(uint16_t)SPU_REVERB_OUT_L, preset.outVolL});
        ev.writes.push_back({(uint16_t)SPU_REVERB_OUT_R, preset.outVolR});

        // Enable reverb master via SPUCNT. This is a full register write, so we
        // must set all critical bits: bit 15 = SPU enable, bit 14 = unmute,
        // bit 7 = reverb master enable. Writing just 0x80 would clear SPU enable
        // and unmute, silencing everything.
        ev.writes.push_back({(uint16_t)SPU_SPUCNT, 0xC080});

        // Set main volume (ensure it's set)
        ev.writes.push_back({(uint16_t)SPU_MAIN_VOL_L, 0x3FFF});
        ev.writes.push_back({(uint16_t)SPU_MAIN_VOL_R, 0x3FFF});

        stream.push_back(ev);
    }

    // If no loop marker found, emit loop point at start
    if (loopPointTick < 0) {
        StreamEvent ev;
        ev.type = StreamEvent::LOOP_POINT;
        stream.push_back(ev);
        loopPointEmitted = true;
    }

    // Tick-level accumulation
    StreamEvent tickRegs;
    tickRegs.type = StreamEvent::REG_WRITES;
    uint32_t tickKeyOn = 0;
    uint32_t tickKeyOff = 0;

    auto flushTick = [&](uint32_t waitTicks) {
        bool hasContent = tickKeyOff || !tickRegs.writes.empty() || tickKeyOn || waitTicks;
        if (!hasContent) return;

        StreamEvent ev;
        ev.type = StreamEvent::REG_WRITES;

        // Key-off first
        if (tickKeyOff) {
            if (tickKeyOff & 0xFFFF) ev.writes.push_back({SPU_KEY_OFF_LO, (uint16_t)(tickKeyOff & 0xFFFF)});
            if (tickKeyOff >> 16) ev.writes.push_back({SPU_KEY_OFF_HI, (uint16_t)(tickKeyOff >> 16)});
            tickKeyOff = 0;
        }

        // Register writes
        for (auto& w : tickRegs.writes) {
            ev.writes.push_back(w);
        }
        tickRegs.writes.clear();

        // Key-on after setup
        if (tickKeyOn) {
            if (tickKeyOn & 0xFFFF) ev.writes.push_back({SPU_KEY_ON_LO, (uint16_t)(tickKeyOn & 0xFFFF)});
            if (tickKeyOn >> 16) ev.writes.push_back({SPU_KEY_ON_HI, (uint16_t)(tickKeyOn >> 16)});
            tickKeyOn = 0;
        }

        // Inline wait at the end
        if (waitTicks > 0) {
            while (waitTicks > 0xFFFF) {
                ev.writes.push_back({INLINE_WAIT_ADDR, 0xFFFF});
                waitTicks -= 0xFFFF;
            }
            ev.writes.push_back({INLINE_WAIT_ADDR, (uint16_t)waitTicks});
        }

        if (!ev.writes.empty()) stream.push_back(ev);
    };

    // Helper: update volume registers for all active voices on a channel
    auto updateChannelVolumes = [&](uint8_t ch) {
        auto& chanState = channels[ch];
        for (unsigned v = 0; v < maxVoices; v++) {
            auto& slot = allocator.voices[v];
            if (slot.active && slot.midiChannel == ch) {
                uint16_t volL, volR;
                computeVolumes(slot.velocity, chanState.volume, chanState.expression, chanState.pan, slot.sf2Pan,
                               slot.sf2Attenuation, volL, volR);
                cachedWrite(tickRegs, (uint16_t)SPU_VOL_LEFT(v), volL);
                cachedWrite(tickRegs, (uint16_t)SPU_VOL_RIGHT(v), volR);
            }
        }
    };

    // Compute the vibrato LFO offset in cents for a given tick
    auto vibratoOffset = [&](uint8_t ch) -> double {
        auto& chanState = channels[ch];
        if (chanState.modulation == 0) return 0.0;
        // Elapsed real time at the current LFO position: seconds banked under prior tempos plus
        // the current segment converted at the current tempo. Continuous across tempo changes.
        double ticksPerSec = (double)midi.tpqn * 1000000.0 / (double)currentTempo;
        double timeSec = accumulatedSeconds + (double)(globalTick - lastTempoTick) / ticksPerSec;
        // LFO phase at VIBRATO_RATE_HZ
        double phase = timeSec * VIBRATO_RATE_HZ * 2.0 * std::numbers::pi_v<double>;
        double depth = (chanState.modulation / 127.0) * VIBRATO_MAX_CENTS;
        return sin(phase) * depth;
    };

    // Helper: update pitch registers for all active voices on a channel (for pitch bend + vibrato)
    auto updateChannelPitch = [&](uint8_t ch) {
        auto& chanState = channels[ch];
        double modCents = vibratoOffset(ch);
        for (unsigned v = 0; v < maxVoices; v++) {
            auto& slot = allocator.voices[v];
            if (slot.active && slot.midiChannel == ch) {
                uint16_t pitch =
                    computeSpuPitch(slot.baseMidiNote, slot.sampleRate, slot.rootKey, slot.transpose, slot.tuneCents,
                                    chanState.pitchBend, chanState.pitchBendRange, modCents);
                cachedWrite(tickRegs, (uint16_t)SPU_PITCH(v), pitch);
            }
        }
    };

    // Per-tick voice update: runs vibrato LFO on channels that have modulation active.
    // Only emits writes when the pitch value actually changes (register cache handles this).
    auto perTickUpdate = [&]() {
        for (int ch = 0; ch < 16; ch++) {
            if (channels[ch].modulation > 0) {
                updateChannelPitch((uint8_t)ch);
            }
        }
    };

    // Process all MIDI events
    for (size_t i = 0; i < midi.events.size(); i++) {
        auto& mev = midi.events[i];

        // Flush and emit wait if time has advanced
        if (mev.absoluteTick > lastTick) {
            uint32_t waitTicks = mev.absoluteTick - lastTick;

            // If any channel has modulation active, we need to split long waits
            // into smaller segments to update the vibrato LFO periodically.
            // Update rate: ~100 Hz (every ~10ms) for smooth vibrato.
            bool hasModulation = false;
            for (int ch = 0; ch < 16; ch++) {
                if (channels[ch].modulation > 0) {
                    hasModulation = true;
                    break;
                }
            }

            if (hasModulation) {
                double ticksPerSec = (double)midi.tpqn * 1000000.0 / (double)currentTempo;
                uint32_t updateInterval = (uint32_t)(ticksPerSec / 100.0);  // ~100 Hz
                if (updateInterval < 1) updateInterval = 1;

                uint32_t remaining = waitTicks;
                while (remaining > 0) {
                    uint32_t step = (remaining > updateInterval) ? updateInterval : remaining;
                    globalTick = lastTick + (waitTicks - remaining) + step;
                    perTickUpdate();
                    flushTick(step);
                    remaining -= step;
                }
            } else {
                globalTick = mev.absoluteTick;
                flushTick(waitTicks);
            }
            lastTick = mev.absoluteTick;
        }

        // Check if we need to emit the loop point at this tick
        if (!loopPointEmitted && loopPointTick >= 0 && (int32_t)mev.absoluteTick >= loopPointTick) {
            StreamEvent ev;
            ev.type = StreamEvent::LOOP_POINT;
            stream.push_back(ev);
            loopPointEmitted = true;
        }

        if (mev.type == MIDI_META && mev.data1 == MIDI_META_TEMPO) {
            // Bank the seconds elapsed under the old tempo before switching, so the vibrato LFO
            // phase stays continuous (see accumulatedSeconds / lastTempoTick).
            double oldTicksPerSec = (double)midi.tpqn * 1000000.0 / (double)currentTempo;
            accumulatedSeconds += (double)(mev.absoluteTick - lastTempoTick) / oldTicksPerSec;
            lastTempoTick = mev.absoluteTick;
            currentTempo = mev.tempo;
            StreamEvent ev;
            ev.type = StreamEvent::TICK_RATE;
            ev.tickRate = computeTickRate();
            stream.push_back(ev);
            continue;
        }

        // Skip other meta events in the main loop (already pre-scanned)
        if (mev.type == MIDI_META) continue;

        if (mev.type == MIDI_PROGRAM_CHANGE) {
            channels[mev.channel].program = mev.data1;
            continue;
        }

        if (mev.type == MIDI_CONTROL_CHANGE) {
            auto& chanState = channels[mev.channel];
            switch (mev.data1) {
                case MIDI_CC_VOLUME:
                    chanState.volume = mev.data2;
                    updateChannelVolumes(mev.channel);
                    break;
                case MIDI_CC_PAN:
                    chanState.pan = mev.data2;
                    updateChannelVolumes(mev.channel);
                    break;
                case MIDI_CC_EXPRESSION:
                    chanState.expression = mev.data2;
                    updateChannelVolumes(mev.channel);
                    break;

                case MIDI_CC_MODULATION:
                    chanState.modulation = mev.data2;
                    // Reapply pitch now so the change lands immediately. In particular, when
                    // modulation returns to 0 the per-tick LFO stops running, so without this
                    // active voices would keep their last vibrato offset until the next event.
                    updateChannelPitch(mev.channel);
                    break;

                case MIDI_CC_REVERB_SEND:
                    chanState.reverbSend = mev.data2;
                    break;

                case MIDI_CC_BANK_MSB:
                    chanState.bankMSB = mev.data2;
                    break;
                case MIDI_CC_BANK_LSB:
                    chanState.bankLSB = mev.data2;
                    break;

                case MIDI_CC_SUSTAIN:
                    if (mev.data2 >= 64) {
                        // Sustain on
                        chanState.sustainOn = true;
                    } else {
                        // Sustain off: release all held voices on this channel
                        chanState.sustainOn = false;
                        uint32_t releasedMask = allocator.releaseSustainedVoices(mev.channel);
                        tickKeyOff |= releasedMask;
                    }
                    break;

                case MIDI_CC_RPN_MSB:
                    chanState.rpnMSB = mev.data2;
                    break;
                case MIDI_CC_RPN_LSB:
                    chanState.rpnLSB = mev.data2;
                    break;
                case MIDI_CC_DATA_ENTRY_MSB:
                    // RPN 0,0 = pitch bend range
                    if (chanState.rpnMSB == 0 && chanState.rpnLSB == 0) {
                        // MSB = semitones, keep existing LSB (cents)
                        chanState.pitchBendRange = (uint16_t)(mev.data2 * 100 + (chanState.pitchBendRange % 100));
                    }
                    break;
                case MIDI_CC_DATA_ENTRY_LSB:
                    if (chanState.rpnMSB == 0 && chanState.rpnLSB == 0) {
                        // LSB = cents
                        chanState.pitchBendRange = (uint16_t)((chanState.pitchBendRange / 100) * 100 + mev.data2);
                    }
                    break;

                case MIDI_CC_ALL_SOUND_OFF:
                case MIDI_CC_ALL_NOTES_OFF:
                    // Kill all active voices on this channel
                    for (unsigned v = 0; v < maxVoices; v++) {
                        if (allocator.voices[v].active && allocator.voices[v].midiChannel == mev.channel) {
                            tickKeyOff |= (1 << v);
                            allocator.release((int)v);
                        }
                    }
                    if (mev.data1 == MIDI_CC_ALL_SOUND_OFF) {
                        chanState.sustainOn = false;
                    }
                    break;

                case MIDI_CC_RESET_ALL:
                    chanState.reset();
                    updateChannelVolumes(mev.channel);
                    updateChannelPitch(mev.channel);
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
            channels[mev.channel].pitchBend = (int16_t)bend;
            // Update pitch of all active voices on this channel
            updateChannelPitch(mev.channel);
            continue;
        }

        if (mev.type == MIDI_NOTE_ON) {
            uint8_t note = mev.data1;
            uint8_t velocity = mev.data2;
            uint8_t ch = mev.channel;
            auto& chanState = channels[ch];
            bool isDrum = (ch == MIDI_DRUM_CHANNEL);

            // Find the SF2 preset and all matching regions for this note
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

            // Cap the number of layers per note to conserve voices
            if (maxLayers > 0 && regions.size() > maxLayers) {
                regions.resize(maxLayers);
            }

            // Handle exclusive groups: kill all voices in matching groups before allocating.
            // Check all regions since different layers may belong to different groups.
            for (auto* region : regions) {
                if (region->group != 0) {
                    uint32_t killedMask = allocator.killExclusiveGroup((int)region->group);
                    tickKeyOff |= killedMask;
                }
            }

            double modCents = vibratoOffset(ch);

            // Trigger a voice for each matching region (layered sounds)
            for (auto* region : regions) {
                size_t sampleIdx = extractAndEncode(sf2, region, samples, sampleMap, nextSpuAddr, maxSpuAddr, true);
                if (sampleIdx == (size_t)-1) continue;

                auto& sample = samples[sampleIdx];

                int voice = allocator.allocate(ch, note, velocity, mev.absoluteTick);

                auto& slot = allocator.voices[voice];
                slot.baseMidiNote = note;
                slot.sampleRate = sample.sampleRate;
                slot.rootKey = sample.rootKey;
                slot.transpose = sample.transpose;
                slot.tuneCents = sample.tune;
                slot.exclusiveGroup = (int)region->group;
                slot.sf2Pan = region->pan;
                slot.sf2Attenuation = region->attenuation;

                uint16_t volL, volR;
                computeVolumes(velocity, chanState.volume, chanState.expression, chanState.pan, region->pan,
                               region->attenuation, volL, volR);

                // Apply pitch keytrack: default is 100 (cents per key), meaning standard tuning.
                // Non-100 values scale the pitch deviation from the root key.
                int tuneCents = sample.tune;
                int transpose = sample.transpose;
                if (region->pitch_keytrack != 100) {
                    // Recompute effective transposition with keytrack scaling
                    // Standard: each semitone = 100 cents. With keytrack K:
                    // effective cents from root = (note - rootKey) * K
                    // We achieve this by adjusting the tune offset
                    double standardCents = (note - (int)sample.rootKey) * 100.0;
                    double trackedCents = (note - (int)sample.rootKey) * region->pitch_keytrack;
                    double correction = trackedCents - standardCents;
                    tuneCents += (int)correction;
                    // Update the voice slot too for pitch bend recalculation
                    slot.tuneCents = tuneCents;
                }

                uint16_t pitch = computeSpuPitch(note, sample.sampleRate, sample.rootKey, transpose, tuneCents,
                                                 chanState.pitchBend, chanState.pitchBendRange, modCents);

                uint16_t adsrLo, adsrHi;
                sf2RegionToSpuADSR(region, isDrum, adsrLo, adsrHi);

                uint16_t sampleStart8 = (uint16_t)(sample.spuAddr >> 3);
                uint16_t ssIdx = SPU_SAMPLE_START(voice) / 2;
                uint16_t alIdx = SPU_ADSR_LO(voice) / 2;
                uint16_t ahIdx = SPU_ADSR_HI(voice) / 2;
                bool needMacro = false;
                if (ssIdx < 0x100 && (!regCacheValid[ssIdx] || regCache[ssIdx] != sampleStart8)) needMacro = true;
                if (alIdx < 0x100 && (!regCacheValid[alIdx] || regCache[alIdx] != adsrLo)) needMacro = true;
                if (ahIdx < 0x100 && (!regCacheValid[ahIdx] || regCache[ahIdx] != adsrHi)) needMacro = true;

                if (needMacro) {
                    uint16_t macroIdx = getOrCreateMacro(adsrLo, adsrHi, sampleStart8);
                    tickRegs.writes.push_back({(uint16_t)(MACRO_INVOKE_BASE | macroIdx), (uint16_t)voice});
                    if (ssIdx < 0x100) {
                        regCache[ssIdx] = sampleStart8;
                        regCacheValid[ssIdx] = true;
                    }
                    if (alIdx < 0x100) {
                        regCache[alIdx] = adsrLo;
                        regCacheValid[alIdx] = true;
                    }
                    if (ahIdx < 0x100) {
                        regCache[ahIdx] = adsrHi;
                        regCacheValid[ahIdx] = true;
                    }
                }

                cachedWrite(tickRegs, (uint16_t)SPU_VOL_LEFT(voice), volL);
                cachedWrite(tickRegs, (uint16_t)SPU_VOL_RIGHT(voice), volR);
                cachedWrite(tickRegs, (uint16_t)SPU_PITCH(voice), pitch);

                tickKeyOn |= (1 << voice);

                // Update reverb enable mask for this voice based on channel CC#91
                if (reverbPreset > 0 && chanState.reverbSend > 0) {
                    reverbOnMask |= (1 << voice);
                } else {
                    reverbOnMask &= ~(1 << voice);
                }
            }

            // Emit updated reverb on/off bitmask if reverb is active
            if (reverbPreset > 0) {
                cachedWrite(tickRegs, (uint16_t)SPU_REVERB_LO, (uint16_t)(reverbOnMask & 0xFFFF));
                if (maxVoices > 16) {
                    cachedWrite(tickRegs, (uint16_t)SPU_REVERB_HI, (uint16_t)(reverbOnMask >> 16));
                }
            }
            continue;
        }

        if (mev.type == MIDI_NOTE_OFF) {
            // Release ALL voices matching this channel+note (there may be multiple from layers)
            for (unsigned v = 0; v < maxVoices; v++) {
                auto& slot = allocator.voices[v];
                if (slot.active && !slot.sustainHeld && slot.midiChannel == mev.channel && slot.midiNote == mev.data1) {
                    if (channels[mev.channel].sustainOn) {
                        allocator.holdSustain((int)v);
                    } else {
                        tickKeyOff |= (1 << v);
                        allocator.release((int)v);
                    }
                }
            }
            continue;
        }
    }

    // Flush any remaining events from the last tick
    flushTick(0);

    // If loop point was never emitted (loopPointTick past end of song), emit at end
    if (!loopPointEmitted) {
        StreamEvent ev;
        ev.type = StreamEvent::LOOP_POINT;
        stream.push_back(ev);
    }

    // End of pattern
    {
        StreamEvent ev;
        ev.type = StreamEvent::END_PATTERN;
        stream.push_back(ev);
    }
}

void ConvertContext::writeSpdHead(FilePtr& f, const char* title) {
    writeMagic(f);

    // Metadata
    if (title) writePacketString(f, SPD_PKT_TITLE, title);
    if (!copyright.empty()) writePacketString(f, SPD_PKT_AUTHOR, copyright.c_str());
    writePacketString(f, SPD_PKT_COMMENT, "Generated by midi2spd");

    // Voice count
    uint32_t vc = maxVoices;
    writePacket(f, SPD_PKT_VOICE_COUNT, &vc, 1);

    // Tick rate (initial - will also be in stream but good to have in header)
    double ticksPerSec = (double)midi.tpqn * 1000000.0 / 500000.0;
    uint32_t tickRate = (uint32_t)(ticksPerSec * 65536.0);
    writePacket(f, SPD_PKT_TICK_RATE, &tickRate, 1);
}

void ConvertContext::writeSpdTail(FilePtr& f) {
    // Macro definitions
    writeMacroDefs(f, macroDefs);

    // Order table: single order, looping
    uint32_t orderPayload[3] = {1, 0, 0};
    writePacket(f, SPD_PKT_ORDER_TABLE, orderPayload, 3);

    // Pattern header carries the absolute offset to the pattern data. Write a placeholder, note where
    // the data lands, then back-patch the offset word (4 bytes past the packet header) via writeAt,
    // which restores the write cursor for us so the stream picks up right where it left off.
    auto patternHdrPos = f->wTell();
    uint32_t offset = 0;
    writePacket(f, SPD_PKT_PATTERN_HDR, &offset, 1);
    auto patternDataStart = f->wTell();
    f->writeAt<uint32_t>((uint32_t)patternDataStart, patternHdrPos + 4);

    // Write pattern data from stream
    writeStreamPackets(f, stream);
}

bool ConvertContext::writeSpd(const char* filename, const char* title) {
    FilePtr f(new PCSX::PosixFile(filename, PCSX::FileOps::TRUNCATE));
    if (f->failed()) return false;

    writeSpdHead(f, title);
    // Combined layout: sample data + directory live inline between the header and the patterns.
    writeSamplePackets(f, samples);
    writeSpdTail(f);

    f->close();
    return true;
}

static void writeMagic(FilePtr& f) {
    static const uint8_t magic[16] = {'P', 'S', 'X', 'S', 'P', 'U', 'D', 'U', 'M', 'P', 'v', '1', 'r', '1', '\0', '\0'};
    f->write(magic, 16);
}

static void writeSampleDir(FilePtr& f, const std::vector<SpuSample>& samples) {
    uint32_t dirPayload = 1 + samples.size() * 2;
    f->write<uint32_t>((dirPayload & 0x00FFFFFF) | (SPD_PKT_SAMPLE_DIR << 24));
    f->write<uint32_t>((uint32_t)samples.size());
    for (auto& s : samples) {
        f->write<uint32_t>(((s.spuAddr >> 3) << 16) | (s.adpcmSize >> 3));
        f->write<uint32_t>((((s.spuAddr + s.loopStartByte) >> 3) << 16) | (s.hasLoop ? 1 : 0));
    }
}

// The SPU DMA moves whole 64-byte blocks, so the sample data payload is padded to a multiple of 64.
static uint32_t paddedSampleBytes(const std::vector<SpuSample>& samples) {
    uint32_t total = 0;
    for (auto& s : samples) total += s.adpcmSize;
    return (total + 63) & ~63u;
}

static void writeSamplePackets(FilePtr& f, const std::vector<SpuSample>& samples) {
    if (samples.empty()) return;

    uint32_t totalAdpcm = 0;
    for (auto& s : samples) totalAdpcm += s.adpcmSize;

    uint32_t paddedAdpcm = paddedSampleBytes(samples);
    uint32_t baseAddr8 = SPU_RAM_BASE >> 3;
    uint32_t totalWords = 1 + paddedAdpcm / 4;
    f->write<uint32_t>((totalWords & 0x00FFFFFF) | (SPD_PKT_SAMPLE_DATA << 24));
    f->write<uint32_t>(baseAddr8);
    for (auto& s : samples) {
        f->write(s.adpcmData.data(), s.adpcmData.size());
    }
    for (uint32_t i = totalAdpcm; i < paddedAdpcm; i++) f->write<uint8_t>(0);

    writeSampleDir(f, samples);
}

static void writeMacroDefs(FilePtr& f, const std::vector<MacroDef>& macroDefs) {
    for (size_t i = 0; i < macroDefs.size(); i++) {
        auto& macro = macroDefs[i];
        std::vector<uint32_t> payload;
        payload.push_back((uint32_t)i);
        for (auto& w : macro.writes) {
            payload.push_back(((uint32_t)w.offset << 16) | w.value);
        }
        writePacket(f, SPD_PKT_MACRO_DEF, payload.data(), (uint32_t)payload.size());
    }
}

static void writeStreamPackets(FilePtr& f, const std::vector<StreamEvent>& stream) {
    for (auto& ev : stream) {
        switch (ev.type) {
            case StreamEvent::REG_WRITES: {
                std::vector<uint32_t> payload;
                for (auto& w : ev.writes) {
                    payload.push_back(((uint32_t)w.offset << 16) | w.value);
                }
                writePacket(f, SPD_PKT_REG_WRITE, payload.data(), (uint32_t)payload.size());
                break;
            }
            case StreamEvent::WAIT: {
                uint32_t ticks = ev.waitTicks;
                writePacket(f, SPD_PKT_WAIT, &ticks, 1);
                break;
            }
            case StreamEvent::END_PATTERN:
                writePacketEmpty(f, SPD_PKT_END_PATTERN);
                break;
            case StreamEvent::LOOP_POINT:
                writePacketEmpty(f, SPD_PKT_LOOP_POINT);
                break;
            case StreamEvent::TICK_RATE: {
                uint32_t rate = ev.tickRate;
                writePacket(f, SPD_PKT_TICK_RATE, &rate, 1);
                break;
            }
        }
    }
}

bool ConvertContext::writeSplit(const char* musicFile, const char* sampleFile, const char* title) {
    // Write sample file: magic + sample data + sample directory
    {
        FilePtr f(new PCSX::PosixFile(sampleFile, PCSX::FileOps::TRUNCATE));
        if (f->failed()) return false;
        writeMagic(f);
        writeSamplePackets(f, samples);
        f->close();
    }

    // Write music file: header + sample directory (no sample data) + patterns
    {
        FilePtr f(new PCSX::PosixFile(musicFile, PCSX::FileOps::TRUNCATE));
        if (f->failed()) return false;

        writeSpdHead(f, title);
        // Split layout: only the sample directory stays in the music file (kept for the SFX API); the
        // sample data itself lives in the separate sample file written above.
        if (!samples.empty()) writeSampleDir(f, samples);
        writeSpdTail(f);

        f->close();
    }

    return true;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> soundfontFlag(parser, "soundfont", "", {"s"});
    args::ValueFlag<unsigned> maxVoicesFlag(parser, "maxvoices", "", {"v"});
    args::ValueFlag<std::string> sampleOutputFlag(parser, "samples", "", {"i"});
    args::ValueFlag<std::string> reverbFlag(parser, "reverb", "", {"r"});
    args::ValueFlag<unsigned> maxLayersFlag(parser, "maxlayers", "", {"l"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
    const auto output = PCSX::ToolArgs::get(outputFlag);
    const auto soundfont = PCSX::ToolArgs::get(soundfontFlag);
    const auto maxVoicesOpt = PCSX::ToolArgs::get(maxVoicesFlag);
    const auto sampleOutput = PCSX::ToolArgs::get(sampleOutputFlag);
    const auto reverbOpt = PCSX::ToolArgs::get(reverbFlag);
    const auto maxLayersOpt = PCSX::ToolArgs::get(maxLayersFlag);

    fmt::print(R"(
midi2spd - MIDI to SPUDUMP converter
Part of PCSX-Redux - https://github.com/pcsx-redux/pcsx-redux

)");

    const auto inputs = args::get(inputsList);
    const bool asksForHelp = args::get(helpFlag);
    const bool hasOutput = output.has_value();
    const bool hasSoundfont = soundfont.has_value();
    const bool oneInput = inputs.size() == 1;

    if (asksForHelp || !oneInput || !hasOutput || !hasSoundfont) {
        fmt::print(R"(
Usage: {} input.mid -s soundfont.sf2 -o output.spd [-i output.smp] [-v maxvoices] [-r reverb] [-h]
  input.mid          mandatory: the input MIDI file.
  -s soundfont.sf2   mandatory: the SoundFont instrument bank.
  -o output.spd      mandatory: the output SPUDUMP file.
  -i output.smp      optional: separate sample data file. When specified,
                     output.spd contains only music data and output.smp
                     contains only sample data. The sample file can be
                     freed after uploading to SPU RAM via SPD_LoadEx().
  -v maxvoices       optional: max SPU voices for music (default 16, max 24).
  -r reverb          optional: reverb preset (default: hall).
                     Available: off, room, studio, hall, space.
  -l maxlayers       optional: max overlapping SF2 regions per note (default 0 = unlimited).
                     Setting to 1 disables layered instruments. Useful when voice
                     count is limited and layers cause excessive voice stealing.
  -h                 display this help and exit.
)",
                   argv[0]);
        return asksForHelp ? 0 : -1;
    }

    unsigned maxVoices = maxVoicesOpt.value_or(16);
    if (maxVoices > 24) maxVoices = 24;

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

    ctx.maxVoices = maxVoices;
    ctx.maxLayers = maxLayersOpt.value_or(0);
    ctx.nextSpuAddr = 0x1010;
    ctx.loopPointTick = -1;

    // Select reverb preset
    ctx.reverbPreset = REVERB_DEFAULT_PRESET;  // hall by default
    if (reverbOpt.has_value()) {
        std::string rv = reverbOpt.value();
        ctx.reverbPreset = -1;
        for (int i = 0; i < REVERB_PRESET_COUNT; i++) {
            if (rv == REVERB_PRESETS[i].name) {
                ctx.reverbPreset = i;
                break;
            }
        }
        if (ctx.reverbPreset < 0) {
            fmt::print(stderr, "Error: unknown reverb preset '{}'. Available:", rv);
            for (int i = 0; i < REVERB_PRESET_COUNT; i++) fmt::print(stderr, " {}", REVERB_PRESETS[i].name);
            fmt::print(stderr, "\n");
            tsf_close(ctx.sf2);
            return -1;
        }
    }

    // Compute max SPU address: total RAM minus reverb buffer at top
    if (ctx.reverbPreset > 0 && ctx.reverbPreset < REVERB_PRESET_COUNT) {
        ctx.maxSpuAddr = SPU_RAM_SIZE - REVERB_PRESETS[ctx.reverbPreset].bufferSize;
        fmt::print("Reverb: {} (buffer: {} bytes, usable sample RAM: {} bytes)\n",
                   REVERB_PRESETS[ctx.reverbPreset].name, REVERB_PRESETS[ctx.reverbPreset].bufferSize,
                   ctx.maxSpuAddr - SPU_RAM_BASE);
    } else {
        ctx.maxSpuAddr = SPU_RAM_SIZE;
        fmt::print("Reverb: off\n");
    }

    // Generate the stream (this also builds the sample bank on-demand)
    fmt::print("Converting...\n");
    ctx.generateStream();

    fmt::print("Samples: {}, SPU RAM used: {} / {} bytes ({:.1f}%)\n", ctx.samples.size(), ctx.nextSpuAddr,
               ctx.maxSpuAddr, ctx.nextSpuAddr * 100.0 / ctx.maxSpuAddr);
    // The spdplayer reference player caps its sample directory at 128 entries (MAX_SAMPLES) and
    // silently truncates beyond that, so any tone referencing a later sample would go silent.
    if (ctx.samples.size() > 128) {
        fmt::print(stderr, "Warning: {} samples exceeds the player's 128-sample limit; samples past "
                           "128 will not load. Reduce layering (-l) or simplify the SoundFont.\n",
                   ctx.samples.size());
    }
    // The zero fill after the last sample is uploaded too, so it has to fit below the reverb area.
    const uint32_t paddedEnd = SPU_RAM_BASE + paddedSampleBytes(ctx.samples);
    if (ctx.nextSpuAddr <= ctx.maxSpuAddr && paddedEnd > ctx.maxSpuAddr) {
        fmt::print(stderr, "Warning: sample data padded to 64 bytes ends at {}, past the {} available\n", paddedEnd,
                   ctx.maxSpuAddr);
    }
    fmt::print("Macros: {}\n", ctx.macroDefs.size());
    fmt::print("Peak voices: {} / {}\n", ctx.allocator.peakVoices, maxVoices);
    fmt::print("Voice steals: {}\n", ctx.allocator.voiceSteals);
    fmt::print("Stream events: {}\n", ctx.stream.size());
    if (ctx.loopPointTick >= 0) {
        fmt::print("Loop point: tick {}\n", ctx.loopPointTick);
    }

    // Write output - use track name from MIDI if available
    std::string titleStr = ctx.trackName.empty() ? std::string(inputs[0]) : ctx.trackName;
    if (sampleOutput.has_value()) {
        // Split mode: separate sample and music files
        if (!ctx.writeSplit(output.value().c_str(), sampleOutput.value().c_str(), titleStr.c_str())) {
            fmt::print(stderr, "Error: failed to write output files\n");
            tsf_close(ctx.sf2);
            return -1;
        }
        fmt::print("Wrote: {} (music) + {} (samples)\n", output.value(), sampleOutput.value());
    } else {
        // Combined mode: everything in one file
        if (!ctx.writeSpd(output.value().c_str(), titleStr.c_str())) {
            fmt::print(stderr, "Error: failed to write output file\n");
            tsf_close(ctx.sf2);
            return -1;
        }
        fmt::print("Wrote: {}\n", output.value());
    }

    tsf_close(ctx.sf2);
    return 0;
}
