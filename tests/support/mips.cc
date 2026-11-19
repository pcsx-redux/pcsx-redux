/***************************************************************************
 *   Copyright (C) 2025 PCSX-Redux authors                                 *
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
#include "mips-common/util/decoder.hh"
#include "mips-common/util/encoder.hh"

using namespace Mips;
using namespace Mips::Decoder;
using namespace Mips::Encoder;

TEST(MipsTest, RegisterEnum) {
    EXPECT_EQ(static_cast<int>(Reg::R0), 0);
    EXPECT_EQ(static_cast<int>(Reg::RA), 31);
}

TEST(MipsTest, GPRRegsSize) {
    GPRRegs regs;
    EXPECT_EQ(sizeof(regs), 34 * sizeof(uint32_t));
}

TEST(MipsTest, InstructionDecoding) {
    Instruction addInstr(0x012a4020);  // add $t0, $t1, $t2
    EXPECT_EQ(addInstr.mnemonic(), Instruction::ADD);
    EXPECT_EQ(addInstr.rs(), 9);
    EXPECT_EQ(addInstr.rt(), 10);
    EXPECT_EQ(addInstr.rd(), 8);
}

TEST(MipsTest, InstructionEncoding) {
    uint32_t encoded = add(Reg::T0, Reg::T1, Reg::T2);
    EXPECT_EQ(encoded, 0x012a4020);
}

TEST(MipsTest, LoadInstruction) {
    Instruction lwInstr(0x8c890004);  // lw $t1, 4($a0)
    EXPECT_EQ(lwInstr.mnemonic(), Instruction::LW);
    EXPECT_EQ(lwInstr.rs(), 4);
    EXPECT_EQ(lwInstr.rt(), 9);
    EXPECT_EQ(lwInstr.imm(), 4);
}

TEST(MipsTest, StoreInstruction) {
    Instruction swInstr(0xac890004);  // sw $t1, 4($a0)
    EXPECT_EQ(swInstr.mnemonic(), Instruction::SW);
    EXPECT_EQ(swInstr.rs(), 4);
    EXPECT_EQ(swInstr.rt(), 9);
    EXPECT_EQ(swInstr.imm(), 4);
}

TEST(MipsTest, BranchInstruction) {
    Instruction beqInstr(0x112a0003);  // beq $t1, $t2, 3
    EXPECT_EQ(beqInstr.mnemonic(), Instruction::BEQ);
    EXPECT_EQ(beqInstr.rs(), 9);
    EXPECT_EQ(beqInstr.rt(), 10);
    EXPECT_EQ(beqInstr.imm(), 3);
    EXPECT_EQ(beqInstr.getBranchAddress(0x1000), 0x1010);
}

TEST(MipsTest, JumpInstruction) {
    Instruction jInstr(0x0800000a);  // j 0x00000028
    EXPECT_EQ(jInstr.mnemonic(), Instruction::J);
    EXPECT_EQ(jInstr.target(), 0x0a);
}

TEST(MipsTest, EncoderDecoderIntegration) {
    uint32_t encoded = add(Reg::T0, Reg::T1, Reg::T2);
    Instruction decoded(encoded);
    EXPECT_EQ(decoded.mnemonic(), Instruction::ADD);
    EXPECT_EQ(decoded.rs(), 9);
    EXPECT_EQ(decoded.rt(), 10);
    EXPECT_EQ(decoded.rd(), 8);
}
