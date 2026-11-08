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

#include "core/psxdma.h"

#include "core/debug.h"
#include "core/spulogger.h"
#include "spu/interface.h"

void spuInterrupt() {
    auto &mem = PCSX::g_emulator->m_mem;
    mem->clearDMABusy<4>();
    mem->dmaInterrupt<4>();
}

void dma4(uint32_t madr, uint32_t bcr, uint32_t chcr) {  // SPU
    uint16_t *ptr = PCSX::g_emulator->m_mem->getPointer<uint16_t>(madr);
    uint32_t size;

    switch (chcr) {
        case 0x01000201:  // cpu to spu transfer
            PSXDMA_LOG("*** DMA4 SPU - mem2spu *** %x addr = %x size = %x\n", chcr, madr, bcr);
            if (ptr == nullptr) {
                PSXDMA_LOG("*** DMA4 SPU - mem2spu *** NULL Pointer!!!\n");
                break;
            }
            size = (bcr >> 16) * (bcr & 0xffff) * 2;
            // Sampled before the transfer, which is what advances it. madr rides along in the
            // value field so an upload can be traced back to where in main RAM it came from.
            PCSX::g_emulator->m_spuLogger->log(PCSX::SPULogger::Kind::DMAWrite,
                                               PCSX::g_emulator->m_spu->getTransferAddr(), size * 2, madr);
            PCSX::g_emulator->m_spu->writeDMAMem(ptr, size);
            if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                    .get<PCSX::Emulator::DebugSettings::Debug>()) {
                PCSX::g_emulator->m_debug->checkDMAread(4, madr, size * 2);
            }

            // Jungle Book - max 0.333x DMA length
            // Harry Potter and the Philosopher's Stone - max 0.5x DMA length
            // uint32_t dmalen=64 + ((bcr >> 18) * (bcr & 0xffff)); // less linear to DMA length which should work with
            // both games above?
            {
                const uint32_t cycles = (bcr >> 16) * (bcr & 0xffff) / 2;
                scheduleSPUDMAIRQ(cycles);
                PCSX::g_emulator->m_spu->writeDMAEndsAt(PCSX::g_emulator->m_cpu->m_regs.cycle + cycles);
            }
            return;

        case 0x01000200:  // spu to cpu transfer
            PSXDMA_LOG("*** DMA4 SPU - spu2mem *** %x addr = %x size = %x\n", chcr, madr, bcr);
            if (ptr == nullptr) {
                PSXDMA_LOG("*** DMA4 SPU - spu2mem *** NULL Pointer!!!\n");
                break;
            }
            size = (bcr >> 16) * (bcr & 0xffff) * 2;
            PCSX::g_emulator->m_spuLogger->log(PCSX::SPULogger::Kind::DMARead,
                                               PCSX::g_emulator->m_spu->getTransferAddr(), size * 2, madr);
            PCSX::g_emulator->m_spu->readDMAMem(ptr, size);
            PCSX::g_emulator->m_mem->msanDmaWrite(madr, size * 2);
            if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                    .get<PCSX::Emulator::DebugSettings::Debug>()) {
                PCSX::g_emulator->m_debug->checkDMAwrite(4, madr, size * 2);
            }
            PCSX::g_emulator->m_cpu->Clear(madr, size * 2);

#if 1
            scheduleSPUDMAIRQ((bcr >> 16) * (bcr & 0xffff) / 2);
#else
            // Experimental burst dma transfer (0.333x max)
            scheduleSPUDMAIRQ((bcr >> 16) * (bcr & 0xffff) / 3);
#endif
            return;

        default:
            PSXDMA_LOG("*** DMA4 SPU - unknown *** %x addr = %x size = %x\n", chcr, madr, bcr);
            break;
    }

    spuInterrupt();
}

void dma6(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    uint32_t size;
    uint32_t *mem = PCSX::g_emulator->m_mem->getPointer<uint32_t>(madr);

    PSXDMA_LOG("*** DMA6 OT *** %x addr = %x size = %x\n", chcr, madr, bcr);

    // The OTC never raises a DMA request, so bit 28 (start/trigger) is required: with bit 24 alone the
    // channel stays busy until software clears bit 24.
    if ((chcr & 0x11000000) == 0x11000000) {
        if (mem == nullptr) {
            PSXDMA_LOG("*** DMA6 OT *** NULL Pointer!!!\n");
            gpuotcInterrupt();
            return;
        }

        // already 32-bit size
        size = bcr;

        if (PCSX::g_emulator->m_mem->msanInitialized() && PCSX::Memory::inMsanRange(madr)) {
            while (bcr--) {
                // use write32 instead of the direct pointer so that the msan memory gets marked as usable
                PCSX::g_emulator->m_mem->write32(madr, PCSX::Memory::c_msanChainMarker);
                PCSX::g_emulator->m_mem->msanSetChainPtr(madr, madr - 4, 0);
                madr -= 4;
            }
            madr += 4;
            PCSX::g_emulator->m_mem->write32(madr, 0xffffff);
        } else {
            while (bcr--) {
                *mem-- = SWAP_LE32((madr - 4) & 0xffffff);
                madr -= 4;
            }
            mem++;
            *mem = 0xffffff;
        }
        PCSX::g_emulator->m_mem->msanDmaWrite(madr, size * 4);
        if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                .get<PCSX::Emulator::DebugSettings::Debug>()) {
            PCSX::g_emulator->m_debug->checkDMAwrite(6, madr, size * 4);
        }

#if 1
        scheduleGPUOTCDMAIRQ(size);
#else
        // Experimental burst dma transfer (0.333x max)
        scheduleGPUOTCDMAIRQ(size / 3);
#endif
    }
}

void gpuotcInterrupt() {
    auto &mem = PCSX::g_emulator->m_mem;
    // Bits 24 and 28 both clear on completion.
    mem->setCHCR<6>(mem->getCHCR<6>() & ~0x11000000);
    mem->dmaInterrupt<6>();
}
