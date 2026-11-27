/*

MIT License

Copyright (c) 2026 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "supportpsx/dct.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

#include "support/cpu-features.h"

#if defined(__i386__) || defined(_M_IX86) || defined(__x86_64) || defined(_M_AMD64)
#define DCT_X86
#if defined(__GNUC__) || defined(__clang__)
#define DCT_AVX2_FUNC [[gnu::target("avx2")]]
#else
#define DCT_AVX2_FUNC
#endif
#include <immintrin.h>
#endif

namespace {

// SIMD batch width: one lane per block. 16 int16 lanes is one AVX2 register and
// is also a perfectly reasonable unroll factor for the scalar lane, so both lanes
// run the identical loop structure over the identical memory layout. That is the
// whole reason for batching across blocks rather than across the eight
// coefficients of one block: a row-per-register kernel is a different algorithm
// on every register width and needs transposes that this one does not have.
constexpr int kBatch = 16;

constexpr int kFixedBits = 14;
constexpr int kRound = 1 << (kFixedBits - 1);

inline int16_t saturate16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

// Scalar lane. c is batch-major: c[coefficient * kBatch + lane].
void dctBatchScalar(int16_t *c, const int16_t *basis) {
    int16_t mid[64 * kBatch];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int32_t acc[kBatch];
            for (int l = 0; l < kBatch; l++) acc[l] = 0;
            for (int k = 0; k < 8; k++) {
                const int32_t b = basis[8 * i + k];
                const int16_t *src = c + (8 * j + k) * kBatch;
                for (int l = 0; l < kBatch; l++) acc[l] += static_cast<int32_t>(src[l]) * b;
            }
            int16_t *dst = mid + (8 * i + j) * kBatch;
            for (int l = 0; l < kBatch; l++) dst[l] = saturate16((acc[l] + kRound) >> kFixedBits);
        }
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int32_t acc[kBatch];
            for (int l = 0; l < kBatch; l++) acc[l] = 0;
            for (int k = 0; k < 8; k++) {
                const int32_t b = basis[8 * i + k];
                const int16_t *src = mid + (8 * j + k) * kBatch;
                for (int l = 0; l < kBatch; l++) acc[l] += static_cast<int32_t>(src[l]) * b;
            }
            int16_t *dst = c + (8 * i + j) * kBatch;
            for (int l = 0; l < kBatch; l++) dst[l] = saturate16((acc[l] + kRound) >> kFixedBits);
        }
    }
}

// Scalar twin of _mm256_mulhrs_epi16: (a*b + 0x4000) >> 15, low 16 bits, no
// saturation. Intel's semantics exactly; the FastMatrix lanes are only
// bit-identical because this matches.
inline int16_t mulhrs(int16_t a, int16_t b) {
    return static_cast<int16_t>((static_cast<int32_t>(a) * static_cast<int32_t>(b) + 0x4000) >> 15);
}

inline int16_t addsSat(int16_t a, int16_t b) { return saturate16(static_cast<int32_t>(a) + static_cast<int32_t>(b)); }

// FastMatrix scalar lane. Narrows after every multiply instead of accumulating in
// int32, which is what makes the 16-lane int16 vector form possible.
void dctBatchFastScalar(int16_t *c, const int16_t *basisQ15) {
    int16_t mid[64 * kBatch];
    auto pass = [&](const int16_t *src, int16_t *dst) {
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                int16_t acc[kBatch];
                for (int l = 0; l < kBatch; l++) acc[l] = 0;
                for (int k = 0; k < 8; k++) {
                    const int16_t b = basisQ15[8 * i + k];
                    const int16_t *p = src + (8 * j + k) * kBatch;
                    for (int l = 0; l < kBatch; l++) acc[l] = addsSat(acc[l], mulhrs(p[l], b));
                }
                int16_t *d = dst + (8 * i + j) * kBatch;
                for (int l = 0; l < kBatch; l++) d[l] = acc[l];
            }
        }
    };
    pass(c, mid);
    pass(mid, c);
}

// FastSymmetric. One 1D pass, batch-major, Q15 arithmetic.
//
// s_k = x_k + x_(7-k), d_k = x_k - x_(7-k), k = 0..3.  Even output rows of a
// symmetric basis depend only on s, odd rows only on d, so each output is a
// 4-term dot product instead of an 8-term one: 22 multiplies per pass against 64.
// Exact decomposition; the only error is the Q15 rounding, of which there is less
// than FastMatrix does.
//
// The scalar and AVX2 bodies below are the SAME algebra over different primitive
// sets (adds/subs/mulhrs). Keeping them textually parallel is the only reason the
// bit-identity test passes, so do not "simplify" one of them alone.
void dctSymPassScalar(const int16_t *src, int16_t *dst, const int16_t *q15) {
    for (int j = 0; j < 8; j++) {
        int16_t sv[4][kBatch], dv[4][kBatch];
        for (int k = 0; k < 4; k++) {
            const int16_t *a = src + (8 * j + k) * kBatch;
            const int16_t *b = src + (8 * j + (7 - k)) * kBatch;
            for (int l = 0; l < kBatch; l++) {
                sv[k][l] = addsSat(a[l], b[l]);
                dv[k][l] = saturate16(static_cast<int32_t>(a[l]) - static_cast<int32_t>(b[l]));
            }
        }
        for (int i = 0; i < 8; i++) {
            const int16_t(*in)[kBatch] = (i & 1) ? dv : sv;
            int16_t *d = dst + (8 * i + j) * kBatch;
            for (int l = 0; l < kBatch; l++) {
                int16_t acc = 0;
                for (int k = 0; k < 4; k++) acc = addsSat(acc, mulhrs(in[k][l], q15[8 * i + k]));
                d[l] = acc;
            }
        }
    }
}

void dctBatchSymScalar(int16_t *c, const int16_t *q15) {
    int16_t mid[64 * kBatch];
    dctSymPassScalar(c, mid, q15);
    dctSymPassScalar(mid, c, q15);
}

#ifdef DCT_X86
DCT_AVX2_FUNC void dctSymPassAvx2(const int16_t *src, int16_t *dst, const int16_t *q15) {
    for (int j = 0; j < 8; j++) {
        __m256i sv[4], dv[4];
        for (int k = 0; k < 4; k++) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + (8 * j + k) * kBatch));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + (8 * j + (7 - k)) * kBatch));
            sv[k] = _mm256_adds_epi16(a, b);
            dv[k] = _mm256_subs_epi16(a, b);
        }
        for (int i = 0; i < 8; i++) {
            const __m256i *in = (i & 1) ? dv : sv;
            __m256i acc = _mm256_setzero_si256();
            for (int k = 0; k < 4; k++) {
                acc = _mm256_adds_epi16(acc, _mm256_mulhrs_epi16(in[k], _mm256_set1_epi16(q15[8 * i + k])));
            }
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + (8 * i + j) * kBatch), acc);
        }
    }
}

DCT_AVX2_FUNC void dctBatchSymAvx2(int16_t *c, const int16_t *q15) {
    alignas(32) int16_t mid[64 * kBatch];
    dctSymPassAvx2(c, mid, q15);
    dctSymPassAvx2(mid, c, q15);
}

DCT_AVX2_FUNC void dctFastPassAvx2(const int16_t *src, int16_t *dst, const int16_t *basisQ15) {
    for (int i = 0; i < 8; i++) {
        __m256i bv[8];
        for (int k = 0; k < 8; k++) bv[k] = _mm256_set1_epi16(basisQ15[8 * i + k]);
        for (int j = 0; j < 8; j++) {
            __m256i acc = _mm256_setzero_si256();
            for (int k = 0; k < 8; k++) {
                const __m256i sv = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + (8 * j + k) * kBatch));
                acc = _mm256_adds_epi16(acc, _mm256_mulhrs_epi16(sv, bv[k]));
            }
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + (8 * i + j) * kBatch), acc);
        }
    }
}

DCT_AVX2_FUNC void dctBatchFastAvx2(int16_t *c, const int16_t *basisQ15) {
    alignas(32) int16_t mid[64 * kBatch];
    dctFastPassAvx2(c, mid, basisQ15);
    dctFastPassAvx2(mid, c, basisQ15);
}

// AVX2 lane. Deliberately accumulates in int32 and saturates on narrowing, which
// makes it BIT-IDENTICAL to the scalar lane. A 16-lane int16 path using
// _mm256_mulhrs_epi16 is about 1.6x faster and is NOT bit-identical: measured max
// absolute deviation from a double-precision reference is 8.75 against 1.25 for
// this one. An encoder whose output depends on which machine ran it is a bad
// surprise to hand someone, so the accurate lane is the default. The fast one is
// worth adding behind an explicit opt-in, not behind a CPU probe.
DCT_AVX2_FUNC void dctPassAvx2(const int16_t *src, int16_t *dst, const int32_t *basis32) {
    const __m256i round = _mm256_set1_epi32(kRound);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            __m256i accLo = _mm256_setzero_si256();
            __m256i accHi = _mm256_setzero_si256();
            for (int k = 0; k < 8; k++) {
                const __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + (8 * j + k) * kBatch));
                const __m256i b = _mm256_set1_epi32(basis32[8 * i + k]);
                const __m256i sLo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(s));
                const __m256i sHi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(s, 1));
                accLo = _mm256_add_epi32(accLo, _mm256_mullo_epi32(sLo, b));
                accHi = _mm256_add_epi32(accHi, _mm256_mullo_epi32(sHi, b));
            }
            accLo = _mm256_srai_epi32(_mm256_add_epi32(accLo, round), kFixedBits);
            accHi = _mm256_srai_epi32(_mm256_add_epi32(accHi, round), kFixedBits);
            // packs_epi32 saturates (matching saturate16) but interleaves the two
            // 128-bit halves, so undo that with a 64-bit lane permute.
            __m256i packed = _mm256_packs_epi32(accLo, accHi);
            packed = _mm256_permute4x64_epi64(packed, 0xd8);
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + (8 * i + j) * kBatch), packed);
        }
    }
}

DCT_AVX2_FUNC void dctBatchAvx2(int16_t *c, const int32_t *basis32) {
    alignas(32) int16_t mid[64 * kBatch];
    dctPassAvx2(c, mid, basis32);
    dctPassAvx2(mid, c, basis32);
}
#endif

}  // namespace

const PCSX::DCT::Basis &PCSX::DCT::standardBasis() {
    static const Basis s_basis = [] {
        // cos(n * pi / 16) / 2, Q14. Same table the MDEC's own command 3 is handed
        // by every shipping title, and the same one psxavenc builds from its SFn
        // constants.
        double sf[8];
        for (int n = 0; n < 8; n++) sf[n] = cos(n * std::numbers::pi / 16.0) / 2.0;
        static const int idx[8][8] = {{0, 0, 0, 0, 0, 0, 0, 0},     {1, 3, 5, 7, -7, -5, -3, -1},
                                      {2, 6, -6, -2, -2, -6, 6, 2}, {3, -7, -1, -5, 5, 1, 7, -3},
                                      {4, -4, -4, 4, 4, -4, -4, 4}, {5, -1, 7, 3, -3, -7, 1, -5},
                                      {6, -2, 2, -6, -6, 2, -2, 6}, {7, -5, 3, -1, 1, -3, 5, -7}};
        Basis b{};
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                const int k = idx[i][j];
                const double v = (k < 0) ? -sf[-k] : sf[k];
                b[i * 8 + j] = static_cast<int16_t>(lrint(v * 16384.0));
            }
        }
        return b;
    }();
    return s_basis;
}

void PCSX::DCT::transformBlockReference(int16_t *block, const Basis &basis, Transform transform) {
    if (transform == Transform::FastSymmetric) {
        int16_t q15[64];
        for (int i = 0; i < 64; i++) q15[i] = saturate16(static_cast<int32_t>(basis[i]) * 2);
        int16_t mid[64];
        auto pass = [&](const int16_t *src, int16_t *dst) {
            for (int j = 0; j < 8; j++) {
                int16_t sv[4], dv[4];
                for (int k = 0; k < 4; k++) {
                    sv[k] = addsSat(src[8 * j + k], src[8 * j + (7 - k)]);
                    dv[k] =
                        saturate16(static_cast<int32_t>(src[8 * j + k]) - static_cast<int32_t>(src[8 * j + (7 - k)]));
                }
                for (int i = 0; i < 8; i++) {
                    const int16_t *in = (i & 1) ? dv : sv;
                    int16_t acc = 0;
                    for (int k = 0; k < 4; k++) acc = addsSat(acc, mulhrs(in[k], q15[8 * i + k]));
                    dst[8 * i + j] = acc;
                }
            }
        };
        pass(block, mid);
        pass(mid, block);
        return;
    }
    if (transform == Transform::FastMatrix) {
        int16_t q15[64];
        for (int i = 0; i < 64; i++) q15[i] = saturate16(static_cast<int32_t>(basis[i]) * 2);
        int16_t mid[64];
        auto pass = [&](const int16_t *src, int16_t *dst) {
            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 8; j++) {
                    int16_t acc = 0;
                    for (int k = 0; k < 8; k++) acc = addsSat(acc, mulhrs(src[8 * j + k], q15[8 * i + k]));
                    dst[8 * i + j] = acc;
                }
            }
        };
        pass(block, mid);
        pass(mid, block);
        return;
    }
    int16_t mid[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int32_t v = 0;
            for (int k = 0; k < 8; k++) v += static_cast<int32_t>(block[8 * j + k]) * basis[8 * i + k];
            mid[8 * i + j] = saturate16((v + kRound) >> kFixedBits);
        }
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int32_t v = 0;
            for (int k = 0; k < 8; k++) v += static_cast<int32_t>(mid[8 * j + k]) * basis[8 * i + k];
            block[8 * i + j] = saturate16((v + kRound) >> kFixedBits);
        }
    }
}

bool PCSX::DCT::basisIsSymmetric(const Basis &basis) {
    for (int i = 0; i < 8; i++) {
        const int sign = (i & 1) ? -1 : 1;
        for (int j = 0; j < 4; j++) {
            if (basis[i * 8 + (7 - j)] != sign * basis[i * 8 + j]) return false;
        }
    }
    return true;
}

PCSX::DCT::Encoder::Encoder(unsigned threads, Transform transform, Basis basis)
    : m_basis(basis), m_transform(transform) {
    if (transform == Transform::FastSymmetric && !basisIsSymmetric(basis)) {
        // Loudly, at construction. The decomposition simply cannot express this
        // basis, and silently falling back to another Transform would hand the
        // caller different coefficients than the ones they selected.
        throw std::invalid_argument("DCT::Transform::FastSymmetric requires a basis with even/odd symmetry");
    }
    // Q14 -> Q15. The standard basis peaks at 0.5 so this never saturates, but a
    // caller-supplied basis with a coefficient at or above 1.0 would, hence the clamp.
    for (int i = 0; i < 64; i++) {
        const int32_t v = static_cast<int32_t>(m_basis[i]) * 2;
        m_basisQ15[i] = saturate16(v);
    }
    m_threadCount = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
#ifdef DCT_X86
    // The override exists so the scalar lane stays reachable and therefore
    // testable on a machine that has AVX2, and so a lane can be bisected out in
    // the field without a rebuild. It can only ever turn features off.
    m_useAvx2 = CPUFeatures::get().avx2 && (getenv("PCSX_DCT_NO_SIMD") == nullptr);
#endif
    m_threads.reserve(m_threadCount);
    try {
        for (unsigned i = 0; i < m_threadCount; i++) m_threads.emplace_back([this] { worker(); });
    } catch (...) {
        // The destructor does not run for a constructor that throws, and
        // destroying a joinable std::thread calls std::terminate.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shutdown = true;
        }
        m_cv.notify_all();
        for (auto &t : m_threads) t.join();
        throw;
    }
}

PCSX::DCT::Encoder::~Encoder() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    for (auto &t : m_threads) t.join();
}

const char *PCSX::DCT::Encoder::lane() const { return m_useAvx2 ? "avx2" : "scalar"; }

PCSX::DCT::Promise PCSX::DCT::Encoder::submit(const Frame &frame, std::span<int16_t> out) {
    Job job;
    job.frame = frame;
    job.out = out;
    auto future = job.result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(job));
    }
    m_cv.notify_one();
    return Promise(std::move(future));
}

namespace {

// Where block `index` lives in the source frame. MDEC block order within a
// macroblock is Cr, Cb, Y1, Y2, Y3, Y4.
struct BlockSite {
    const uint8_t *base;
    uint32_t stride;
    uint32_t pixelStride;
};

BlockSite locate(const PCSX::DCT::Frame &f, uint32_t mbX, uint32_t mbY, uint32_t sub) {
    switch (sub) {
        case 0:
            return {f.cr + f.cStride * (mbY * 8) + f.cPixelStride * (mbX * 8), f.cStride, f.cPixelStride};
        case 1:
            return {f.cb + f.cStride * (mbY * 8) + f.cPixelStride * (mbX * 8), f.cStride, f.cPixelStride};
        default: {
            const uint32_t dx = ((sub - 2) & 1) * 8;
            const uint32_t dy = ((sub - 2) >> 1) * 8;
            return {f.y + f.yStride * (mbY * 16 + dy) + (mbX * 16 + dx), f.yStride, 1};
        }
    }
}

// Sony's standard VLC code book, FileFormat47 Table 1-7. GENERATED - see
// reference/sony-vlc-codebook.txt for the extraction and its prefix-free check.
// Each entry is the code for a POSITIVE level; the negative is code|1, so the
// low bit carries the sign. Indexed [run][level-1].
struct VlcCode {
    uint16_t code;
    uint8_t bits;
};
constexpr uint8_t c_vlcMaxLevel[32] = {40, 18, 5, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                       2,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
constexpr VlcCode c_vlcRun[32][40] = {
    {{0x6, 3},   {0x8, 5},   {0xa, 6},   {0xc, 8},   {0x4c, 9},  {0x42, 9},  {0x14, 11}, {0x3a, 13},
     {0x30, 13}, {0x26, 13}, {0x20, 13}, {0x34, 14}, {0x32, 14}, {0x30, 14}, {0x2e, 14}, {0x3e, 15},
     {0x3c, 15}, {0x3a, 15}, {0x38, 15}, {0x36, 15}, {0x34, 15}, {0x32, 15}, {0x30, 15}, {0x2e, 15},
     {0x2c, 15}, {0x2a, 15}, {0x28, 15}, {0x26, 15}, {0x24, 15}, {0x22, 15}, {0x20, 15}, {0x30, 16},
     {0x2e, 16}, {0x2c, 16}, {0x2a, 16}, {0x28, 16}, {0x26, 16}, {0x24, 16}, {0x22, 16}, {0x20, 16}},
    {{0x6, 4},   {0xc, 7},   {0x4a, 9},  {0x18, 11}, {0x36, 13}, {0x2c, 14}, {0x2a, 14}, {0x3e, 16},
     {0x3c, 16}, {0x3a, 16}, {0x38, 16}, {0x36, 16}, {0x34, 16}, {0x32, 16}, {0x26, 17}, {0x24, 17},
     {0x22, 17}, {0x20, 17}, {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},
     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},
     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0},     {0, 0}},
    {{0xa, 5}, {0x8, 8}, {0x16, 11}, {0x28, 13}, {0x28, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},   {0, 0},     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},   {0, 0},     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},   {0, 0},     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0xe, 6}, {0x48, 9}, {0x38, 13}, {0x26, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},    {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},    {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},    {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0xc, 6}, {0x1e, 11}, {0x24, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0xe, 7}, {0x12, 11}, {0x24, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0xa, 7}, {0x3c, 13}, {0x28, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x8, 7}, {0x2a, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0xe, 8}, {0x22, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0xa, 8}, {0x22, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},   {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x4e, 9}, {0x20, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x46, 9}, {0x34, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x44, 9}, {0x32, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x40, 9}, {0x30, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x1c, 11}, {0x2e, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x1a, 11}, {0x2c, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x10, 11}, {0x2a, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x3e, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x34, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x32, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x2e, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x2c, 13}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x3e, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x3c, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x3a, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x38, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x36, 14}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x3e, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x3c, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x3a, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x38, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0x36, 17}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
     {0, 0},     {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
};
constexpr VlcCode c_vlcEob = {0x2, 2};
// The v1/v2 End of Frame code: this value in a block's raw 10-bit DC slot.
constexpr uint32_t c_bsEndOfFrame = 0x1ff;
constexpr VlcCode c_vlcEsc = {0x1, 6};

// MSB-first into a 16-bit accumulator, flushed LOW BYTE FIRST. Sony's "coded bit
// sequences are ordered starting with low-order bits" is about BYTE order, not the
// bits inside a code - reversing those gives a well-formed stream that decodes to
// garbage and reads like a quantization bug. Matches psxavenc's encode_bits.
struct BitWriter {
    std::vector<uint8_t> &out;
    uint32_t acc = 0;
    int left = 16;
    explicit BitWriter(std::vector<uint8_t> &o) : out(o) {}
    void put(uint32_t code, int bits) {
        while (bits > 0) {
            const int take = std::min(bits, left);
            const uint32_t chunk = (code >> (bits - take)) & ((1u << take) - 1);
            acc |= chunk << (left - take);
            left -= take;
            bits -= take;
            if (left == 0) flush();
        }
    }
    void flush() {
        out.push_back(static_cast<uint8_t>(acc & 0xff));
        out.push_back(static_cast<uint8_t>(acc >> 8));
        acc = 0;
        left = 16;
    }
    void finish() {
        if (left != 16) flush();
    }
};

}  // namespace

void PCSX::DCT::Encoder::worker() {
    std::array<int32_t, 64> basis32{};
    for (int i = 0; i < 64; i++) basis32[i] = m_basis[i];

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_shutdown || !m_queue.empty(); });
            if (m_queue.empty()) {
                if (m_shutdown) return;
                continue;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }

        const Frame &f = job.frame;
        Result result;
        if (!f.y || !f.cb || !f.cr || (f.width % 16) || (f.height % 16) || !f.width || !f.height ||
            job.out.size() < requiredCoefficientCount(f.width, f.height)) {
            result.failed = true;
            job.result.set_value(std::move(result));
            continue;
        }

        result.macroblocksX = f.width / 16;
        result.macroblocksY = f.height / 16;
        const uint32_t macroblocks = result.macroblocksX * result.macroblocksY;
        result.blockCount = macroblocks * 6;

        alignas(32) int16_t batch[64 * kBatch];

        for (uint32_t base = 0; base < result.blockCount; base += kBatch) {
            const uint32_t count = std::min<uint32_t>(kBatch, result.blockCount - base);
            // The tail of a frame whose block count is not a multiple of kBatch is
            // zero-filled rather than handled by a second code path. 1800 blocks
            // for 320x240 leaves a remainder of 8, so this is the common case and
            // not an edge case; one code path is worth 0.4% of wasted transform.
            memset(batch, 0, sizeof(batch));
            for (uint32_t l = 0; l < count; l++) {
                const uint32_t bi = base + l;
                const uint32_t mb = bi / 6;
                const bool column = f.order == MacroblockOrder::Column;
                const uint32_t mbx = column ? mb / result.macroblocksY : mb % result.macroblocksX;
                const uint32_t mby = column ? mb % result.macroblocksY : mb / result.macroblocksX;
                const BlockSite site = locate(f, mbx, mby, bi % 6);
                for (uint32_t row = 0; row < 8; row++) {
                    const uint8_t *p = site.base + site.stride * row;
                    for (uint32_t col = 0; col < 8; col++) {
                        batch[(row * 8 + col) * kBatch + l] = static_cast<int16_t>(p[col * site.pixelStride]) - 128;
                    }
                }
            }

            switch (m_transform) {
                case Transform::FastSymmetric:
#ifdef DCT_X86
                    if (m_useAvx2) {
                        dctBatchSymAvx2(batch, m_basisQ15.data());
                        break;
                    }
#endif
                    dctBatchSymScalar(batch, m_basisQ15.data());
                    break;
                case Transform::FastMatrix:
#ifdef DCT_X86
                    if (m_useAvx2) {
                        dctBatchFastAvx2(batch, m_basisQ15.data());
                        break;
                    }
#endif
                    dctBatchFastScalar(batch, m_basisQ15.data());
                    break;
                case Transform::ExactMatrix:
                default:
#ifdef DCT_X86
                    if (m_useAvx2) {
                        dctBatchAvx2(batch, basis32.data());
                        break;
                    }
#endif
                    dctBatchScalar(batch, m_basis.data());
                    break;
            }

            for (uint32_t l = 0; l < count; l++) {
                int16_t *out = job.out.data() + static_cast<size_t>(base + l) * 64;
                for (uint32_t k = 0; k < 64; k++) out[k] = batch[k * kBatch + l];
            }
        }

        job.result.set_value(std::move(result));
    }
}

// ---------------------------------------------------------------------------
// Run-level packing and rate control.

namespace {

// Sony's own zscan[], FileFormat47 p.1-12, verified identical entry for entry.
// c_packZscan[k] is the natural-order index of zigzag position k.
constexpr int c_packZscan[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Sony's Qtab, FileFormat47 p.1-11, in the order MDEC(2) receives it: zigzag,
// entry k being the divisor for stream coefficient k. FileFormat47 prints the
// matrix in raster order and its encoder indexes it by raster position; the
// MDEC indexes the uploaded table by stream position, and the copy of this
// table found in retail executables is this zigzag permutation of the print.
// Their matrix is drawn "x 1/16", so the printed integers are what goes here
// and the 16 in their quantization formula is what cancels it.
//
// Not an all-ones table: the run-level DC field is signed TEN BITS and an
// unquantized DC runs to about 1150, so identity quant clips every block
// brighter than mid-grey.
constexpr uint8_t c_packStandardQuant[64] = {
    2,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
    27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
    37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
};

int clampField10(int v, uint32_t &clipped) {
    if (v > 511) {
        clipped++;
        return 511;
    }
    if (v < -512) {
        clipped++;
        return -512;
    }
    return v;
}

int divRoundPack(int num, int den) {
    if (den == 0) return 0;
    return (num < 0) ? -((-num + den / 2) / den) : ((num + den / 2) / den);
}

// One 8x8 block, appended. Returns nothing; the caller tracks sizes by watching
// `out`. Calibration constants are MEASURED against real_idct_core, not derived,
// and DctPack.theAcDivisorSplitsOnHowManyAxesSitAtDc pins the AC half of it:
//   the forward transform puts 16*luma in blk[0] and real_idct_core turns a DC of
//   D back into D/8, so the composite DC gain is 2 and the divisor is qt[0]*2;
//   the AC composite gain is NOT uniform, because the psx-spx scale matrix carries
//   the orthonormal DCT's 1/sqrt(2) on its DC row and the forward basis does not.
//   That term is per-axis, so it lands once for every axis sitting at DC:
//     both axes at DC (k == 0)          composite 2      divisor qt[0]*2
//     one axis at DC (row 0 or col 0)   composite sqrt(2)  8/sqrt(2) = 5793/1024
//     neither axis at DC                composite 1        8        = 8192/1024
//   The 14 one-axis slots measure Gfwd 8.000 * Ginv 0.1768 = 1.4139; the 49
//   two-axis slots measure Gfwd 4.000 * Ginv 0.2500 = 1.0000. The class is a
//   property of the RASTER position, so it is keyed off c_packZscan[k], not k.
// ⚠ q_scale appears in the AC divisor and NOT the DC one. Sony says so twice in
// FileFormat47 p.1-11 - "DC elements are not affected by QUANT" in prose, and a
// separate blk_zig[0] statement using a constant 8 where the i >= 1 loop uses
// q_scale - and it is measured on hardware, 2026-09-14: arms holding everything
// but q_scale and running it at 8 against 63 decode byte-identical.
void packBlock(const int16_t *blk, const uint8_t *qt, int qScale, std::vector<uint16_t> &out, uint32_t &clippedAc,
               uint32_t &clippedDc) {
    const int dcDen = (qt[0] ? qt[0] : 1) * 2;
    const int dc = clampField10(divRoundPack(blk[0], dcDen), clippedDc);
    out.push_back(static_cast<uint16_t>(((qScale & 0x3f) << 10) | (dc & 0x3ff)));
    int run = 0;
    for (int k = 1; k < 64; k++) {
        const int den = qt[k] * qScale;
        const int raster = c_packZscan[k];
        const int num = ((raster >> 3) == 0 || (raster & 7) == 0) ? 5793 : 8192;
        const int ac = clampField10(divRoundPack(blk[raster] * num, (den ? den : 1) * 1024), clippedAc);
        if (ac == 0) {
            run++;
            continue;
        }
        out.push_back(static_cast<uint16_t>(((run & 0x3f) << 10) | (ac & 0x3ff)));
        run = 0;
    }
    out.push_back(0xfe00);
}

}  // namespace

const uint8_t *PCSX::DCT::standardQuantTable() { return c_packStandardQuant; }

namespace {

// Reverse lookup for the code book, built once. 224 codes over lengths 2..17, so a
// per-length table keyed by the accumulated bits is enough and needs no trie.
struct VlcDecodeEntry {
    int run;
    int level;
    bool escape;
    bool eob;
};
const std::map<uint32_t, VlcDecodeEntry> &vlcDecodeTable() {
    static const std::map<uint32_t, VlcDecodeEntry> s_table = [] {
        std::map<uint32_t, VlcDecodeEntry> t;
        // Key packs the LENGTH with the code, because a code value is only unique
        // within its length - 0b10 at 2 bits and 0b10 at 6 bits are different symbols.
        auto key = [](uint32_t code, int bits) { return (static_cast<uint32_t>(bits) << 20) | code; };
        t[key(c_vlcEob.code, c_vlcEob.bits)] = {0, 0, false, true};
        t[key(c_vlcEsc.code, c_vlcEsc.bits)] = {0, 0, true, false};
        for (int run = 0; run < 32; run++) {
            for (int lvl = 1; lvl <= c_vlcMaxLevel[run]; lvl++) {
                const VlcCode &c = c_vlcRun[run][lvl - 1];
                if (!c.bits) continue;
                t[key(c.code, c.bits)] = {run, lvl, false, false};
                t[key(c.code | 1u, c.bits)] = {run, -lvl, false, false};
            }
        }
        return t;
    }();
    return s_table;
}

// MSB-first out of little-endian halfwords, the mirror of BitWriter.
struct BitReader {
    std::span<const uint8_t> in;
    size_t pos = 0;  // bit position
    bool overrun = false;
    explicit BitReader(std::span<const uint8_t> i) : in(i) {}
    uint32_t get(int bits) {
        uint32_t v = 0;
        for (int b = 0; b < bits; b++) {
            const size_t hw = pos >> 4;
            const size_t bit = pos & 15;
            uint32_t word = 0;
            if (hw * 2 + 1 < in.size()) {
                word = static_cast<uint32_t>(in[hw * 2]) | (static_cast<uint32_t>(in[hw * 2 + 1]) << 8);
            } else {
                overrun = true;
            }
            v = (v << 1) | ((word >> (15 - bit)) & 1);
            pos++;
        }
        return v;
    }
    // Look at the next `bits` without consuming them or latching an overrun. The
    // delta-DC size codes are variable length with no separator, so they have to be
    // matched against a peeked window.
    uint32_t peek(int bits) {
        const size_t savedPos = pos;
        const bool savedOverrun = overrun;
        const uint32_t v = get(bits);
        pos = savedPos;
        overrun = savedOverrun;
        return v;
    }
};

// Delta-DC size codes for BS versions other than 2, as {code, codeLength} indexed by
// size. These are the MPEG-1 DC size code books (ISO/IEC 11172-2 tables B-12 and B-13),
// not a transcription of anyone's source. Sony's DecDCTvlc corroborates them without
// spelling them out: its long-code arm derives the length arithmetically, `bit = 3;
// while (Show_Bits(bit) & 1) bit++; bit++;` for luma and `bit = 4; while (...) bit++;`
// for chroma, which reproduces exactly this progression of leading ones for sizes 4..8.
struct DcSizeCode {
    uint32_t code;
    int length;
};
static constexpr DcSizeCode c_dcSizeLuma[9] = {
    {0b100, 3}, {0b00, 2}, {0b01, 2}, {0b101, 3}, {0b110, 3}, {0b1110, 4}, {0b11110, 5}, {0b111110, 6}, {0b1111110, 7},
};
static constexpr DcSizeCode c_dcSizeChroma[9] = {
    {0b00, 2},    {0b01, 2},     {0b10, 2},      {0b110, 3},      {0b1110, 4},
    {0b11110, 5}, {0b111110, 6}, {0b1111110, 7}, {0b11111110, 8},
};

}  // namespace

PCSX::DCT::ContainerResult PCSX::DCT::toContainer(std::span<const uint16_t> rl, Container container,
                                                  std::vector<uint8_t> &out) {
    ContainerResult r;
    out.clear();
    if (rl.empty()) {
        r.failed = true;
        r.error = "empty run-level stream";
        return r;
    }

    // Walk it once to learn the shape before emitting a byte, so a stream that
    // cannot be expressed fails with a reason instead of half a file.
    //
    // A block is (q_scale << 10) | dc, then (run << 10) | level pairs, then 0xfe00.
    // Trailing 0xfe00 is pack()'s DMA padding to a 64-halfword boundary.
    // ⚠ KNOWN AMBIGUITY, narrow and deliberate: a block with q_scale 63 and dc
    // -512 (0x200) has 0xfe00 as its FIRST word, so a final such block carrying no ACs is
    // indistinguishable from padding. Callers that know their block count should
    // check `blocks` against it.
    int minQ = 64, maxQ = -1;
    size_t i = 0;
    while (i < rl.size()) {
        bool restIsPadding = true;
        for (size_t j = i; j < rl.size(); j++) {
            if (rl[j] != 0xfe00) {
                restIsPadding = false;
                break;
            }
        }
        if (restIsPadding) break;
        const int q = (rl[i] >> 10) & 0x3f;
        minQ = std::min(minQ, q);
        maxQ = std::max(maxQ, q);
        i++;
        while (i < rl.size() && rl[i] != 0xfe00) i++;
        if (i >= rl.size()) {
            r.failed = true;
            r.error = "stream ends mid-block, no 0xfe00 terminator";
            return r;
        }
        i++;
        r.blocks++;
    }
    if (r.blocks == 0) {
        r.failed = true;
        r.error = "no blocks in stream";
        return r;
    }
    r.qScale = minQ;
    // What the MDEC command in a BS header carries: 32-bit words of decompressed
    // run-level WITH the padding, because that is what DMA0 will receive.
    const size_t padded = (rl.size() + 0x3f) & ~size_t(0x3f);
    r.rlWords = static_cast<uint32_t>((padded + 1) >> 1);

    if (container == Container::Raw) {
        // The stream as DMA0 wants it, padding and all. A compressing front end
        // takes these bytes; what it links against is its own licence problem.
        out.resize(rl.size() * 2);
        for (size_t k = 0; k < rl.size(); k++) {
            out[k * 2] = static_cast<uint8_t>(rl[k] & 0xff);
            out[k * 2 + 1] = static_cast<uint8_t>(rl[k] >> 8);
        }
        r.bytes = static_cast<uint32_t>(out.size());
        return r;
    }

    if (minQ != maxQ) {
        r.failed = true;
        r.error = "q_scale varies across the frame and BS carries QUANT once";
        return r;
    }
    if (r.rlWords > 0xffff) {
        // The header field is 16 bits; masking would describe a different,
        // shorter stream than the one that follows.
        r.failed = true;
        r.error = "run-level stream too long for the BS header's 16-bit length";
        return r;
    }

    // Header. Word 0 IS the MDEC(1) decode command, which is why the magic is
    // 0x3800 rather than something arbitrary.
    out.reserve(8 + rl.size());
    auto put16 = [&out](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    };
    put16(r.rlWords & 0xffff);
    put16(0x3800);
    put16(static_cast<uint32_t>(r.qScale));
    put16(2);  // BS v2: raw DC. v3 delta-codes it and is a different format.

    BitWriter bw(out);
    i = 0;
    for (uint32_t b = 0; b < r.blocks; b++) {
        // DC, raw 10 bits in v2. 0x1FF in this slot is the End of Frame code, so a
        // DC clamped to +511 saturates to +510 instead: psx-spx gives the v1/v2 DC
        // range as -200h..+1FEh, and a retail player stops the frame at a 0x1FF.
        const uint32_t dc = rl[i] & 0x3ff;
        bw.put(dc == c_bsEndOfFrame ? c_bsEndOfFrame - 1 : dc, 10);
        i++;
        while (rl[i] != 0xfe00) {
            const int run = (rl[i] >> 10) & 0x3f;
            int level = rl[i] & 0x3ff;
            if (level & 0x200) level -= 0x400;  // sign-extend the 10-bit field
            const int mag = level < 0 ? -level : level;
            if (run < 32 && mag >= 1 && mag <= c_vlcMaxLevel[run] && c_vlcRun[run][mag - 1].bits) {
                const VlcCode &c = c_vlcRun[run][mag - 1];
                bw.put(level < 0 ? (c.code | 1u) : c.code, c.bits);
            } else {
                // Off the book: ESCAPE, then the run-level word VERBATIM. Sony's
                // DecDCTvlc does `Flush_Buffer(6); code2 = Show_Bits(16)`, and reads
                // that with RUNOF(a)=(a)>>10 and VALOF(a)=((short)((a)<<6)>>6) - so
                // the payload is SIXTEEN bits total, 6-bit run then 10-bit signed
                // level, which is exactly the run-level word already in hand.
                // ⛔ FileFormat47's Table 1-9 lists 16-bit patterns for the LEVEL and
                // reads as though run and level were separate fields. Encoding it
                // that way costs 28 bits per escape, desyncs the bitstream at the
                // first one, and still round-trips against a decoder written from the
                // same misreading. The decoder is ground truth here, not the table.
                bw.put(c_vlcEsc.code, c_vlcEsc.bits);
                bw.put(static_cast<uint32_t>(rl[i]) & 0xffff, 16);
            }
            i++;
        }
        bw.put(c_vlcEob.code, c_vlcEob.bits);
        i++;
    }
    // End of Frame: 0x1FF where the next block's raw DC would be. psx-spx, "DC
    // v1/v2/ea": `0111111111  End of Frame (+1FFh, that, in place of Cr)`. Retail
    // players loop until they read it rather than counting blocks, so a frame
    // without it runs them off the end of the bitstream. Retail v2 frames end
    // `10 0111111111` then zeros to a 32-bit word, and the header's length counts
    // run-level only: the code is not a word.
    bw.put(c_bsEndOfFrame, 10);
    bw.finish();
    // BS payload is a whole number of 32-bit words.
    while (out.size() & 3) out.push_back(0);
    r.bytes = static_cast<uint32_t>(out.size());
    return r;
}

int PCSX::DCT::qualityToQScale(int quality) {
    quality = std::clamp(quality, 1, 100);
    // 63^((100-q)/99): 100 -> 1, 50 -> 8, 1 -> 63.
    const double e = static_cast<double>(100 - quality) / 99.0;
    const int v = static_cast<int>(std::lround(std::pow(63.0, e)));
    return std::clamp(v, 1, 63);
}

void PCSX::DCT::scaleQuantTable(const uint8_t *in, uint8_t *out, int quality) {
    if (!in) in = c_packStandardQuant;
    quality = std::clamp(quality, 1, 100);
    // The IJG curve. Below 50 the divisors grow without bound as quality falls;
    // above it they shrink linearly to 2% of nominal at 100.
    const int scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (int i = 0; i < 64; i++) {
        const int v = (in[i] * scale + 50) / 100;
        out[i] = static_cast<uint8_t>(std::clamp(v, 1, 255));
    }
}

PCSX::DCT::PackResult PCSX::DCT::pack(std::span<const int16_t> coefficients, const Result &shape,
                                      const QuantTables &tables, int qScale, std::vector<uint16_t> &out,
                                      const RateControl &rateControl) {
    PackResult result;
    if (shape.failed || shape.blockCount == 0 || (shape.blockCount % 6) != 0) {
        result.failed = true;
        return result;
    }
    if (coefficients.size() < static_cast<size_t>(shape.blockCount) * 64) {
        result.failed = true;
        return result;
    }
    const uint8_t *qy = tables.y ? tables.y : c_packStandardQuant;
    const uint8_t *quv = tables.uv ? tables.uv : c_packStandardQuant;
    const int initial = std::clamp(qScale, 1, 63);
    // `out` is appended to, so every size reported or padded below is relative
    // to what the caller already had in it.
    const size_t base = out.size();

    const uint32_t macroblocks = shape.blockCount / 6;
    for (uint32_t mb = 0; mb < macroblocks; mb++) {
        const size_t acceptedSoFar = out.size();
        int q = initial;
        uint32_t attempt = 0;
        uint32_t acHere = 0, dcHere = 0;
        for (;;) {
            out.resize(acceptedSoFar);
            acHere = 0;
            dcHere = 0;
            for (uint32_t i = 0; i < 6; i++) {
                const size_t b = static_cast<size_t>(mb) * 6 + i;
                packBlock(coefficients.data() + b * 64, (i < 2) ? quv : qy, q, out, acHere, dcHere);
            }
            result.attempts++;
            if (!rateControl) break;
            PackAttempt info;
            info.macroblock = mb;
            info.attempt = attempt;
            info.qScale = q;
            info.sizeHalfwords = out.size() - acceptedSoFar;
            info.totalHalfwords = out.size() - base;
            info.clippedAc = acHere;
            info.clippedDc = dcHere;
            const auto next = rateControl(info);
            if (!next.has_value()) break;
            const int want = std::clamp(*next, 1, 63);
            // Two independent stops. Repeating a value means converged, which is
            // the common case. The hard cap is what a functor ALTERNATING between
            // two values needs: it never repeats consecutively, so the first test
            // alone loops forever. 64 is the count of distinct q_scale values, so
            // no search that makes progress can reach it.
            if (want == q) break;
            if (attempt + 1 >= 64) break;
            q = want;
            attempt++;
        }
        result.clippedAc += acHere;
        result.clippedDc += dcHere;
        // The q_scale this macroblock actually settled on. Rate control can move it
        // per macroblock and BS cannot express that, so the range is REPORTED and
        // the container decides - see PackResult and toContainer(Bs).
        result.minQScale = (result.minQScale == 0) ? q : std::min(result.minQScale, q);
        result.maxQScale = std::max(result.maxQScale, q);
    }

    // psx-spx: MDEC(1) parameters want padding to 40h halfwords so the DMA block
    // count is a whole number of 20h-word blocks. A half block hangs DMA0.
    while ((out.size() - base) % 64) out.push_back(0xfe00);
    result.halfwords = out.size() - base;
    return result;
}

PCSX::DCT::ContainerResult PCSX::DCT::fromContainer(std::span<const uint8_t> in, Container container,
                                                    std::vector<uint16_t> &rl) {
    ContainerResult r;
    rl.clear();
    if (container == Container::Raw) {
        if (in.size() & 1) {
            r.failed = true;
            r.error = "raw stream has an odd byte count";
            return r;
        }
        rl.resize(in.size() / 2);
        for (size_t k = 0; k < rl.size(); k++) rl[k] = in[k * 2] | (static_cast<uint16_t>(in[k * 2 + 1]) << 8);
        r.bytes = static_cast<uint32_t>(in.size());
        r.rlWords = static_cast<uint32_t>((rl.size() + 1) >> 1);
        return r;
    }

    if (in.size() < 8) {
        r.failed = true;
        r.error = "shorter than a BS header";
        return r;
    }
    const uint32_t rlWords = in[0] | (static_cast<uint32_t>(in[1]) << 8);
    const uint32_t magic = in[2] | (static_cast<uint32_t>(in[3]) << 8);
    const uint32_t quant = in[4] | (static_cast<uint32_t>(in[5]) << 8);
    const uint32_t version = in[6] | (static_cast<uint32_t>(in[7]) << 8);
    if (magic != 0x3800) {
        r.failed = true;
        r.error = "not a BS stream: word 0 is not the MDEC decode command";
        return r;
    }
    // The version field is NOT a v2/v3 binary, and reading it as one is what kept this
    // decoder from opening two of the eleven retail streams it was measured against.
    // v1 and v2 both carry a raw 10-bit DC; v3 delta-codes it against a per-component
    // predictor. All three ship on retail discs: Suikoden II `_KONAMIC.STR` is v1,
    // Castlevania SOTN `LOGO15XA.STR` is v3, everything else measured is v2.
    // ⛔ ONLY v3 delta-codes the DC. Sony's DecDCTvlc tests `type == 2` and sends
    // everything else down the delta path, which is WRONG for v1 and measurably so:
    // on Suikoden II's `_KONAMIC.STR` it yields 149 of 4416 halfwords before it
    // degenerates to filling the buffer with EOB, against 4358 of 4416 and a correct
    // 1800-block count when v1 is read as raw DC. Do not "fix" this to match
    // DecDCTvlc - a zero return from it means the buffer got filled, not decoded.
    const bool deltaDc = version == 3;
    if (version == 0 || version > 3) {
        r.failed = true;
        r.error = "BS version out of range: expected 1, 2 or 3";
        return r;
    }
    r.qScale = static_cast<int>(quant);
    r.rlWords = rlWords;

    const size_t target = static_cast<size_t>(rlWords) * 2;
    rl.reserve(target);
    BitReader br(in.subspan(8));
    const auto &table = vlcDecodeTable();

    // ⚠ Stop at the last block that FITS, rather than filling to `target`. Sony's
    // DecDCTvlc loops `while (mdec_rl < rl_end)` and so decodes one more block header
    // out of exhausted bits when the payload does not reach the padded length: it
    // writes a spurious DC word into the first pad slot. Measured on two streams, one
    // halfword each time. The doc says the decoder ADDS footers to reach the boundary,
    // which is what this does instead - so expect a one-halfword disagreement with
    // that implementation, in the pad, in this direction.
    // Block order within a macroblock is Cr, Cb, Y0, Y1, Y2, Y3, so blocks 0 and 1 take
    // the chroma code book and their own predictors, and 2..5 share the luma one.
    int lastDc[3] = {0, 0, 0};
    int blockInMacroblock = 0;

    while (rl.size() < target) {
        const size_t blockStart = rl.size();
        if (br.overrun) break;
        uint32_t dc;
        if (!deltaDc) {
            dc = br.get(10);
        } else {
            const bool luma = blockInMacroblock >= 2;
            const DcSizeCode *book = luma ? c_dcSizeLuma : c_dcSizeChroma;
            int size = -1;
            for (int s = 0; s < 9; s++) {
                if (br.peek(book[s].length) == book[s].code) {
                    size = s;
                    break;
                }
            }
            if (size < 0 || br.overrun) {
                rl.resize(blockStart);
                br.overrun = true;
                break;
            }
            br.get(book[size].length);
            int delta = 0;
            if (size > 0) {
                const uint32_t raw = br.get(size);
                // Top bit set means positive; clear means the value was stored as an
                // offset from -(2^size - 1).
                delta = (raw & (1u << (size - 1))) ? static_cast<int>(raw) : static_cast<int>(raw) - ((1 << size) - 1);
            }
            const int predictor = luma ? 2 : blockInMacroblock;
            lastDc[predictor] += delta * 4;
            dc = static_cast<uint32_t>(lastDc[predictor]) & 0x3ff;
            if (++blockInMacroblock == 6) blockInMacroblock = 0;
        }
        if (br.overrun) break;
        // End of Frame. Stop on it rather than decoding whatever pads the frame -
        // zeros, 0xFF or garbage on retail discs. v3's code matches no size code
        // and so already ends the loop above.
        if (!deltaDc && dc == c_bsEndOfFrame) break;
        rl.push_back(static_cast<uint16_t>(((quant & 0x3f) << 10) | (dc & 0x3ff)));
        bool closed = false;
        while (!closed) {
            if (rl.size() >= target) {
                // The block has not closed and the header's length is used up:
                // it does not fit, so drop it like any other that runs out.
                rl.resize(blockStart);
                br.overrun = true;
                break;
            }
            uint32_t code = 0;
            int bits = 0;
            const VlcDecodeEntry *found = nullptr;
            while (bits < 17) {
                code = (code << 1) | br.get(1);
                bits++;
                auto it = table.find((static_cast<uint32_t>(bits) << 20) | code);
                if (it != table.end()) {
                    found = &it->second;
                    break;
                }
            }
            if (!found || br.overrun) {
                rl.resize(blockStart);
                closed = true;
                br.overrun = true;
                break;
            }
            if (found->eob) {
                rl.push_back(0xfe00);
                closed = true;
            } else if (found->escape) {
                rl.push_back(static_cast<uint16_t>(br.get(16)));
            } else {
                rl.push_back(static_cast<uint16_t>(((found->run & 0x3f) << 10) | (found->level & 0x3ff)));
            }
        }
        if (br.overrun) break;
        r.blocks++;
    }
    // The footers the header's length implies, which the bitstream does not carry.
    while (rl.size() < target) rl.push_back(0xfe00);
    r.bytes = static_cast<uint32_t>(in.size());
    return r;
}
