/***************************************************************************
 *   Copyright (C) 2023 PCSX-Redux authors                                 *
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

#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/psxemulator.h"
#include "support/eventbus.h"
#include "support/polyfills.h"
#include "support/sharedmem.h"

#if defined(__BIGENDIAN__)

#define SWAP_LE16(v) ((((v) & 0xff00) >> 8) | (((v) & 0xff) << 8))
#define SWAP_LE32(v) \
    ((((v) & 0xff000000ul) >> 24) | (((v) & 0xff0000ul) >> 8) | (((v) & 0xff00ul) << 8) | (((v) & 0xfful) << 24))
#define SWAP_LEu16(v) SWAP_LE16((uint16_t)(v))
#define SWAP_LEu32(v) SWAP_LE32((uint32_t)(v))

#else

#define SWAP_LE16(b) (b)
#define SWAP_LE32(b) (b)
#define SWAP_LEu16(b) (b)
#define SWAP_LEu32(b) (b)

#endif

namespace PCSX {

enum class MsanStatus {
    UNUSABLE,       // memory that hasn't been allocated or has been freed
    UNINITIALIZED,  // allocated memory that has never been written to, has undefined contents
    OK              // free to use
};

class Memory {
  public:
    Memory();
    int init();
    void reset();
    void shutdown();

    void setLuts();

    enum class ReadType { Data, Instr };

    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address, ReadType = ReadType::Data);
    void write8(uint32_t address, uint32_t value);
    void write16(uint32_t address, uint32_t value);
    void write32(uint32_t address, uint32_t value);
    // Aligned 32-bit accesses for LWL/LWR/SWL/SWR, which only consume or overwrite some bytes of the
    // word. Msan only checks the bytes set in byteMask (bit n = byte n of the word at address).
    uint32_t read32Masked(uint32_t address, uint32_t byteMask);
    void write32Masked(uint32_t address, uint32_t value, uint32_t byteMask);
    // Bytes of the aligned word touched by LWL/SWL and LWR/SWR, for the unaligned address
    static constexpr uint32_t leftByteMask(uint32_t address) { return (2 << (address & 3)) - 1; }
    static constexpr uint32_t rightByteMask(uint32_t address) { return (0xf << (address & 3)) & 0xf; }
    const void *pointerRead(uint32_t address);
    const void *pointerWrite(uint32_t address, int size);

    static constexpr uint16_t ISTAT = 0x1070;
    static constexpr uint16_t IMASK = 0x1074;

    static constexpr uint16_t DMA_BASE = 0x1080;
    static constexpr uint16_t DMA_MADR = 0;
    static constexpr uint16_t DMA_BCR = 4;
    static constexpr uint16_t DMA_CHCR = 8;
    static constexpr uint16_t DMA_PCR = 0x10f0;
    static constexpr uint16_t DMA_ICR = 0x10f4;

    void initMsan(bool reset);
    inline bool msanInitialized() const { return m_msanRAM != nullptr; }
    uint32_t msanAlloc(uint32_t size);
    void msanFree(uint32_t ptr);
    uint32_t msanRealloc(uint32_t ptr, uint32_t size);
    uint32_t msanSetChainPtr(uint32_t headerAddr, uint32_t ptrToNext, uint32_t size);
    uint32_t msanGetChainPtr(uint32_t addr) const;

    template <uint32_t length>
    MsanStatus msanGetStatus(uint32_t addr) const {
        return msanGetStatus(addr, (1 << length) - 1);
    }

    // byteMask selects which of the bytes starting at addr are checked, bit n = byte n
    MsanStatus msanGetStatus(uint32_t addr, uint32_t byteMask) const {
        uint32_t bitmapIndex = (addr - c_msanStart) / 8;
        uint32_t bitmask = byteMask << addr % 8;
        MsanStatus bestCase = MsanStatus::OK;
        if (uint32_t nextBitmask = bitmask >> 8) [[unlikely]] {
            if ((m_msanInitializedBitmap[bitmapIndex + 1] & nextBitmask) != nextBitmask) {
                if ((m_msanUsableBitmap[bitmapIndex + 1] & nextBitmask) != nextBitmask) {
                    return MsanStatus::UNUSABLE;
                }
                bestCase = MsanStatus::UNINITIALIZED;
            }
            bitmask &= 0xff;
        }
        if ((m_msanInitializedBitmap[bitmapIndex] & bitmask) != bitmask) [[unlikely]] {
            if ((m_msanUsableBitmap[bitmapIndex] & bitmask) != bitmask) {
                return MsanStatus::UNUSABLE;
            }
            return MsanStatus::UNINITIALIZED;
        }
        return bestCase;
    }

    // if the write is valid, marks the address as initialized, otherwise returns false
    template <uint32_t length>
    bool msanValidateWrite(uint32_t addr) {
        return msanValidateWrite(addr, (1 << length) - 1);
    }

    bool msanValidateWrite(uint32_t addr, uint32_t byteMask) {
        uint32_t bitmapIndex = (addr - c_msanStart) / 8;
        uint32_t bitmask = byteMask << addr % 8;
        if (uint32_t nextBitmask = bitmask >> 8) [[unlikely]] {
            if ((m_msanUsableBitmap[bitmapIndex + 1] & nextBitmask) != nextBitmask) {
                return false;
            }
            m_msanInitializedBitmap[bitmapIndex + 1] |= nextBitmask;
            bitmask &= 0xff;
        }
        if ((m_msanUsableBitmap[bitmapIndex] & bitmask) != bitmask) [[unlikely]] {
            return false;
        }
        m_msanInitializedBitmap[bitmapIndex] |= bitmask;
        return true;
    }

    void msanDmaWrite(uint32_t addr, uint32_t size) {
        if (!msanInitialized() || !inMsanRange(addr)) return;
        addr -= c_msanStart;
        for (uint32_t i = 0; i < size; ++i) {
            m_msanInitializedBitmap[(addr + i) / 8] |= 1 << ((addr + i) % 8);
        }
    }

    static inline bool inMsanRange(uint32_t addr) { return addr >= c_msanStart && addr < c_msanEnd; }

    template <unsigned n>
    void dmaInterrupt() {
        uint32_t icr = readHardwareRegister<DMA_ICR>();
        if ((icr & 0x00800000) == 0) return;
        bool triggeredIRQ = false;
        if (icr & (1 << (16 + n))) {
            icr |= (1 << (24 + n));
            triggeredIRQ = true;
        }
        if (triggeredIRQ) {
            writeHardwareRegister<DMA_ICR>(icr | 0x80000000);
            if ((icr & 0x80000000) == 0) {
                setIRQ(8);
            }
        }
    }

    void dmaInterruptError() {
        uint32_t icr = readHardwareRegister<DMA_ICR>();
        writeHardwareRegister<DMA_ICR>(icr | 0x00008000);
        setIRQ(8);
    }

    template <unsigned n>
    bool isDMAEnabled() {
        uint32_t pcr = readHardwareRegister<DMA_PCR>();
        return pcr & (8 << (n * 4));
    }

    template <unsigned n>
    bool isDMABusy() {
        uint32_t chcr = readHardwareRegister<DMA_BASE + DMA_CHCR + n * 0x10>();
        return chcr & 0x01000000;
    }

    template <unsigned n>
    void setDMABusy() {
        uint32_t chcr = readHardwareRegister<DMA_BASE + DMA_CHCR + n * 0x10>();
        writeHardwareRegister<DMA_BASE + DMA_CHCR + n * 0x10>(chcr | 0x01000000);
    }

    template <unsigned n>
    void clearDMABusy() {
        uint32_t chcr = readHardwareRegister<DMA_BASE + DMA_CHCR + n * 0x10>();
        writeHardwareRegister<DMA_BASE + DMA_CHCR + n * 0x10>(chcr & ~0x01000000);
    }

    template <unsigned n>
    uint32_t getMADR() {
        return readHardwareRegister<DMA_BASE + DMA_MADR + n * 0x10>();
    }

    template <unsigned n>
    void setMADR(uint32_t value) {
        if (!msanInitialized() || !inMsanRange(value)) {
            value &= 0xffffff;
        }
        writeHardwareRegister<DMA_BASE + DMA_MADR + n * 0x10>(value);
    }

    template <unsigned n>
    uint32_t getBCR() {
        return readHardwareRegister<DMA_BASE + DMA_BCR + n * 0x10>();
    }

    template <unsigned n>
    void setBCR(uint32_t value) {
        writeHardwareRegister<DMA_BASE + DMA_BCR + n * 0x10>(value);
    }

    template <unsigned n>
    uint32_t getCHCR() {
        return readHardwareRegister<DMA_BASE + DMA_CHCR + n * 0x10>();
    }

    template <unsigned n>
    void setCHCR(uint32_t value) {
        writeHardwareRegister<DMA_BASE + DMA_CHCR + n * 0x10>(value);
    }

    template <uint16_t reg, typename T = uint32_t>
    T readHardwareRegister() {
        T *ptr = (T *)&m_hard[reg];
        if constexpr (std::endian::native == std::endian::big) {
            return PolyFill::byteSwap(*ptr);
        } else if constexpr (std::endian::native == std::endian::little) {
            return *ptr;
        }
    }

    template <uint16_t reg, typename T = uint32_t>
    void writeHardwareRegister(T value) {
        T *ptr = (T *)&m_hard[reg];
        if constexpr (std::endian::native == std::endian::big) {
            *ptr = PolyFill::byteSwap(value);
        } else if constexpr (std::endian::native == std::endian::little) {
            *ptr = value;
        }
    }

    void setIRQ(uint32_t irq) {
        uint32_t istat = readHardwareRegister<ISTAT>();
        istat |= irq;
        writeHardwareRegister<ISTAT>(istat);
    }

    void clearIRQ(uint32_t irq) {
        uint32_t istat = readHardwareRegister<ISTAT>();
        istat &= ~irq;
        writeHardwareRegister<ISTAT>(istat);
    }

    uint32_t getBiosCRC32() { return m_biosCRC; }
    std::string_view getBiosVersionString();

    bool loadEXP1FromFile(std::filesystem::path rom_path);
    int sendReadToLua(uint32_t address, size_t size);
    bool sendWriteToLua(uint32_t address, size_t size, uint32_t value);

    class MemoryAsFile : public File {
      public:
        ssize_t rSeek(ssize_t pos, int wheel) final override;
        ssize_t rTell() final override { return m_ptrR; }
        ssize_t wSeek(ssize_t pos, int wheel) final override;
        ssize_t wTell() final override { return m_ptrW; }
        size_t size() final override { return c_size; }
        ssize_t read(void *dest, size_t size) final override {
            auto ret = readAt(dest, size, m_ptrR);
            m_ptrR += ret;
            return ret;
        }
        ssize_t write(const void *dest, size_t size) final override {
            auto ret = writeAt(dest, size, m_ptrW);
            m_ptrW += ret;
            return ret;
        }
        ssize_t readAt(void *dest, size_t size, size_t ptr) final override;
        ssize_t writeAt(const void *src, size_t size, size_t ptr) final override;

      private:
        MemoryAsFile(Memory *memory) : File(File::FileType::RW_SEEKABLE), m_memory(memory) {}
        constexpr static size_t c_size = 0x100000000ULL;
        constexpr static size_t c_blockSize = 64 * 1024;
        size_t m_ptrR = 0;
        size_t m_ptrW = 0;
        size_t cappedSize(size_t size, size_t ptr) const { return std::min(size, c_size - ptr); }
        Memory *m_memory;

        void readBlock(void *dest, size_t size, size_t ptr);
        void writeBlock(const void *src, size_t size, size_t ptr);
        friend class ::PCSX::Memory;
    };

    IO<MemoryAsFile> getMemoryAsFile() { return m_memoryAsFile; }

    bool isiCacheEnabled() { return m_BIU == 0x1e988; }
    uint32_t getBIU() const { return m_BIU; }
    // For save states: the guest's own write to 0xfffe0130 also invalidates the code cache on
    // some values, which a restore does by itself.
    void setBIU(uint32_t value) {
        m_BIU = value;
        setLuts();
    }

  private:
    friend class MemoryAsFile;
    IO<MemoryAsFile> m_memoryAsFile;

    // The scratchpad and the hardware registers share a single 64kB page, and the guest path has
    // to dispatch register accesses to the hardware itself for their side effects, so that page is
    // deliberately absent from the read LUT. Both are shadowed by m_hard at the same offsets,
    // which spans the whole page and is safe to read at any time, so debugger-style accesses -
    // which want a faithful view of memory and none of the side effects - resolve through here
    // instead of through the read LUT directly.
    static constexpr uint32_t c_scratchpadSize = 0x400;
    uint8_t *debugPointer(uint32_t page) const {
        auto pointer = m_readLUT[page];
        if (pointer != nullptr) return pointer;
        if ((page == 0x1f80) || (page == 0x9f80) || (page == 0xbf80)) return m_hard;
        return nullptr;
    }

    uint32_t m_biosCRC = 0;

    // Shared memory wrappers, pointers below point to these where appropriate
    friend class GdbClient;
    SharedMem m_wramShared;

    uint32_t m_BIU = 0;

    // hopefully this should become private eventually, with only certain classes having direct access.
  public:
    /* Counts accesses to anything that isn't plain RAM or BIOS ROM, with the scratchpad
       excluded because a RAM-scoped snapshot covers that too. Zero of these across a stretch
       of guest execution is positive evidence that the code touched nothing but memory - which
       turns "this routine is safe to snapshot RAM-only" from an assertion into something with
       an answer. Keyed on the READ LUT deliberately: the write LUT is nulled wholesale
       whenever the BIU says the caches are isolated, so keying on it would call every RAM
       store on a cold machine a hardware access. */
    uint64_t m_nonRamAccesses = 0;
    void countNonRamAccess(uint32_t address, uint32_t page) {
        if (m_readLUT[page] != nullptr) return;
        if ((page == 0x1f80 || page == 0x9f80 || page == 0xbf80) && ((address & 0xffff) < 0x400)) return;
        m_nonRamAccesses++;
    }

    uint8_t *m_wram = nullptr;  // Kernel & User Memory (8 Meg)
    uint8_t *m_exp1 = nullptr;  // Expansion Region 1 (ROM/RAM) / Parallel Port (8 Meg buffer; up to 256K loaded)
    uint8_t *m_bios = nullptr;  // BIOS ROM (512K)
    uint8_t *m_sram = nullptr;  // DTL-H2000 dev board BIOS SRAM (2 Meg) @ 0x1fa00000
    uint8_t *m_hard = nullptr;  // Scratch Pad (1K) & Hardware Registers (8K)

    uint8_t **m_writeLUT = nullptr;
    uint8_t **m_readLUT = nullptr;

    static constexpr uint32_t c_msanSize = 1'610'612'736;
    static constexpr uint32_t c_msanStart = 0x20000000;
    static constexpr uint32_t c_msanEnd = c_msanStart + c_msanSize;
    // Exit code for msan violations in test mode, matching a failing guest test's exit code.
    static constexpr int c_msanExitCode = 1;
    uint8_t *m_msanRAM = nullptr;
    uint8_t *m_msanUsableBitmap = nullptr;
    uint8_t *m_msanInitializedBitmap = nullptr;
    uint32_t m_msanPtr = 1024;
    EventBus::Listener m_listener;

    // Address of the psyqo heap metadata struct in guest memory,
    // registered by the MIPS allocator via pcsxhw write to 0x1f8020a0.
    uint32_t m_psyqoHeapMetadata = 0;

    std::unordered_map<uint32_t, uint32_t> m_msanAllocs;
    static constexpr uint32_t c_msanChainMarker = 0x7ffffd;
    std::unordered_map<uint32_t, uint32_t> m_msanChainRegistry;

    template <typename T = void>
    T *getPointer(uint32_t address) {
        auto lut = m_readLUT[address >> 16];
        if (!lut) return nullptr;
        uint8_t *ptr = reinterpret_cast<uint8_t *>(lut);
        return reinterpret_cast<T *>(ptr + (address & 0xffff));
    }
};

}  // namespace PCSX
