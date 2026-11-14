/***************************************************************************
 *   Copyright (C) 2010 Gabriele Gorla                                     *
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
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

// The decode half of the MDEC: run-level stream in, pixels out. The encode half
// is DCT in dct.h.
//
// This is the emulator's decoder, and the emulator is what anchors it: every
// clause that has been measured on a console was measured against this code
// (src/mips/tests/mdec/roundtrip is the rig, genjob.py names the arms). Both
// src/core/mdec.cc and tools/mdec decode through here, so a decode in the tool
// is byte-for-byte what the emulator's MDEC hands a game for the same stream and
// tables. Change it the way the emulator's MDEC was always changed: against a
// hardware arm, and grading on the ordinary-content control.
//
// NOTE the license: this file and its .cc are GPLv2+, because the AAN IDCT and
// the run-level walk are PCSX-df's. That is why they live in src/core and not in
// the MIT supportpsx, and why tools/mdec links this one object explicitly.

namespace PCSX {

namespace MDECDecoder {

static constexpr unsigned DSIZE = 8;
static constexpr unsigned DSIZE2 = DSIZE * DSIZE;
// One macroblock's worth of decoded blocks: Cr, Cb, Y1, Y2, Y3, Y4.
static constexpr unsigned BLOCKS = 6;

// The scale matrix every known title uploads with MDEC(3), straight out of
// psx-spx's set_scale_table section. Signed halfwords, 14 fractional bits.
const int16_t *standardScaleTable();

// An uploaded quant table as the AAN decoder caches it (each entry times its
// prescale factor), and back. The factors are all non-zero, so the reverse is an
// exact division.
void prescaleQuantTable(int *iqtab, const uint8_t *qt);
void quantTableFromPrescaled(uint8_t *qt, const int *iqtab);

// Everything MDEC(2) and MDEC(3) leave behind in the chip.
struct Tables {
    // The quant tables twice, both in upload (zigzag) order: prescaled for the
    // AAN path, and raw. real_idct_core must not see the prescale (psx-spx
    // annotates "val=val*scalezag[i]" as fast_idct_core only), and psx-spx
    // saturates the raw dequantized value BEFORE that multiply, so the fast path
    // needs the two factors separately too.
    int iqY[DSIZE2] = {};
    int iqUV[DSIZE2] = {};
    uint8_t qtY[DSIZE2] = {};
    uint8_t qtUV[DSIZE2] = {};
    // MDEC(3), host order. All zero until something uploads one: silicon has no
    // default, and a decode before MDEC(3) produces flat grey on hardware.
    int16_t scale[DSIZE2] = {};
    // Anything but the standard matrix runs the general matrix multiply instead
    // of the AAN IDCT. An all-zero matrix is not the standard one.
    bool customScale = true;

    // MDEC(2). 64 bytes each, in the order the command receives them.
    void setQuant(const uint8_t *y, const uint8_t *uv);
    // MDEC(3). 64 host-order signed halfwords.
    void setScale(const int16_t *table);
    // What the chip holds before any MDEC(3): an all-zero matrix.
    void clearScale();
    // The standard quant table for both planes and the standard matrix.
    void setStandard();
};

// Why a bounded decode stopped early. The emulator decodes unbounded and never
// sees these; the tool decodes a file and reports them.
enum class Anomaly {
    None,
    // The bound was reached. atHeader says it was reached where a block's DC
    // header was due, rather than inside a block before its FE00h terminator.
    Exhausted,
    // A run took the coefficient index past 63. The block ends there, as on the
    // emulator, and decoding carries on with the next halfword.
    RunPast63,
};

struct Status {
    Anomaly anomaly = Anomaly::None;
    unsigned block = 0;  // 0..5, the first block that went wrong
    bool atHeader = false;
};

// Decodes one macroblock, six blocks of DSIZE2 ints each into blk, from a stream
// of little-endian halfwords as they sit in guest memory or a file. Returns the
// position after the macroblock.
//
// end bounds the read and may be null, which reads unbounded the way the
// emulator's MDEC does. status, if non-null, receives the first anomaly.
//
// The output is in the domain toRGB24/toRGB15 consume, NOT pixels: the colour
// stage carries a 2^10 prescale.
const uint16_t *decodeMacroblock(int *blk, const uint16_t *rl, const Tables &tables, const uint16_t *end = nullptr,
                                 Status *status = nullptr);

// One decoded macroblock to 16x16 pixels, rows packed (48 bytes per row at 24
// bits, 16 halfwords per row at 15). mono is the emulator's black-and-white
// setting. stp sets bit 15 of every 15-bit pixel, as MDEC(1)'s bit 25 does. The
// 15-bit pixels are written little-endian.
void toRGB24(const int *blk, uint8_t *image, bool mono = false);
void toRGB15(const int *blk, uint16_t *image, bool mono = false, bool stp = false);

struct FrameStatus {
    Status status;              // the first anomaly, which stops the frame
    unsigned mbX = 0, mbY = 0;  // the macroblock it happened in
    unsigned consumed = 0;      // halfwords read
};

// A whole width x height frame (multiples of 16) to 24-bit pixels, rows packed,
// from `words` halfwords of stream. Macroblocks run across then down, or down
// then across with columnMajor, which is the order retail STR uses. Stops at the
// first anomaly, leaving the rest of the frame untouched.
FrameStatus decodeFrame(const uint16_t *rl, unsigned words, const Tables &tables, unsigned width, unsigned height,
                        bool columnMajor, uint8_t *rgb24);

}  // namespace MDECDecoder

}  // namespace PCSX
