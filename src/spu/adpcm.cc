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

#include "spu/adpcm.h"

void PCSX::SPU::AdpcmDecoder::saveTo(Protobuf::Int32 &history1, Protobuf::Int32 &history2, Protobuf::Int32 &startOffset,
                                     Protobuf::Int32 &currOffset, Protobuf::Int32 &loopOffset, uint8_t *ramBase,
                                     Protobuf::Int32 *sb, Protobuf::Int32 &sbPos) const {
    history1.value = m_s1;
    history2.value = m_s2;
    auto storeOffset = [ramBase](uint8_t *ptr, Protobuf::Int32 &offset) {
        if (!ptr) {
            offset.value = kNullOffset;
        } else if (ptr == kStopped) {
            offset.value = kStoppedOffset;
        } else {
            offset.value = ptr - ramBase;
        }
    };
    storeOffset(m_start, startOffset);
    storeOffset(m_curr, currOffset);
    storeOffset(m_loop, loopOffset);
    for (int i = 0; i < kSamplesPerBlock; i++) sb[i].value = m_samples[i];
    sbPos.value = m_pos;
}

void PCSX::SPU::AdpcmDecoder::loadFrom(const Protobuf::Int32 &history1, const Protobuf::Int32 &history2,
                                       const Protobuf::Int32 &startOffset, const Protobuf::Int32 &currOffset,
                                       const Protobuf::Int32 &loopOffset, uint8_t *ramBase, const Protobuf::Int32 *sb,
                                       const Protobuf::Int32 &sbPos) {
    m_s1 = history1.value;
    m_s2 = history2.value;
    // Anything outside sound RAM other than the null marker restores as stopped. That covers
    // kStoppedOffset, and also states written before it existed, which stored kStopped as a
    // truncated pointer difference.
    auto restore = [ramBase](const Protobuf::Int32 &offset) -> uint8_t * {
        if (offset.value == kNullOffset) return nullptr;
        if (offset.value < 0 || offset.value >= kRamSize) return kStopped;
        return offset.value + ramBase;
    };
    m_start = restore(startOffset);
    m_curr = restore(currOffset);
    m_loop = restore(loopOffset);
    for (int i = 0; i < kSamplesPerBlock; i++) m_samples[i] = sb[i].value;
    m_pos = sbPos.value;
}

PCSX::SPU::AdpcmDecoder::DecodeResult PCSX::SPU::AdpcmDecoder::decodeBlock(uint8_t *block) {
    m_block = block;
    // Header byte 0: high nibble selects the IIR predictor, low nibble is the
    // per-sample right shift. Byte 1 holds the loop/repeat/end flags.
    // The high nibble is a full 0..15 but only five filters exist. What the hardware does with
    // 5..15 is not documented and has not been measured here, so this only keeps the table read
    // in range; it does not claim to model whatever silicon actually returns.
    const int rawPredictor = *block >> 4;
    const int predictor = rawPredictor < kFilterCount ? rawPredictor : kFilterCount - 1;
    const int shift = *block++ & 0xf;
    const int flags = *block++;

    // Two running IIR taps, kept in locals across the 28 samples then stored back.
    int history1 = m_s1;
    int history2 = m_s2;

    // Decode one 4-bit sample already positioned at bits 12..15 and sign-extended
    // to 16 bits: shift it down, then add the predictor's feedback.
    auto decodeSample = [&](int sample) {
        const int out = (sample >> shift) + ((history1 * kFilterCoeff[predictor][0]) >> kCoeffShift) +
                        ((history2 * kFilterCoeff[predictor][1]) >> kCoeffShift);
        history2 = history1;
        history1 = out;
        return out;
    };

    for (unsigned n = 0; n < 28; block++) {
        const int nibbles = *block;
        m_samples[n++] = decodeSample(static_cast<int16_t>((nibbles & 0x0f) << 12));
        m_samples[n++] = decodeSample(static_cast<int16_t>((nibbles & 0xf0) << 8));
    }

    m_s1 = history1;
    m_s2 = history2;
    // The caller reads this block from the top.
    m_pos = 0;
    return {block, flags};
}
