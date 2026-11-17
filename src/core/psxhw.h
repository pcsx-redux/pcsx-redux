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

#include "core/psxcounters.h"
#include "core/psxdma.h"
#include "core/hwevents.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"

namespace PCSX {

class HW {
  public:
    void reset();
    uint8_t read8(uint32_t add);
    uint16_t read16(uint32_t add);
    uint32_t read32(uint32_t add);
    void write8(uint32_t add, uint32_t value);
    void write16(uint32_t add, uint32_t value);
    void write32(uint32_t add, uint32_t value);

  private:
    void clearDelaySizeBit30(uint32_t hwadd);
    void dma0(uint32_t madr, uint32_t bcr, uint32_t chcr);
    void dma1(uint32_t madr, uint32_t bcr, uint32_t chcr);
    bool dma2(uint32_t madr, uint32_t bcr, uint32_t chcr);
    void dma3(uint32_t madr, uint32_t bcr, uint32_t chcr);

    template <unsigned n>
    void dmaExec(uint32_t chcr) {
        auto &mem = g_emulator->m_mem;
        uint32_t written = chcr;
        if constexpr (n == 6) {
            // Only bits 24, 28 and 30 of DMA6 CHCR are writable; bit 1 (step backwards) always reads 1.
            chcr = (chcr & 0x51000000) | 0x00000002;
        }
        mem->setCHCR<n>(chcr);
        if ((chcr & 0x01000000) && mem->template isDMAEnabled<n>()) {
            uint32_t madr = mem->template getMADR<n>();
            auto &events = *g_emulator->m_hwEvents;
            if (events.armed(HWEvents::Source::DMA)) [[unlikely]] {
                events.record(HWEvents::DMAStart, n, madr, mem->template getBCR<n>(), written);
            }
            bool usingMsan = g_emulator->m_mem->msanInitialized();
            if (usingMsan && PCSX::Memory::inMsanRange(madr)) {
                madr &= 0xfffffffc;
            } else {
                madr &= 0x7ffffc;
            }
            uint32_t bcr = mem->template getBCR<n>();
            uint32_t mode = (chcr & 0x00000600) >> 9;
            if constexpr (n == 0) {
                dma0(madr, bcr, chcr);
            } else if constexpr (n == 1) {
                dma1(madr, bcr, chcr);
            } else if constexpr (n == 2) {
                if (dma2(madr, bcr, chcr)) return;
            } else if constexpr (n == 3) {
                dma3(madr, bcr, chcr);
            } else if constexpr (n == 4) {
                dma4(madr, bcr, chcr);
            } else if constexpr (n == 6) {
                // The OTC leaves MADR and BCR as they were written.
                dma6(madr, bcr, chcr);
                return;
            }
            if (mode == 2) {
                uint32_t usedAddr[3] = {0xffffff, 0xffffff, 0xffffff};
                uint32_t DMACommandCounter = 0;

                do {
                    if (usingMsan && PCSX::Memory::inMsanRange(madr)) {
                        madr &= 0xfffffffc;
                        switch (g_emulator->m_mem->msanGetStatus<4>(madr)) {
                            case PCSX::MsanStatus::UNINITIALIZED:
                                g_system->log(LogClass::GPU,
                                              _("GPU DMA went into usable but uninitialized msan memory: %8.8lx\n"),
                                              madr);
                                g_system->pause();
                                return;
                            case PCSX::MsanStatus::UNUSABLE:
                                g_system->log(LogClass::GPU, _("GPU DMA went into unusable msan memory: %8.8lx\n"),
                                              madr);
                                g_system->pause();
                                return;
                            case PCSX::MsanStatus::OK:
                                break;
                        }
                    } else {
                        madr &= 0x7ffffc;
                    }

                    if (DMACommandCounter++ > 2000000) break;
                    if (madr == usedAddr[1]) break;
                    if (madr == usedAddr[2]) break;

                    if (madr < usedAddr[0]) {
                        usedAddr[1] = madr;
                    } else {
                        usedAddr[2] = madr;
                    }

                    usedAddr[0] = madr;
                    uint32_t nextMadr = SWAP_LEu32(*mem->getPointer<uint32_t>(madr)) & 0xffffff;
                    if (usingMsan && nextMadr == Memory::c_msanChainMarker) {
                        madr = g_emulator->m_mem->msanGetChainPtr(madr);
                        continue;
                    }
                    madr = nextMadr;
                } while (!(madr & 0x800000));
                if ((madr & 0xffffff) != 0xffffff) {
                    mem->dmaInterruptError();
                }
            } else {
                uint32_t blocSize = bcr >> 16;
                if (blocSize == 0) blocSize = 0x10000;
                uint32_t size = blocSize * (bcr & 0xffff);
                madr = madr + size * 4;
            }
            mem->template setMADR<n>(madr);
            if (mode == 0) {
                mem->template setBCR<n>(bcr & 0xffff0000);
            } else if (mode == 1) {
                mem->template setBCR<n>(bcr & 0x0000ffff);
            }
        }
    }
};

}  // namespace PCSX
