/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
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

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "spu/externals.h"
#include "spu/interface.h"

// SPU RAM -> Main RAM DMA.
void PCSX::SPU::impl::readDMAMem(uint16_t* mainMem, int size) {
    // The mixer writes the capture and reverb areas as it plays, and it runs behind the CPU:
    // bring it up to this cycle first.
    catchUp(PCSX::g_emulator->m_cpu->m_regs.cycle);
    // A new transfer drops SPUSTAT bits 7 and 9 left over from the last write.
    m_writeDMALandedCycle = 0;
    // Always lock: the mixer thread writes the capture areas of spuMem under cbMtx,
    // and deciding from an unlocked read of mixIrqAddress would itself be a race.
    std::lock_guard<std::mutex> lock(cbMtx);

    for (int i = 0; i < size; i++) {
        // Copy 2 bytes.
        *mainMem++ = spuMem[spuAddr >> 1];
        // Increment the SPU address and wrap around.
        spuAddr = (spuAddr + 2) & 0x7ffff;
    }
}

// To investigate: do sound data updates by DMA writes affect SPU IRQs? Will an IRQ be triggered if new
// data is written to the memory IRQ address?

void PCSX::SPU::impl::lockSPURAM() { cbMtx.lock(); }
void PCSX::SPU::impl::unlockSPURAM() { cbMtx.unlock(); }

void PCSX::SPU::impl::resetCaptureBuffer() {
    // The capture buffers are always live: hardware writes them continuously and
    // raises the IRQ whenever the write reaches SPU_IRQ_ADDR. Nothing about that is
    // optional, so the cursor is always armed.
    // Everything below is shared with the mixer thread (MainThread), so hold cbMtx.
    std::lock_guard<std::mutex> lock(cbMtx);
    mixIrqAddress = spuRamBase;
    memset(captureBuffer.CDCapLeft, 0, sizeof(captureBuffer.CDCapLeft));
    memset(captureBuffer.CDCapRight, 0, sizeof(captureBuffer.CDCapRight));
    captureBuffer.endIndex = 0;
    captureBuffer.startIndex = 0;
    // The capture write positions follow the mixer's sample clock and are not reset here.
    // A reset restarts the CPU cycle count, so a pending landing cycle from before it is stale.
    m_writeDMALandedCycle = 0;
}

// After CHCR goes idle on a write DMA, the SPU still has the tail of the transfer to put into
// sound RAM. On SCPH-1001, SCPH-5501 and SCPH-7001, SPUSTAT bits 7 and 9 read low right after
// CHCR idle and rise together 172-213 sysclk/8 ticks later, once every byte has landed; they
// stay set until the next transfer starts. 192 ticks is the middle of that.
static constexpr uint64_t kWriteDMALandingCycles = 192 * 8;

void PCSX::SPU::impl::writeDMAEndsAt(uint64_t endCycle) { m_writeDMALandedCycle = endCycle + kWriteDMALandingCycles; }

// Main RAM -> SPU RAM DMA.
void PCSX::SPU::impl::writeDMAMem(uint16_t* mainMem, int size) {
    // The mixer reads sound RAM as it plays: it has to have played everything before this
    // cycle from the old contents.
    catchUp(PCSX::g_emulator->m_cpu->m_regs.cycle);
    std::lock_guard<std::mutex> lock(cbMtx);

    for (int i = 0; i < size; i++) {
        // Copy 2 bytes.
        spuMem[spuAddr >> 1] = *mainMem++;
        // Increment the SPU address and wrap around.
        spuAddr = (spuAddr + 2) & 0x7ffff;
    }
}
