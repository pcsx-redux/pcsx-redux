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

// The guest points the generic allocator tracker at PSYQo's allocator and
// checks the tracker's conclusions against what it actually did. A nonzero
// exit code is the number of the check that failed.
TEST(AllocTracker, Interpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-debugger", "-loadexe", "src/mips/tests/tooling/alloc-tracker/alloc-tracker.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// Negative control, and it is the reason the test above means anything.
// Execution breakpoints do not exist under the dynamic recompiler, so the
// tracker must observe nothing at all. If this ever starts passing, either the
// dynarec grew breakpoint support or the guest's checks went vacuous, and both
// of those want a human to look. Check 8 is the first one that ranges over
// breakpoint-driven tracking; everything before it exercises the command port,
// which lives in the hardware write path and works under either core.
TEST(AllocTracker, DynarecObservesNothing) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-debugger", "-loadexe", "src/mips/tests/tooling/alloc-tracker/alloc-tracker.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 8);
}
