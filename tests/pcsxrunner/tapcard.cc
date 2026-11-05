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

// The guest test drives SIO0 directly. setup-tap.lua puts a multitap on port 1 with cards in slots B and D
// and none in slot C; setup-notap.lua leaves the tap off with the slot B and D cards still inserted. Those
// cards are memcard1b.mcd and memcard1d.mcd, created formatted beside memcard1.mcd when missing. Each
// leaves its arm for the guest in the scratchpad.

TEST(Tapcard, TapInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-luacov", "-dofile", "src/mips/tests/tapcard/setup-tap.lua", "-loadexe",
                        "src/mips/tests/tapcard/tapcard.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(Tapcard, TapDynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-luacov",
                        "-dofile", "src/mips/tests/tapcard/setup-tap.lua", "-loadexe",
                        "src/mips/tests/tapcard/tapcard.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(Tapcard, NoTapInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-luacov", "-dofile", "src/mips/tests/tapcard/setup-notap.lua", "-loadexe",
                        "src/mips/tests/tapcard/tapcard.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
