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

// ENDX (1F801D9Ch/1D9Eh) drives a full latch -> key-on clear -> re-latch cycle with a
// second voice held as a control, and asserts its own verdict rather than printing one
// to be read by eye, so a non-zero exit here is a regression and not a diagnostic.
// Behaviour checked against an SCPH-1001. The interpreter is used because the SPU runs
// on its own thread, so some CPU/SPU jitter is expected; the dynarec widens it.
TEST(SPU, Endx) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe", "src/mips/tests/spu/endx/spu-endx.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// A voice that plays an ADPCM block flagged end-without-repeat goes to Release with its
// envelope forced to 0, without any KEY OFF, and games poll ENVX for exactly that. The
// guest checks when ENVX first reads 0 against the SPUSTAT bit 11 clock, at a constant
// pitch and across a mid-sample pitch change, with a looping voice as the control.
TEST(SPU, EndMute) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe", "src/mips/tests/spu/endmute/spu-endmute.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// After an end block the readout jumps to the repeat address whether or not the repeat flag
// is set; without it the voice is only silenced. The guest redirects a one-shot sample to a
// silent loop elsewhere in SPU RAM and waits for IRQ9 at an address inside that loop, with an
// end+repeat block as the control.
TEST(SPU, EndRedirect) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe",
                        "src/mips/tests/spu/endredirect/spu-endredirect.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// The key-on timeline measured on SCPH-1001, -5501 and -7001: KEY ON is serviced on
// every other sample, three header fetches apply the first block's loop-start flag, the
// attack is four samples after ENDX clears, and a repeat address write only holds once
// the third fetch has passed.
TEST(SPU, KeyOnLatch) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe", "src/mips/tests/spu/konlatch/spu-konlatch.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// Two things about SPU IRQ9, checked on silicon: an address match latches SPUSTAT bit 6
// and leaves the SPUCNT bit 6 enable set until software writes it to 0, and a voice keeps
// reading SPU RAM after it has been keyed off and its envelope has fallen to zero. The guest asserts both against a playing voice first,
// so a dead status flag cannot be mistaken for a silent voice that stopped reading.
// IRQ9 re-fire rules, plus SPUSTAT writes: a guest writing 0000 or ffff to the read-only
// SPUSTAT leaves the latched IRQ9 flag set, raises no extra IRQ9 edge and sets no bits
// 12-15. Measured on SCPH-1001, -5501 and -7001.
TEST(SPU, IrqRefire) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe", "src/mips/tests/spu/irqrefire/spu-irqrefire.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(SPU, OffVoiceIrq) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe",
                        "src/mips/tests/spu/offvoice/spu-offvoice.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// The accuracy suite: ADPCM decode, the envelope shapes, the capture mirror, sample rates and
// IRQ9, every one graded against a capture from an SCPH-1001 rather than against us. The
// families that do not hold here yet are CESTER_MAYBE_TEST in the guest and compile out under
// PCSX_TESTS, so what runs below is the part that already matches silicon and a failure is a
// regression in it. Interpreter for the same reason as above. Building the guest with
// PCSX_TESTS=true is what selects the emulator subset, and src/mips/tests/Makefile passes it.
TEST(SPU, Accuracy) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe", "src/mips/tests/spu/spu/spu.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// Voice volume sweep, read back through VOLXL. The guest checks 14 sweep settings
// against a trace taken on an SCPH-1001, two envelope steps of tolerance.
TEST(SPU, VolSweep) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-loadexe", "src/mips/tests/spu/volsweep/spu-volsweep.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
