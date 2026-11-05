/***************************************************************************
/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include <chrono>
#include <string>

#include "core/memorycard.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/sstate.h"
#include "support/eventbus.h"

namespace PCSX {

namespace SaveStates {
struct Access;
}

struct SIORegisters {
    uint32_t data;
    uint32_t status;
    uint16_t mode;
    uint16_t control;
    uint16_t baud;
};

class SIO {
  public:
    // Advance any docked PocketStation by the elapsed R3000A cycles. Called from
    // R3000Acpu::branchTest() (the inter-burst boundary); public so the CPU can drive it.
    void stepPocketstation();

    // Advance an enabled PocketStation off REAL wall-clock time, for the true-standalone case where
    // the R3000A is not executing (emulation paused/stopped, or no game loaded) so branchTest never
    // fires and stepPocketstation() never runs. Driven once per GUI frame from the main loop's
    // not-running branch (see main.cc). MUST NOT be called while the core is running: that path is
    // mutually exclusive with stepPocketstation() and calling both would double-clock the device.
    void stepPocketstationWallClock();

    struct McdBlock {
        McdBlock() { reset(); }
        int mcd;
        int number;
        std::string titleAscii;
        std::string titleSjis;
        std::string titleUtf8;
        std::string id;
        std::string name;
        uint32_t fileSize;
        uint32_t iconCount;
        uint16_t icon[16 * 16 * 3];
        uint32_t allocState;
        int16_t nextBlock;
        void reset() {
            mcd = 0;
            number = 0;
            titleAscii.clear();
            titleSjis.clear();
            titleUtf8.clear();
            id.clear();
            name.clear();
            fileSize = 0;
            iconCount = 0;
            memset(icon, 0, sizeof(icon));
            allocState = 0;
            nextBlock = -1;
        }
        bool isErased() const { return (allocState & 0xa0) == 0xa0; }
        bool isChained() const { return (allocState & ~1) == 0x52; }
    };

    static constexpr size_t c_sectorSize = 8 * 16;            // 80h bytes per sector/frame
    static constexpr size_t c_blockSize = c_sectorSize * 64;  // 40h sectors per block
    static constexpr size_t c_cardSize = c_blockSize * 16;    // 16 blocks per frame(directory+15 saves)
    static constexpr size_t c_cardCount = 2;
    static constexpr size_t c_tapCardCount = 6;

    // Defined in sio.cc: wires the per-frame PocketStation catch-up to the VSync event.
    SIO();

    void write8(uint8_t value);
    void writeStatus16(uint16_t value);
    void writeMode16(uint16_t value);
    void writeCtrl16(uint16_t value);
    void writeBaud16(uint16_t value);

    uint8_t read8();
    uint16_t readStatus16();
    uint16_t readMode16() { return m_regs.mode; }
    uint16_t readCtrl16() { return m_regs.control; }
    uint16_t readBaud16() { return m_regs.baud; }

    void acknowledge();
    void init();
    void scheduledCallback();
    void reset();

    bool copyMcdFile(McdBlock block);
    void eraseMcdFile(const McdBlock &block);
    void eraseMcdFile(int mcd, int block) {
        McdBlock info;
        getMcdBlockInfo(mcd, block, info);
        eraseMcdFile(info);
    }
    int findFirstFree(int mcd);
    unsigned getFreeSpace(int mcd);
    unsigned getFileBlockCount(McdBlock block);
    void getMcdBlockInfo(int mcd, int block, McdBlock &info);
    char *getMcdData(int mcd);
    char *getMcdData(const McdBlock &block) { return getMcdData(block.mcd); }
    void loadMcd(const PCSX::u8string &path, int mcd) {
        if (mcd > 0 && mcd <= c_cardCount) m_memoryCard[mcd - 1].loadMcd(path);
    }
    void loadMcds(const PCSX::u8string &mcd1, const PCSX::u8string &mcd2) {
        m_memoryCard[0].loadMcd(mcd1);
        m_memoryCard[1].loadMcd(mcd2);
    }
    void saveMcd(int mcd);
    static constexpr int otherMcd(int mcd) {
        if ((mcd != 1) && (mcd != 2)) throw std::runtime_error("Bad memory card number");
        if (mcd == 1) return 2;
        return 1;
    }

    void togglePocketstationMode();
    // Rebuild the devices of enabled slots, e.g. after the kernel image setting changed.
    void reloadPocketstations();
    // Reconcile each device's flash with its card image, saving the card file when the device
    // wrote to it.
    void syncPocketstationFlash();
    // Docked PocketStation device for a card slot (0-based), or nullptr if none. Used by the GUI
    // LCD widget to read out the framebuffer.
    // The card in a slot, for the UI: it owns the dock state as well as the device.
    MemoryCard *getMemoryCard(unsigned slot) { return slot < c_cardCount ? &m_memoryCard[slot] : nullptr; }
    PocketStation::PocketStation *getPocketstation(unsigned slot) {
        return slot < c_cardCount ? m_memoryCard[slot].getPocketstation() : nullptr;
    }
    static constexpr int otherMcd(const McdBlock &block) { return otherMcd(block.mcd); }

  private:
    struct StatusFlags {
        enum : uint16_t {
            TX_DATACLEAR = 0x0001,  // 0 = pending transmit, 1 = transmit completed
            RX_FIFONOTEMPTY = 0x0002,
            TX_FINISHED = 0x0004,
            RX_PARITYERR = 0x0008,
            RX_OVERRUN =
                0x0010,  //(unlike SIO, this isn't RX FIFO Overrun flag), to-do: investigate this claim -skitchin
            FRAMING_ERR = 0x0020,
            SYNC_DETECT = 0x0040,
            ACK = 0x0080,  // ack input level
            CTS = 0x0100,  // unknown
            IRQ = 0x0200,
        };
    };
    struct ControlFlags {
        enum : uint16_t {
            TX_ENABLE = 0x0001,
            SELECT_ENABLE = 0x0002,
            RX_ENABLE = 0x0004,
            BREAK = 0x0008,
            RESET_ERR = 0x0010,
            RTS = 0x0020,
            RESET = 0x0040,
            RX_IRQMODE = 0x0100,  // FIFO byte count, to-do: implement
            TX_IRQEN = 0x0400,
            RX_IRQEN = 0x0800,
            ACK_IRQEN = 0x1000,
            WHICH_PORT = 0x2000,  // 0=/JOY1, 1=/JOY2
        };
    };
    enum {
        // MCD flags
        MCDST_CHANGED = 0x08,
    };

    struct PAD_Commands {
        enum : uint8_t {
            Read = 0x42,  // Read Command
            None = 0x00,  // No command, idle state
            Error = 0xFF  // Bad command
        };
    };
    struct DeviceType {
        enum : uint8_t {
            None = 0x00,            // No device selected yet
            PAD = 0x01,             // Pad Select
            NetYaroze = 0x21,       // Net Yaroze Select
            MemoryCard = 0x81,      // Memory Card Select
            TapMemoryCardB = 0x82,  // Multitap slot B Memory Card Select
            TapMemoryCardC = 0x83,  // Multitap slot C Memory Card Select
            TapMemoryCardD = 0x84,  // Multitap slot D Memory Card Select
            Ignore = 0xFF,          // Ignore incoming commands
        };
    };
    struct SelectedPort {
        enum : uint16_t {
            Port1 = 0x0000,
            Port2 = 0x2000,
        };
    };

    template <typename T, size_t buffer_size>
    class FIFO {
      public:
        ~FIFO() { clear(); }

        void clear() {
            while (!queue_.empty()) queue_.pop();
        }
        bool isEmpty() { return queue_.empty(); }
        T peek() {
            T ret = T();
            if (!queue_.empty()) {
                ret = queue_.front();
            }

            return ret;
        }
        T pull() {
            T ret = T();
            if (!queue_.empty()) {
                ret = queue_.front();
                queue_.pop();
            }

            return ret;
        }
        void push(T data) {
            if (queue_.size() >= buffer_size) {
                queue_.back() = data;
            } else {
                queue_.push(data);
            }
        }

        size_t size() { return queue_.size(); }

      private:
        std::queue<T> queue_;
    };

    friend MemoryCard;
    friend struct SaveStates::Access;

    static constexpr size_t c_padBufferSize = 0x1010;

    bool isReceiveIRQReady();
    bool isTransmitReady();
    static inline void scheduleInterrupt(uint64_t eCycle) {
        g_emulator->m_cpu->schedule(Schedule::SIO, eCycle);
#if 0
// Breaks Twisted Metal 2 intro
        m_statusReg &= ~RX_FIFONOTEMPTY;
        m_statusReg &= ~TX_DATACLEAR;
#endif
    }
    void transmitData();
    void updateFIFOStatus();
    void writePad(uint8_t value);
    void deselectCards();

    SIORegisters m_regs = {
        .status = StatusFlags::TX_DATACLEAR | StatusFlags::TX_FINISHED,  // Transfer Ready and the Buffer is Empty
    };

    uint8_t m_currentDevice = DeviceType::None;
    // The first byte of the current pad access: 01h, or 01h to 04h for a multitap's slots.
    uint8_t m_padAddress = DeviceType::PAD;

    // Pads
    uint8_t m_buffer[c_padBufferSize];
    uint32_t m_bufferIndex;
    uint32_t m_maxBufferIndex;
    uint32_t m_padState;

    MemoryCard m_memoryCard[c_cardCount] = {this, this};
    // The cards in a multitap's slots B to D: 0 to 2 on port 1, 3 to 5 on port 2. Slot A is card 1 or 2.
    // Each is read from its file the first time it is selected while inserted, so no file is created for a
    // slot nobody uses.
    MemoryCard m_tapMemoryCard[c_tapCardCount] = {this, this, this, this, this, this};
    bool m_tapMemoryCardLoaded[c_tapCardCount] = {};

    FIFO<uint8_t, 8> m_rxFIFO;

    // ---- PocketStation cycle-delta catch-up -------------------------------------------------
    // A docked PocketStation runs its ARM7 off the shared R3000A cycle counter. The catch-up is
    // driven from R3000Acpu::branchTest() (the inter-burst boundary both backends funnel through),
    // NOT from VSync: a whole card transaction (138 byte exchanges) fits inside one frame, so a
    // per-frame catch-up would never advance the ARM7 *between* SIO bytes and the kernel's COM/FIQ
    // handler could not keep up. At the inter-burst boundary the ARM7 stays within a few cycles of
    // current always, so its COM poll loop services each byte in time. When no device is docked the
    // call early-returns, so cost is ~zero when off. Sub-1-ARM-cycle deltas are accumulated (the
    // anchor only advances by the PSX cycles actually consumed) so frequent small calls don't
    // starve the device. (Declaration is in the public section above.)
    uint64_t m_lastPsxCycle = 0;   // R3000A cycle at the previous catch-up (now advanced fully to now).
    bool m_psxCycleValid = false;  // false until the first catch-up after a reset re-syncs the anchor.
    // A savestate load can move the cycle counter forward as well as back; drop the anchor so the
    // next catch-up re-syncs instead of running the whole jump through the ARM7.
    EventBus::Listener m_listener;
    // Per-device PSX-cycle catch-up remainder, accumulated in (PSX-cycle * armHz) units. Carrying the
    // fraction per device (rather than a single shared anchor) is what lets each docked PocketStation
    // convert at its OWN live CLK_MODE.FREQ clock while never discarding a sub-1-ARM-cycle delta.
    uint64_t m_psxArmAccum[c_cardCount] = {0};
    // Wall-clock standalone driver state (stepPocketstationWallClock). m_lastWallClock anchors real
    // elapsed time; the per-device remainder (ns*Hz) carries the sub-1-ARM-cycle fraction so frequent small
    // frame deltas don't starve the device (same accumulate-the-remainder discipline as the PSX path).
    // The two anchors cross-invalidate when control passes between paths so each re-syncs cleanly on
    // the running<->paused transition (no bogus first delta).
    std::chrono::steady_clock::time_point m_lastWallClock{};
    bool m_wallClockValid = false;
    uint64_t m_wallClockRemainderNs[c_cardCount] = {0};
    // The R3000A clock. The ARM7 clock is no longer a constant here: it is software-configurable via
    // CLK_MODE.FREQ and read per-device (PocketStation::armClockHz) at each catch-up site, so the
    // PSX->ARM cycle scale tracks an SWI-4 clock change. (Was a fixed kArmClockHz = 3997696.)
    static constexpr uint64_t kPsxClockHz = 33868800;
    // -----------------------------------------------------------------------------------------
};

}  // namespace PCSX
