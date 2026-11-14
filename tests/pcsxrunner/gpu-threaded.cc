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

// The soft GPU's worker-thread renderer, run against the GPU guest suites.
// Each suite already passes on the inline renderer; with -softgputhreaded the
// same exit code means draws, uploads, copies, readbacks and GPUSTAT stayed
// in command order.

static int runThreaded(const char *exe) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgputhreaded", "-loadexe", exe);
    return invoker.invoke();
}

TEST(GPUAsyncStress, Inline) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-loadexe", "src/mips/tests/gpu/async-stress/gpu-async-stress.ps-exe");
    EXPECT_EQ(invoker.invoke(), 0);
}

TEST(GPUAsyncStress, Threaded) { EXPECT_EQ(runThreaded("src/mips/tests/gpu/async-stress/gpu-async-stress.ps-exe"), 0); }
TEST(GPUThreaded, Command) { EXPECT_EQ(runThreaded("src/mips/tests/gpu/command/gpu-command.ps-exe"), 0); }
TEST(GPUThreaded, FBRect) { EXPECT_EQ(runThreaded("src/mips/tests/gpu/fbrect/gpu-fbrect.ps-exe"), 0); }
TEST(GPUThreaded, VRAMMove) { EXPECT_EQ(runThreaded("src/mips/tests/gpu/vram-move/gpu-vram-move.ps-exe"), 0); }
TEST(GPUThreaded, Polyline) { EXPECT_EQ(runThreaded("src/mips/tests/gpu/polyline/gpu-polyline.ps-exe"), 0); }

// The 2MB VRAM suites under the worker. Only GateFlipQueued writes GP1(09h)
// while primitives are still queued; the others set the gate with nothing in
// flight and pass with drainRenderer() removed.
static int runThreaded2MB(const char *exe) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgputhreaded", "-2mbvram", "-loadexe", exe);
    return invoker.invoke();
}

TEST(GPUThreaded, BankProbe1MB) {
    EXPECT_EQ(runThreaded("src/mips/tests/gpu/2mb-vram/bank-probe/bank-probe.ps-exe"), 0);
}
TEST(GPUThreaded, BankProbe2MB) {
    EXPECT_EQ(runThreaded2MB("src/mips/tests/gpu/2mb-vram/bank-probe/bank-probe.ps-exe"), 0);
}
TEST(GPUThreaded, TexpageUpper) {
    EXPECT_EQ(runThreaded2MB("src/mips/tests/gpu/2mb-vram/texpage-upper/texpage-upper.ps-exe"), 0);
}
TEST(GPUThreaded, VramBlitY) {
    EXPECT_EQ(runThreaded2MB("src/mips/tests/gpu/2mb-vram/vram-blit-y/vram-blit-y.ps-exe"), 0);
}
TEST(GPUThreaded, DrawingAreaY) {
    EXPECT_EQ(runThreaded2MB("src/mips/tests/gpu/2mb-vram/drawing-area-y/drawing-area-y.ps-exe"), 0);
}
TEST(GPUThreaded, PrimitivesCross) {
    EXPECT_EQ(runThreaded2MB("src/mips/tests/gpu/2mb-vram/primitives-cross/primitives-cross.ps-exe"), 0);
}
TEST(GPUThreaded, GateFlipQueued) {
    EXPECT_EQ(runThreaded2MB("src/mips/tests/gpu/2mb-vram/gate-flip-queued/gate-flip-queued.ps-exe"), 0);
}
