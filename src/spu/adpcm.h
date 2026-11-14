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

// Self-contained per-voice ADPCM decoder. Owns the decode cursor into sound RAM
// (start/current/loop pointers) and the two-sample IIR decode history, and
// turns a 16-byte ADPCM block into 28 PCM samples. Extracted out of the SPUCHAN
// per-voice struct and the MainThread synthesis loop; the math and the cursor
// bookkeeping are unchanged. The loop/stop flag resolution and the SPU IRQ check
// stay in MainThread, which owns the SPU-wide state they need; this class just
// exposes the cursor so that code can keep driving it.
class AdpcmDecoder {
  public:
    // Key-on: rewind the decode cursor to the sample start and clear the IIR
    // history, exactly as StartSound used to do inline.
    void keyOn() {
        m_curr = m_start;
        m_block = nullptr;
        m_s1 = 0;
        m_s2 = 0;
    }

    // Key-on timeline. A KEY ON write reaching the mixer at sample `applied` is serviced
    // on the first odd sample after it. The voice then stays silent for four samples: on
    // each of the first three it fetches the header of the first block and applies its
    // loop-start flag, ENDX is cleared on the first, and the envelope's first step and the
    // first decoded sample come on the fifth. A CPU write to the repeat address only stops
    // loop-start flags from replacing it once the third fetch has passed.
    static constexpr int kKeyOnHeaderFetches = 3;
    static constexpr int kKeyOnSilentSamples = 4;
    static constexpr uint64_t keyOnServiceSample(uint64_t applied) { return (applied + 1) | 1; }

    // Seed the timeline for a key-on that reached the mixer at sample `applied`.
    void startKeyOn(uint64_t applied) {
        m_keyOnAge = 0;
        m_keyOnService = static_cast<int>(keyOnServiceSample(applied) - applied);
    }
    // The timeline is active until the first audible sample.
    bool startupDelayActive() const { return m_keyOnAge < m_keyOnService + kKeyOnSilentSamples; }
    // Samples since key-on reached the mixer, and how many of them precede the service.
    int keyOnAge() const { return m_keyOnAge; }
    int keyOnService() const { return m_keyOnService; }
    void tickStartupDelay() {
        if (startupDelayActive()) m_keyOnAge++;
    }
    // Whether a repeat address write landing now holds against loop-start flags.
    bool repeatWriteLatches() const {
        return !startupDelayActive() || m_keyOnAge >= m_keyOnService + kKeyOnHeaderFetches;
    }

    // Clear all decode state (voice wipe).
    void reset() {
        m_start = nullptr;
        m_curr = nullptr;
        m_loop = nullptr;
        m_block = nullptr;
        m_s1 = 0;
        m_s2 = 0;
    }

    // Cursor sentinel for a voice that decoded a stop/loop-end block with no loop
    // target: the next synthesis pass sees it in the cursor and turns the voice
    // off. It is a deliberately invalid, non-null pointer, distinct from the
    // nullptr "wiped" state. The savestate bridge stores it as kStoppedOffset.
    inline static uint8_t *const kStopped = reinterpret_cast<uint8_t *>(~uintptr_t(0));
    bool stopped() const { return m_curr == kStopped; }
    static constexpr int32_t kNullOffset = -1;
    static constexpr int32_t kStoppedOffset = -2;
    static constexpr int32_t kRamSize = 512 * 1024;

    // Decode cursor accessors (raw pointers into sound RAM).
    uint8_t *start() const { return m_start; }
    uint8_t *curr() const { return m_curr; }
    uint8_t *loop() const { return m_loop; }
    // The 16-byte block whose samples are being played, or nullptr before the first decode. Unlike
    // curr(), it stays on that block after a loop-end redirect. Not saved: a loaded state has none
    // until the next decode.
    uint8_t *block() const { return m_block; }
    void clearBlock() { m_block = nullptr; }
    void setStart(uint8_t *p) { m_start = p; }
    void setCurr(uint8_t *p) { m_curr = p; }
    void setLoop(uint8_t *p) { m_loop = p; }

    // Savestate bridge (freeze.cc only). Encode/decode this decoder's runtime
    // state - the two-sample IIR history and the three sound-RAM cursors - into
    // the per-channel savestate fields. The fields are passed in rather than the
    // whole channel message because adpcm.h cannot include types.h (it is
    // included by it). `ramBase` is spuRamBase; each cursor is stored as a byte
    // offset, with kNullOffset for a null pointer and kStoppedOffset for kStopped.
    void saveTo(Protobuf::Int32 &history1, Protobuf::Int32 &history2, Protobuf::Int32 &startOffset,
                Protobuf::Int32 &currOffset, Protobuf::Int32 &loopOffset, uint8_t *ramBase, Protobuf::Int32 *sb,
                Protobuf::Int32 &sbPos) const;
    void loadFrom(const Protobuf::Int32 &history1, const Protobuf::Int32 &history2, const Protobuf::Int32 &startOffset,
                  const Protobuf::Int32 &currOffset, const Protobuf::Int32 &loopOffset, uint8_t *ramBase,
                  const Protobuf::Int32 *sb, const Protobuf::Int32 &sbPos);

    struct DecodeResult {
        uint8_t *blockEnd;  // one past the 16-byte block just decoded
        int flags;          // the block's flag byte (loop/repeat/end bits)
    };

    // Decode the 16-byte ADPCM block beginning at `block` into our own 28-sample
    // buffer, advancing the IIR history, and rewind the read cursor to the top of
    // that buffer. The buffer is ours because we are the only thing that fills it;
    // the voice pulls samples back out one at a time. Returns the address just past
    // the block and the block's flag byte, which the caller needs for its IRQ check
    // and loop handling.
    DecodeResult decodeBlock(uint8_t *block);

    static constexpr int kSamplesPerBlock = 28;
    bool bufferExhausted() const { return m_pos >= kSamplesPerBlock; }
    int32_t takeSample() { return m_samples[m_pos++]; }
    // Key-on leaves the buffer empty so the first sample forces a decode.
    void emptyBuffer() {
        m_pos = kSamplesPerBlock;
        m_block = nullptr;
    }

  private:
    // ADPCM IIR filter coefficient pairs, indexed by the block's predictor. The
    // coefficients are Q6 fixed point, so the feedback term is shifted right by 6.
    static constexpr int kFilterCount = 5;
    static constexpr int kFilterCoeff[kFilterCount][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    static constexpr int kCoeffShift = 6;

    uint8_t *m_start = nullptr;                // start pointer into sound RAM
    uint8_t *m_curr = nullptr;                 // current position in sound RAM
    uint8_t *m_loop = nullptr;                 // loop pointer into sound RAM
    uint8_t *m_block = nullptr;                // block being played
    int32_t m_s1 = 0;                          // last decoded sample (IIR history)
    int32_t m_s2 = 0;                          // next-to-last decoded sample (IIR history)
    int m_keyOnAge = 1 << 30;                  // samples since key-on reached the mixer
    int m_keyOnService = 0;                    // samples from that to the key-on service
    int32_t m_samples[kSamplesPerBlock] = {};  // the decoded block
    int m_pos = kSamplesPerBlock;              // read cursor into it
};

}  // namespace SPU

}  // namespace PCSX
