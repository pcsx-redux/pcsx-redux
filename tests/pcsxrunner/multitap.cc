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

// The guest test drives SIO0 directly. setup-host.lua writes the host input fields of the shared display
// block that -shmdisplay creates: a multitap on port 1 with three of its four slots filled, and a mouse
// with pending motion on port 2. setup-nohost.lua writes the same fields with hostInput left at 0, which
// must change nothing: no tap, no mouse, no host buttons. Each leaves its arm for the guest in the
// scratchpad.

TEST(Multitap, HostInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-luacov", "-shmdisplay", "-dofile", "src/mips/tests/multitap/setup-host.lua", "-loadexe",
                        "src/mips/tests/multitap/multitap.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(Multitap, HostDynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-luacov", "-shmdisplay", "-dofile", "src/mips/tests/multitap/setup-host.lua", "-loadexe",
                        "src/mips/tests/multitap/multitap.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(Multitap, NoHostInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-luacov", "-shmdisplay", "-dofile", "src/mips/tests/multitap/setup-nohost.lua", "-loadexe",
                        "src/mips/tests/multitap/multitap.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// setup-req1.lua plugs a neGcon in slot A and leaves B to D empty, as on the real tap the guest's replay
// was measured with: long reads after a long read that did not send 42h at the start of slot A's block are
// the four-byte garbage response.
TEST(Multitap, Req1Interpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-luacov", "-shmdisplay", "-dofile", "src/mips/tests/multitap/setup-req1.lua", "-loadexe",
                        "src/mips/tests/multitap/multitap.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(Multitap, Req1Dynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-luacov", "-shmdisplay", "-dofile", "src/mips/tests/multitap/setup-req1.lua", "-loadexe",
                        "src/mips/tests/multitap/multitap.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
