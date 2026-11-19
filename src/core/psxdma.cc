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

// The SPU asks for a sync-1 DMA4 block while SPUCNT's transfer mode is DMA write (2) for a
// transfer into sound RAM, or DMA read (3) for one out of it.
static bool spuRequests(uint32_t chcr) {
    const unsigned mode = (PCSX::g_emulator->m_spu->getCtrl() >> 4) & 3;
    return (chcr & 1) ? mode == 2 : mode == 3;
}

// Moves the blocks left in BCR from MADR one at a time, re-reading direction and step from CHCR, so
// MADR and BCR rewritten while the channel waited between blocks take effect. A forced block (CHCR
// bit 28) taken while the transfer mode does not ask for it is read from RAM, MADR moves past it,
// and none of it reaches sound RAM (SCPH-1000, 5501, 7001).
static bool dma4Sync1(uint32_t chcr) {
    auto &mem = PCSX::g_emulator->m_mem;
    auto &spu = PCSX::g_emulator->m_spu;
    // As in HW::dmaExec: MSAN addresses stay as they are, anything else wraps into RAM.
    auto physical = [&](uint32_t addr) -> uint32_t {
        return (mem->msanInitialized() && PCSX::Memory::inMsanRange(addr)) ? (addr & 0xfffffffc) : (addr & 0x7ffffc);
    };
    uint32_t madr = physical(mem->getMADR<4>());
    const uint32_t bcr = mem->getBCR<4>();
    const uint32_t bs = bcr & 0xffff;
    uint32_t blocks = bcr >> 16;
    const uint32_t step = (chcr & 2) ? uint32_t(-4) : 4u;
    const bool toSpu = chcr & 1;
    uint32_t moved = 0;
    spu->setDMAWaiting(false);

    auto block = [&](bool lands) {
        if (lands) {
            PCSX::g_emulator->m_spuLogger->log(toSpu ? PCSX::SPULogger::Kind::DMAWrite : PCSX::SPULogger::Kind::DMARead,
                                               spu->getTransferAddr(), bs * 4, madr);
        }
        for (uint32_t i = 0; lands && i < bs; i++) {
            const uint32_t addr = physical(madr + i * step);
            uint16_t *p = mem->getPointer<uint16_t>(addr);
            if (!p) continue;
            if (toSpu) {
                spu->writeDMAMem(p, 2);
            } else {
                spu->readDMAMem(p, 2);
                mem->msanDmaWrite(addr, 4);
                PCSX::g_emulator->m_cpu->Clear(addr, 1);
            }
        }
        madr += bs * step;
        blocks--;
        moved++;
    };

    if (chcr & 0x10000000) {
        mem->setCHCR<4>(chcr & ~0x10000000);
        if (blocks && bs) block(spuRequests(chcr));
    }
    while (blocks && bs) {
        if (!spuRequests(chcr)) {
            mem->setMADR<4>(madr);
            mem->setBCR<4>((blocks << 16) | bs);
            spu->setDMAWaiting(true);
            return true;
        }
        block(true);
    }
    mem->setMADR<4>(madr);
    mem->setBCR<4>(bs);
    const uint32_t cycles = moved * bs / 2;
    scheduleSPUDMAIRQ(cycles ? cycles : 1);
    if (toSpu) spu->writeDMAEndsAt(PCSX::g_emulator->m_cpu->m_regs.cycle + cycles);
    return true;
}

void dma4Resume() {
    auto &mem = PCSX::g_emulator->m_mem;
    auto &spu = PCSX::g_emulator->m_spu;
    if (!spu->getDMAWaiting()) return;
    const uint32_t chcr = mem->getCHCR<4>();
    if (!(chcr & 0x01000000)) {
        spu->setDMAWaiting(false);
        return;
    }
    // A busy channel that DPCR has disabled keeps waiting.
    if (!mem->isDMAEnabled<4>() || !spuRequests(chcr)) return;
    dma4Sync1(chcr);
}

bool dma4(uint32_t madr, uint32_t bcr, uint32_t chcr) {  // SPU
    // A CHCR write that keeps bit 24 while the channel waits between blocks does not start a new
    // transfer: the remaining blocks take its direction and step when the transfer mode asks.
    if (PCSX::g_emulator->m_spu->getDMAWaiting()) return true;
    // Sync 1 that is forced (bit 28), steps backwards (bit 1), or that the SPU is not asking for yet.
    if ((chcr & 0x00000600) == 0x00000200 && ((chcr & 0x10000002) || !spuRequests(chcr))) {
        return dma4Sync1(chcr);
    }
    uint16_t *ptr = PCSX::g_emulator->m_mem->getPointer<uint16_t>(madr);
    uint32_t size;

    switch (chcr) {
        case 0x01000201:  // cpu to spu transfer
            if (ptr == nullptr) {
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
            return false;

        case 0x01000200:  // spu to cpu transfer
            if (ptr == nullptr) {
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
            return false;

        default:
            break;
    }

    spuInterrupt();
    return false;
}

void dma6(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    uint32_t size;
    uint32_t *mem = PCSX::g_emulator->m_mem->getPointer<uint32_t>(madr);

    // The OTC never raises a DMA request, so bit 28 (start/trigger) is required: with bit 24 alone the
    // channel stays busy until software clears bit 24.
    if ((chcr & 0x11000000) == 0x11000000) {
        if (mem == nullptr) {
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
