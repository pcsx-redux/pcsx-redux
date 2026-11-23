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

#include "core/r3000a.h"
#include "gtest/gtest.h"
#include "main/main.h"

TEST(GTE, Interpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-luacov", "-loadexe", "src/mips/tests/gte/gte/gte.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(GTE, Dynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-luacov",
                        "-loadexe", "src/mips/tests/gte/gte/gte.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// A GTE register write still in flight survives a save state. Code poked into RAM queues
// MTC2 IR1 in a jump's delay slot and reads it back three instructions later, storing to
// 0x80100100 on a mismatch. The state is saved at a vsync where the write has not landed,
// then loaded again; the readback must still match.
TEST(GTE, PendingWriteSaveState) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-dofile", "tests/lua/gte-pending-write-savestate.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// MFC2 and CFC2 whose destination is read by the next instruction. That instruction sees the
// old value, and the cop2 value must land after it.
TEST(GTE, MoveFromLoadDelayInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-dofile", "tests/lua/gte-cop2-load-delay.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(GTE, MoveFromLoadDelayDynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-dofile",
                        "tests/lua/gte-cop2-load-delay.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// Sony's libgte MulMatrix writes VZ0 with mtc2 and issues MVMVA one instruction later, relying
// on the new value. Code poked into RAM writes VZ0 = 0x100, waits, writes VZ0 = 0x200, runs N nops
// (N = 0, 1, 2), then MVMVA with RT = identity, storing IR3 to 0x80110000 + 4 * N. Interrupts are
// off. 0x600d at 0x8011000c marks the end.
TEST(GTE, MvmvaAfterMtc2) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-dofile", "tests/lua/gte-mvmva-after-mtc2.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(GTE, MvmvaAfterMtc2Dynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-dofile",
                        "tests/lua/gte-mvmva-after-mtc2.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// nugget tests/gte/write-to-use: each (command, input register) pair, with N = 0..3 instructions
// between the write and the command. The program exits 0 when every checked result matches the
// consoles, and 2 when only the results that mix old and new terms differ, which a delay per
// command and register cannot produce. Both CPUs are expected to get to 2.
TEST(GTE, WriteToUseCoarseInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-loadexe", "src/mips/tests/gte/write-to-use/gte-write-to-use.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 2);
}

// -dynarec runs the interpreter where no recompiler is built, which would pass this test without testing one.
TEST(GTE, WriteToUseCoarseDynarec) {
#if defined(DYNAREC_NONE)
    GTEST_SKIP();
#endif
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-loadexe", "src/mips/tests/gte/write-to-use/gte-write-to-use.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 2);
}

// MFC2, CFC2 and SWC2 reading a GTE register before a write to it has landed, with the
// GTEStaleReads setting on and the debugger enabled: each such read pauses, and the reads that
// get the written value (enough instructions in between, or a stall on a running command) don't.
TEST(GTE, StaleReadsCaught) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-dofile", "tests/lua/gte-stale-reads.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// Same program with the setting off: the reads return the same values and nothing pauses.
TEST(GTE, StaleReadsSettingOff) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-dofile", "tests/lua/gte-stale-reads.lua", "-exec", "GteStaleReadsOff = true");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
