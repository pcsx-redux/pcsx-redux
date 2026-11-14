/***************************************************************************
 *   Copyright (C) 2010 by Blade_Arma                                      *
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

#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"

namespace PCSX {

struct SaveStateWrapper;

class Counters {
  private:
    static inline void setIrq(uint32_t irq) { g_emulator->m_mem->setIRQ(irq); }
    uint32_t readCounterInternal(uint32_t index);
    void writeCounterInternal(uint32_t index, uint32_t value, int64_t startAdjust = 0);
    // Cycles a counter sits at 0 after a reset on target before counting again. Measured as 2 on
    // the system clock for Timer 2 (period target + 2); 0 on sysclock/8. Timers 0 and 1 unmeasured.
    uint32_t resetDwell(uint32_t index) const {
        return (index == 2 && m_rcnts[index].rate == 1 && m_rcnts[index].rateDen == 1) ? 2 : 0;
    }
    // A counter ticks every rate / rateDen CPU cycles. Only the dotclock has a fractional rate.
    void setRate(uint32_t index, uint32_t rate, uint32_t rateDen = 1) {
        m_rcnts[index].rate = rate;
        m_rcnts[index].rateDen = rateDen;
    }
    // The counter's exact start is cycleStart - startFrac / rateDen CPU cycles.
    // CPU cycles after cycleStart until the counter reaches `ticks`.
    uint64_t ticksToCycles(uint32_t index, uint64_t ticks) const {
        const auto& r = m_rcnts[index];
        if (r.rateDen <= 1) return ticks * r.rate;
        const uint64_t span = ticks * r.rate;
        if (span <= r.startFrac) return 0;
        return (span - r.startFrac + r.rateDen - 1) / r.rateDen;
    }
    // Ticks counted `cycles` CPU cycles after cycleStart.
    uint64_t cyclesToTicks(uint32_t index, uint64_t cycles) const {
        const auto& r = m_rcnts[index];
        if (r.rateDen <= 1) return cycles / r.rate;
        return (cycles * r.rateDen + r.startFrac) / r.rate;
    }
    // Move the exact start forward by `ticks`, keeping the fraction of a cycle.
    void advanceStart(uint32_t index, uint64_t ticks) {
        auto& r = m_rcnts[index];
        if (r.rateDen <= 1) {
            r.cycleStart += ticks * r.rate;
            return;
        }
        const uint64_t span = ticks * r.rate - r.startFrac;
        const uint64_t whole = (span + r.rateDen - 1) / r.rateDen;
        r.cycleStart += whole;
        r.startFrac = uint32_t(whole * r.rateDen - span);
    }
    void scheduleEvent(uint32_t index, uint32_t value);
    // Guest writes to counter 1 on the hblank clock count from the last hblank edge.
    void anchorToHblank(uint32_t index);

    void set();
    void reset(uint32_t index);
    void calculateHsync();
    uint32_t nextLineLength();
    uint32_t wholeCyclesPerLine() const { return m_lineNum / m_lineDen; }
    void recalculateRate(uint32_t index);

    struct Rcnt {
        uint16_t mode, target;
        uint32_t rate, irq, counterState, irqState;
        uint32_t rateDen = 1, startFrac = 0;
        uint64_t cycle, cycleStart;
        uint32_t gateStarted;  // Gate mode 3: first blank has occurred, counter is free running
    };

    // Gate mode helpers - derived from mode register, not stored
    static bool isGateEnabled(uint32_t index, uint16_t mode) { return (index <= 1) && (mode & RcSyncEnable); }
    static uint8_t gateSyncMode(uint16_t mode) { return (mode >> 1) & 3; }

    enum {
        RcSyncEnable = 0x0001,       // 0    Sync/gate enable
        Rc2Disable = 0x0001,         // 0    Timer 2: modes 0,3 = stop counter
        RcSyncMode0 = 0x0002,        // 1    Sync mode bit 0
        RcSyncMode1 = 0x0004,        // 2    Sync mode bit 1
        RcCountToTarget = 0x0008,    // 3
        RcIrqOnTarget = 0x0010,      // 4
        RcIrqOnOverflow = 0x0020,    // 5
        RcIrqRegenerate = 0x0040,    // 6
        RcIrqToggle = 0x0080,        // 7    IRQ toggle mode (0=pulse, 1=toggle bit10)
        Rc0PixelClock = 0x0100,      // 8    dotclock (varies with GPU hres)
        Rc1HSyncClock = 0x0100,      // 8
        Rc2Unknown8 = 0x0100,        // 8    ?
        Rc0Unknown9 = 0x0200,        // 9    ?
        Rc1Unknown9 = 0x0200,        // 9    ?
        Rc2OneEighthClock = 0x0200,  // 9
        RcIrqRequest = 0x0400,       // 10   Interrupt request flag (0 disabled or during int, 1 request)
        RcCountEqTarget = 0x0800,    // 11
        RcOverflow = 0x1000,         // 12
        RcUnknown13 = 0x2000,        // 13   ? (always zero)
        RcUnknown14 = 0x4000,        // 14   ? (always zero)
        RcUnknown15 = 0x8000         // 15   ? (always zero)
    };

    static const uint32_t CounterQuantity = 4;
    static const uint32_t CountToOverflow = 0;
    static const uint32_t CountToTarget = 1;

    static inline const uint32_t VBlankStart[] = {243, 256};

    static const uint16_t JITTER_FLAGS = (Rc2OneEighthClock | RcIrqRegenerate | RcCountToTarget);

    Rcnt m_rcnts[CounterQuantity];

    uint32_t m_hSyncCount = 0;
    // Incremented at every vblank start; its parity names the field that vblank opened.
    uint32_t m_fieldCount = 0;
    uint32_t m_audioFrames = 0;
    // How far the CPU may trail the audio clock, in 44.1kHz frames (250ms).
    static constexpr int32_t kMaxAudioLagFrames = 44100 / 4;

    uint32_t m_HSyncTotal[PCSX::Emulator::PSX_TYPE_PAL + 1];  // 2
    // A scanline lasts m_lineNum / m_lineDen CPU cycles, which is not a whole number. Each line
    // gets the floor or the ceiling, and m_lineRemainder carries the fraction to the next one.
    uint64_t m_lineNum = 1, m_lineDen = 1, m_lineRemainder = 0;
  public:
    uint64_t m_psxNextCounter;
    bool m_pollSIO1 = false;
    void init();
    void update();

    void writeCounter(uint32_t index, uint32_t value);
    void writeMode(uint32_t index, uint32_t value);
    void writeTarget(uint32_t index, uint32_t value);

    uint32_t readCounter(uint32_t index);
    uint32_t readMode(uint32_t index);
    uint32_t readTarget(uint32_t index);
    // The value a guest read of the counter would return right now, without
    // updating any counter state, so debugger views can call it.
    uint32_t peekCounter(uint32_t index);

    // Brings the scanline position up to date, then returns the hblanks elapsed since the
    // last vblank start. Only call from the CPU side: it can raise the vblank IRQ.
    uint32_t linesSinceVBlank();
    // Lines from one vblank IRQ to the next at the current system type.
    uint32_t linesPerFrame();
    // True for the field silicon runs at 263 lines in NTSC 480i, the one GPUSTAT.31 reads 1
    // in. Redux runs both fields at the same length.
    bool longField() const { return (m_fieldCount & 1) == 0; }

    void serialize(SaveStateWrapper *);
    void deserialize(const SaveStateWrapper *);
};

}  // namespace PCSX
