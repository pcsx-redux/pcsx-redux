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
#include "core/mdec-decoder.h"

#include <string.h>

#include <algorithm>
#include <bit>
#include <utility>

#include "supportpsx/dct.h"

using PCSX::MDECDecoder::DSIZE;
using PCSX::MDECDecoder::DSIZE2;

// The stream and the 15-bit pixels are little-endian wherever they live: guest
// memory, a file. The scale and quant tables are host order by the time they get
// here.
static inline uint16_t SWAP_LE16(uint16_t v) {
    if constexpr (std::endian::native == std::endian::big) return static_cast<uint16_t>((v >> 8) | (v << 8));
    return v;
}

static const int zscan[DSIZE2] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,   // 00
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,  // 10
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,  // 20
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,  // 30
};

static const int aanscales[DSIZE2] = {
    1048576, 1454417, 1370031, 1232995, 1048576, 823861,  567485, 289301,  // 00
    1454417, 2017334, 1900287, 1710213, 1454417, 1142728, 787125, 401273,  // 08
    1370031, 1900287, 1790031, 1610986, 1370031, 1076426, 741455, 377991,  // 10
    1232995, 1710213, 1610986, 1449849, 1232995, 968758,  667292, 340183,  // 18
    1048576, 1454417, 1370031, 1232995, 1048576, 823861,  567485, 289301,  // 20
    823861,  1142728, 1076426, 968758,  823861,  647303,  445870, 227303,  // 28
    567485,  787125,  741455,  667292,  567485,  445870,  307121, 156569,  // 30
    289301,  401273,  377991,  340183,  289301,  227303,  156569, 79818    // 38
};

#define AAN_CONST_BITS 12
#define AAN_PRESCALE_BITS 16

#define AAN_CONST_SIZE 24
#define AAN_CONST_SCALE (AAN_CONST_SIZE - AAN_CONST_BITS)

#define AAN_PRESCALE_SIZE 20
#define AAN_PRESCALE_SCALE (AAN_PRESCALE_SIZE - AAN_PRESCALE_BITS)
#define AAN_EXTRA 12

#define SCALE(x, n) ((x) >> (n))
#define SCALER(x, n) (((x) + ((1 << (n)) >> 1)) >> (n))

#define MULS(var, const) (SCALE((var) * (const), AAN_CONST_BITS))

#define RLE_RUN(a) ((a) >> 10)
#define RLE_VAL(a) (((int)(a) << (sizeof(int) * 8 - 10)) >> (sizeof(int) * 8 - 10))

#define FIX_1_082392200 SCALER(18159528, AAN_CONST_SCALE)  // B6
#define FIX_1_414213562 SCALER(23726566, AAN_CONST_SCALE)  // A4
#define FIX_1_847759065 SCALER(31000253, AAN_CONST_SCALE)  // A2
#define FIX_2_613125930 SCALER(43840978, AAN_CONST_SCALE)  // B2

static inline void fillcol(int *blk, int val) {
    blk[0 * DSIZE] = blk[1 * DSIZE] = blk[2 * DSIZE] = blk[3 * DSIZE] = blk[4 * DSIZE] = blk[5 * DSIZE] =
        blk[6 * DSIZE] = blk[7 * DSIZE] = val;
}

static inline void fillrow(int *blk, int val) {
    blk[0] = blk[1] = blk[2] = blk[3] = blk[4] = blk[5] = blk[6] = blk[7] = val;
}

static void idct(int *block, int used_col) {
    int tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int z5, z10, z11, z12, z13;
    int *ptr;

    // the block has only the DC coefficient
    if (used_col == -1) {
        int v = block[0];
        for (int i = 0; i < DSIZE2; i++) block[i] = v;
        return;
    }

    // last_col keeps track of the highest column with non zero coefficients
    ptr = block;
    for (int i = 0; i < DSIZE; i++, ptr++) {
        if ((used_col & (1 << i)) == 0) {
            // the column is empty or has only the DC coefficient
            if (ptr[DSIZE * 0]) {
                fillcol(ptr, ptr[0]);
                used_col |= (1 << i);
            }
            continue;
        }

        // further optimization could be made by keeping track of
        // last_row in rl2blk
        z10 = ptr[DSIZE * 0] + ptr[DSIZE * 4];  // s04
        z11 = ptr[DSIZE * 0] - ptr[DSIZE * 4];  // d04
        z13 = ptr[DSIZE * 2] + ptr[DSIZE * 6];  // s26
        z12 = MULS(ptr[DSIZE * 2] - ptr[DSIZE * 6], FIX_1_414213562) - z13;
        //^^^^  d26=d26*2*A4-s26

        tmp0 = z10 + z13;  // os07 = s04 + s26
        tmp3 = z10 - z13;  // os34 = s04 - s26
        tmp1 = z11 + z12;  // os16 = d04 + d26
        tmp2 = z11 - z12;  // os25 = d04 - d26

        z13 = ptr[DSIZE * 3] + ptr[DSIZE * 5];  // s53
        z10 = ptr[DSIZE * 3] - ptr[DSIZE * 5];  //-d53
        z11 = ptr[DSIZE * 1] + ptr[DSIZE * 7];  // s17
        z12 = ptr[DSIZE * 1] - ptr[DSIZE * 7];  // d17

        tmp7 = z11 + z13;  // od07 = s17 + s53

        z5 = (z12 - z10) * (FIX_1_847759065);
        tmp6 = SCALE(z10 * (FIX_2_613125930) + z5, AAN_CONST_BITS) - tmp7;
        tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
        tmp4 = SCALE(z12 * (FIX_1_082392200)-z5, AAN_CONST_BITS) + tmp5;

        // path #1
        // z5 = (z12 - z10)* FIX_1_847759065;
        // tmp0 = (d17 + d53) * 2*A2

        // tmp6 = DESCALE(z10*FIX_2_613125930 + z5, CONST_BITS) - tmp7;
        // od16 = (d53*-2*B2 + tmp0) - od07

        // tmp4 = DESCALE(z12*FIX_1_082392200 - z5, CONST_BITS) + tmp5;
        // od34 = (d17*2*B6 - tmp0) + od25

        // path #2

        // od34 = d17*2*(B6-A2) - d53*2*A2
        // od16 = d53*2*(A2-B2) + d17*2*A2

        // end

        //    tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
        // od25 = (s17 - s53)*2*A4 - od16

        ptr[DSIZE * 0] = (tmp0 + tmp7);  // os07 + od07
        ptr[DSIZE * 7] = (tmp0 - tmp7);  // os07 - od07
        ptr[DSIZE * 1] = (tmp1 + tmp6);  // os16 + od16
        ptr[DSIZE * 6] = (tmp1 - tmp6);  // os16 - od16
        ptr[DSIZE * 2] = (tmp2 + tmp5);  // os25 + od25
        ptr[DSIZE * 5] = (tmp2 - tmp5);  // os25 - od25
        ptr[DSIZE * 4] = (tmp3 + tmp4);  // os34 + od34
        ptr[DSIZE * 3] = (tmp3 - tmp4);  // os34 - od34
    }

    ptr = block;
    if (used_col == 1) {
        for (int i = 0; i < DSIZE; i++) fillrow(block + DSIZE * i, block[DSIZE * i]);
    } else {
        for (int i = 0; i < DSIZE; i++, ptr += DSIZE) {
            z10 = ptr[0] + ptr[4];
            z11 = ptr[0] - ptr[4];
            z13 = ptr[2] + ptr[6];
            z12 = MULS(ptr[2] - ptr[6], FIX_1_414213562) - z13;

            tmp0 = z10 + z13;
            tmp3 = z10 - z13;
            tmp1 = z11 + z12;
            tmp2 = z11 - z12;

            z13 = ptr[3] + ptr[5];
            z10 = ptr[3] - ptr[5];
            z11 = ptr[1] + ptr[7];
            z12 = ptr[1] - ptr[7];

            tmp7 = z11 + z13;
            z5 = (z12 - z10) * FIX_1_847759065;
            tmp6 = SCALE(z10 * FIX_2_613125930 + z5, AAN_CONST_BITS) - tmp7;
            tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
            tmp4 = SCALE(z12 * FIX_1_082392200 - z5, AAN_CONST_BITS) + tmp5;

            ptr[0] = tmp0 + tmp7;

            ptr[7] = tmp0 - tmp7;
            ptr[1] = tmp1 + tmp6;
            ptr[6] = tmp1 - tmp6;
            ptr[2] = tmp2 + tmp5;
            ptr[5] = tmp2 - tmp5;
            ptr[4] = tmp3 + tmp4;
            ptr[3] = tmp3 - tmp4;
        }
    }
}

// The scale matrix every known PSX game uploads, straight out of psx-spx's
// set_scale_table section. Signed halfwords, 14 fractional bits.
static const int16_t c_standardScaleTable[DSIZE2] = {
    (int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82,
    (int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x7D8A, (int16_t)0x6A6D, (int16_t)0x471C, (int16_t)0x18F8,
    (int16_t)0xE707, (int16_t)0xB8E3, (int16_t)0x9592, (int16_t)0x8275, (int16_t)0x7641, (int16_t)0x30FB,
    (int16_t)0xCF04, (int16_t)0x89BE, (int16_t)0x89BE, (int16_t)0xCF04, (int16_t)0x30FB, (int16_t)0x7641,
    (int16_t)0x6A6D, (int16_t)0xE707, (int16_t)0x8275, (int16_t)0xB8E3, (int16_t)0x471C, (int16_t)0x7D8A,
    (int16_t)0x18F8, (int16_t)0x9592, (int16_t)0x5A82, (int16_t)0xA57D, (int16_t)0xA57D, (int16_t)0x5A82,
    (int16_t)0x5A82, (int16_t)0xA57D, (int16_t)0xA57D, (int16_t)0x5A82, (int16_t)0x471C, (int16_t)0x8275,
    (int16_t)0x18F8, (int16_t)0x6A6D, (int16_t)0x9592, (int16_t)0xE707, (int16_t)0x7D8A, (int16_t)0xB8E3,
    (int16_t)0x30FB, (int16_t)0x89BE, (int16_t)0x7641, (int16_t)0xCF04, (int16_t)0xCF04, (int16_t)0x7641,
    (int16_t)0x89BE, (int16_t)0x30FB, (int16_t)0x18F8, (int16_t)0xB8E3, (int16_t)0x6A6D, (int16_t)0x8275,
    (int16_t)0x7D8A, (int16_t)0x9592, (int16_t)0x471C, (int16_t)0xE707,
};

// The dequantizer follows psx-spx's rl_decode_block, and every clause of it has
// now been checked against real consoles with an arm that varies ONE term. Five
// of those arms are bit-exact, 0 of 768 bytes differing: the q_scale == 0 DC rule
// and its ordinary-route control, both zigzag arms, and the DC-saturation arm.
// The rig is src/mips/tests/mdec/roundtrip, one arm per clause, and genjob.py
// documents what each one holds fixed.
//
// What is left is NOT in this function. Every single-term arm's residual is
// exactly +1 on the quadrants whose flat level is an exact .5 and exact
// everywhere else, because the MDEC rounds a tie DOWN and SCALER here rounds it
// up: measured 153.00 -> 153, 153.25 -> 153, 153.75 -> 154, 165.50 -> 165, which
// is round-half-down and not floor. Separately, arms driven far out of range keep
// isolated bytes where silicon reads 0 and this reads 255 - a wrap where we clamp,
// in the colour conversion, which psx-spx does not document at all (yuv_to_rgb is
// called four times on that page and never defined). Both are output-stage
// findings with their own arms still to build; neither is a dequant bug.
//
// Regression control for anything touched here: arm C, the ordinary path, must
// stay bit-identical. It is 332/768 max 2 against hardware and was 0/768 against
// the previous build across this change. The first attempt at the saturation
// clamped in the divided domain, which cost 407 extra differing bytes on an
// ordinary frame while the SATURATING arm appeared to improve - grade on C, never
// on the arm the change was written for.
//
// psx-spx saturates the dequantized value to signed 11 bits. The fast path works
// in an UN-DIVIDED domain: its expression is RLE_VAL*qt*q_scale with the /8 folded
// into the AAN normalisation, so the clamp has to be applied to the spec's value
// while the ORIGINAL expression is what reaches the IDCT. Clamping the divided
// value and multiplying the prescale onto that instead is an 8x error, and it
// costs 407 extra differing bytes against hardware on an ordinary frame - the
// regression control is the only thing that sees it, because the saturating case
// still looks like it improved.
static inline int saturateAanAc(int x) {
    const int v = (x + 4) / 8;
    if (v > 0x3ff) return 0x3ff * 8;
    if (v < -0x400) return -0x400 * 8;
    return x;
}

const int16_t *PCSX::MDECDecoder::standardScaleTable() { return c_standardScaleTable; }

void PCSX::MDECDecoder::Tables::setQuant(const uint8_t *y, const uint8_t *uv) {
    prescaleQuantTable(iqY, y);
    prescaleQuantTable(iqUV, uv);
    memcpy(qtY, y, sizeof(qtY));
    memcpy(qtUV, uv, sizeof(qtUV));
}

void PCSX::MDECDecoder::Tables::setScale(const int16_t *table) {
    memcpy(scale, table, sizeof(scale));
    customScale = memcmp(scale, c_standardScaleTable, sizeof(scale)) != 0;
}

void PCSX::MDECDecoder::Tables::clearScale() {
    memset(scale, 0, sizeof(scale));
    customScale = true;  // an all-zero matrix is not the standard one
}

void PCSX::MDECDecoder::Tables::setStandard() {
    // The standard quant table is what nearly every game uploads.
    memcpy(qtY, PCSX::DCT::standardQuantTable(), sizeof(qtY));
    memcpy(qtUV, PCSX::DCT::standardQuantTable(), sizeof(qtUV));
    memcpy(scale, c_standardScaleTable, sizeof(scale));
    customScale = false;
}

// psx-spx real_idct_core. dst = src * scaletable with src diagonally mirrored,
// two passes with src/dst swapped. 1024 multiplications, and the hardware has no
// idea the table contains cosines: any matrix uploaded here is faithfully applied.
//
// NOTE the hardware only uses the upper 13 bits of each 16-bit table entry, and
// psx-spx itself says of this pseudocode that "the results aren't perfect" and
// that the real rounding points are not known. So this is the documented model,
// not a bit-exact hardware model, and a hardware roundtrip is the only thing that
// can say how far off it is.
static void real_idct(int *block, const int16_t *scaletable) {
    int temp[DSIZE2];
    int *src = block;
    int *dst = temp;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++) {
                    sum += static_cast<int64_t>(src[y + z * 8]) * (scaletable[x + z * 8] >> 3);
                }
                int v = static_cast<int>((sum + 0xfff) >> 13);
                // The second pass leaves psx-spx's pixel-domain result, but the
                // colour conversion downstream expects the AAN path's domain,
                // which carries a 2^10 prescale: MULR/MULB/MULG2 are 1024-fixed
                // point and SCALE8 shifts by 20. Without this the general path
                // hands yuv2rgb values about 1024x too small and every macroblock
                // collapses to flat mid-grey - which reads exactly like the scale
                // table being ignored, and is not.
                if (pass == 1) v *= 1024;
                dst[x + y * 8] = v;
            }
        }
        std::swap(src, dst);
    }
    // Two swaps put the result back in `block` already when passes are even; be
    // explicit rather than relying on it.
    if (src != block) memcpy(block, src, sizeof(temp));
}

void PCSX::MDECDecoder::prescaleQuantTable(int *iqtab, const uint8_t *qt) {
    for (unsigned i = 0; i < DSIZE2; i++) {
        iqtab[i] = (qt[i] * SCALER(aanscales[zscan[i]], AAN_PRESCALE_SCALE));
    }
}

void PCSX::MDECDecoder::quantTableFromPrescaled(uint8_t *qt, const int *iqtab) {
    for (unsigned i = 0; i < DSIZE2; i++) {
        qt[i] = static_cast<uint8_t>(iqtab[i] / SCALER(aanscales[zscan[i]], AAN_PRESCALE_SCALE));
    }
}

#define MDEC_END_OF_DATA 0xfe00

const uint16_t *PCSX::MDECDecoder::decodeMacroblock(int *blk, const uint16_t *mdec_rl, const Tables &tables,
                                                    const uint16_t *end, Status *status) {
    int k, q_scale, rl, used_col;
    const int *iqtab;

    // The bound and the anomaly report are for callers decoding a file. With no
    // bound and no anomaly this is the emulator's walk, read for read.
    auto report = [status](Anomaly anomaly, int block, bool atHeader) {
        if (status && status->anomaly == Anomaly::None) *status = {anomaly, static_cast<unsigned>(block), atHeader};
    };
    auto exhausted = [&mdec_rl, end]() { return end && mdec_rl >= end; };

    memset(blk, 0, BLOCKS * DSIZE2 * sizeof(int));
    iqtab = tables.iqUV;
    const uint8_t *qtab = tables.qtUV;
    for (int i = 0; i < 6; i++) {
        // decode blocks (Cr,Cb,Y1,Y2,Y3,Y4)
        if (i == 2) {
            iqtab = tables.iqY;
            qtab = tables.qtY;
        }

        if (tables.customScale) {
            // General path: no AAN prescale in the dequantized values, saturation
            // to signed 11 bits per psx-spx's rl_decode_block, and the full matrix
            // multiply afterwards. Slower by construction; only taken when
            // something has actually uploaded a non-standard matrix.
            if (exhausted()) {
                report(Anomaly::Exhausted, i, true);
                return mdec_rl;
            }
            rl = SWAP_LE16(*mdec_rl);
            mdec_rl++;
            q_scale = RLE_RUN(rl);
            // The q_scale == 0 clauses bind here too, and their absence is why
            // running this mode through the general path scored no better than
            // through the fast one - both were wrong about the same two things.
            blk[0] = std::clamp(q_scale == 0 ? RLE_VAL(rl) * 2 : RLE_VAL(rl) * qtab[0], -0x400, 0x3ff);
            for (k = 0;;) {
                if (exhausted()) {
                    report(Anomaly::Exhausted, i, false);
                    return mdec_rl;
                }
                rl = SWAP_LE16(*mdec_rl);
                mdec_rl++;
                if (rl == MDEC_END_OF_DATA) break;
                k += RLE_RUN(rl) + 1;
                if (k > 63) {
                    report(Anomaly::RunPast63, i, false);
                    break;
                }
                const int spec = q_scale == 0 ? RLE_VAL(rl) * 2 : (RLE_VAL(rl) * qtab[k] * q_scale + 4) / 8;
                blk[q_scale == 0 ? k : zscan[k]] = std::clamp(spec, -0x400, 0x3ff);
            }
            real_idct(blk, tables.scale);
            blk += DSIZE2;
            continue;
        }

        if (exhausted()) {
            report(Anomaly::Exhausted, i, true);
            return mdec_rl;
        }
        rl = SWAP_LE16(*mdec_rl);
        mdec_rl++;
        q_scale = RLE_RUN(rl);
        {
            // psx-spx's DC term. Every clause here is MEASURED ON SILICON rather
            // than taken from the document on trust, one hardware arm per clause:
            //   val = signed10bit * qt[0], with NO q_scale and NO /8. Arms D8 and
            //     D63 hold everything else and decode byte-identical at q_scale 8
            //     and 63, which settles the "(?)" psx-spx prints on that line.
            //   q_scale == 0 gives val = signed10bit * 2 with no quant table at
            //     all. Arm ZDC against ZDCC, which reaches the same value by the
            //     ordinary route, differ 0/768.
            //   val = minmax(val, -400h, +3FFh). Arm DSAT2 puts two DCs at 1024
            //     and 2044 on a HALVED basis, where the clamp bites before the
            //     8-bit output rail does, and they decode byte-identical. At the
            //     standard basis this is unobservable: a flat block decodes to
            //     128 + val/8, so the clamp at 1023 and the output rail at 1024
            //     are one LSB apart and no DC-only arm can separate them.
            // The clamp lives in the spec's divided domain while this path works
            // in an un-divided one, so the ORIGINAL expression is what is returned
            // when nothing saturates - that keeps the ordinary case bit-identical.
            const int dc = RLE_VAL(rl);
            const int spec = (q_scale == 0) ? dc * 2 : dc * qtab[0];
            const int sat = std::clamp(spec, -0x400, 0x3ff);
            blk[0] = (q_scale != 0 && sat == spec)
                         ? SCALER(iqtab[0] * dc, AAN_EXTRA - 3)
                         : SCALER(sat * SCALER(aanscales[0], AAN_PRESCALE_SCALE), AAN_EXTRA - 3);
        }
        for (k = 0, used_col = 0;;) {
            if (exhausted()) {
                report(Anomaly::Exhausted, i, false);
                return mdec_rl;
            }
            rl = SWAP_LE16(*mdec_rl);
            mdec_rl++;
            if (rl == MDEC_END_OF_DATA) break;
            k += RLE_RUN(rl) + 1;  // skip zero-coefficients

            if (k > 63) {
                report(Anomaly::RunPast63, i, false);
                break;
            }

            // zigzag transformation. q_scale == 0 selects psx-spx's mode with no
            // quant table, a doubled value and NO zigzag; the *8 puts it in this
            // path's un-divided domain. When nothing saturates, the normal branch
            // is bit-identical to what was here before.
            const int dest = q_scale == 0 ? k : zscan[k];
            if (q_scale == 0) {
                // MEASURED: arm ZAC puts one coefficient at k = 2 with q_scale 0
                // and silicon decodes a horizontal frequency-2 basis, i.e. block
                // position 2; the matched-magnitude control ZACC at q_scale 1
                // decodes a vertical frequency-1 basis, position zscan[2] = 8. So
                // psx-spx's "no zigzag" is right - and the AAN prescale has to
                // follow the value to its DESTINATION. iqtab[k] carries
                // aanscales[zscan[k]], which is the factor for the slot this mode
                // specifically does not use, so it cannot be reused here. The *8
                // puts the value in this path's un-divided domain, and psx-spx's
                // clamp is a no-op in this mode because signed10bit*2 spans
                // [-1024, +1022], inside [-400h, +3FFh] at both ends.
                blk[dest] = SCALER(RLE_VAL(rl) * 2 * 8 * SCALER(aanscales[k], AAN_PRESCALE_SCALE), AAN_EXTRA);
            } else {
                blk[dest] = SCALER(
                    saturateAanAc(RLE_VAL(rl) * qtab[k] * q_scale) * (iqtab[k] / (qtab[k] ? qtab[k] : 1)), AAN_EXTRA);
            }
            // keep track of used columns to speed up the idtc
            used_col |= (dest > 7) ? 1 << (dest & 7) : 0;
        }

        if (k == 0) used_col = -1;
        // used_col is -1 for blocks with only the DC coefficient
        // any other value is a bitmask of the columns that have
        // at least one non zero cofficient in the rows 1-7
        // single coefficients in row 0 are treted specially
        // in the idtc function
        idct(blk, used_col);
        blk += DSIZE2;
    }
    return mdec_rl;
}

// The colour matrix, at 2^10. Measured on silicon with flat-chroma captures (one chroma plane per
// capture, four luma levels), each chroma plane taken between 37 and 101 in magnitude and of both
// signs. Under the
// MDEC's round-half-down output these fit, at 1/1024: R 1435-1438, G from Cr 729-733, G from Cb
// 350-359, B 1817-1819. B is about 1.7754, not the JPEG 1.772: at Cb +64 the console adds 114 to
// blue, and 1.772 gives 113.4. 728 for G from Cr lands exactly on 45.5 at plane 64, where half-down
// gives 45 and the console 46. The coarsest grid that fits every term is 1/512, so the hardware
// carries at least 9 fractional bits here; its actual width is not measured.
#define MULR(a) ((1436 * (a)))
#define MULB(a) ((1818 * (a)))
#define MULG2(a, b) ((-352 * (a) - 731 * (b)))
#define MULY(a) ((a) << 10)

#define MAKERGB15(r, g, b, a) (SWAP_LE16(a | ((b) << 10) | ((g) << 5) | (r)))
// MEASURED: the MDEC rounds to nearest with TIES GOING DOWN. Arm D8F puts four
// flat quadrants at exactly 153.00, 153.25, 153.75 and 165.50; silicon returns
// 153, 153, 154, 165 - so .75 rounds up and .50 rounds down, which is
// round-half-down and NOT floor (floor would have given 153 for the .75 quadrant).
// SCALER adds half and shifts, i.e. rounds ties up. Subtracting 1 from the bias
// moves the tie and leaves every other fraction alone.
//
// SCOPE, and it is narrower than it first looked. This takes the flat-field DC
// arms D8, D63 and D8F from 384/768 each to 0/768, and is a byte-for-byte no-op on
// arms A, B, C, D, S and Z - ordinary content with AC energy essentially never
// lands on an exact tie. ⛔ It does NOT account for the residual on DSAT2 (192,
// max 1) or ASAT (96, max 1), which are unchanged by it: DSAT2 runs the general
// matrix path, whose own `(sum + 0xfff) >> 13` is a separate rounding stage, and
// ASAT's ties arise inside the AAN butterfly rather than here. Those are a
// different measurement and have not been made. An earlier revision of this
// comment claimed this one bit was the whole residual on every single-term arm;
// that was written before the confirming sweep finished and it was wrong.
#define SCALE8(c) ((((c) + (1 << 19)) - 1) >> 20)
// ⚠ DELIBERATELY still SCALER, i.e. still rounds ties UP, and the asymmetry with
// SCALE8 above is unfinished rather than verified. Every arm of the roundtrip rig
// requests 24bpp, so nothing has driven this path on silicon and I would rather it
// look unmeasured than look checked. The 15-bit arm is a rig change (depth 3 in the
// command word, half the output bytes) plus one farm run.
#define SCALE5(c) SCALER(c, 23)

#define CLAMP5(c) (((c) < -16) ? 0 : (((c) > (31 - 16)) ? 31 : ((c) + 16)))
// MEASURED: the output stage masks to signed 9 bits BEFORE it saturates, so a
// value that overruns the range comes out BLACK rather than white.
//
// psx-spx spells this out for y_to_mono - `Y = Y AND 1FFh ;clip to signed 9bit
// range` ahead of `MinMax(-128,127,Y)` - and says of yuv_to_rgb only that
// "there's probably also some 9bit limit (similar as in y_to_mono)". There is.
// Arm WRAP of the roundtrip rig puts a clamped DC and a clamped AC in one block so
// the bright end of the quadrant crosses 256 (a DC-only arm cannot get there: the
// 11-bit dequant clamp caps Y at 127.875). One row, silicon against this emulator
// before the fix:
//
//   hardware    0    0  255  255  221  155  105   78
//   emulator  255  255  255  255  221  155  106   79
//
// The two BRIGHTEST pixels read 0 on hardware. Clamping cannot put a dark band
// inside a bright region, so this is a wrap and not a rounding difference. It is
// also the whole of the remaining max-255 residual on the out-of-range arms.
//
// CLAMP5, the 15-bit sibling, is deliberately left alone: every arm of the rig
// requests 24bpp, so the equivalent limit on that path is unmeasured here.
static inline uint8_t clamp8(int c) {
    const int v = ((c & 0x1ff) ^ 0x100) - 0x100;  // sign-extend the low 9 bits
    if (v < -128) return 0;
    if (v > 127) return 255;
    return static_cast<uint8_t>(v + 128);
}
#define CLAMP8(c) (clamp8(c))

#define CLAMP_SCALE8(a) (CLAMP8(SCALE8(a)))
#define CLAMP_SCALE5(a) (CLAMP5(SCALE5(a)))

static inline void putlinebw15(uint16_t *image, const int *Yblk, int A) {
    for (int i = 0; i < 8; i++, Yblk++) {
        int Y = *Yblk;
        // missing rounding
        image[i] = SWAP_LE16((CLAMP5(Y >> 3) * 0x421) | A);
    }
}

static inline void putquadrgb15(uint16_t *image, const int *Yblk, int Cr, int Cb, int A) {
    int Y, R, G, B;
    R = MULR(Cr);
    G = MULG2(Cb, Cr);
    B = MULB(Cb);

    // added transparency
    Y = MULY(Yblk[0]);
    image[0] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
    Y = MULY(Yblk[1]);
    image[1] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
    Y = MULY(Yblk[8]);
    image[16] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
    Y = MULY(Yblk[9]);
    image[17] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
}

void PCSX::MDECDecoder::toRGB15(const int *blk, uint16_t *image, bool mono, bool stp) {
    const int A = stp ? 0x8000 : 0;
    const int *Yblk = blk + DSIZE2 * 2;
    const int *Crblk = blk;
    const int *Cbblk = blk + DSIZE2;

    if (!mono) {
        for (int y = 0; y < 16; y += 2, Crblk += 4, Cbblk += 4, Yblk += 8, image += 24) {
            if (y == 8) Yblk += DSIZE2;
            for (int x = 0; x < 4; x++, image += 2, Crblk++, Cbblk++, Yblk += 2) {
                putquadrgb15(image, Yblk, *Crblk, *Cbblk, A);
                putquadrgb15(image + 8, Yblk + DSIZE2, *(Crblk + 4), *(Cbblk + 4), A);
            }
        }
    } else {
        for (int y = 0; y < 16; y++, Yblk += 8, image += 16) {
            if (y == 8) Yblk += DSIZE2;
            putlinebw15(image, Yblk, A);
            putlinebw15(image + 8, Yblk + DSIZE2, A);
        }
    }
}

static inline void putlinebw24(uint8_t *image, const int *Yblk) {
    for (int i = 0; i < 8 * 3; i += 3, Yblk++) {
        uint8_t Y = CLAMP8(*Yblk);
        image[i + 0] = Y;
        image[i + 1] = Y;
        image[i + 2] = Y;
    }
}

static inline void putquadrgb24(uint8_t *image, const int *Yblk, int Cr, int Cb) {
    int Y, R, G, B;

    R = MULR(Cr);
    G = MULG2(Cb, Cr);
    B = MULB(Cb);

    Y = MULY(Yblk[0]);
    image[0 * 3 + 0] = CLAMP_SCALE8(Y + R);
    image[0 * 3 + 1] = CLAMP_SCALE8(Y + G);
    image[0 * 3 + 2] = CLAMP_SCALE8(Y + B);
    Y = MULY(Yblk[1]);
    image[1 * 3 + 0] = CLAMP_SCALE8(Y + R);
    image[1 * 3 + 1] = CLAMP_SCALE8(Y + G);
    image[1 * 3 + 2] = CLAMP_SCALE8(Y + B);
    Y = MULY(Yblk[8]);
    image[16 * 3 + 0] = CLAMP_SCALE8(Y + R);
    image[16 * 3 + 1] = CLAMP_SCALE8(Y + G);
    image[16 * 3 + 2] = CLAMP_SCALE8(Y + B);
    Y = MULY(Yblk[9]);
    image[17 * 3 + 0] = CLAMP_SCALE8(Y + R);
    image[17 * 3 + 1] = CLAMP_SCALE8(Y + G);
    image[17 * 3 + 2] = CLAMP_SCALE8(Y + B);
}

void PCSX::MDECDecoder::toRGB24(const int *blk, uint8_t *image, bool mono) {
    const int *Yblk = blk + DSIZE2 * 2;
    const int *Crblk = blk;
    const int *Cbblk = blk + DSIZE2;

    if (!mono) {
        for (int y = 0; y < 16; y += 2, Crblk += 4, Cbblk += 4, Yblk += 8, image += 8 * 3 * 3) {
            if (y == 8) Yblk += DSIZE2;
            for (int x = 0; x < 4; x++, image += 6, Crblk++, Cbblk++, Yblk += 2) {
                putquadrgb24(image, Yblk, *Crblk, *Cbblk);
                putquadrgb24(image + 8 * 3, Yblk + DSIZE2, *(Crblk + 4), *(Cbblk + 4));
            }
        }
    } else {
        for (int y = 0; y < 16; y++, Yblk += 8, image += 16 * 3) {
            if (y == 8) Yblk += DSIZE2;
            putlinebw24(image, Yblk);
            putlinebw24(image + 8 * 3, Yblk + DSIZE2);
        }
    }
}

PCSX::MDECDecoder::FrameStatus PCSX::MDECDecoder::decodeFrame(const uint16_t *rl, unsigned words, const Tables &tables,
                                                              unsigned width, unsigned height, bool columnMajor,
                                                              uint8_t *rgb24) {
    FrameStatus frame;
    const uint16_t *pos = rl;
    const uint16_t *end = rl + words;
    const unsigned mbx = width / 16, mby = height / 16;
    // decodeMacroblock reads a null end as "no bound", and an empty stream can
    // hand us a null rl, so an empty input is reported here before any read.
    if (words == 0 && mbx != 0 && mby != 0) {
        frame.status = {Anomaly::Exhausted, 0, true};
        return frame;
    }
    for (unsigned outer = 0; outer < (columnMajor ? mbx : mby); outer++) {
        for (unsigned inner = 0; inner < (columnMajor ? mby : mbx); inner++) {
            const unsigned mx = columnMajor ? outer : inner;
            const unsigned my = columnMajor ? inner : outer;
            int blk[BLOCKS * DSIZE2];
            uint8_t pixels[16 * 16 * 3];
            pos = decodeMacroblock(blk, pos, tables, end, &frame.status);
            frame.consumed = static_cast<unsigned>(pos - rl);
            if (frame.status.anomaly != Anomaly::None) {
                frame.mbX = mx;
                frame.mbY = my;
                return frame;
            }
            toRGB24(blk, pixels);
            for (unsigned py = 0; py < 16; py++) {
                memcpy(rgb24 + ((my * 16 + py) * width + mx * 16) * 3, pixels + py * 16 * 3, 16 * 3);
            }
        }
    }
    return frame;
}
