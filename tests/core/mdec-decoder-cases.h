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

// A fixed set of one-macroblock MDEC jobs: tables plus a run-level stream. Each
// case is shaped like a job of src/mips/tests/mdec/roundtrip, so the very same
// input can be pushed through the emulator's MDEC by a booted guest and through
// the decoder in-process, and the two compared byte for byte.
//
// The goldens in mdec-decoder.cc were captured from the emulator BEFORE its
// decoder moved into src/core/mdec-decoder.cc, so this list is frozen: changing a case changes
// its stream and invalidates its golden. Add cases at the end, with new goldens.

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

namespace MDECDecoderCases {

struct Case {
    std::string name;  // fits the rig's 12-byte arm field
    uint8_t quant[128];  // Y then UV, MDEC(2) order
    bool uploadScale;
    int16_t scale[64];  // MDEC(3)
    std::vector<uint16_t> rl;
};

// The scale matrix every shipping title uploads.
static constexpr int16_t c_stdScale[64] = {
    0x5A82,          0x5A82,          0x5A82,          0x5A82,          0x5A82,          0x5A82,
    0x5A82,          0x5A82,          0x7D8A,          0x6A6D,          0x471C,          0x18F8,
    (int16_t)0xE707, (int16_t)0xB8E3, (int16_t)0x9592, (int16_t)0x8275, 0x7641,          0x30FB,
    (int16_t)0xCF04, (int16_t)0x89BE, (int16_t)0x89BE, (int16_t)0xCF04, 0x30FB,          0x7641,
    0x6A6D,          (int16_t)0xE707, (int16_t)0x8275, (int16_t)0xB8E3, 0x471C,          0x7D8A,
    0x18F8,          (int16_t)0x9592, 0x5A82,          (int16_t)0xA57D, (int16_t)0xA57D, 0x5A82,
    0x5A82,          (int16_t)0xA57D, (int16_t)0xA57D, 0x5A82,          0x471C,          (int16_t)0x8275,
    0x18F8,          0x6A6D,          (int16_t)0x9592, (int16_t)0xE707, 0x7D8A,          (int16_t)0xB8E3,
    0x30FB,          (int16_t)0x89BE, 0x7641,          (int16_t)0xCF04, (int16_t)0xCF04, 0x7641,
    (int16_t)0x89BE, 0x30FB,          0x18F8,          (int16_t)0xB8E3, 0x6A6D,          (int16_t)0x8275,
    0x7D8A,          (int16_t)0x9592, 0x471C,          (int16_t)0xE707,
};

// The standard quant table in upload (zigzag) order, as DCT::standardQuantTable()
// returns it. Spelled out so the cases do not move if that function ever does.
static constexpr uint8_t c_stdQuant[64] = {
    2,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
    27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
    37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
};

// Deterministic, platform-independent.
struct Lcg {
    uint32_t s;
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

inline uint16_t header(int qscale, int dc) { return static_cast<uint16_t>((qscale << 10) | (dc & 0x3ff)); }
inline uint16_t ac(int run, int val) { return static_cast<uint16_t>((run << 10) | (val & 0x3ff)); }

struct Block {
    int qscale;
    int dc;
    std::vector<std::pair<int, int>> acs;  // (run, value)
};

inline Case makeCase(const std::string &name, const uint8_t *qy, const uint8_t *quv, const int16_t *scale,
                     const Block (&blocks)[6]) {
    Case c;
    c.name = name;
    memcpy(c.quant, qy, 64);
    memcpy(c.quant + 64, quv, 64);
    c.uploadScale = scale != nullptr;
    if (scale) {
        memcpy(c.scale, scale, sizeof(c.scale));
    } else {
        memset(c.scale, 0, sizeof(c.scale));
    }
    for (auto &b : blocks) {
        c.rl.push_back(header(b.qscale, b.dc));
        for (auto &[run, val] : b.acs) c.rl.push_back(ac(run, val));
        c.rl.push_back(0xfe00);
    }
    return c;
}

inline std::vector<std::pair<int, int>> fullAc(Lcg &r, int lo, int hi) {
    std::vector<std::pair<int, int>> v;
    for (int k = 1; k < 64; k++) v.push_back({0, r.range(lo, hi)});
    return v;
}

inline std::vector<std::pair<int, int>> sparseAc(Lcg &r, int lo, int hi) {
    std::vector<std::pair<int, int>> v;
    int k = 0;
    for (;;) {
        const int run = r.range(0, 9);
        if (k + run + 1 > 63) break;
        k += run + 1;
        int val = r.range(lo, hi);
        if (val == 0) val = 1;
        v.push_back({run, val});
    }
    return v;
}

// The rig takes A-Z and 0-9 only.
inline std::string pointName(const char *prefix, int v) {
    return std::string(prefix) + (v < 0 ? "N" : "P") + std::to_string(v < 0 ? -v : v);
}

inline std::vector<Case> all() {
    std::vector<Case> cases;
    uint8_t flat1[64], flat8[64], flat83[64];
    memset(flat1, 1, 64);
    memset(flat8, 8, 64);
    memset(flat83, 83, 64);
    const uint8_t *sq = c_stdQuant;
    const int16_t *ss = c_stdScale;

    // DC only, standard tables, both signs, both block kinds.
    cases.push_back(makeCase("DCONLY", sq, sq, ss,
                             {{8, 10, {}}, {8, -7, {}}, {8, 100, {}}, {8, -100, {}}, {8, 37, {}}, {8, -3, {}}}));
    // DC at the ends of the 10-bit field, and the q_scale 0 doubling of them.
    cases.push_back(makeCase("DCEDGE", sq, sq, ss,
                             {{8, 511, {}}, {8, -512, {}}, {1, 511, {}}, {63, -512, {}}, {0, 511, {}}, {0, -512, {}}}));
    // Every AC present, standard tables, at three q_scales.
    {
        Lcg r{1};
        cases.push_back(makeCase("FULLAC8", sq, sq, ss,
                                 {{8, 20, fullAc(r, -40, 40)},
                                  {8, -20, fullAc(r, -40, 40)},
                                  {8, 60, fullAc(r, -40, 40)},
                                  {8, -60, fullAc(r, -40, 40)},
                                  {8, 0, fullAc(r, -40, 40)},
                                  {8, 5, fullAc(r, -40, 40)}}));
    }
    {
        Lcg r{2};
        cases.push_back(makeCase("FULLAC1", sq, sq, ss,
                                 {{1, 20, fullAc(r, -200, 200)},
                                  {1, -20, fullAc(r, -200, 200)},
                                  {1, 60, fullAc(r, -200, 200)},
                                  {1, -60, fullAc(r, -200, 200)},
                                  {1, 0, fullAc(r, -200, 200)},
                                  {1, 5, fullAc(r, -200, 200)}}));
    }
    {
        Lcg r{3};
        cases.push_back(makeCase("FULLAC63", sq, sq, ss,
                                 {{63, 20, fullAc(r, -30, 30)},
                                  {63, -20, fullAc(r, -30, 30)},
                                  {63, 60, fullAc(r, -30, 30)},
                                  {63, -60, fullAc(r, -30, 30)},
                                  {63, 0, fullAc(r, -30, 30)},
                                  {63, 5, fullAc(r, -30, 30)}}));
    }
    // q_scale 0: no table, doubled, no zigzag. Isolated positions, then everything.
    cases.push_back(makeCase("Q0", sq, sq, ss,
                             {{0, 0, {}},
                              {0, 0, {}},
                              {0, 40, {{1, 200}}},
                              {0, -40, {{0, 100}, {2, -150}}},
                              {0, 10, {{8, 300}, {20, -300}}},
                              {0, 0, {{62, 511}}}}));
    {
        Lcg r{4};
        cases.push_back(makeCase("Q0FULL", sq, sq, ss,
                                 {{0, 30, fullAc(r, -300, 300)},
                                  {0, -30, fullAc(r, -300, 300)},
                                  {0, 100, fullAc(r, -300, 300)},
                                  {0, -100, fullAc(r, -300, 300)},
                                  {0, 0, fullAc(r, -300, 300)},
                                  {0, 7, fullAc(r, -300, 300)}}));
    }
    // 11-bit saturation of DC and AC.
    cases.push_back(makeCase("SATDC", flat83, flat83, ss,
                             {{8, 400, {}}, {8, -400, {}}, {8, 500, {}}, {8, -500, {}}, {8, 13, {}}, {8, -13, {}}}));
    cases.push_back(makeCase("SATAC", flat83, flat83, ss,
                             {{63, 400, {{0, 500}, {1, -500}}},
                              {63, -400, {{0, -500}, {1, 500}}},
                              {63, 500, {{0, 500}, {1, -500}}},
                              {63, -500, {{2, 300}, {5, -511}}},
                              {63, 450, {{0, 12}, {0, -13}}},
                              {63, -450, {{10, 511}}}}));
    // General path: a scale table that is not the standard one. Standard plus one
    // in the first entry keeps the basis but leaves the AAN fast path.
    {
        int16_t s[64];
        memcpy(s, ss, sizeof(s));
        s[0] += 1;
        Lcg r{5};
        cases.push_back(makeCase("NEGSC", sq, sq, s,
                                 {{8, 20, fullAc(r, -40, 40)},
                                  {8, -20, fullAc(r, -40, 40)},
                                  {8, 60, fullAc(r, -40, 40)},
                                  {8, -60, fullAc(r, -40, 40)},
                                  {8, 0, fullAc(r, -40, 40)},
                                  {8, 5, fullAc(r, -40, 40)}}));
    }
    // Halved basis: the negative entries are not multiples of 8, so a truncating
    // /8 and a flooring >>3 of the table disagree on them.
    {
        int16_t s[64];
        for (int i = 0; i < 64; i++) s[i] = static_cast<int16_t>(ss[i] / 2);
        Lcg r{6};
        cases.push_back(makeCase("HALFSC", sq, sq, s,
                                 {{8, 200, fullAc(r, -60, 60)},
                                  {8, -150, fullAc(r, -60, 60)},
                                  {8, 300, fullAc(r, -60, 60)},
                                  {8, -80, fullAc(r, -60, 60)},
                                  {8, 120, fullAc(r, -60, 60)},
                                  {8, -40, fullAc(r, -60, 60)}}));
    }
    // A random matrix, every entry anything.
    {
        int16_t s[64];
        Lcg r{7};
        for (int i = 0; i < 64; i++) s[i] = static_cast<int16_t>(r.range(-0x8000, 0x7fff));
        cases.push_back(makeCase("RANDSC", sq, sq, s,
                                 {{8, 20, sparseAc(r, -20, 20)},
                                  {8, -20, sparseAc(r, -20, 20)},
                                  {8, 60, sparseAc(r, -20, 20)},
                                  {8, -60, sparseAc(r, -20, 20)},
                                  {8, 0, sparseAc(r, -20, 20)},
                                  {8, 5, sparseAc(r, -20, 20)}}));
    }
    // No MDEC(3) at all: the emulator decodes against an all-zero matrix.
    {
        Lcg r{8};
        cases.push_back(makeCase("NOSCALE", sq, sq, nullptr,
                                 {{8, 20, sparseAc(r, -40, 40)},
                                  {8, -20, sparseAc(r, -40, 40)},
                                  {8, 60, sparseAc(r, -40, 40)},
                                  {8, -60, sparseAc(r, -40, 40)},
                                  {8, 0, sparseAc(r, -40, 40)},
                                  {8, 5, sparseAc(r, -40, 40)}}));
    }
    // Colour points from the 2026-10-11 YUV sweep: flat chroma, luma quadrants at
    // 64 / 128 / 192 / 160, quant 8 and q_scale 8 so every plane is exactly its dc.
    {
        const int points[] = {64, -64, 37, -37, 101, -101, 50, -50, 63};
        for (int cr : points) {
            cases.push_back(makeCase(pointName("YUVR", cr), flat8, flat8, ss,
                                     {{8, cr, {}}, {8, 0, {}}, {8, -64, {}}, {8, 0, {}}, {8, 64, {}}, {8, 32, {}}}));
        }
        for (int cb : points) {
            cases.push_back(makeCase(pointName("YUVB", cb), flat8, flat8, ss,
                                     {{8, 0, {}}, {8, cb, {}}, {8, -64, {}}, {8, 0, {}}, {8, 64, {}}, {8, 32, {}}}));
        }
        cases.push_back(makeCase("YUVBOTH", flat8, flat8, ss,
                                 {{8, 90, {}}, {8, -110, {}}, {8, -64, {}}, {8, 0, {}}, {8, 64, {}}, {8, 32, {}}}));
    }
    // Round-half-down ties: quadrants at x.0, x.25, x.5, x.75.
    cases.push_back(makeCase("TIES", flat1, flat1, ss,
                             {{8, 0, {}}, {8, 0, {}}, {8, 200, {}}, {8, 202, {}}, {8, 204, {}}, {8, 206, {}}}));
    // 9-bit output wrap: a clamped DC and a clamped AC in one block overrun 255.
    cases.push_back(makeCase("WRAP", flat83, flat83, ss,
                             {{63, 0, {}},
                              {63, 0, {}},
                              {63, 511, {{0, 511}}},
                              {63, -512, {{0, -512}}},
                              {63, 511, {{1, 511}}},
                              {63, 300, {{0, -511}, {0, 511}}}}));
    // And noise, on both paths.
    for (uint32_t seed = 10; seed < 14; seed++) {
        Lcg r{seed};
        Block b[6];
        for (auto &blk : b) blk = {r.range(1, 63), r.range(-300, 300), sparseAc(r, -120, 120)};
        cases.push_back(makeCase("RAND" + std::to_string(seed), sq, sq, ss, b));
    }
    for (uint32_t seed = 20; seed < 24; seed++) {
        Lcg r{seed};
        int16_t s[64];
        memcpy(s, ss, sizeof(s));
        for (int i = 0; i < 8; i++) s[r.range(0, 63)] = static_cast<int16_t>(r.range(-0x7000, 0x7000));
        Block b[6];
        for (auto &blk : b) blk = {r.range(0, 63), r.range(-300, 300), sparseAc(r, -120, 120)};
        cases.push_back(makeCase("RANDG" + std::to_string(seed), sq, sq, s, b));
    }
    return cases;
}

}  // namespace MDECDecoderCases
