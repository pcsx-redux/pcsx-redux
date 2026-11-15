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

#include "core/disr3000a.h"
#include "gtest/gtest.h"
#include "main/main.h"

TEST(COPBranch, Interpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-debugger", "-luacov", "-loadexe", "src/mips/tests/cpu/cop-branch/cop-branch.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(COPBranch, Dynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-luacov",
                        "-loadexe", "src/mips/tests/cpu/cop-branch/cop-branch.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(COPBranch, Disassembler) {
    static constexpr struct {
        uint32_t code;
        const char* mnemonic;
    } cases[] = {
        {0x41000004, "bc0f"}, {0x41010004, "bc0t"}, {0x45000004, "bc1f"}, {0x45030004, "bc1t"},
        {0x49000004, "bc2f"}, {0x49010004, "bc2t"}, {0x4d020004, "bc3f"}, {0x4d010004, "bc3t"},
    };
    for (auto& c : cases) {
        std::string s = PCSX::Disasm::asString(c.code, 0, 0x80010000);
        EXPECT_NE(s.find(std::string(": ") + c.mnemonic + " "), std::string::npos) << s;
        EXPECT_NE(s.find("0x80010014"), std::string::npos) << s;
    }
}
