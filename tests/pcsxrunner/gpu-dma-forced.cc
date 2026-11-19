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

#include "gtest/gtest.h"
#include "main/main.h"

// Sync-1 GPU DMA started with CHCR bit 28 while GP1(04h)=0: the forced block clears bit 28, the
// channel then waits for GP1(04h)=2, and a CHCR rewritten while it waits applies to the remaining
// blocks. ret == 0 means every arm the emulator runs passed.
static int runSuite(const char* cpu, const char* exe) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", cpu, "-loadexe",
                        exe);
    return invoker.invoke();
}

TEST(GPUDMAForced, Interpreter) {
    EXPECT_EQ(runSuite("-interpreter", "src/mips/tests/gpu/dma-forced/gpu-dma-forced.ps-exe"), 0);
}

TEST(GPUDMAForced, Dynarec) {
    EXPECT_EQ(runSuite("-dynarec", "src/mips/tests/gpu/dma-forced/gpu-dma-forced.ps-exe"), 0);
}

TEST(DMARewrite2, Interpreter) {
    EXPECT_EQ(runSuite("-interpreter", "src/mips/tests/dma/rewrite2/dma-rewrite2.ps-exe"), 0);
}

TEST(DMARewrite2, Dynarec) { EXPECT_EQ(runSuite("-dynarec", "src/mips/tests/dma/rewrite2/dma-rewrite2.ps-exe"), 0); }
