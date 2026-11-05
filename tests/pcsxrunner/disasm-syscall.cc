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

namespace {

std::string dis(uint32_t code, uint32_t nextCode, bool *skipNext) {
    return PCSX::Disasm::asString(code, nextCode, 0x80010000, skipNext);
}

}  // namespace

// li $a0, n followed by syscall reads as one "syscall <name>" line, and the syscall is skipped.
TEST(DisasmSyscall, MergesLiA0AndSyscall) {
    static constexpr struct {
        uint32_t li;
        const char *name;
    } cases[] = {
        {0x24040000, "NoFunction"},               // addiu $a0, $zero, 0
        {0x24040001, "EnterCriticalSection"},     // addiu $a0, $zero, 1
        {0x34040002, "ExitCriticalSection"},      // ori $a0, $zero, 2
        {0x24040003, "ChangeThreadSubFunction"},  // addiu $a0, $zero, 3
        {0x24040010, "DeliverEvent"},             // addiu $a0, $zero, 0x10
    };
    for (auto &c : cases) {
        bool skip = false;
        std::string s = dis(c.li, 0x0000000c, &skip);
        std::string expected = std::string(": syscall ") + c.name;
        ASSERT_GE(s.size(), expected.size()) << s;
        EXPECT_EQ(s.substr(s.size() - expected.size()), expected) << s;
        EXPECT_TRUE(skip) << s;
    }
}

// No merge when the register is not $a0, when the next opcode is not a syscall, when the syscall
// carries a nonzero code, or when the caller cannot skip the next instruction.
TEST(DisasmSyscall, KeepsPlainLi) {
    bool skip = false;
    std::string s = dis(0x24050001, 0x0000000c, &skip);  // li $a1, 1
    EXPECT_NE(s.find("li"), std::string::npos) << s;
    EXPECT_EQ(s.find("syscall"), std::string::npos) << s;
    EXPECT_FALSE(skip);

    s = dis(0x24040001, 0x00000000, &skip);  // li $a0, 1 then nop
    EXPECT_EQ(s.find("syscall"), std::string::npos) << s;
    EXPECT_FALSE(skip);

    s = dis(0x24040001, 0x0000010c, &skip);  // li $a0, 1 then syscall 4
    EXPECT_EQ(s.find("syscall"), std::string::npos) << s;
    EXPECT_FALSE(skip);

    s = dis(0x24040001, 0x0000000c, nullptr);
    EXPECT_EQ(s.find("syscall"), std::string::npos) << s;
}
