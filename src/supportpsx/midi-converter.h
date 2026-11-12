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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "support/binstruct.h"
#include "support/file.h"
#include "support/typestring-wrapper.h"
#include "supportpsx/adpcm.h"

// tsf.h decodes the Ogg Vorbis samples of SF3 banks when stb_vorbis is declared before it.
#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"

#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "tsf.h"

namespace PCSX::MidiConverter {

// MIDI status byte types (upper nibble of status byte)
enum MidiStatus : uint8_t {
    MIDI_NOTE_OFF = 0x80,
    MIDI_NOTE_ON = 0x90,
    MIDI_POLY_PRESSURE = 0xA0,
    MIDI_CONTROL_CHANGE = 0xB0,
    MIDI_PROGRAM_CHANGE = 0xC0,
    MIDI_CHANNEL_PRESSURE = 0xD0,
    MIDI_PITCH_BEND = 0xE0,
    MIDI_SYSTEM = 0xF0,
    MIDI_META = 0xFF,
};

// MIDI meta event types
enum MidiMeta : uint8_t {
    MIDI_META_TEXT = 0x01,
    MIDI_META_COPYRIGHT = 0x02,
    MIDI_META_TRACK_NAME = 0x03,
    MIDI_META_MARKER = 0x06,
    MIDI_META_CUE_POINT = 0x07,
    MIDI_META_END_OF_TRACK = 0x2F,
    MIDI_META_TEMPO = 0x51,
};

// MIDI control change numbers
enum MidiCC : uint8_t {
    MIDI_CC_BANK_MSB = 0,
    MIDI_CC_MODULATION = 1,
    MIDI_CC_VOLUME = 7,
    MIDI_CC_PAN = 10,
    MIDI_CC_EXPRESSION = 11,
    MIDI_CC_BANK_LSB = 32,
    MIDI_CC_SUSTAIN = 64,
    MIDI_CC_RPN_LSB = 100,
    MIDI_CC_RPN_MSB = 101,
    MIDI_CC_DATA_ENTRY_MSB = 6,
    MIDI_CC_DATA_ENTRY_LSB = 38,
    MIDI_CC_REVERB_SEND = 91,
    MIDI_CC_ALL_SOUND_OFF = 120,
    MIDI_CC_RESET_ALL = 121,
    MIDI_CC_ALL_NOTES_OFF = 123,
    MIDI_CC_LOOP_POINT = 111,
};

// MIDI drum channel (0-indexed)
static constexpr uint8_t MIDI_DRUM_CHANNEL = 9;

// SPU RAM layout
static constexpr uint32_t SPU_RAM_BASE = 0x1010;   // first usable address (after capture buffers)
static constexpr uint32_t SPU_RAM_SIZE = 0x80000;  // 512KB

struct MidiEvent {
    uint32_t absoluteTick;
    uint8_t type;     // MidiStatus value (upper nibble) or MIDI_META
    uint8_t channel;  // 0-15
    uint8_t data1;
    uint8_t data2;
    uint32_t tempo;        // microseconds per quarter note (for tempo meta events)
    std::string textData;  // for marker/text meta events
};

struct MidiFile {
    uint16_t format;
    uint16_t trackCount;
    uint16_t tpqn;  // ticks per quarter note
    std::vector<MidiEvent> events;

    bool parse(PCSX::IO<PCSX::File> file);

  private:
    void parseTrack(PCSX::IO<PCSX::File> track);
};

struct SpuSample {
    uint32_t spuAddr;     // SPU RAM address in bytes
    uint32_t adpcmSize;   // size in bytes
    uint32_t sampleRate;  // original sample rate
    uint32_t rootKey;     // MIDI note of natural pitch (pitch_keycenter)
    int32_t transpose;    // semitone offset (SF2 coarseTune)
    int32_t tune;         // fine tuning in cents (SF2 fineTune + pitchCorrection)
    bool hasLoop;
    uint32_t loopStartByte;  // byte offset within ADPCM data where loop starts
    std::vector<uint8_t> adpcmData;
};

// Key for deduplicating SF2 samples. Two regions share one SPU sample only when the
// produced sample is genuinely identical: same PCM range AND same parameters that change
// the output. The encoded ADPCM bytes depend on the loop flag and loop start; the tone's
// center note/fine tune are computed from rootKey/transpose/tune (which callers read back
// off the cached SpuSample). A region that shares PCM bytes but overrides any of these is a
// distinct sample, so it must not collapse onto the first one's parameters.
struct SampleKey {
    unsigned int offset;     // start index in fontSamples
    unsigned int end;        // end index in fontSamples
    int hasLoop;             // loop flag (baked into the ADPCM block flags)
    unsigned int loopStart;  // loop start index when looping (affects the encoded loop point)
    unsigned int loopEnd;    // loop end index when looping (sets the cut point and the loop pitch correction)
    int rootKey;             // region pitch_keycenter (read back for the tone center note)
    int transpose;           // region coarse tune
    int tune;                // region fine tune (cents)
    bool operator<(const SampleKey& o) const {
        if (offset != o.offset) return offset < o.offset;
        if (end != o.end) return end < o.end;
        if (hasLoop != o.hasLoop) return hasLoop < o.hasLoop;
        if (loopStart != o.loopStart) return loopStart < o.loopStart;
        if (loopEnd != o.loopEnd) return loopEnd < o.loopEnd;
        if (rootKey != o.rootKey) return rootKey < o.rootKey;
        if (transpose != o.transpose) return transpose < o.transpose;
        return tune < o.tune;
    }
};

uint32_t readVLQ(PCSX::IO<PCSX::File> f);

bool encodeSample(const int16_t* pcm, size_t sampleCount, bool loop, size_t loopStart, SpuSample& out);
double midiNoteToFreq(int note);
uint8_t sf2AttackToSpu(float seconds);
uint8_t sf2DecayToSpu(float seconds);
uint8_t sf2SustainToSpu(float sustainGain);
uint8_t sf2ReleaseToSpu(float seconds);
void sf2RegionToSpuADSR(tsf_region* region, bool isDrum, uint16_t& adsrLo, uint16_t& adsrHi);
std::vector<tsf_region*> findRegions(tsf* sf2, int presetIndex, int note, int velocity);
size_t extractAndEncode(tsf* sf2, tsf_region* region, std::vector<SpuSample>& samples,
                        std::map<SampleKey, size_t>& sampleMap, uint32_t& nextSpuAddr,
                        uint32_t maxSpuAddr = SPU_RAM_SIZE, bool warnPitchCeiling = false);
int32_t findLoopPointTick(const MidiFile& midi);
void extractMidiMetadata(const MidiFile& midi, std::string& trackName, std::string& copyright);

inline uint32_t readVLQ(PCSX::IO<PCSX::File> f) {
    uint32_t value = 0;
    while (!f->eof()) {
        uint8_t b = f->byte();
        value = (value << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) break;
    }
    return value;
}

// Standard MIDI file chunk headers, as binstructs. The length/word fields are big-endian (SMF is a
// big-endian format), so the BE field types do the swap on read. Track/header bodies are variable and
// stay as SubFile walks; only the fixed-size chunk headers are modeled here.
typedef BinStruct::Field<BinStruct::CString<4>, TYPESTRING("mthdMagic")> MThdMagic;      // "MThd"
typedef BinStruct::Field<BinStruct::BEUInt32, TYPESTRING("mthdLength")> MThdLength;      // header byte count (6)
typedef BinStruct::Field<BinStruct::BEUInt16, TYPESTRING("mthdFormat")> MThdFormat;      // 0/1/2
typedef BinStruct::Field<BinStruct::BEUInt16, TYPESTRING("mthdTracks")> MThdTracks;      // number of MTrk chunks
typedef BinStruct::Field<BinStruct::BEUInt16, TYPESTRING("mthdDivision")> MThdDivision;  // ticks per quarter note
typedef BinStruct::Struct<TYPESTRING("MThd"), MThdMagic, MThdLength, MThdFormat, MThdTracks, MThdDivision> MThd;

typedef BinStruct::Field<BinStruct::CString<4>, TYPESTRING("mtrkMagic")> MTrkMagic;  // "MTrk"
typedef BinStruct::Field<BinStruct::BEUInt32, TYPESTRING("mtrkLength")> MTrkLength;  // track byte count
typedef BinStruct::Struct<TYPESTRING("MTrk"), MTrkMagic, MTrkLength> MTrk;

inline bool MidiFile::parse(PCSX::IO<PCSX::File> file) {
    // The stream we actually parse the MThd/MTrk chunks from. For a plain SMF this is the file
    // itself; for an RMID (RIFF-wrapped MIDI) it's a SubFile windowed over the "data" chunk so the
    // rest of the parse is naturally bounded to the embedded MIDI payload.
    PCSX::IO<PCSX::File> in = file;

    // Check for RIFF/RMID wrapper (RMI files)
    file->rSeek(0, SEEK_SET);
    if (file->size() >= 20) {
        char riff[12];
        file->read(riff, 12);
        if (memcmp(riff, "RIFF", 4) == 0 && memcmp(riff + 8, "RMID", 4) == 0) {
            // Walk the RIFF chunks (from offset 12) for the "data" chunk holding the MIDI. All bounds
            // are checked explicitly: the File layer clamps reads silently and PosixFile::rSeek can land
            // past EOF, so the unsigned (size() - rTell()) gap must never be allowed to underflow.
            const size_t sz = file->size();
            while (true) {
                ssize_t pos = file->rTell();
                if (pos < 0 || (size_t)pos + 8 > sz) break;  // no room for another chunk header
                char chunkId[4];
                file->read(chunkId, 4);
                uint32_t chunkSize = file->read<uint32_t>();  // RIFF sizes are little-endian
                size_t avail = sz - (size_t)file->rTell();
                if (memcmp(chunkId, "data", 4) == 0) {
                    // Window the MIDI payload, clamped to what the file actually holds.
                    size_t dataSize = std::min((size_t)chunkSize, avail);
                    in = PCSX::IO<PCSX::File>(new PCSX::SubFile(file, file->rTell(), dataSize));
                    break;
                }
                size_t padded = ((size_t)chunkSize + 1) & ~size_t(1);  // RIFF chunks are 2-byte aligned
                if (padded > avail) break;                             // declared chunk overruns the file
                file->rSeek((ssize_t)padded, SEEK_CUR);
            }
        }
    }

    // MThd header
    in->rSeek(0, SEEK_SET);
    if (in->size() < 14) return false;
    MThd mthd;
    mthd.deserialize(in);
    if (memcmp(mthd.get<MThdMagic>().value, "MThd", 4) != 0) return false;
    uint32_t headerLen = mthd.get<MThdLength>().value;
    format = mthd.get<MThdFormat>().value;
    trackCount = mthd.get<MThdTracks>().value;
    tpqn = mthd.get<MThdDivision>().value;
    // The MThd body is at least the 6 bytes already consumed, and any extra must fit in the stream;
    // otherwise the skip would seek backwards or past EOF (the latter underflows the track-loop gap).
    if (headerLen < 6) return false;
    if (headerLen - 6 > in->size() - (size_t)in->rTell()) return false;
    in->rSeek((ssize_t)(headerLen - 6), SEEK_CUR);  // skip any remaining MThd body past the 6 bytes read

    // Parse all tracks. Each MTrk is wrapped in a SubFile so the per-track bound is enforced by the
    // file layer (the track parser literally cannot read past its chunk into the next one).
    for (uint16_t t = 0; t < trackCount && (in->size() - in->rTell()) >= 8; t++) {
        MTrk mtrk;
        mtrk.deserialize(in);
        if (memcmp(mtrk.get<MTrkMagic>().value, "MTrk", 4) != 0) return false;
        uint32_t trackLen = mtrk.get<MTrkLength>().value;
        if (in->rTell() + trackLen > in->size()) return false;
        PCSX::IO<PCSX::File> track(new PCSX::SubFile(in, in->rTell(), trackLen));
        parseTrack(track);
        in->rSeek(trackLen, SEEK_CUR);
    }

    // Sort all events by absolute tick (stable sort preserves order within same tick)
    std::stable_sort(events.begin(), events.end(),
                     [](const MidiEvent& a, const MidiEvent& b) { return a.absoluteTick < b.absoluteTick; });
    return true;
}

inline void MidiFile::parseTrack(PCSX::IO<PCSX::File> track) {
    uint32_t absTick = 0;
    uint8_t runningStatus = 0;

    // Bytes left to read in this track's window.
    auto remaining = [&]() -> size_t { return track->size() - track->rTell(); };

    while (!track->eof()) {
        uint32_t delta = readVLQ(track);
        absTick += delta;
        if (track->eof()) break;

        uint8_t status = track->peek<uint8_t>();
        if (status & 0x80) {
            track->byte();
            if (status < MIDI_SYSTEM) runningStatus = status;
        } else {
            status = runningStatus;
        }

        uint8_t type = status & 0xF0;
        uint8_t channel = status & 0x0F;

        if (status == MIDI_META) {
            // Meta event
            if (remaining() < 2) break;
            uint8_t metaType = track->byte();
            uint32_t metaLen = readVLQ(track);
            if (remaining() < metaLen) break;
            if (metaType == MIDI_META_TEMPO && metaLen == 3) {
                // Tempo change
                MidiEvent ev = {};
                ev.absoluteTick = absTick;
                ev.type = MIDI_META;
                ev.data1 = MIDI_META_TEMPO;
                uint8_t b0 = track->byte();
                uint8_t b1 = track->byte();
                uint8_t b2 = track->byte();
                ev.tempo = (b0 << 16) | (b1 << 8) | b2;
                events.push_back(ev);
            } else if (metaType == MIDI_META_MARKER || metaType == MIDI_META_CUE_POINT || metaType == MIDI_META_TEXT ||
                       metaType == MIDI_META_TRACK_NAME || metaType == MIDI_META_COPYRIGHT) {
                // Text-based meta events
                MidiEvent ev = {};
                ev.absoluteTick = absTick;
                ev.type = MIDI_META;
                ev.data1 = metaType;
                ev.textData = track->readString(metaLen);
                events.push_back(ev);
            } else if (metaType == MIDI_META_END_OF_TRACK) {
                // End of track
                break;
            } else {
                track->skip(metaLen);
            }
        } else if (status >= MIDI_SYSTEM) {
            // SysEx - skip
            uint32_t sysLen = readVLQ(track);
            track->skip(sysLen);
        } else if (type == MIDI_NOTE_OFF || type == MIDI_NOTE_ON || type == MIDI_POLY_PRESSURE ||
                   type == MIDI_CONTROL_CHANGE || type == MIDI_PITCH_BEND) {
            // Two data bytes
            if (remaining() < 2) break;
            MidiEvent ev = {};
            ev.absoluteTick = absTick;
            ev.type = type;
            ev.channel = channel;
            ev.data1 = track->byte();
            ev.data2 = track->byte();
            // Note on with velocity 0 is actually note off
            if (type == MIDI_NOTE_ON && ev.data2 == 0) {
                ev.type = MIDI_NOTE_OFF;
            }
            events.push_back(ev);
        } else if (type == MIDI_PROGRAM_CHANGE || type == MIDI_CHANNEL_PRESSURE) {
            // One data byte
            if (remaining() < 1) break;
            MidiEvent ev = {};
            ev.absoluteTick = absTick;
            ev.type = type;
            ev.channel = channel;
            ev.data1 = track->byte();
            events.push_back(ev);
        }
    }
}

inline bool encodeSample(const int16_t* pcm, size_t sampleCount, bool loop, size_t loopStart, SpuSample& out) {
    PCSX::ADPCM::Encoder encoder;
    encoder.reset();

    // Pad to multiple of 28 samples
    size_t paddedCount = ((sampleCount + 27) / 28) * 28;
    std::vector<int16_t> padded(paddedCount, 0);
    memcpy(padded.data(), pcm, sampleCount * sizeof(int16_t));

    size_t totalBlocks = paddedCount / 28;
    out.adpcmData.resize(totalBlocks * 16 + 16);  // +16 for end block

    size_t loopStartBlock = loop ? (loopStart / 28) : 0;

    for (size_t b = 0; b < totalBlocks; b++) {
        PCSX::ADPCM::Encoder::BlockAttribute attr;
        if (loop) {
            if (b == loopStartBlock) {
                attr = PCSX::ADPCM::Encoder::BlockAttribute::LoopStart;
            } else if (b == totalBlocks - 1) {
                attr = PCSX::ADPCM::Encoder::BlockAttribute::LoopEnd;
            } else if (b > loopStartBlock) {
                attr = PCSX::ADPCM::Encoder::BlockAttribute::LoopBody;
            } else {
                attr = PCSX::ADPCM::Encoder::BlockAttribute::OneShot;
            }
        } else {
            attr = (b == totalBlocks - 1) ? PCSX::ADPCM::Encoder::BlockAttribute::OneShotEnd
                                          : PCSX::ADPCM::Encoder::BlockAttribute::OneShot;
        }
        encoder.processSPUBlock(&padded[b * 28], &out.adpcmData[b * 16], attr);
    }

    if (!loop) {
        encoder.finishSPU(&out.adpcmData[totalBlocks * 16]);
        out.adpcmData.resize((totalBlocks + 1) * 16);
    } else {
        out.adpcmData.resize(totalBlocks * 16);
    }

    out.loopStartByte = loopStartBlock * 16;
    out.hasLoop = loop;
    out.adpcmSize = out.adpcmData.size();
    return true;
}

// The SPU can only loop on 28-sample ADPCM block boundaries, and the loop always runs to the
// end of the sample. Rebuild a looped sample so that it ends exactly at the loop end, the loop
// starts on a block boundary, and the loop length is a whole number of blocks. The loop is
// unrolled a few times when that brings its length closer to a block multiple, and the whole
// sample is resampled to absorb the remainder. loopCents receives the pitch change caused by
// the resampling, which the caller subtracts from the tone's tuning.
inline std::vector<int16_t> alignLoop(const std::vector<int16_t>& pcm, size_t ls, size_t le, size_t& newLoopStart,
                                      double& loopCents) {
    size_t loopLen = le - ls;
    size_t bestUnroll = 1, bestBlocks = 1;
    double bestError = 1e9;
    for (size_t unroll = 1; unroll <= 64 && (unroll == 1 || unroll * loopLen <= 4096); unroll++) {
        size_t len = unroll * loopLen;
        size_t blocks = std::max<size_t>(1, (len + 14) / 28);
        double error = fabs(log2((double)(blocks * 28) / (double)len));
        if (error < bestError - 1e-9) {
            bestError = error;
            bestUnroll = unroll;
            bestBlocks = blocks;
        }
        if (error < 0.0005) break;  // under a cent
    }
    double srcLoop = (double)(bestUnroll * loopLen);
    double dstLoop = (double)(bestBlocks * 28);
    double ratio = dstLoop / srcLoop;
    loopCents = 1200.0 * log2(ratio);

    // Source reader that wraps inside the loop, so the loop is resampled as a periodic signal.
    auto src = [&](long i) -> double {
        if (i < 0) return 0.0;
        if ((size_t)i >= le) i = (long)(ls + (i - ls) % loopLen);
        return pcm[i];
    };
    auto interp = [&](double pos) -> double {
        long i = (long)floor(pos);
        double t = pos - i;
        double p0 = src(i - 1), p1 = src(i), p2 = src(i + 1), p3 = src(i + 2);
        return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
    };

    // Loop start after resampling, moved forward to the next block boundary. The samples
    // skipped over are loop material, so the loop simply starts a little later in its cycle.
    size_t start = (size_t)ceil(ls * ratio);
    newLoopStart = ((start + 27) / 28) * 28;
    size_t total = newLoopStart + bestBlocks * 28;
    std::vector<int16_t> out(total);
    for (size_t j = 0; j < total; j++) {
        double v = interp(j / ratio);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out[j] = (int16_t)lround(v);
    }
    return out;
}

inline double midiNoteToFreq(int note) { return 440.0 * pow(2.0, (note - 69) / 12.0); }

inline uint8_t sf2AttackToSpu(float seconds) {
    if (seconds <= 0.0f) return 0x7F;   // instant
    if (seconds >= 10.0f) return 0x00;  // slowest

    // Calibrated against pcsx-redux ADSR tables:
    // rate 127 -> ~0ms, rate 80 -> ~2890 frames (~48s at 60fps)
    // Using K = 0.0005 gives a reasonable fit
    float rate = 127.0f - 4.0f * log2f(seconds / 0.0005f);
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 127.0f) rate = 127.0f;
    return (uint8_t)(rate + 0.5f);
}

inline uint8_t sf2DecayToSpu(float seconds) {
    if (seconds <= 0.0f) return 0x0F;
    if (seconds >= 30.0f) return 0x00;

    // value = 15 - log2(seconds / 0.000292)
    float value = 15.0f - log2f(seconds / 0.000292f);
    if (value < 0.0f) value = 0.0f;
    if (value > 15.0f) value = 15.0f;
    return (uint8_t)(value + 0.5f);
}

inline uint8_t sf2SustainToSpu(float sustainGain) {
    if (sustainGain >= 1.0f) return 0x0F;
    if (sustainGain <= 0.0f) return 0x00;
    // The SPU sustain levels are roughly linear, so a linear mapping works.
    int level = (int)(sustainGain * 15.0f + 0.5f);
    if (level < 0) level = 0;
    if (level > 0x0F) level = 0x0F;
    return (uint8_t)level;
}

inline uint8_t sf2ReleaseToSpu(float seconds) {
    if (seconds <= 0.0f) return 0x1F;
    if (seconds >= 30.0f) return 0x00;

    // value = 31 - log2(seconds / 0.000446)
    float value = 31.0f - log2f(seconds / 0.000446f);
    if (value < 0.0f) value = 0.0f;
    if (value > 31.0f) value = 31.0f;
    return (uint8_t)(value + 0.5f);
}

inline void sf2RegionToSpuADSR(tsf_region* region, bool isDrum, uint16_t& adsrLo, uint16_t& adsrHi) {
    // TSF pre-converts timecents to seconds for attack, hold, decay, release.
    // For ampenv, sustain is converted to a gain value (1.0 = full, 0.0 = silence)
    // via tsf_decibelsToGain(-sustain/10).
    // Hold and decay may still be in timecents if keynumToHold/keynumToDecay are set.

    float attackSec = region->ampenv.attack;
    float decaySec = region->ampenv.decay;
    // If keynumToDecay is set, decay is still in timecents
    if (region->ampenv.keynumToDecay != 0.0f) {
        decaySec = (decaySec < -11950.0f) ? 0.0f : powf(2.0f, decaySec / 1200.0f);
    }
    float sustainGain = region->ampenv.sustain;  // gain: 1.0 = full, 0.0 = silence
    float releaseSec = region->ampenv.release;

    // Clamp to reasonable ranges
    if (attackSec < 0.0f) attackSec = 0.0f;
    if (decaySec < 0.0f) decaySec = 0.0f;
    if (releaseSec < 0.0f) releaseSec = 0.0f;
    if (sustainGain < 0.0f) sustainGain = 0.0f;

    // For drums, force fast release if SF2 doesn't specify one
    if (isDrum && releaseSec > 0.3f) releaseSec = 0.1f;

    uint8_t attackRate = sf2AttackToSpu(attackSec);
    uint8_t decayRate = sf2DecayToSpu(decaySec);
    uint8_t sustainLevel = sf2SustainToSpu(sustainGain);
    uint8_t releaseRate = sf2ReleaseToSpu(releaseSec);

    // SPU sustain rate: controls what happens at sustain level.
    // For most instruments, sustain should hold (rate = 0, direction = increase for hold effect).
    // For drums, sustain should decrease to fade out.
    uint8_t sustainRate = isDrum ? 0x1F : 0x00;
    uint8_t sustainDir = isDrum ? 1 : 0;   // 1 = decrease
    uint8_t sustainMode = isDrum ? 0 : 0;  // 0 = linear

    // Build register values
    // ADSR upper: attack mode (bit 15) | attack rate (bits 8-14) | decay rate (bits 4-7) | sustain level (bits 0-3)
    adsrHi = (uint16_t)((1 << 15) |  // exponential attack for smoother sound
                        ((attackRate & 0x7F) << 8) | ((decayRate & 0x0F) << 4) | (sustainLevel & 0x0F));

    // ADSR lower: sustain mode (bit 14) | sustain dir (bit 13) | sustain rate (bits 6-10) |
    //             release mode (bit 5) | release rate (bits 0-4)
    adsrLo = (uint16_t)(((sustainMode & 1) << 14) | ((sustainDir & 1) << 13) | ((sustainRate & 0x1F) << 6) |
                        (1 << 5) |  // exponential release for natural decay
                        (releaseRate & 0x1F));
}

inline std::vector<tsf_region*> findRegions(tsf* sf2, int presetIndex, int note, int velocity) {
    std::vector<tsf_region*> result;
    if (presetIndex < 0 || presetIndex >= sf2->presetNum) return result;
    tsf_preset* preset = &sf2->presets[presetIndex];
    for (int i = 0; i < preset->regionNum; i++) {
        tsf_region* r = &preset->regions[i];
        if (note >= r->lokey && note <= r->hikey && velocity >= r->lovel && velocity <= r->hivel) {
            result.push_back(r);
        }
    }
    return result;
}

inline size_t extractAndEncode(tsf* sf2, tsf_region* region, std::vector<SpuSample>& samples,
                               std::map<SampleKey, size_t>& sampleMap, uint32_t& nextSpuAddr, uint32_t maxSpuAddr,
                               bool warnPitchCeiling) {
    bool keyHasLoop = (region->loop_mode == TSF_LOOPMODE_CONTINUOUS || region->loop_mode == TSF_LOOPMODE_SUSTAIN) &&
                      region->loop_start >= region->offset && region->loop_end > region->loop_start &&
                      region->loop_end < region->end;
    SampleKey key = {region->offset,
                     region->end,
                     keyHasLoop ? 1 : 0,
                     keyHasLoop ? region->loop_start : 0u,
                     keyHasLoop ? region->loop_end : 0u,
                     (int)region->pitch_keycenter,
                     region->transpose,
                     region->tune};
    auto it = sampleMap.find(key);
    if (it != sampleMap.end()) return it->second;

    // Extract raw PCM from fontSamples
    unsigned int sampleCount = region->end - region->offset;
    if (sampleCount == 0) return (size_t)-1;

    std::vector<int16_t> pcm(sampleCount);
    for (unsigned int i = 0; i < sampleCount; i++) {
        float v = sf2->fontSamples[region->offset + i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm[i] = (int16_t)v;
    }

    // Handle loop points
    bool hasLoop = keyHasLoop;
    size_t loopStart = 0;
    double loopCents = 0.0;
    if (hasLoop) {
        // tsf's loop_end is inclusive.
        size_t ls = region->loop_start - region->offset;
        size_t le = region->loop_end - region->offset + 1;
        pcm = alignLoop(pcm, ls, le, loopStart, loopCents);
    }

    SpuSample sample;
    sample.sampleRate = region->sample_rate;
    sample.rootKey = region->pitch_keycenter;
    sample.transpose = region->transpose;
    sample.tune = region->tune + (int)lround(loopCents);
    encodeSample(pcm.data(), pcm.size(), hasLoop, loopStart, sample);

    sample.spuAddr = nextSpuAddr;
    nextSpuAddr += sample.adpcmSize;

    if (nextSpuAddr > maxSpuAddr) {
        fmt::print(stderr, "Warning: SPU RAM overflow at {} bytes (max {})\n", nextSpuAddr, maxSpuAddr);
    }

    // Check for pitch ceiling: if this sample would need pitch > 0x3FFF for notes
    // at the top of its key range, warn the user.
    if (warnPitchCeiling) {
        int highNote = region->hikey + region->transpose;
        double effectiveNote = highNote + region->tune / 100.0;
        double noteFreq = 440.0 * pow(2.0, (effectiveNote - 69) / 12.0);
        double rootFreq = 440.0 * pow(2.0, ((int)region->pitch_keycenter - 69) / 12.0);
        double maxPitch = (noteFreq / rootFreq) * ((double)region->sample_rate / 44100.0) * 0x1000;
        if (maxPitch > 0x3FFF) {
            fmt::print(stderr,
                       "  Warning: sample {} (root={}, rate={}Hz) hits pitch ceiling at note {} "
                       "(pitch {:.0f} > 16383). High notes will play flat.\n",
                       samples.size(), region->pitch_keycenter, region->sample_rate, region->hikey, maxPitch);
        }
    }

    size_t idx = samples.size();
    fmt::print("  Sample {}: offset={} end={} rate={}Hz rootKey={} transpose={} tune={} cents loop={} size={} bytes\n",
               idx, region->offset, region->end, sample.sampleRate, sample.rootKey, sample.transpose, sample.tune,
               sample.hasLoop ? "yes" : "no", sample.adpcmSize);
    samples.push_back(std::move(sample));
    sampleMap[key] = idx;
    return idx;
}

inline int32_t findLoopPointTick(const MidiFile& midi) {
    for (auto& mev : midi.events) {
        if (mev.type == MIDI_CONTROL_CHANGE && mev.data1 == MIDI_CC_LOOP_POINT) {
            return (int32_t)mev.absoluteTick;
        }
        if (mev.type == MIDI_META && mev.data1 == MIDI_META_MARKER) {
            // Check for common loop marker text
            if (mev.textData == "loopStart" || mev.textData == "LoopStart" || mev.textData == "loop") {
                return (int32_t)mev.absoluteTick;
            }
        }
    }
    return -1;
}

inline void extractMidiMetadata(const MidiFile& midi, std::string& trackName, std::string& copyright) {
    for (auto& mev : midi.events) {
        if (mev.type == MIDI_META) {
            if (mev.data1 == MIDI_META_TRACK_NAME && trackName.empty()) {
                trackName = mev.textData;
            } else if (mev.data1 == MIDI_META_COPYRIGHT && copyright.empty()) {
                copyright = mev.textData;
            }
        }
    }
}

}  // namespace PCSX::MidiConverter
