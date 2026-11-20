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
    // The pattern of a buffer is the set of byte equalities between every pair of its positions. The AVX2 search
    // gets it by comparing the buffer against all of its rotations. A 256-bit register holds 32 / bufferSize copies
    // of the buffer, and each copy is compared against a different rotation, so bufferSize rotations (rotation 0
    // included) take bufferSize * bufferSize / 32 registers. _mm256_shuffle_epi8 only indexes within a 128-bit lane,
    // which holds whole copies for both supported sizes.
    template <int bufferSize>
    static constexpr int c_avx2ShuffleCount = bufferSize * bufferSize / 32;

    template <int bufferSize>
    static constexpr auto c_avx2ShuffleMasks = [] {
        constexpr int copiesPerRegister = 32 / bufferSize;
        std::array<std::array<uint8_t, 32>, c_avx2ShuffleCount<bufferSize>> masks{};
        for (int reg = 0; reg < c_avx2ShuffleCount<bufferSize>; ++reg) {
            for (int copy = 0; copy < copiesPerRegister; ++copy) {
                const int rotation = reg * copiesPerRegister + copy;
                const int laneOffset = (copy * bufferSize) % 16;
                for (int i = 0; i < bufferSize; ++i) {
                    masks[reg][copy * bufferSize + i] = laneOffset + (i + rotation) % bufferSize;
                }
            }
        }
        return masks;
    }();

    template <int bufferSize>
    AVX2_FUNC static std::array<__m256i, c_avx2ShuffleCount<bufferSize>> avx2_getShuffleResultsFor(
        const uint8_t* buffer) {
        static_assert(bufferSize == 8 || bufferSize == 16);

        alignas(32) std::array<uint8_t, 32> extendedBuffer;
        for (auto j = 0u; j < (32 / bufferSize); ++j) {
            std::memcpy(&extendedBuffer[j * bufferSize], buffer, bufferSize);
        }
        const auto copies = _mm256_load_si256(reinterpret_cast<const __m256i*>(extendedBuffer.data()));

        std::array<__m256i, c_avx2ShuffleCount<bufferSize>> results;
        for (int i = 0; i < c_avx2ShuffleCount<bufferSize>; ++i) {
            const auto mask =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c_avx2ShuffleMasks<bufferSize>[i].data()));
            results[i] = _mm256_cmpeq_epi8(copies, _mm256_shuffle_epi8(copies, mask));
        }
        return results;
    }

    // Only called with a sequence exactly bufferSize long: a shorter one would compare the bytes after it too.
    template <int bufferSize>
    AVX2_FUNC void simd_populateAddressList(const uint8_t* memData, uint32_t memBase, uint32_t memSize) {
        static_assert(bufferSize == 8 || bufferSize == 16);

        const auto patternShuffleResults =
            avx2_getShuffleResultsFor<bufferSize>(reinterpret_cast<const uint8_t*>(m_sequence));

        m_addresses.clear();
        for (auto i = 0u; i + bufferSize <= memSize; i += m_step) {
            const auto memoryShuffleResults = avx2_getShuffleResultsFor<bufferSize>(memData + i);

            bool allEqual = true;
            for (auto j = 0u; j < patternShuffleResults.size(); ++j) {
                allEqual = all_ones(_mm256_cmpeq_epi8(patternShuffleResults[j], memoryShuffleResults[j]));
                if (!allEqual) {
                    break;
                }
            }

            if (allEqual) {
                m_addresses.push_back(memBase + i);
            }
        }
    }
    AVX2_FUNC static bool all_ones(__m256i input) { return _mm256_movemask_epi8(input) == -1; }
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
    size_t m_searchedSequenceLength = 0;
    int m_step = 1;
    std::vector<uint32_t> m_addresses;
};

}  // namespace Widgets

}  // namespace PCSX
