/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui.h"
#include "support/eventbus.h"
#if defined(__i386__) || defined(_M_IX86) || defined(__x86_64) || defined(_M_AMD64)
#define MEMORY_OBSERVER_X86  // Do not include immintrin/xbyak or use avx intrinsics unless we're compiling for x86
#if defined(__GNUC__) || defined(__clang__)
#define AVX2_FUNC [[gnu::target("avx2")]]
#else
#define AVX2_FUNC
#endif
#include "immintrin.h"
#endif

namespace PCSX {

namespace Widgets {

class MemoryObserver {
  public:
    void draw(const char* title);
    bool& m_show;
    MemoryObserver(bool& show);

  private:
    static int getMemValue(uint32_t absoluteAddress, const uint8_t* memData, uint32_t memSize, uint32_t memBase,
                           uint8_t stride);

    /**
     * Plain search.
     */

    // The GNU libc has one, but we may want to do simd versions of it.
    static const void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen);
    std::vector<uint32_t> m_plainAddresses;
    bool m_plainHex = false;
    std::string m_plainSearchString;

    /**
     * Delta-over-time search.
     */

    enum class ScanType {
        ExactValue,
        GreaterThan,
        LessThan,
        Changed,
        Unchanged,
        Increased,
        Decreased,
        UnknownInitialValue
    };

    enum class ScanValueType { Char, Uchar, Short, Ushort, Int, Uint };
    static uint8_t getStrideFromValueType(ScanValueType valueType);
    static int64_t getValueAsType(ScanValueType valueType, int64_t memValue);
    int64_t getValueAsSelectedType(int64_t memValue) { return getValueAsType(m_scanValueType, memValue); }

    struct AddressValuePair {
        uint32_t address = 0;
        int64_t scannedValue = 0;
        // The type the address was scanned as. Results keep it when the Value type combo changes.
        ScanValueType valueType = ScanValueType::Short;

        bool frozen = false;
        int64_t frozenValue = 0;
    };

    ScanType m_scanType = ScanType::ExactValue;
    ScanValueType m_scanValueType = ScanValueType::Short;
    std::vector<AddressValuePair> m_addressValuePairs;
    bool m_hex = false;
    bool m_fixedPoint = false;
    bool m_useSIMD = false;
    int64_t m_value = 0;
    EventBus::Listener m_listener;  // For value freezing.

    /**
     * Pattern search.
     */

#ifdef MEMORY_OBSERVER_X86
    template <int bufferSize>
    AVX2_FUNC static __m256i avx2_getShuffleResultsFor(const std::array<uint8_t, bufferSize>& buffer,
                                                       std::array<uint8_t, 32>& extendedBuffer, int mask) {
        static_assert(bufferSize == 8 || bufferSize == 16);

        for (auto j = 0u; j < (32 / bufferSize); ++j) {
            std::memcpy(&extendedBuffer[j * bufferSize], &buffer[0], bufferSize);
        }
        const auto copies = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(extendedBuffer.data()));

        switch (mask) {
            case 0: {
                const auto firstShuffleMask = _mm256_set_epi8(3, 2, 1, 0, 7, 6, 5, 4, 2, 1, 0, 7, 6, 5, 4, 3, 1, 0, 7,
                                                              6, 5, 4, 3, 2, 0, 7, 6, 5, 4, 3, 2, 1);
                const auto firstShuffle = _mm256_shuffle_epi8(copies, firstShuffleMask);
                return _mm256_cmpeq_epi8(copies, firstShuffle);
            }
            case 1: {
                const auto secondShuffleMask = _mm256_set_epi8(7, 6, 5, 4, 3, 2, 1, 0, 6, 5, 4, 3, 2, 1, 0, 7, 5, 4, 3,
                                                               2, 1, 0, 7, 6, 4, 3, 2, 1, 0, 7, 6, 5);
                const auto secondShuffle = _mm256_shuffle_epi8(copies, secondShuffleMask);
                return _mm256_cmpeq_epi8(copies, secondShuffle);
            }
            case 2: {
                assert(bufferSize == 16);
                const auto thirdShuffleMask = _mm256_set_epi8(11, 10, 9, 8, 7, 6, 5, 4, 10, 9, 8, 7, 6, 5, 4, 3, 9, 8,
                                                              7, 6, 5, 4, 3, 2, 8, 7, 6, 5, 4, 3, 2, 1);
                const auto thirdShuffle = _mm256_shuffle_epi8(copies, thirdShuffleMask);
                return _mm256_cmpeq_epi8(copies, thirdShuffle);
            }
            case 3: {
                assert(bufferSize == 16);
                const auto fourthShuffleMask =
                    _mm256_set_epi8(15, 14, 13, 12, 11, 10, 9, 8, 14, 13, 12, 11, 10, 9, 8, 7, 13, 12, 11, 10, 9, 8, 7,
                                    6, 12, 11, 10, 9, 8, 7, 6, 5);
                const auto fourthShuffle = _mm256_shuffle_epi8(copies, fourthShuffleMask);
                return _mm256_cmpeq_epi8(copies, fourthShuffle);
            }
            default:
                return _mm256_setzero_si256();
        }
    }

    template <int bufferSize>
    AVX2_FUNC void simd_populateAddressList(const uint8_t* memData, uint32_t memBase, uint32_t memSize) {
        static_assert(bufferSize == 8 || bufferSize == 16);

        alignas(32) auto buffer = std::array<uint8_t, bufferSize>{};
        alignas(32) auto extendedBuffer = std::array<uint8_t, 32>{};

        const auto sequenceSize = m_sequenceSize;
        std::copy_n(m_sequence, sequenceSize, buffer.data());
        auto patternShuffleResults =
            std::vector<__m256i>{avx2_getShuffleResultsFor<bufferSize>(buffer, extendedBuffer, 0),
                                 avx2_getShuffleResultsFor<bufferSize>(buffer, extendedBuffer, 1)};
        if constexpr (bufferSize == 16) {
            patternShuffleResults.push_back(avx2_getShuffleResultsFor<bufferSize>(buffer, extendedBuffer, 2));
            patternShuffleResults.push_back(avx2_getShuffleResultsFor<bufferSize>(buffer, extendedBuffer, 3));
        }

        m_addresses.clear();
        for (auto i = 0u; i + sequenceSize < memSize; i += m_step) {
            std::copy_n(memData + i, sequenceSize, buffer.data());

            bool allEqual = true;
            for (auto j = 0u; j < patternShuffleResults.size(); ++j) {
                allEqual = all_equal(_mm256_cmpeq_epi8(
                    patternShuffleResults[j], avx2_getShuffleResultsFor<bufferSize>(buffer, extendedBuffer, j)));
                if (!allEqual) {
                    break;
                }
            }

            if (allEqual) {
                m_addresses.push_back(memBase + i);
            }
        }
    }
    AVX2_FUNC static bool all_equal(__m256i input);
#else
    template <int bufferSize>
    void simd_populateAddressList(const uint8_t* memData, uint32_t memBase, uint32_t memSize) {
        throw std::runtime_error("SIMD pattern searching is not supported on this platform!");
    }
#endif  // MEMORY_OBSERVER_X86

    static std::vector<uint8_t> getShuffleResultsFor(const std::vector<uint8_t>& buffer);
    static bool matchesPattern(const std::vector<uint8_t>& buffer, const std::vector<uint8_t>& patternShuffleResults);
    void populateAddressList(const uint8_t* memData, uint32_t memBase, uint32_t memSize);

    int m_sequenceSize = 255;
    char m_sequence[256]{};
    int m_step = 1;
    std::vector<uint32_t> m_addresses;
};

}  // namespace Widgets

}  // namespace PCSX
