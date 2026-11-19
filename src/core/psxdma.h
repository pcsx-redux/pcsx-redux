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

#include "core/psxemulator.h"
#include "core/r3000a.h"

static inline void scheduleGPUDMAIRQ(uint32_t eCycle) {
    PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::GPUDMA, eCycle);
}

static inline void scheduleSPUDMAIRQ(uint32_t eCycle) {
    PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::SPUDMA, eCycle);
}

static inline void scheduleMDECOUTDMAIRQ(uint32_t eCycle) {
    PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::MDECOUTDMA, eCycle);
}

static inline void scheduleMDECINDMAIRQ(uint32_t eCycle) {
    PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::MDECINDMA, eCycle);
}

static inline void scheduleGPUOTCDMAIRQ(uint32_t eCycle) {
    PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::GPUOTCDMA, eCycle);
}

// True when it left MADR and BCR as the transfer stopped, which dmaExec must not overwrite.
bool dma4(uint32_t madr, uint32_t bcr, uint32_t chcr);
// SPUCNT was written: a sync-1 DMA4 waiting between blocks goes on if the transfer mode now asks.
void dma4Resume();
void dma6(uint32_t madr, uint32_t bcr, uint32_t chcr);
void spuInterrupt();
void gpuotcInterrupt();
