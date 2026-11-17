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

#include <cstdlib>
#include <string>

#include "gtest/gtest.h"
#include "main/main.h"

// 2MB-VRAM (Konami System 573 second bank) regression gates. These probes
// self-check on the emulator and pcsx_exit() a non-zero code on mismatch, so
// the gtest just asserts the exit code is 0. Probes that are pure hardware
// characterization (they log values for comparison against silicon rather than
// asserting a single correct answer) are run manually and are not wired here
// yet; they need hardware baselines before they can be exit-code gated.

// bank-probe characterizes the GP1(09h) gate. It self-verifies that the lower
// bank round-trips and that the gate flips the upper bank between mirror and
// (real | open-bus); both 1MB and 2MB fitments are valid and pass.
TEST(TwoMBVram, BankProbe1MB) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-loadexe", "src/mips/tests/gpu/2mb-vram/bank-probe/bank-probe.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(TwoMBVram, BankProbe2MB) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/bank-probe/bank-probe.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// texpage-upper uploads a 16-bit texture page into the upper bank and samples
// it through a textured rectangle, asserting the drawn texels match the source.
// It self-validates against a lower-bank page first, so a passing run proves
// upper-bank texture sampling specifically. Requires the 2MB fitment.
TEST(TwoMBVram, TexpageUpper) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/texpage-upper/texpage-upper.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// transfer-wrap-y verifies that a VRAM transfer running past the top of the
// upper bank wraps the 10-bit Y counter (1023 -> 0) rather than dropping the
// overflow rows. Silicon (573) WRAPS for both the GP0(A0h) upload and the
// GP0(80h) copy; the probe self-asserts that exact outcome (full high zone
// plus every overflow row reappearing at the bottom of VRAM). Requires the
// 2MB fitment with the bank gate open.
TEST(TwoMBVram, TransferWrapY) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/transfer-wrap-y/transfer-wrap-y.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// transfer-h-quirk verifies the COPY size masking eff_h = ((h-1)&0x1ff)+1 for
// both the GP0(A0h) upload and the GP0(80h) blit: each transfers exactly eff_h
// rows. The probe self-asserts exact == eff_h per case. Requires the 2MB
// fitment with the gate open.
TEST(TwoMBVram, TransferHQuirk) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/transfer-h-quirk/transfer-h-quirk.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// vram-blit-y verifies VRAM-to-VRAM copy fidelity into and across the upper
// bank (src/dst on either side of Y=512, both crossing, and the tall h512/
// h1024 cases). The probe self-asserts exact_matches == eff_h per case.
TEST(TwoMBVram, VramBlitY) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/vram-blit-y/vram-blit-y.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// vram-transfers-y verifies CPU-to-VRAM upload fidelity into and across the
// upper bank. The probe self-asserts the high-zone match count
// min(eff_h, 1024-y) per case (the rows that wrap past Y=1023 are covered by
// transfer-wrap-y, not counted here).
TEST(TwoMBVram, VramTransfersY) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/vram-transfers-y/vram-transfers-y.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// drawing-area-y verifies the drawing-area scissor honors 10-bit Y: a [top,
// bot) area draws rows top..bot-1 (bottom exclusive) all the way to Y=1023.
// The probe self-asserts that formula per case.
TEST(TwoMBVram, DrawingAreaY) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/drawing-area-y/drawing-area-y.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// drawing-offset-y verifies the drawing offset shifts a primitive into the
// upper bank with 10-bit Y, and that an offset pushing it past Y=1023 does
// NOT wrap on draw. Expected values are measured 573 silicon.
TEST(TwoMBVram, DrawingOffsetY) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/drawing-offset-y/drawing-offset-y.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// primitives-cross verifies triangles, rectangles, lines and sprites all
// rasterize correctly across the Y=512 bank boundary into the upper bank.
// Expected per-band counts are measured 573 silicon.
TEST(TwoMBVram, PrimitivesCross) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/primitives-cross/primitives-cross.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// gate-flip-queued writes GP1(09h) right after queuing a marker behind a long
// run of large rectangles; the marker must land in the bank of the gate it was
// drawn under.
TEST(TwoMBVram, GateFlipQueued) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-2mbvram", "-loadexe", "src/mips/tests/gpu/2mb-vram/gate-flip-queued/gate-flip-queued.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// The same self-checking probes under the OpenGL renderer. It needs a GL
// context, so these run with the GUI rather than -no-ui, and only on Linux,
// where CI runs the suite under xvfb. Without a display they are skipped.
#if defined(__linux__)
static bool haveDisplay() { return std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY"); }

static int runOpenGLProbe(const char* name) {
    std::string exe = std::string("src/mips/tests/gpu/2mb-vram/") + name + "/" + name + ".ps-exe";
    MainInvoker invoker("-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter", "-openglgpu",
                        "-2mbvram", "-loadexe", exe.c_str());
    return invoker.invoke();
}

TEST(TwoMBVramOpenGL, BankProbe) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("bank-probe"), 0);
}

TEST(TwoMBVramOpenGL, TexpageUpper) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("texpage-upper"), 0);
}

TEST(TwoMBVramOpenGL, TransferWrapY) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("transfer-wrap-y"), 0);
}

TEST(TwoMBVramOpenGL, TransferHQuirk) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("transfer-h-quirk"), 0);
}

TEST(TwoMBVramOpenGL, VramBlitY) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("vram-blit-y"), 0);
}

TEST(TwoMBVramOpenGL, VramTransfersY) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("vram-transfers-y"), 0);
}

TEST(TwoMBVramOpenGL, DrawingAreaY) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("drawing-area-y"), 0);
}

TEST(TwoMBVramOpenGL, DrawingOffsetY) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("drawing-offset-y"), 0);
}

TEST(TwoMBVramOpenGL, PrimitivesCross) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("primitives-cross"), 0);
}

TEST(TwoMBVramOpenGL, GateFlipQueued) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    EXPECT_EQ(runOpenGLProbe("gate-flip-queued"), 0);
}
#endif
