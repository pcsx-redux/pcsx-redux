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
            SIO1_LOG("SIO1.DATA read8 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801054:  // stat register
            hard = g_emulator->m_sio1->readStat8();
            // Log command below is overly spammy
            // SIO1_LOG("SIO1.STAT read8 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801058:  // mode register
            hard = g_emulator->m_sio1->readMode8();
            SIO1_LOG("SIO1.MODE read8 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80105a:  // control register
            hard = g_emulator->m_sio1->readCtrl8();
            SIO1_LOG("SIO1.CTRL read8 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80105c:  // guizmo register
            hard = g_emulator->m_sio1->readGuizmo8();
            SIO1_LOG("SIO1.GUIZMO read8 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80105e:  // baudrate register
            hard = g_emulator->m_sio1->readBaud8();
            SIO1_LOG("SIO1.BAUD read8 %x; ret = %x\n", add & 0xf, hard);
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
            PSXHW_LOG("*Unknown 8bit read at address %x\n", add);
            return hard;
    }

    PSXHW_LOG("*Known 8bit read at address %x value %x\n", add, hard);
    return hard;
}

uint16_t PCSX::HW::read16(uint32_t add) {
    uint16_t hard;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801070: {
            uint32_t hard = g_emulator->m_mem->readHardwareRegister<Memory::ISTAT>();
            PSXHW_LOG("ISTAT 16bit read %x\n", hard);
            return hard;
        }
        case 0x1f801074: {
            uint32_t hard = g_emulator->m_mem->readHardwareRegister<Memory::IMASK>();
            PSXHW_LOG("IMASK 16bit read %x\n", hard);
            return hard;
        }
        case 0x1f801040:
            hard = g_emulator->m_sio->read8();
            hard |= g_emulator->m_sio->read8() << 8;
            SIO0_LOG("sio read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801044:
            hard = g_emulator->m_sio->readStatus16();
            SIO0_LOG("sio read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801048:
            hard = g_emulator->m_sio->readMode16();
            SIO0_LOG("sio read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80104a:
            hard = g_emulator->m_sio->readCtrl16();
            SIO0_LOG("sio read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80104e:
            hard = g_emulator->m_sio->readBaud16();
            SIO0_LOG("sio read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801050:  // rx/tx data register
            hard = g_emulator->m_sio1->readData16();
            SIO1_LOG("SIO1.DATA read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801054:  // stat register
            hard = g_emulator->m_sio1->readStat16();
            // Log command below is overly spammy
            // SIO1_LOG("SIO1.STAT read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801058:  // mode register
            hard = g_emulator->m_sio1->readMode16();
            SIO1_LOG("SIO1.MODE read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80105a:  // control register
            hard = g_emulator->m_sio1->readCtrl16();
            SIO1_LOG("SIO1.CTRL read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80105c:  // guizmo register
            hard = g_emulator->m_sio1->readGuizmo16();
            SIO1_LOG("SIO1.GUIZMO read16 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f80105e:  // baudrate register
            hard = g_emulator->m_sio1->readBaud16();
            SIO1_LOG("SIO1.BAUD read16 %x; ret = %x\n", add & 0xf, hard);
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
            PSXHW_LOG("T0 count read16: %x\n", hard);
            break;
        case 0x1f801104:
            hard = g_emulator->m_counters->readMode(0);
            PSXHW_LOG("T0 mode read16: %x\n", hard);
            break;
        case 0x1f801108:
            hard = g_emulator->m_counters->readTarget(0);
            PSXHW_LOG("T0 target read16: %x\n", hard);
            break;
        case 0x1f801110:
            hard = g_emulator->m_counters->readCounter(1);
            PSXHW_LOG("T1 count read16: %x\n", hard);
            break;
        case 0x1f801114:
            hard = g_emulator->m_counters->readMode(1);
            PSXHW_LOG("T1 mode read16: %x\n", hard);
            break;
        case 0x1f801118:
            hard = g_emulator->m_counters->readTarget(1);
            PSXHW_LOG("T1 target read16: %x\n", hard);
            break;
        case 0x1f801120:
            hard = g_emulator->m_counters->readCounter(2);
            PSXHW_LOG("T2 count read16: %x\n", hard);
            break;
        case 0x1f801124:
            hard = g_emulator->m_counters->readMode(2);
            PSXHW_LOG("T2 mode read16: %x\n", hard);
            break;
        case 0x1f801128:
            hard = g_emulator->m_counters->readTarget(2);
            PSXHW_LOG("T2 target read16: %x\n", hard);
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
                PSXHW_LOG("*Unknown 16bit read at address %x\n", add);
            }
            return hard;
    }

    uint16_t *ptr = (uint16_t *)&g_emulator->m_mem->m_hard[add & 0xffff];
    PSXHW_LOG("*Known 16bit read at address %x value %x\n", add, hard);
    return hard;
}

uint32_t PCSX::HW::read32(uint32_t add) {
    uint32_t hard;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801008: {
            hard = g_emulator->m_mem->readHardwareRegister<0x1008>();
            PSXHW_LOG("EXP1 delay/size read %x\n", hard);
            return hard;
        }
        case 0x1f801040:
            hard = g_emulator->m_sio->read8();
            hard |= g_emulator->m_sio->read8() << 8;
            hard |= g_emulator->m_sio->read8() << 16;
            hard |= g_emulator->m_sio->read8() << 24;
            SIO0_LOG("sio read32 ;ret = %x\n", hard);
            break;
        case 0x1f801050:  // rx/tx data register
            hard = g_emulator->m_sio1->readData32();
            SIO1_LOG("SIO1.DATA read32 ;ret = %x\n", hard);
            break;
        case 0x1f801054:  // stat register
            hard = g_emulator->m_sio1->readStat32();
            // Log command below is overly spammy
            // SIO1_LOG("SIO1.STAT read32 ;ret = %x\n", hard);
            break;
        case 0x1f80105c:  // guizmo register
            hard = g_emulator->m_sio1->readGuizmo32();
            SIO1_LOG("SIO1.GUIZMO read32 %x; ret = %x\n", add & 0xf, hard);
            break;
        case 0x1f801060: {
            hard = g_emulator->m_mem->readHardwareRegister<0x1060>();
            PSXHW_LOG("RAM size read %x\n", hard);
            return hard;
        }
        case 0x1f801070: {
            hard = g_emulator->m_mem->readHardwareRegister<Memory::ISTAT>();
            PSXHW_LOG("ISTAT 32bit read %x\n", hard);
            return hard;
        }
        case 0x1f801074: {
            hard = g_emulator->m_mem->readHardwareRegister<Memory::IMASK>();
            PSXHW_LOG("IMASK 32bit read %x\n", hard);
            return hard;
        }
        case 0x1f801810:
            hard = g_emulator->m_gpu->readData();
            PSXHW_LOG("GPU DATA 32bit read %x\n", hard);
            break;
        case 0x1f801814:
            hard = PCSX::g_emulator->m_gpu->readStatusFromBus();
            PSXHW_LOG("GPU STATUS 32bit read %x\n", hard);
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
            PSXHW_LOG("T0 count read32: %x\n", hard);
            break;
        case 0x1f801104:
            hard = g_emulator->m_counters->readMode(0);
            PSXHW_LOG("T0 mode read32: %x\n", hard);
            break;
        case 0x1f801108:
            hard = g_emulator->m_counters->readTarget(0);
            PSXHW_LOG("T0 target read32: %x\n", hard);
            break;
        case 0x1f801110:
            hard = g_emulator->m_counters->readCounter(1);
            PSXHW_LOG("T1 count read32: %x\n", hard);
            break;
        case 0x1f801114:
            hard = g_emulator->m_counters->readMode(1);
            PSXHW_LOG("T1 mode read32: %x\n", hard);
            break;
        case 0x1f801118:
            hard = g_emulator->m_counters->readTarget(1);
            PSXHW_LOG("T1 target read32: %x\n", hard);
            break;
        case 0x1f801120:
            hard = g_emulator->m_counters->readCounter(2);
            PSXHW_LOG("T2 count read32: %x\n", hard);
            break;
        case 0x1f801124:
            hard = g_emulator->m_counters->readMode(2);
            PSXHW_LOG("T2 mode read32: %x\n", hard);
            break;
        case 0x1f801128:
            hard = g_emulator->m_counters->readTarget(2);
            PSXHW_LOG("T2 target read32: %x\n", hard);
            break;
        case 0x1f801014:
            hard = g_emulator->m_mem->readHardwareRegister<0x1014>();
            PSXHW_LOG("SPU delay [0x1014] read32: %8.8lx\n", hard);
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
            PSXHW_LOG("*Unknown 32bit read at address %x (0x%8.8lx)\n", add, hard);
            return hard;
        }
    }
    uint32_t *ptr = (uint32_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
    *ptr = hard;
    PSXHW_LOG("*Known 32bit read at address %x\n", add);
    return hard;
}

void PCSX::HW::write8(uint32_t add, uint32_t rawvalue) {
    uint8_t value = (uint8_t)rawvalue;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801040:
            g_emulator->m_sio->write8(value);
            break;
        case 0x1f801050:  // rx/tx data register
            g_emulator->m_sio1->writeData8(value);
            SIO1_LOG("SIO1.DATA write8 %x; ret = %x\n", add & 0xf, value);
            break;
        case 0x1f801054:  // stat register
            g_emulator->m_sio1->writeStat8(value);
            SIO1_LOG("SIO1.STAT write8 %x; ret = %x\n", add & 0xf, value);
            break;
        case 0x1f801058:  // mode register
            g_emulator->m_sio1->writeMode8(value);
            SIO1_LOG("SIO1.MODE write8 %x; ret = %x\n", add & 0xf, value);
            break;
        case 0x1f80105a:  // control register
            g_emulator->m_sio1->writeCtrl8(value);
            SIO1_LOG("SIO1.CTRL write8 %x; ret = %x\n", add & 0xf, value);
            break;
        case 0x1f80105c:  // guizmo register
            g_emulator->m_sio1->writeGuizmo(rawvalue);
            SIO1_LOG("SIO1.GUIZMO write8 %x; ret = %x\n", add & 0xf, rawvalue);
            return;
        case 0x1f80105d:  // guizmo register again
            g_emulator->m_sio1->writeGuizmo(((uint16_t)value) << 8);
            SIO1_LOG("SIO1.GUIZMO write8 %x; ret = %x\n", add & 0xf, value);
            return;
        case 0x1f80105e:  // baudrate register
            g_emulator->m_sio1->writeBaud16(rawvalue);
            SIO1_LOG("SIO1.Baud write8 %x; ret = %x\n", add & 0xf, rawvalue);
            return;
        case 0x1f80105f:  // baudrate register, upper 8 bits
            g_emulator->m_sio1->writeBaud16(((uint16_t)value) << 8);
            SIO1_LOG("SIO1.Baud write8 %x; ret = %x\n", add & 0xf, value);
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
                g_emulator->m_spu->writeRegister(add, (uint16_t)rawvalue);
                break;
            }

            if (addressInRegisterSpace(hwadd)) {
                const uint32_t v = SWAP_LEu32(rawvalue);
                memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
                PSXHW_LOG("*Unknown 8bit(actually 32bit) write at address %x value %x\n", add, rawvalue);
            } else {
                g_emulator->m_mem->m_hard[hwadd & 0xffff] = value;
                PSXHW_LOG("*Unknown 8bit write at address %x value %x\n", add, value);
            }
            return;
    }
    if (addressInRegisterSpace(hwadd)) {
        const uint32_t v = SWAP_LEu32(rawvalue);
        memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
        PSXHW_LOG("*Known 8bit(actually 32bit) write at address %x value %x\n", add, rawvalue);
    } else {
        g_emulator->m_mem->m_hard[hwadd & 0xffff] = value;
        PSXHW_LOG("*Known 8bit write at address %x value %x\n", add, value);
    }
}

void PCSX::HW::write16(uint32_t add, uint32_t rawvalue) {
    uint16_t value = (uint16_t)rawvalue;
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801040:
            g_emulator->m_sio->write8((uint8_t)value);  // 8-bit reg, ignore upper 8 bits
            SIO0_LOG("sio write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f801044:
            g_emulator->m_sio->writeStatus16(value);
            SIO0_LOG("sio write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f801048:
            g_emulator->m_sio->writeMode16(value);
            SIO0_LOG("sio write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f80104a:  // control register
            g_emulator->m_sio->writeCtrl16(value);
            SIO0_LOG("sio write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f80104e:  // baudrate register
            g_emulator->m_sio->writeBaud16(value);
            SIO0_LOG("sio write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f801050:                                     // rx/tx data register
            g_emulator->m_sio1->writeData8((uint8_t)value);  // 8-bit reg, ignore upper 8 bits
            SIO1_LOG("SIO1.DATA write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f801054:  // stat register
            g_emulator->m_sio1->writeStat16(value);
            SIO1_LOG("SIO1.STAT write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f801058:  // mode register
            g_emulator->m_sio1->writeMode16(value);
            SIO1_LOG("SIO1.MODE write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f80105a:  // control register
            g_emulator->m_sio1->writeCtrl16(value);
            SIO1_LOG("SIO1.CTRL write16 %x, %x\n", add & 0xf, value);
            break;
        case 0x1f80105c:  // guizmo register
            g_emulator->m_sio1->writeGuizmo(value);
            SIO1_LOG("SIO1.GUIZMO write16 %x, %x\n", add & 0xf, value);
            return;
        case 0x1f80105e:  // baudrate register
            g_emulator->m_sio1->writeBaud16(value);
            SIO1_LOG("SIO1.BAUD write16 %x, %x\n", add & 0xf, value);
            return;
        case 0x1f801070:
            PSXHW_LOG("ISTAT 16bit(actually 32bit) write %x\n", rawvalue);
            if (g_emulator->settings.get<Emulator::SettingSpuIrq>()) g_emulator->m_mem->setIRQ(0x200);
            g_emulator->m_mem->clearIRQ(~rawvalue);
            return;

        case 0x1f801074:
            PSXHW_LOG("IMASK 16bit write %x\n", value);
            break;

        case 0x1f801100:
            PSXHW_LOG("COUNTER 0 COUNT 16bit write %x\n", value);
            g_emulator->m_counters->writeCounter(0, value);
            break;
        case 0x1f801104:
            PSXHW_LOG("COUNTER 0 MODE 16bit write %x\n", value);
            g_emulator->m_counters->writeMode(0, value);
            break;
        case 0x1f801108:
            PSXHW_LOG("COUNTER 0 TARGET 16bit write %x\n", value);
            g_emulator->m_counters->writeTarget(0, value);
            break;

        case 0x1f801110:
            PSXHW_LOG("COUNTER 1 COUNT 16bit write %x\n", value);
            g_emulator->m_counters->writeCounter(1, value);
            break;
        case 0x1f801114:
            PSXHW_LOG("COUNTER 1 MODE 16bit write %x\n", value);
            g_emulator->m_counters->writeMode(1, value);
            break;
        case 0x1f801118:
            PSXHW_LOG("COUNTER 1 TARGET 16bit write %x\n", value);
            g_emulator->m_counters->writeTarget(1, value);
            break;

        case 0x1f801120:
            PSXHW_LOG("COUNTER 2 COUNT 16bit write %x\n", value);
            g_emulator->m_counters->writeCounter(2, value);
            break;
        case 0x1f801124:
            PSXHW_LOG("COUNTER 2 MODE 16bit write %x\n", value);
            g_emulator->m_counters->writeMode(2, value);
            break;
        case 0x1f801128:
            PSXHW_LOG("COUNTER 2 TARGET 16bit write %x\n", value);
            g_emulator->m_counters->writeTarget(2, value);
            break;
        case 0x1f802082:
            g_system->testQuit((int16_t)value);
            break;

        default:
            if ((hwadd >= 0x1f801c00) && (hwadd < 0x1f801e00)) {
                g_emulator->m_spu->writeRegister(add, value);
                break;
            }

            if (addressInRegisterSpace(hwadd)) {
                const uint32_t v = SWAP_LEu32(rawvalue);
                memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
                PSXHW_LOG("*Unknown 16bit(actually 32bit) write at address %x value %x\n", add, rawvalue);
            } else {
                uint16_t *ptr = (uint16_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
                *ptr = SWAP_LEu16(value);
                PSXHW_LOG("*Unknown 16bit write at address %x value %x\n", add, value);
            }
            return;
    }
    if (addressInRegisterSpace(hwadd)) {
        const uint32_t v = SWAP_LEu32(rawvalue);
        memcpy(&g_emulator->m_mem->m_hard[hwadd & 0xffff], &v, sizeof(v));
        PSXHW_LOG("*Known 16bit(actually 32bit) write at address %x value %x\n", add, rawvalue);
    } else {
        uint16_t *ptr = (uint16_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
        *ptr = SWAP_LEu16(value);
        PSXHW_LOG("*Known 16bit write at address %x value %x\n", add, value);
    }
}

inline void PCSX::HW::dma0(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    PSXDMA_LOG("*** DMA0 MDEC *** %x addr = %x size = %x\n", chcr, madr, bcr);
    g_emulator->m_mdec->dma0(madr, bcr, chcr);
}

inline void PCSX::HW::dma1(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    PSXDMA_LOG("*** DMA1 MDEC *** %x addr = %x size = %x\n", chcr, madr, bcr);
    g_emulator->m_mdec->dma1(madr, bcr, chcr);
}

inline bool PCSX::HW::dma2(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    return g_emulator->m_gpu->dma(madr, bcr, chcr);
}

inline void PCSX::HW::dma3(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    PSXDMA_LOG("*** DMA3 CDROM *** %x addr = %x size = %x\n", chcr, madr, bcr);
    g_emulator->m_cdrom->dma(madr, bcr, chcr);
}

void PCSX::HW::write32(uint32_t add, uint32_t value) {
    uint32_t hwadd = add & 0x1fffffff;

    switch (hwadd) {
        case 0x1f801008:
            PSXHW_LOG("EXP1 delay/size write %x\n", value);
            break;
        case 0x1f801040:
            g_emulator->m_sio->write8((uint8_t)value);  // 8-bit reg, ignore upper 24 bits
            SIO0_LOG("sio write32 %x\n", value);
            break;
        case 0x1f801050:
            g_emulator->m_sio1->writeData8((uint8_t)value);  // 8-bit reg, ignore upper 24 bits
            SIO1_LOG("SIO1.DATA write32 %x\n", value);
            break;
        case 0x1f801054:
            g_emulator->m_sio1->writeStat32(value);
            SIO1_LOG("SIO1.STAT write32 %x\n", value);
            break;
        case 0x1f80105c:  // guizmo register
            g_emulator->m_sio1->writeGuizmo(value);
            SIO1_LOG("SIO1.GUIZMO write32 %x\n", value);
            return;
        case 0x1f801060:
            PSXHW_LOG("RAM size write %x\n", value);
            g_emulator->m_mem->writeHardwareRegister<0x1060>(value);
            g_emulator->m_mem->setLuts();
            break;  // Ram size
        case 0x1f801070:
            PSXHW_LOG("ISTAT 32bit write %x\n", value);
            if (g_emulator->settings.get<Emulator::SettingSpuIrq>()) g_emulator->m_mem->setIRQ(0x200);
            g_emulator->m_mem->clearIRQ(~value);
            return;
        case 0x1f801074:
            PSXHW_LOG("IMASK 32bit write %x\n", value);
            g_emulator->m_mem->writeHardwareRegister<0x1074>(value);
            break;
        case 0x1f801080:
            PSXHW_LOG("DMA0 MADR 32bit write %x\n", value);
            g_emulator->m_mem->setMADR<0>(value);
            return;
        case 0x1f801088:
            PSXHW_LOG("DMA0 CHCR 32bit write %x\n", value);
            dmaExec<0>(value);  // DMA0 chcr (MDEC in DMA)
            return;
        case 0x1f801090:
            PSXHW_LOG("DMA1 MADR 32bit write %x\n", value);
            g_emulator->m_mem->setMADR<1>(value);
            return;
        case 0x1f801098:
            PSXHW_LOG("DMA1 CHCR 32bit write %x\n", value);
            dmaExec<1>(value);  // DMA1 chcr (MDEC out DMA)
            return;
        case 0x1f8010a0:
            PSXHW_LOG("DMA2 MADR 32bit write %x\n", value);
            g_emulator->m_mem->setMADR<2>(value);
            return;
        case 0x1f8010a8:
            PSXHW_LOG("DMA2 CHCR 32bit write %x\n", value);
            dmaExec<2>(value);  // DMA2 chcr (GPU DMA)
            return;
        case 0x1f8010b0:
            PSXHW_LOG("DMA3 MADR 32bit write %x\n", value);
            g_emulator->m_mem->setMADR<3>(value);
            return;
        case 0x1f8010b8:
            PSXHW_LOG("DMA3 CHCR 32bit write %x\n", value);
            dmaExec<3>(value);  // DMA3 chcr (CDROM DMA)
            return;
        case 0x1f8010c0:
            PSXHW_LOG("DMA4 MADR 32bit write %x\n", value);
            g_emulator->m_mem->setMADR<4>(value);
            return;
        case 0x1f8010c8:
            PSXHW_LOG("DMA4 CHCR 32bit write %x\n", value);
            dmaExec<4>(value);  // DMA4 chcr (SPU DMA)
            return;
        case 0x1f8010d0:
            PSXHW_LOG("DMA5 MADR 32bit write %x\n", value);
            g_emulator->m_mem->setMADR<5>(value);
            return;
#if 0
        case 0x1f8010d8:
            PSXHW_LOG("DMA5 CHCR 32bit write %x\n", value);
            dmaExec<5>(value);  // DMA5 chcr (PIO DMA)
            return;
#endif
        case 0x1f8010e0:
            PSXHW_LOG("DMA6 MADR 32bit write %x\n", value);
            g_emulator->m_mem->setMADR<6>(value);
            return;
        case 0x1f8010e8:
            PSXHW_LOG("DMA6 CHCR 32bit write %x\n", value);
            dmaExec<6>(value);  // DMA6 chcr (OT clear)
            return;
        case 0x1f8010f0:
            // TODO: check if toggling PCR triggers pending DMAs.
            PSXHW_LOG("DMA PCR 32bit write %x\n", value);
            g_emulator->m_mem->writeHardwareRegister<0x10f0>(value);
            break;
        case 0x1f8010f4:
            PSXHW_LOG("DMA ICR 32bit write %x\n", value);
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
            PSXHW_LOG("SPU delay [0x1014] write32: %8.8lx\n", value);
            g_emulator->m_mem->writeHardwareRegister<0x1014>(value);
            break;
        case 0x1f801810:
            PSXHW_LOG("GPU DATA 32bit write %x (CMD/MSB %x)\n", value, value >> 24);
            g_emulator->m_gpu->writeData(value);
            break;
        case 0x1f801814:
            PSXHW_LOG("GPU STATUS 32bit write %x\n", value);
            g_emulator->m_gpu->writeStatus(value);
            break;
        case 0x1f801820:
            g_emulator->m_mdec->write0(value);
            break;
        case 0x1f801824:
            g_emulator->m_mdec->write1(value);
            break;
        case 0x1f801100:
            PSXHW_LOG("COUNTER 0 COUNT 32bit write %x\n", value);
            g_emulator->m_counters->writeCounter(0, value & 0xffff);
            break;
        case 0x1f801104:
            PSXHW_LOG("COUNTER 0 MODE 32bit write %x\n", value);
            g_emulator->m_counters->writeMode(0, value);
            break;
        case 0x1f801108:
            PSXHW_LOG("COUNTER 0 TARGET 32bit write %x\n", value);
            g_emulator->m_counters->writeTarget(0, value & 0xffff);
            break;  //  HW_DMA_ICR&= SWAP_LE32((~value)&0xff000000);
        case 0x1f801110:
            PSXHW_LOG("COUNTER 1 COUNT 32bit write %x\n", value);
            g_emulator->m_counters->writeCounter(1, value & 0xffff);
            break;
        case 0x1f801114:
            PSXHW_LOG("COUNTER 1 MODE 32bit write %x\n", value);
            g_emulator->m_counters->writeMode(1, value);
            break;
        case 0x1f801118:
            PSXHW_LOG("COUNTER 1 TARGET 32bit write %x\n", value);
            g_emulator->m_counters->writeTarget(1, value & 0xffff);
            break;
        case 0x1f801120:
            PSXHW_LOG("COUNTER 2 COUNT 32bit write %x\n", value);
            g_emulator->m_counters->writeCounter(2, value & 0xffff);
            break;
        case 0x1f801124:
            PSXHW_LOG("COUNTER 2 MODE 32bit write %x\n", value);
            g_emulator->m_counters->writeMode(2, value);
            break;
        case 0x1f801128:
            PSXHW_LOG("COUNTER 2 TARGET 32bit write %x\n", value);
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
            PSXHW_LOG("*Unknown 32bit write at address %x value %x\n", add, value);
            return;
        }
    }
    uint32_t *ptr = (uint32_t *)&g_emulator->m_mem->m_hard[hwadd & 0xffff];
    *ptr = SWAP_LEu32(value);
    PSXHW_LOG("*Known 32bit write at address %x value %x\n", add, value);
}
