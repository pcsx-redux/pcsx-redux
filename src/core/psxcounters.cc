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

/*
 * Internal PSX counters.
 */

#include "core/psxcounters.h"

#include "core/debug.h"
#include "core/gpu.h"
#include "core/sio1.h"
#include "fmt/printf.h"
#include "spu/interface.h"

template <typename... Args>
void verboseLog(int32_t level, const char *str, const Args &...args) {
    PSXHW_LOG(str, args...);
}

inline void PCSX::Counters::writeCounterInternal(uint32_t index, uint32_t value, int64_t startAdjust) {
    if (value > 0xffff) {
        verboseLog(1, "[RCNT %i] writeCounter > 0xffff: %x\n", index, value);
        value &= 0xffff;
    }

    m_rcnts[index].cycleStart = PCSX::g_emulator->m_cpu->m_regs.cycle;
    m_rcnts[index].cycleStart -= value * m_rcnts[index].rate;
    // A positive adjust puts cycleStart in the future: the counter reads 0 until then.
    m_rcnts[index].cycleStart += startAdjust;

    // Hardware tested (SCPH-5501): counter reaches target value briefly before
    // resetting. Fast reads can observe the target value; slow reads (printf) cannot.
    // The comparison boundary needs more precise measurement to determine if
    // reset happens on the same cycle as reaching target or the next cycle.
    // TODO: determine exact reset timing with cycle-accurate measurement.
    // Schedule next event: target hit first (for flag/IRQ), then overflow.
    // If RcCountToTarget is not set, the counter doesn't reset at target,
    // but bit 11 (RcCountEqTarget) is still set when the counter passes
    // the target value. However, if target == 0xFFFF, the target and
    // overflow events coincide - skip straight to overflow to avoid
    // redundant processing.
    if (value < m_rcnts[index].target && m_rcnts[index].target != 0xffff) {
        m_rcnts[index].cycle = m_rcnts[index].target * m_rcnts[index].rate;
        m_rcnts[index].counterState = CountToTarget;
    } else {
        m_rcnts[index].cycle = 0xffff * m_rcnts[index].rate;
        m_rcnts[index].counterState = CountToOverflow;
    }
    verboseLog(5, "[RCNT %i] scount: %x\n", index, value);
}

inline uint32_t PCSX::Counters::readCounterInternal(uint32_t index) {
    uint64_t count;

    count = PCSX::g_emulator->m_cpu->m_regs.cycle;
    if (count < m_rcnts[index].cycleStart) return 0;  // still in the post-reset dwell
    count -= m_rcnts[index].cycleStart;

    // The reset event is only processed between blocks, so a read can land after the
    // counter reached its target but before reset() ran. Report what the hardware shows
    // there: the target for that one cycle, then the dwell, then the new count.
    if (m_rcnts[index].counterState == CountToTarget && (m_rcnts[index].mode & RcCountToTarget)) {
        const uint64_t span = uint64_t(m_rcnts[index].target) * m_rcnts[index].rate;
        if (count > span) {
            const uint64_t over = count - span;
            const uint64_t dwell = resetDwell(index);
            if (over < dwell) return 0;
            return ((over - dwell) / m_rcnts[index].rate) & 0xffff;
        }
    }
    count /= m_rcnts[index].rate;

    if (count > 0xffff) {
        verboseLog(1, "[RCNT %i] readCounter > 0xffff: %x\n", index, count);
        count &= 0xffff;
    }

    return count;
}

void PCSX::Counters::set() {
    m_psxNextCounter = PCSX::g_emulator->m_cpu->m_regs.cycle;
    uint64_t next = 0x7fffffffffffffff;

    for (int i = 0; i < CounterQuantity; ++i) {
        int64_t countToUpdate = m_rcnts[i].cycle - (m_psxNextCounter - m_rcnts[i].cycleStart);

        if (countToUpdate < 0) {
            next = 0;
            break;
        }

        if (countToUpdate < (int64_t)next) {
            next = countToUpdate;
        }
    }

    m_psxNextCounter += next;
}

void PCSX::Counters::reset(uint32_t index) {
    uint64_t count;

    if (m_rcnts[index].counterState == CountToTarget) {
        if (m_rcnts[index].mode & RcCountToTarget) {
            // Restart from the cycle the target was reached, keeping the sub-tick phase so a
            // late event does not stretch the period. On the system clock the counter then sits
            // at 0 for 2 more cycles, making the period target + 2 (measured on SCPH-1000, 5501
            // and 7001 for Timer 2; the /8 source stays exactly 8 * target). Timers 0 and 1 are
            // unmeasured and get no dwell.
            const uint64_t elapsed = PCSX::g_emulator->m_cpu->m_regs.cycle - m_rcnts[index].cycleStart;
            const uint64_t over = elapsed - uint64_t(m_rcnts[index].target) * m_rcnts[index].rate;
            const uint64_t dwell = resetDwell(index);
            // The count the next event is chosen from is the one after the dwell: a reset
            // that runs late but still inside the dwell leaves the counter at 0, not at `over`.
            count = over > dwell ? (over - dwell) / m_rcnts[index].rate : 0;
            writeCounterInternal(index, count, int64_t(dwell) - int64_t(over) + int64_t(count * m_rcnts[index].rate));
        } else {
            count = readCounterInternal(index);
            writeCounterInternal(index, count);
        }

        if (m_rcnts[index].mode & RcIrqOnTarget) {
            if ((m_rcnts[index].mode & RcIrqRegenerate) || (!m_rcnts[index].irqState)) {
                verboseLog(3, "[RCNT %i] irq: %x\n", index, count);
                setIrq(m_rcnts[index].irq);
                m_rcnts[index].irqState = true;
            }
        }

        m_rcnts[index].mode |= RcCountEqTarget;
    } else if (m_rcnts[index].counterState == CountToOverflow) {
        // Keep the sub-tick phase across the wrap, as for the target reset above.
        const uint64_t elapsed = PCSX::g_emulator->m_cpu->m_regs.cycle - m_rcnts[index].cycleStart;
        const uint64_t over = elapsed - uint64_t(0xffff) * m_rcnts[index].rate;
        count = over / m_rcnts[index].rate;
        writeCounterInternal(index, count, -int64_t(over % m_rcnts[index].rate));

        if (m_rcnts[index].mode & RcIrqOnOverflow) {
            if ((m_rcnts[index].mode & RcIrqRegenerate) || (!m_rcnts[index].irqState)) {
                verboseLog(3, "[RCNT %i] irq: %x\n", index, count);
                setIrq(m_rcnts[index].irq);
                m_rcnts[index].irqState = true;
            }
        }

        m_rcnts[index].mode |= RcOverflow;
    }

    // IRQ request flag (bit 10) behavior depends on bit 7 (toggle mode):
    // Pulse mode (bit7=0): bit 10 resets to 1 after IRQ (short pulse low)
    // Toggle mode (bit7=1): bit 10 toggles (XOR) on each IRQ
    // Hardware verified: toggle mode produces different bit10 state than pulse mode.
    if (m_rcnts[index].mode & RcIrqToggle) {
        m_rcnts[index].mode ^= RcIrqRequest;
    } else {
        m_rcnts[index].mode |= RcIrqRequest;
    }

    set();
}

void PCSX::Counters::update() {
    const uint64_t cycle = PCSX::g_emulator->m_cpu->m_regs.cycle;

    // The SPU mixes up to the CPU's cycle and no further, so let it have this one before
    // possibly blocking on the audio device below.
    g_emulator->m_spu->advanceTo(cycle);

    {
        uint64_t prev = g_emulator->m_cpu->m_regs.previousCycles;
        uint64_t diff = cycle - prev;
        // Map elapsed CPU cycles to the realtime (1:1 emulated:hardware) audio-frame target. Emulation
        // speed is no longer controlled here: it lives entirely at the audio sink (SPU::Speed), which is
        // the master clock both this counter and the SPU thread pace against. 44100 == 4410000 / 100, and
        // the old Emulator::SettingScaler defaulted to 100, so this is byte-identical to the prior default.
        diff *= 44100;
        diff /= g_emulator->m_psxClockSpeed;
        uint32_t target = m_audioFrames + diff;
        // The CPU runs this many frames ahead of what the device has played, so the SPU, which
        // cannot mix past the CPU, has something buffered when the device asks for it.
        const uint32_t lead = g_emulator->m_spu->getLeadFrames();
        uint32_t newFrames = g_emulator->m_spu->getCurrentFrames() + lead;
        int32_t framesDiff = target - newFrames;
        if (framesDiff > 0) {
            // Consume only the cycles the whole frames above stand for. Dropping the fraction on
            // every wait ran the emulation slightly faster than realtime, and with the SPU unable
            // to mix ahead of the CPU, that surplus piles up in the audio queue as extra delay.
            g_emulator->m_cpu->m_regs.previousCycles = prev + diff * g_emulator->m_psxClockSpeed / 44100;
            g_emulator->m_spu->waitForGoal(target - lead);
            m_audioFrames = target;
        } else if (framesDiff < -kMaxAudioLagFrames) {
            // The host couldn't sustain the requested speed. Cap the debt, otherwise returning to 1x runs
            // the CPU unthrottled until it has caught up. Resetting it to zero would stall every update on
            // the next audio callback instead.
            g_emulator->m_cpu->m_regs.previousCycles = cycle;
            m_audioFrames = newFrames - kMaxAudioLagFrames;
        }
    }

    // rcnt 0.
    if (int64_t(cycle - m_rcnts[0].cycleStart) >= int64_t(m_rcnts[0].cycle)) {
        reset(0);
    }

    // rcnt 1.
    if (int64_t(cycle - m_rcnts[1].cycleStart) >= int64_t(m_rcnts[1].cycle)) {
        reset(1);
    }

    // rcnt 2.
    if (int64_t(cycle - m_rcnts[2].cycleStart) >= int64_t(m_rcnts[2].cycle)) {
        reset(2);
    }
    // rcnt base.
    if (int64_t(cycle - m_rcnts[3].cycleStart) >= int64_t(m_rcnts[3].cycle)) {
        reset(3);

        m_hSyncCount++;

        // Counter 0 gate: triggered by Hblank
        if (isGateEnabled(0, m_rcnts[0].mode)) {
            switch (gateSyncMode(m_rcnts[0].mode)) {
                case 1:  // Reset at Hblank
                case 2:  // Reset at Hblank + pause outside
                    writeCounterInternal(0, 0);
                    break;
                case 3:  // Pause until first Hblank, then free run
                    if (!m_rcnts[0].gateStarted) {
                        m_rcnts[0].gateStarted = 1;
                        recalculateRate(0);
                        writeCounterInternal(0, 0);
                        set();
                    }
                    break;
            }
        }

        // SIO1 callback on hsync to process data
        if (m_pollSIO1) {
            PCSX::g_emulator->m_sio1->poll();
        }

        // Trigger VBlank IRQ when VBlank starts
        if (m_hSyncCount == VBlankStart[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()]) {
            m_fieldCount++;
            setIrq(0x01);
            PCSX::g_emulator->vsync();

            // Counter 1 gate: triggered by VBlank start
            if (isGateEnabled(1, m_rcnts[1].mode)) {
                switch (gateSyncMode(m_rcnts[1].mode)) {
                    case 1:  // Reset at VBlank
                    case 2:  // Reset at VBlank + pause outside
                        writeCounterInternal(1, 0);
                        break;
                    case 3:  // Pause until first VBlank, then free run
                        if (!m_rcnts[1].gateStarted) {
                            m_rcnts[1].gateStarted = 1;
                            recalculateRate(1);
                            writeCounterInternal(1, 0);
                            set();
                        }
                        break;
                }
            }
        }

        if (m_hSyncCount >= m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()]) {
            m_hSyncCount = 0;
        }
    }
}

void PCSX::Counters::writeCounter(uint32_t index, uint32_t value) {
    verboseLog(2, "[RCNT %i] writeCounter: %x\n", index, value);

    update();
    writeCounterInternal(index, value);
    set();
}

void PCSX::Counters::recalculateRate(uint32_t index) {
    uint16_t value = m_rcnts[index].mode;
    switch (index) {
        case 0:
            if (value & Rc0PixelClock) {
                // Dotclock rate depends on GPU horizontal resolution and video standard.
                static constexpr uint32_t dotclockDividers[] = {10, 8, 5, 4, 7, 7};
                auto hres = PCSX::g_emulator->m_gpu->m_display.info.hres;
                auto videoMode = PCSX::g_emulator->m_gpu->m_display.info.mode;
                uint32_t divider = dotclockDividers[static_cast<int>(hres)];
                uint32_t videoCyclesPerScanline = (videoMode == GPU::CtrlDisplayMode::VM_PAL) ? 3406 : 3413;
                uint32_t dotsPerScanline = videoCyclesPerScanline / divider;
                uint32_t cpuCyclesPerScanline =
                    (PCSX::g_emulator->m_psxClockSpeed /
                     (FrameRate[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()] *
                      m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()]));
                m_rcnts[index].rate = std::max<uint32_t>(cpuCyclesPerScanline / dotsPerScanline, 1);
            } else {
                m_rcnts[index].rate = 1;
            }
            break;
        case 1:
            if (value & Rc1HSyncClock) {
                m_rcnts[index].rate = (PCSX::g_emulator->m_psxClockSpeed /
                                       (FrameRate[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()] *
                                        m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()]));
            } else {
                m_rcnts[index].rate = 1;
            }
            break;
        case 2:
            if (value & Rc2OneEighthClock) {
                m_rcnts[index].rate = 8;
            } else {
                m_rcnts[index].rate = 1;
            }
            // Timer 2 sync modes: 0,3 = stop counter; 1,2 = free run
            // Hardware verified (SCPH-5501): modes 0,3 produce delta=0, modes 1,2 run normally.
            if (value & Rc2Disable) {
                uint8_t syncMode = gateSyncMode(value);
                if (syncMode == 0 || syncMode == 3) {
                    m_rcnts[index].rate = 0xffffffff;
                }
            }
            break;
    }
}

void PCSX::Counters::writeMode(uint32_t index, uint32_t value) {
    verboseLog(1, "[RCNT %i] writeMode: %x\n", index, value);

    update();
    m_rcnts[index].mode = value | RcIrqRequest;  // Hardware sets bit 10 on mode write
    m_rcnts[index].irqState = false;

    recalculateRate(index);

    // Initialize gate state
    m_rcnts[index].gateStarted = 0;

    if (isGateEnabled(index, value)) {
        uint8_t syncMode = gateSyncMode(value);
        if (syncMode == 2 || syncMode == 3) {
            // Mode 2: pause outside blank. Mode 3: pause until first blank.
            // Both start paused.
            m_rcnts[index].rate = 0xffffffff;
        }
    }

    writeCounterInternal(index, 0);
    set();
}

void PCSX::Counters::writeTarget(uint32_t index, uint32_t value) {
    verboseLog(1, "[RCNT %i] wtarget: %x\n", index, value);
    update();

    // The target is only 16 bits. To make sure of this, the 32-bit write handlers mask it with 0xFFFF
    m_rcnts[index].target = value;
    writeCounterInternal(index, readCounterInternal(index));
    set();
}

uint32_t PCSX::Counters::readCounter(uint32_t index) {
    update();
    uint32_t count = readCounterInternal(index);

    // Parasite Eve 2 fix - artificial clock jitter based on PCSX::Emulator::BIAS
    // TODO: any other games depend on getting excepted value from RCNT?
    if (index == 2 && m_rcnts[index].counterState == CountToTarget &&
        (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingRCntFix>() ||
         ((m_rcnts[index].mode & 0x2FF) == JITTER_FLAGS))) {
        /*
         *The problem is that...
         *
         *We generate too many cycles during PSX HW hardware operations.
         *
         *OR
         *
         *We simply count too many cycles here for RCNTs.
         *
         *OR
         *
         *RCNT implementation here is only 99% compatible. Assumed this since easities to fix (only PE2 known to be
         *affected).
         */
        static uint32_t clast = 0xffff;
        static uint64_t cylast = 0;
        uint64_t count1 = count;
        count /= PCSX::Emulator::BIAS;
        verboseLog(4, "[RCNT %i] rcountpe2: %x %x %x (%u)\n", index, count, count1, clast,
                   (PCSX::g_emulator->m_cpu->m_regs.cycle - cylast));
        cylast = PCSX::g_emulator->m_cpu->m_regs.cycle;
        clast = count;
    }

    verboseLog(2, "[RCNT %i] rcount: %x\n", index, count);
    return count;
}

uint32_t PCSX::Counters::peekCounter(uint32_t index) { return readCounterInternal(index); }

uint32_t PCSX::Counters::readMode(uint32_t index) {
    update();

    uint16_t mode = m_rcnts[index].mode;
    m_rcnts[index].mode &= 0xe7ff;

    verboseLog(2, "[RCNT %i] rmode: %x\n", index, mode);
    return mode;
}

uint32_t PCSX::Counters::readTarget(uint32_t index) {
    verboseLog(2, "[RCNT %i] rtarget: %x\n", index, m_rcnts[index].target);
    return m_rcnts[index].target;
}

uint32_t PCSX::Counters::linesSinceVBlank() {
    update();
    auto video = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>();
    uint32_t total = m_HSyncTotal[video];
    return (m_hSyncCount + total - VBlankStart[video]) % total;
}

void PCSX::Counters::calculateHsync() {
    m_HSyncTotal[PCSX::Emulator::PSX_TYPE_NTSC] = 263;
    m_HSyncTotal[PCSX::Emulator::PSX_TYPE_PAL] = 314;  // actually one more on odd lines for PAL
    if (PCSX::g_emulator->config().VSyncWA) {
        m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()] =
            m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()] / PCSX::Emulator::BIAS;
#if 0
    } else if (PCSX::g_emulator->config().HackFix) {
        m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()] =
            m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()] + 1;
#endif
    }
}

void PCSX::Counters::init() {
    calculateHsync();

    // rcnt 0.
    m_rcnts[0].rate = 1;
    m_rcnts[0].irq = 0x10;

    // rcnt 1.
    m_rcnts[1].rate = 1;
    m_rcnts[1].irq = 0x20;

    // rcnt 2.
    m_rcnts[2].rate = 1;
    m_rcnts[2].irq = 0x40;

    // rcnt base.
    m_rcnts[3].rate = 1;
    m_rcnts[3].mode = RcCountToTarget;
    m_rcnts[3].target = (PCSX::g_emulator->m_psxClockSpeed /
                         (FrameRate[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()] *
                          m_HSyncTotal[PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>()]));

    for (int i = 0; i < CounterQuantity; ++i) {
        writeCounterInternal(i, 0);
    }

    m_hSyncCount = 0;
    m_fieldCount = 0;
    m_audioFrames = PCSX::g_emulator->m_spu->getCurrentFrames();
    set();
}
