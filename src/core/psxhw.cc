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

#include "core/psxhw.h"

#include <stdint.h>
#include <string.h>

#include "core/alloc-tracker.h"
#include "core/cdrom.h"
#include "core/debug.h"
#include "core/gpu.h"
#include "core/guest-command.h"
#include "core/hwevents.h"
#include "core/logger.h"
#include "core/mdec.h"
#include "core/pio-cart.h"
#include "core/psxemulator.h"
#include "core/sio.h"
#include "core/sio1.h"
#include "lua/luawrapper.h"
#include "spu/interface.h"
#include "supportpsx/memory.h"

static constexpr bool between(uint32_t val, uint32_t beg, uint32_t end) {
    return (beg > end) ? false : (val >= beg && val <= end - 3);
}

static constexpr bool addressInRegisterSpace(uint32_t address) {
    uint32_t masked_addr = address & 0x1fffffff;

    return (between(masked_addr, 0x1f801000, 0x1f801023) ||               // MEMCTRL
            between(masked_addr, 0x1f801060, 0x1f801063) ||               // RAM_SIZE
            between(masked_addr, 0x1f801070, 0x1f801077) ||               // IRQCTRL
            between(masked_addr & 0xffffff0f, 0x1f801000, 0x1f801003) ||  // DMAx.ADDR
            between(masked_addr & 0xffffff0f, 0x1f801008, 0x1f80100f) ||  // DMAx.CTRL/MIRR
            between(masked_addr, 0x1f8010f0, 0x1f8010f7) ||               // DMA.DPCR/DICR
            between(masked_addr, 0x1f801100, 0x1f80110b) ||               // Timer 0
            between(masked_addr, 0x1f801110, 0x1f80111b) ||               // Timer 1
            between(masked_addr, 0x1f801120, 0x1f80112b));                // Timer 2
}

void PCSX::HW::reset() {
    if (g_emulator->settings.get<Emulator::SettingSpuIrq>()) g_emulator->m_mem->setIRQ(0x200);

    memset(g_emulator->m_mem->m_hard, 0, 0x10000);

    g_emulator->m_mdec->init();
    g_emulator->m_cdrom->reset();
    g_emulator->m_counters->init();
    g_emulator->m_spu->resetCaptureBuffer();
}

uint8_t PCSX::HW::read8(uint32_t add) {
    uint8_t hard;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801040:
            hard = g_emulator->m_sio->read8();
            break;
        case 0x1f801050:  // rx/tx data register
            hard = g_emulator->m_sio1->readData8();
            break;
        case 0x1f801054:  // stat register
            hard = g_emulator->m_sio1->readStat8();
            // Log command below is overly spammy
            break;
        case 0x1f801058:  // mode register
            hard = g_emulator->m_sio1->readMode8();
            break;
        case 0x1f80105a:  // control register
            hard = g_emulator->m_sio1->readCtrl8();
            break;
        case 0x1f80105c:  // guizmo register
            hard = g_emulator->m_sio1->readGuizmo8();
            break;
        case 0x1f80105e:  // baudrate register
            hard = g_emulator->m_sio1->readBaud8();
            break;
        case 0x1f801800:
            hard = g_emulator->m_cdrom->read0();
            break;
        case 0x1f801801:
            hard = g_emulator->m_cdrom->read1();
            break;
        case 0x1f801802:
            hard = g_emulator->m_cdrom->read2();
            break;
        case 0x1f801803:
            hard = g_emulator->m_cdrom->read3();
            break;
        case 0x1f802040:
            hard = 2;
            break;
        case 0x1f802080:
            hard = 0x50;
            break;
        case 0x1f802081:
            hard = 0x43;
            break;
        case 0x1f802082:
            hard = 0x53;
            break;
        case 0x1f802083:
            hard = 0x58;
            break;
        case 0x1f802088:
            hard = g_emulator->m_debug->m_checkKernel;
            break;
        default:
            hard = g_emulator->m_mem->m_hard[hwadd & 0xffff];
            return hard;
    }

    return hard;
}

uint16_t PCSX::HW::read16(uint32_t add) {
    uint16_t hard;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801070: {
            uint32_t hard = g_emulator->m_mem->readHardwareRegister<Memory::ISTAT>();
            return hard;
        }
        case 0x1f801074: {
            uint32_t hard = g_emulator->m_mem->readHardwareRegister<Memory::IMASK>();
            return hard;
        }
        case 0x1f801040:
            hard = g_emulator->m_sio->read8();
            hard |= g_emulator->m_sio->read8() << 8;
            break;
        case 0x1f801044:
            hard = g_emulator->m_sio->readStatus16();
            break;
        case 0x1f801048:
            hard = g_emulator->m_sio->readMode16();
            break;
        case 0x1f80104a:
            hard = g_emulator->m_sio->readCtrl16();
            break;
        case 0x1f80104e:
            hard = g_emulator->m_sio->readBaud16();
            break;
        case 0x1f801050:  // rx/tx data register
            hard = g_emulator->m_sio1->readData16();
            break;
        case 0x1f801054:  // stat register
            hard = g_emulator->m_sio1->readStat16();
            // Log command below is overly spammy
            break;
        case 0x1f801058:  // mode register
            hard = g_emulator->m_sio1->readMode16();
            break;
        case 0x1f80105a:  // control register
            hard = g_emulator->m_sio1->readCtrl16();
            break;
        case 0x1f80105c:  // guizmo register
            hard = g_emulator->m_sio1->readGuizmo16();
            break;
        case 0x1f80105e:  // baudrate register
            hard = g_emulator->m_sio1->readBaud16();
            break;
            /* Fixes Armored Core misdetecting the Link cable being detected.
             * We want to turn that thing off and force it to do local multiplayer instead.
             * Thanks Sony for the fix, they fixed it in their PS Classic fork.
             */
            /* Stat's value set in SIO1/m_sio1, Armored Core local multiplayer is working.
            case 0x1f801054:
                return 0x80;
            */

        case 0x1f801100:
            hard = g_emulator->m_counters->readCounter(0);
            break;
        case 0x1f801104:
            hard = g_emulator->m_counters->readMode(0);
            break;
        case 0x1f801108:
            hard = g_emulator->m_counters->readTarget(0);
            break;
        case 0x1f801110:
            hard = g_emulator->m_counters->readCounter(1);
            break;
        case 0x1f801114:
            hard = g_emulator->m_counters->readMode(1);
            break;
        case 0x1f801118:
            hard = g_emulator->m_counters->readTarget(1);
            break;
        case 0x1f801120:
            hard = g_emulator->m_counters->readCounter(2);
            break;
        case 0x1f801124:
            hard = g_emulator->m_counters->readMode(2);
            break;
        case 0x1f801128:
            hard = g_emulator->m_counters->readTarget(2);
            break;

            // case 0x1f802030: hard =   //int_2000????
            // case 0x1f802040: hard =//dip switches...??

        case 0x1f802080:
            hard = 0x4350;
            break;
        case 0x1f802082:
            hard = 0x5853;
            break;

        default:
            // SPU registers, plus the per-voice current volume block at 1f801e00h.
            if ((hwadd >= 0x1f801c00) && (hwadd < 0x1f801e60)) {
                hard = g_emulator->m_spu->readRegister(add);
            } else {
                uint16_t *ptr = (uint16_t *)&g_emulator->m_mem->m_hard[add & 0xffff];
                hard = *ptr;
            }
            return hard;
    }

    uint16_t *ptr = (uint16_t *)&g_emulator->m_mem->m_hard[add & 0xffff];
    return hard;
}

uint32_t PCSX::HW::read32(uint32_t add) {
    uint32_t hard;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801008: {
            hard = g_emulator->m_mem->readHardwareRegister<0x1008>();
            return hard;
        }
        case 0x1f801040:
            hard = g_emulator->m_sio->read8();
            hard |= g_emulator->m_sio->read8() << 8;
            hard |= g_emulator->m_sio->read8() << 16;
            hard |= g_emulator->m_sio->read8() << 24;
            break;
        case 0x1f801050:  // rx/tx data register
            hard = g_emulator->m_sio1->readData32();
            break;
        case 0x1f801054:  // stat register
            hard = g_emulator->m_sio1->readStat32();
            // Log command below is overly spammy
            break;
        case 0x1f80105c:  // guizmo register
            hard = g_emulator->m_sio1->readGuizmo32();
            break;
        case 0x1f801060: {
            hard = g_emulator->m_mem->readHardwareRegister<0x1060>();
            return hard;
        }
        case 0x1f801070: {
            hard = g_emulator->m_mem->readHardwareRegister<Memory::ISTAT>();
            return hard;
        }
        case 0x1f801074: {
            hard = g_emulator->m_mem->readHardwareRegister<Memory::IMASK>();
            return hard;
        }
        case 0x1f801810:
            hard = g_emulator->m_gpu->readData();
            break;
        case 0x1f801814:
            hard = PCSX::g_emulator->m_gpu->readStatusFromBus();
            break;

        case 0x1f801820:
            hard = g_emulator->m_mdec->read0();
            break;
        case 0x1f801824:
            hard = g_emulator->m_mdec->read1();
            break;
        // time for rootcounters :)
        case 0x1f801100:
            hard = g_emulator->m_counters->readCounter(0);
            break;
        case 0x1f801104:
            hard = g_emulator->m_counters->readMode(0);
            break;
        case 0x1f801108:
            hard = g_emulator->m_counters->readTarget(0);
            break;
        case 0x1f801110:
            hard = g_emulator->m_counters->readCounter(1);
            break;
        case 0x1f801114:
            hard = g_emulator->m_counters->readMode(1);
            break;
        case 0x1f801118:
            hard = g_emulator->m_counters->readTarget(1);
            break;
        case 0x1f801120:
            hard = g_emulator->m_counters->readCounter(2);
            break;
        case 0x1f801124:
            hard = g_emulator->m_counters->readMode(2);
            break;
        case 0x1f801128:
            hard = g_emulator->m_counters->readTarget(2);
            break;
        case 0x1f801014:
            hard = g_emulator->m_mem->readHardwareRegister<0x1014>();
            return hard;
        case 0x1f802080:
            hard = 0x58534350;
            break;
        case 0x1f80208c: {
            uint32_t size = g_emulator->m_cpu->m_regs.GPR.n.a0;
            hard = g_emulator->m_mem->msanAlloc(size);
            break;
        }
        case 0x1f802090: {
            uint32_t ptr = g_emulator->m_cpu->m_regs.GPR.n.a0;
            uint32_t size = g_emulator->m_cpu->m_regs.GPR.n.a1;
            hard = g_emulator->m_mem->msanRealloc(ptr, size);
            break;
        }
        case 0x1f802094: {
            uint32_t headerAddr = g_emulator->m_cpu->m_regs.GPR.n.a0;
            PSXAddress headerAddrInfo(headerAddr);
            switch (headerAddrInfo.type) {
                case PSXAddress::Type::RAM:
                case PSXAddress::Type::MSAN: {
                    uint32_t ptr = g_emulator->m_mem->read32(headerAddr) & 0xffffff;
                    if (ptr == PCSX::Memory::c_msanChainMarker) {
                        return g_emulator->m_mem->msanGetChainPtr(headerAddr);
                    } else {
                        return ptr;
                    }
                }
                default: {
                    g_system->printf(_("Called msanGetChainPtr with invalid header pointer %8.8lx\n"), headerAddr);
                    g_system->testQuit(PCSX::Memory::c_msanExitCode);
                    return 0xffffffff;
                }
            }
        }
        default: {
            uint32_t *ptr = (uint32_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
            hard = SWAP_LEu32(*ptr);
            return hard;
        }
    }
    uint32_t *ptr = (uint32_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
    *ptr = hard;
    return hard;
}

namespace {

void recordSPUWrite(uint32_t hwadd, uint16_t value, uint32_t width) {
    auto &events = *PCSX::g_emulator->m_hwEvents;
    if (!events.armed(PCSX::HWEvents::Source::SPU)) [[likely]] return;
    events.record(PCSX::HWEvents::SPURegWrite, hwadd, value, width);
    // The SPU decodes KON and KOFF on even addresses only, so a byte write to an
    // odd one keys nothing.
    if (hwadd & 1) return;
    // The upper halves of KON and KOFF hold voices 16 to 23 in their low byte.
    const uint32_t voices = (uint32_t(value) << ((hwadd & 2) ? 16 : 0)) & 0xffffff;
    switch (hwadd & ~3u) {
        case 0x1f801d88:
            events.record(PCSX::HWEvents::SPUKeyOn, voices);
            break;
        case 0x1f801d8c:
            events.record(PCSX::HWEvents::SPUKeyOff, voices);
            break;
    }
}

}  // namespace

void PCSX::HW::write8(uint32_t add, uint32_t rawvalue) {
    uint8_t value = (uint8_t)rawvalue;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801040:
            g_emulator->m_sio->write8(value);
            break;
        case 0x1f801050:  // rx/tx data register
            g_emulator->m_sio1->writeData8(value);
            break;
        case 0x1f801054:  // stat register
            g_emulator->m_sio1->writeStat8(value);
            break;
        case 0x1f801058:  // mode register
            g_emulator->m_sio1->writeMode8(value);
            break;
        case 0x1f80105a:  // control register
            g_emulator->m_sio1->writeCtrl8(value);
            break;
        case 0x1f80105c:  // guizmo register
            g_emulator->m_sio1->writeGuizmo(rawvalue);
            return;
        case 0x1f80105d:  // guizmo register again
            g_emulator->m_sio1->writeGuizmo(((uint16_t)value) << 8);
            return;
        case 0x1f80105e:  // baudrate register
            g_emulator->m_sio1->writeBaud16(rawvalue);
            return;
        case 0x1f80105f:  // baudrate register, upper 8 bits
            g_emulator->m_sio1->writeBaud16(((uint16_t)value) << 8);
            return;
        case 0x1f801800:
            g_emulator->m_cdrom->write0(value);
            break;
        case 0x1f801801:
            g_emulator->m_cdrom->write1(value);
            break;
        case 0x1f801802:
            g_emulator->m_cdrom->write2(value);
            break;
        case 0x1f801803:
            g_emulator->m_cdrom->write3(value);
            break;
        case 0x1f802041:
            g_system->log(LogClass::HARDWARE, "BIOS Trace1: 0x%02x\n", value);
            break;
        case 0x1f802042:
            g_system->log(LogClass::HARDWARE, "BIOS Trace2: 0x%02x\n", value);
            break;
        case 0x1f802080:
            g_system->biosPutc(value);
            break;
        case 0x1f802081:
            if (value == 0) {
                g_system->pause();
            } else {
                auto L = *g_emulator->m_lua;
                auto top = L.gettop();
                L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
                L.getfieldtable("execSlots");
                L.push(lua_Number(value));
                L.gettable();
                if (L.isfunction()) {
                    try {
                        L.pcall();
                    } catch (...) {
                    }
                }
                while (top != L.gettop()) L.pop();
            }
            break;
        case 0x1f802088:
            g_emulator->m_debug->m_checkKernel = value;
            break;
        case 0x1f802089:
            g_emulator->m_mem->initMsan(value);
            break;
        default:
            if ((hwadd >= 0x1f801c00) && (hwadd < 0x1f801e00)) {
                recordSPUWrite(hwadd, (uint16_t)rawvalue, 8);
                g_emulator->m_spu->writeRegister(add, (uint16_t)rawvalue);
                break;
            }

            if (addressInRegisterSpace(hwadd)) {
                const uint32_t v = SWAP_LEu32(rawvalue);
                memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
            } else {
                g_emulator->m_mem->m_hard[hwadd & 0xffff] = value;
            }
            return;
    }
    if (addressInRegisterSpace(hwadd)) {
        const uint32_t v = SWAP_LEu32(rawvalue);
        memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
    } else {
        g_emulator->m_mem->m_hard[hwadd & 0xffff] = value;
    }
}

void PCSX::HW::write16(uint32_t add, uint32_t rawvalue) {
    uint16_t value = (uint16_t)rawvalue;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801040:
            g_emulator->m_sio->write8((uint8_t)value);  // 8-bit reg, ignore upper 8 bits
            break;
        case 0x1f801044:
            g_emulator->m_sio->writeStatus16(value);
            break;
        case 0x1f801048:
            g_emulator->m_sio->writeMode16(value);
            break;
        case 0x1f80104a:  // control register
            g_emulator->m_sio->writeCtrl16(value);
            break;
        case 0x1f80104e:  // baudrate register
            g_emulator->m_sio->writeBaud16(value);
            break;
        case 0x1f801050:                                     // rx/tx data register
            g_emulator->m_sio1->writeData8((uint8_t)value);  // 8-bit reg, ignore upper 8 bits
            break;
        case 0x1f801054:  // stat register
            g_emulator->m_sio1->writeStat16(value);
            break;
        case 0x1f801058:  // mode register
            g_emulator->m_sio1->writeMode16(value);
            break;
        case 0x1f80105a:  // control register
            g_emulator->m_sio1->writeCtrl16(value);
            break;
        case 0x1f80105c:  // guizmo register
            g_emulator->m_sio1->writeGuizmo(value);
            return;
        case 0x1f80105e:  // baudrate register
            g_emulator->m_sio1->writeBaud16(value);
            return;
        case 0x1f801070:
            if (g_emulator->settings.get<Emulator::SettingSpuIrq>()) g_emulator->m_mem->setIRQ(0x200);
            g_emulator->m_mem->clearIRQ(~rawvalue);
            return;

        case 0x1f801074:
            break;

        case 0x1f801100:
            g_emulator->m_counters->writeCounter(0, value);
            break;
        case 0x1f801104:
            g_emulator->m_counters->writeMode(0, value);
            break;
        case 0x1f801108:
            g_emulator->m_counters->writeTarget(0, value);
            break;

        case 0x1f801110:
            g_emulator->m_counters->writeCounter(1, value);
            break;
        case 0x1f801114:
            g_emulator->m_counters->writeMode(1, value);
            break;
        case 0x1f801118:
            g_emulator->m_counters->writeTarget(1, value);
            break;

        case 0x1f801120:
            g_emulator->m_counters->writeCounter(2, value);
            break;
        case 0x1f801124:
            g_emulator->m_counters->writeMode(2, value);
            break;
        case 0x1f801128:
            g_emulator->m_counters->writeTarget(2, value);
            break;
        case 0x1f802082:
            g_system->testQuit((int16_t)value);
            break;

        default:
            if ((hwadd >= 0x1f801c00) && (hwadd < 0x1f801e00)) {
                recordSPUWrite(hwadd, value, 16);
                g_emulator->m_spu->writeRegister(add, value);
                break;
            }

            if (addressInRegisterSpace(hwadd)) {
                const uint32_t v = SWAP_LEu32(rawvalue);
                memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
            } else {
                uint16_t *ptr = (uint16_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
                *ptr = SWAP_LEu16(value);
            }
            return;
    }
    if (addressInRegisterSpace(hwadd)) {
        const uint32_t v = SWAP_LEu32(rawvalue);
        memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
    } else {
        uint16_t *ptr = (uint16_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
        *ptr = SWAP_LEu16(value);
    }
}

inline void PCSX::HW::dma0(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    g_emulator->m_mdec->dma0(madr, bcr, chcr);
}

inline void PCSX::HW::dma1(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    g_emulator->m_mdec->dma1(madr, bcr, chcr);
}

inline bool PCSX::HW::dma2(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    return g_emulator->m_gpu->dma(madr, bcr, chcr);
}

inline void PCSX::HW::dma3(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    g_emulator->m_cdrom->dma(madr, bcr, chcr);
}

void PCSX::HW::write32(uint32_t add, uint32_t value) {
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801008:
            break;
        case 0x1f801040:
            g_emulator->m_sio->write8((uint8_t)value);  // 8-bit reg, ignore upper 24 bits
            break;
        case 0x1f801050:
            g_emulator->m_sio1->writeData8((uint8_t)value);  // 8-bit reg, ignore upper 24 bits
            break;
        case 0x1f801054:
            g_emulator->m_sio1->writeStat32(value);
            break;
        case 0x1f80105c:  // guizmo register
            g_emulator->m_sio1->writeGuizmo(value);
            return;
        case 0x1f801060:
            g_emulator->m_mem->writeHardwareRegister<0x1060>(value);
            g_emulator->m_mem->setLuts();
            break;  // Ram size
        case 0x1f801070:
            if (g_emulator->settings.get<Emulator::SettingSpuIrq>()) g_emulator->m_mem->setIRQ(0x200);
            g_emulator->m_mem->clearIRQ(~value);
            return;
        case 0x1f801074:
            g_emulator->m_mem->writeHardwareRegister<0x1074>(value);
            break;
        case 0x1f801080:
            g_emulator->m_mem->setMADR<0>(value);
            return;
        case 0x1f801088:
            dmaExec<0>(value);  // DMA0 chcr (MDEC in DMA)
            return;
        case 0x1f801090:
            g_emulator->m_mem->setMADR<1>(value);
            return;
        case 0x1f801098:
            dmaExec<1>(value);  // DMA1 chcr (MDEC out DMA)
            return;
        case 0x1f8010a0:
            g_emulator->m_mem->setMADR<2>(value);
            return;
        case 0x1f8010a8:
            dmaExec<2>(value);  // DMA2 chcr (GPU DMA)
            return;
        case 0x1f8010b0:
            g_emulator->m_mem->setMADR<3>(value);
            return;
        case 0x1f8010b8:
            dmaExec<3>(value);  // DMA3 chcr (CDROM DMA)
            return;
        case 0x1f8010c0:
            g_emulator->m_mem->setMADR<4>(value);
            return;
        case 0x1f8010c8:
            dmaExec<4>(value);  // DMA4 chcr (SPU DMA)
            return;
        case 0x1f8010d0:
            g_emulator->m_mem->setMADR<5>(value);
            return;
#if 0
        case 0x1f8010d8:
            dmaExec<5>(value);  // DMA5 chcr (PIO DMA)
            return;
#endif
        case 0x1f8010e0:
            g_emulator->m_mem->setMADR<6>(value);
            return;
        case 0x1f8010e8:
            dmaExec<6>(value);  // DMA6 chcr (OT clear)
            return;
        case 0x1f8010f0: {
            auto& mem = g_emulator->m_mem;
            const bool otcWasEnabled = mem->isDMAEnabled<6>();
            mem->writeHardwareRegister<0x10f0>(value);
            // Enabling DMA6 starts a transfer already latched in its CHCR. Whether the other channels do the
            // same has not been measured.
            if (!otcWasEnabled && mem->isDMAEnabled<6>() && mem->isDMABusy<6>()) dmaExec<6>(mem->getCHCR<6>());
            break;
        }
        case 0x1f8010f4:
            {
                auto &mem = g_emulator->m_mem;
                uint32_t icr = mem->readHardwareRegister<Memory::DMA_ICR>();
                uint32_t ack = value & 0b0'1111111'000000000'000000000'000000;
                bool wasNotTriggered = (icr & 0x80000000) == 0;
                bool isTriggered = false;
                bool hasError = value & 0x00008000;
                bool isEnabled = value & 0x00800000;
                ack ^= 0b0'1111111'000000000'000000000'000000;
                value &= 0b0'0000000'111111111'000000000'111111;
                icr &= ack;
                icr |= value;
                if (((icr & 0x7f008000) != 0) && (hasError || isEnabled)) {
                    icr |= 0x80000000;
                    isTriggered = true;
                }
                mem->writeHardwareRegister<Memory::DMA_ICR>(icr);
                if (wasNotTriggered && isTriggered) {
                    mem->setIRQ(8);
                };
                return;
            }
        case 0x1f801014:
            g_emulator->m_mem->writeHardwareRegister<0x1014>(value);
            break;
        case 0x1f801810:
            g_emulator->m_gpu->writeData(value);
            break;
        case 0x1f801814:
            g_emulator->m_gpu->writeStatus(value);
            break;
        case 0x1f801820:
            g_emulator->m_mdec->write0(value);
            break;
        case 0x1f801824:
            g_emulator->m_mdec->write1(value);
            break;
        case 0x1f801100:
            g_emulator->m_counters->writeCounter(0, value & 0xffff);
            break;
        case 0x1f801104:
            g_emulator->m_counters->writeMode(0, value);
            break;
        case 0x1f801108:
            g_emulator->m_counters->writeTarget(0, value & 0xffff);
            break;  //  HW_DMA_ICR&= SWAP_LE32((~value)&0xff000000);
        case 0x1f801110:
            g_emulator->m_counters->writeCounter(1, value & 0xffff);
            break;
        case 0x1f801114:
            g_emulator->m_counters->writeMode(1, value);
            break;
        case 0x1f801118:
            g_emulator->m_counters->writeTarget(1, value & 0xffff);
            break;
        case 0x1f801120:
            g_emulator->m_counters->writeCounter(2, value & 0xffff);
            break;
        case 0x1f801124:
            g_emulator->m_counters->writeMode(2, value);
            break;
        case 0x1f801128:
            g_emulator->m_counters->writeTarget(2, value & 0xffff);
            break;
        case 0x1f802084: {
            IO<File> memFile = g_emulator->m_mem->getMemoryAsFile();
            memFile->rSeek(value);
            g_system->message("%s", memFile->gets<false>());
            break;
        }
        case 0x1f80208c: {
            g_emulator->m_mem->msanFree(value);
            break;
        }
        case 0x1f8020a0: {
            g_emulator->m_mem->m_psyqoHeapMetadata = value;
            break;
        }
        case 0x1f8020a4: {
            GuestCommand::dispatch(value);
            break;
        }
        case 0x1f802094: {
            PSXAddress headerAddrInfo(value);
            switch (headerAddrInfo.type) {
                case PSXAddress::Type::RAM:
                case PSXAddress::Type::MSAN: {
                    uint32_t ptrToNext = g_emulator->m_cpu->m_regs.GPR.n.a0;
                    uint32_t wordCount = g_emulator->m_cpu->m_regs.GPR.n.a1;
                    uint32_t markerValue = g_emulator->m_mem->msanSetChainPtr(value, ptrToNext, wordCount);
                    g_emulator->m_mem->write32(value, markerValue);
                    return;
                }
                default: {
                    g_system->printf(_("Called msanSetChainPtr with invalid header pointer %8.8lx\n"), value);
                    g_system->testQuit(PCSX::Memory::c_msanExitCode);
                    return;
                }
            }
        }
        default: {
            if ((hwadd >= 0x1f801c00) && (hwadd < 0x1f801e00)) {
                write16(add, value & 0xffff);
                write16(add + 2, value >> 16);
                break;
            }

            uint32_t *ptr = (uint32_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
            *ptr = SWAP_LEu32(value);
            return;
        }
    }
    uint32_t *ptr = (uint32_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
    *ptr = SWAP_LEu32(value);
}
