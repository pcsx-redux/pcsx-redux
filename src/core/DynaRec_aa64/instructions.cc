/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include "recompiler.h"
#if defined(DYNAREC_AA64)
#define BAILZERO(x) \
    if (!(x)) {     \
        return;     \
    }

void DynaRecCPU::recUnknown(uint32_t code) {
    PCSX::g_system->message("Unknown instruction for dynarec - address %08x, instruction %08x\n", m_pc, m_regs.code);
    recException(Exception::ReservedInstruction);
}
void DynaRecCPU::recSpecial(uint32_t code) {
    const auto func = m_recSPC[code & 0x3F];  // Look up the opcode in our decoding LUT
    (*this.*func)(code);                      // Jump into the handler to recompile it
}

// The Dynarec doesn't currently handle overflow exceptions, so we treat ADD the same as ADDU
void DynaRecCPU::recADD(uint32_t code) { recADDU(code); }

void DynaRecCPU::recADDU(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val + m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.moveAndAdd(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.moveAndAdd(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Add(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recADDIU(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (_Rs_ == _Rt_) {
        if (m_gprs[_Rt_].isConst()) {
            m_gprs[_Rt_].val += _Imm_;
        } else {
            allocateReg(_Rt_);
            m_gprs[_Rt_].setWriteback(true);
            gen.moveAndAdd(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, _Imm_);
        }
    } else {
        if (m_gprs[_Rs_].isConst()) {
            markConst(_Rt_, m_gprs[_Rs_].val + _Imm_);
        } else {
            alloc_rs_wb_rt(code);
            gen.moveAndAdd(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg, _Imm_);
        }
    }
}

void DynaRecCPU::recAND(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val & m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.andImm(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.andImm(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.And(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recANDI(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (_Rs_ == _Rt_) {
        if (m_gprs[_Rs_].isConst()) {
            m_gprs[_Rt_].val &= _ImmU_;
        } else {
            allocateReg(_Rt_);
            m_gprs[_Rt_].setWriteback(true);
            gen.andImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, _ImmU_);
        }
    } else {
        if (m_gprs[_Rs_].isConst()) {
            markConst(_Rt_, m_gprs[_Rs_].val & _ImmU_);
        } else {
            alloc_rs_wb_rt(code);
            gen.andImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg, _ImmU_);
        }
    }
}

void DynaRecCPU::recBEQ(uint32_t code) {
    const auto target = _Imm_ * 4 + m_pc;
    m_nextIsDelaySlot = true;

    if (target == m_pc + 4) {
        return;
    }

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        if (m_gprs[_Rs_].val == m_gprs[_Rt_].val) {
            m_pcWrittenBack = true;
            m_stopCompiling = true;
            gen.Mov(w0, target);
            gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
            m_linkedPC = target;
        }
        return;
    } else if (m_gprs[_Rs_].isConst()) {
        allocateReg(_Rt_);
        gen.Cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        allocateReg(_Rs_);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs(code);
        gen.Cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    gen.Mov(w0, target);       // w0 = addr if jump taken
    gen.Mov(w1, m_pc + 4);     // w1 = addr if jump not taken
    gen.Csel(w0, w0, w1, eq);  // if taken, return the jump addr into w0
    gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
}

void DynaRecCPU::recBGTZ(uint32_t code) {
    uint32_t target = _Imm_ * 4 + m_pc;

    m_nextIsDelaySlot = true;
    if (target == m_pc + 4) {
        return;
    }

    if (m_gprs[_Rs_].isConst()) {
        if ((int32_t)m_gprs[_Rs_].val > 0) {
            m_pcWrittenBack = true;
            m_stopCompiling = true;
            gen.Mov(w0, target);
            gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
            m_linkedPC = target;
        }
        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    if (m_gprs[_Rs_].isAllocated()) {  // Don't bother allocating Rs unless it's already allocated
        gen.Cmp(m_gprs[_Rs_].allocatedReg, 0);
    } else {
        gen.Ldr(w0, MemOperand(contextPointer, GPR_OFFSET(_Rs_)));
        gen.Cmp(w0, 0);
    }

    gen.Mov(w0, target);       // w0 = addr if jump taken
    gen.Mov(w1, m_pc + 4);     // w1 = addr if jump not taken
    gen.Csel(w0, w0, w1, gt);  // if taken, return the jump addr into w0
    gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
}

void DynaRecCPU::recBLEZ(uint32_t code) {
    uint32_t target = _Imm_ * 4 + m_pc;

    m_nextIsDelaySlot = true;
    if (target == m_pc + 4) {
        return;
    }

    if (m_gprs[_Rs_].isConst()) {
        if ((int32_t)m_gprs[_Rs_].val <= 0) {
            m_pcWrittenBack = true;
            m_stopCompiling = true;
            gen.Mov(w0, target);
            gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
            m_linkedPC = target;
        }
        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    if (m_gprs[_Rs_].isAllocated()) {  // Don't bother allocating Rs unless it's already allocated
        gen.Cmp(m_gprs[_Rs_].allocatedReg, 0);
    } else {
        gen.Ldr(w0, MemOperand(contextPointer, GPR_OFFSET(_Rs_)));
        gen.Cmp(w0, 0);
    }

    gen.Mov(w0, target);       // w0 = addr if jump taken
    gen.Mov(w1, m_pc + 4);     // w1 = addr if jump not taken
    gen.Csel(w0, w0, w1, le);  // if taken, return the jump addr into w0
    gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
}

void DynaRecCPU::recBNE(uint32_t code) {
    const auto target = _Imm_ * 4 + m_pc;
    m_nextIsDelaySlot = true;

    if (target == m_pc + 4) {
        return;
    }

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        if (m_gprs[_Rs_].val != m_gprs[_Rt_].val) {
            m_pcWrittenBack = true;
            m_stopCompiling = true;
            gen.Mov(w0, target);
            gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
            m_linkedPC = target;
        }
        return;
    } else if (m_gprs[_Rs_].isConst()) {
        allocateReg(_Rt_);
        gen.Cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        allocateReg(_Rs_);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs(code);
        gen.Cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    gen.Mov(w0, target);       // w0 = addr if jump taken
    gen.Mov(w1, m_pc + 4);     // w1 = addr if jump not taken
    gen.Csel(w0, w0, w1, ne);  // if taken, return the jump addr into w0
    gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
}

void DynaRecCPU::recCOP0(uint32_t code) {
    switch (_Rs_) {  // figure out the type of COP0 opcode
        case 0:
            recMFC0(code);
            break;
        case 4:
            recMTC0(code);
            break;
        case 8:
            recBCz(code);
            break;
        case 16:
            recRFE(code);
            break;
        default:
            fmt::print("Unimplemented cop0 op {}\n", _Rs_);
            recUnknown(code);
            break;
    }
}

// COP1 and COP3 don't exist, but their branches still decode
void DynaRecCPU::recCOP1or3(uint32_t code) {
    if (_Rs_ == 8) {
        recBCz(code);
    } else {
        recUnknown(code);
    }
}

// BCzF / BCzT. None of the coprocessors drives the condition input, so BCzF
// always branches and BCzT never does. Bit 1 of rt is ignored. With SR.CUz
// clear, the branch raises a coprocessor unusable exception, COP0 included,
// and leaves the block without running the delay slot.
void DynaRecCPU::recBCz(uint32_t code) {
    const unsigned z = (code >> 26) & 3;
    const auto target = _Imm_ * 4 + m_pc;
    Label usable;

    flushRegs();
    gen.Ldr(w4, MemOperand(contextPointer, COP0_OFFSET(12)));
    gen.Tbnz(w4, 28 + z, &usable);

    // The load started by the previous instruction still lands
    if (m_firstInstruction && m_fullLoadDelayEmulation) {
        Label noDelayedLoad;
        const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
        gen.Ldrb(w4, MemOperand(contextPointer, isActiveOffset));
        gen.Cbz(w4, &noDelayedLoad);
        call(m_loadDelayHandler);
        gen.L(noDelayedLoad);
    }
    const auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad ^ 1];
    if (delayedLoad.active && delayedLoad.index != 0) {
        const auto delayedValueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;
        gen.Ldr(w4, MemOperand(contextPointer, delayedValueOffset));
        gen.Str(w4, MemOperand(contextPointer, GPR_OFFSET(delayedLoad.index)));
    }
    emitGTEFlushQueue(true);

    loadThisPointer(arg1.X());
    gen.Mov(arg2, (static_cast<uint32_t>(Exception::CoprocessorUnusable) << 2) | (z << 28));
    gen.Mov(arg3, (int32_t)m_inDelaySlot);
    gen.Mov(w3, exceptionPC());
    gen.Str(w3, MemOperand(contextPointer, PC_OFFSET));
    call(exceptionWrapper);
    gen.Ldr(x0, MemOperand(contextPointer, CYCLE_OFFSET));
    gen.Add(x0, x0,
            (m_instructionCount + m_romInstructionCount * PCSX::Emulator::ROM_EXTRA_BIAS) * PCSX::Emulator::BIAS);
    gen.Str(x0, MemOperand(contextPointer, CYCLE_OFFSET));
    jmp((void*)m_returnFromBlock);

    gen.L(usable);
    m_nextIsDelaySlot = true;

    if ((_Rt_ & 1) || target == m_pc + 4) {
        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;
    gen.Mov(w0, target);
    gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
    m_linkedPC = target;
}

void DynaRecCPU::recDIV(uint32_t code) {
    Label notIntMin, divisionByZero, end;
    bool emitIntMinCheck = true;

    if (m_gprs[_Rt_].isConst()) {     // Check divisor if constant
        if (m_gprs[_Rt_].val == 0) {  // Handle case where divisor is 0
            if (m_gprs[_Rs_].isConst()) {
                // LO = 1 or -1 depending on the sign of $rs. HI = $rs
                gen.Mov(w0, m_gprs[_Rs_].val & 0x80000000 ? 1 : -1);
                gen.Mov(w1, m_gprs[_Rs_].val);
                gen.Stp(w0, w1, MemOperand(contextPointer, LO_OFFSET));
            }

            else {
                allocateReg(_Rs_);
                gen.Lsr(w0, m_gprs[_Rs_].allocatedReg, 31);
                gen.Sub(w1, w0, 1);
                gen.Add(w0, w0, w1);
                // Set lo to 1 or -1 depending on the sign of $rs, set hi to $rs
                gen.Stp(w0, m_gprs[_Rs_].allocatedReg, MemOperand(contextPointer, LO_OFFSET));
            }

            return;
        } else if (m_gprs[_Rt_].val != 0xffffffff) {
            emitIntMinCheck = false;
        }

        if (m_gprs[_Rs_].isConst()) {
            if (m_gprs[_Rs_].val == 0x80000000 && m_gprs[_Rt_].val == 0xffffffff) {
                gen.Mov(w0, 0x80000000);
                gen.Stp(w0, wzr, MemOperand(contextPointer, LO_OFFSET));
            } else {
                gen.Mov(w0, (int32_t)m_gprs[_Rs_].val / (int32_t)m_gprs[_Rt_].val);
                gen.Mov(w1, (int32_t)m_gprs[_Rs_].val % (int32_t)m_gprs[_Rt_].val);
                gen.Stp(w0, w1, MemOperand(contextPointer, LO_OFFSET));
            }
            return;
        }

        allocateReg(_Rs_);
        gen.Mov(w0, m_gprs[_Rs_].allocatedReg);
        gen.Mov(w1, m_gprs[_Rt_].val);  // Divisor in w1
    } else {                            // non-constant divisor
        if (m_gprs[_Rs_].isConst()) {
            allocateReg(_Rt_);
            gen.Mov(w0, m_gprs[_Rs_].val);  // Dividend in w0
            emitIntMinCheck = m_gprs[_Rs_].val == 0x80000000;
        }

        else {
            alloc_rt_rs(code);
            gen.Mov(w0, m_gprs[_Rs_].allocatedReg);  // Dividend in w0
        }

        gen.Mov(w1, m_gprs[_Rt_].allocatedReg);  // Divisor in w1
        gen.Cmp(w1, 0);                          // Check if divisor is 0
        gen.beq(divisionByZero);                 // Jump to divisionByZero label if so
    }

    if (emitIntMinCheck) {
        gen.Cmp(w0, 0x80000000);  // Check if dividend is INT_MIN
        gen.bne(notIntMin);       // Bail if not
        gen.Cmp(w1, 0xffffffff);  // Check if divisor is -1
        gen.bne(notIntMin);       // Bail if not

        // Handle INT_MIN / -1
        gen.Mov(w2, 0x80000000);  // Set lo to INT_MIN
        gen.Mov(w3, 0);           // Set hi to 0
        gen.B(&end);
    }

    gen.L(notIntMin);
    gen.Sdiv(w2, w0, w1);      // Signed divide
    gen.Msub(w3, w2, w1, w0);  // Get remainder by msub

    if (!m_gprs[_Rt_].isConst()) {  // Emit a division by 0 handler if the divisor is unknown at compile time
        gen.B(&end);                // skip to the end if not a div by zero
        gen.L(divisionByZero);      // Here starts our division by 0 handler

        gen.Mov(w3, w0);  // Set hi to $rs
        gen.Lsr(w2, w0, 31);
        gen.Sub(w5, w2, 1);
        gen.Add(w2, w2, w5);  // Set lo to 1 or -1 depending on the sign of $rs
    }

    gen.L(end);
    gen.Stp(w2, w3, MemOperand(contextPointer, LO_OFFSET));  // Lo = quotient, Hi = remainder
}

void DynaRecCPU::recDIVU(uint32_t code) {
    Label divisionByZero;

    if (m_gprs[_Rt_].isConst()) {     // Check divisor if constant
        if (m_gprs[_Rt_].val == 0) {  // Handle case where divisor is 0
            gen.Mov(w0, -1);
            gen.Str(w0, MemOperand(contextPointer, LO_OFFSET));  // Set lo to -1

            if (m_gprs[_Rs_].isConst()) {
                gen.Mov(w0, m_gprs[_Rs_].val);
                gen.Str(w0, MemOperand(contextPointer, HI_OFFSET));  // HI = $rs
            }

            else {
                allocateReg(_Rs_);
                gen.Str(m_gprs[_Rs_].allocatedReg, MemOperand(contextPointer, HI_OFFSET));  // Set hi to $rs
            }

            return;
        }

        if (m_gprs[_Rs_].isConst()) {
            gen.Mov(w0, m_gprs[_Rs_].val / m_gprs[_Rt_].val);
            gen.Mov(w1, m_gprs[_Rs_].val % m_gprs[_Rt_].val);
            gen.Stp(w0, w1, MemOperand(contextPointer, LO_OFFSET));
            return;
        }

        allocateReg(_Rs_);
        gen.Mov(w0, m_gprs[_Rs_].allocatedReg);
        gen.Mov(w1, m_gprs[_Rt_].val);  // Divisor in w1
    } else {                            // non-constant divisor
        if (m_gprs[_Rs_].isConst()) {
            allocateReg(_Rt_);
            gen.Mov(w0, m_gprs[_Rs_].val);  // Dividend in w0
        }

        else {
            alloc_rt_rs(code);
            gen.Mov(w0, m_gprs[_Rs_].allocatedReg);  // Dividend in w0
        }

        gen.Mov(w1, m_gprs[_Rt_].allocatedReg);  // Divisor in w1
        gen.Cmp(w1, 0);                          // Check if divisor is 0
        gen.beq(divisionByZero);                 // Jump to divisionByZero label if so
    }
    gen.Udiv(w2, w0, w1);      // Unsigned division by divisor
    gen.Msub(w3, w2, w1, w0);  // Get remainder by msub

    if (!m_gprs[_Rt_].isConst()) {  // Emit a division by 0 handler if the divisor is unknown at compile time
        Label end;
        gen.B(&end);            // skip to the end if not a div by zero
        gen.L(divisionByZero);  // Here starts our division by 0 handler
        gen.Mov(w3, w0);        // Set hi to $rs
        gen.Mov(w2, -1);        // Set lo to -1

        gen.L(end);
    }
    gen.Stp(w2, w3, MemOperand(contextPointer, LO_OFFSET));  // Lo = quotient, Hi = remainder
}

void DynaRecCPU::recJ(uint32_t code) {
    const uint32_t target = (m_pc & 0xf0000000) | (_Target_ << 2);
    m_nextIsDelaySlot = true;
    m_stopCompiling = true;
    m_pcWrittenBack = true;

    gen.Mov(w0, target);
    gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));  // Write PC
    m_linkedPC = target;
}

void DynaRecCPU::recJAL(uint32_t code) {
    maybeCancelDelayedLoad(31);
    markConst(31, m_pc + 4);  // Set $ra to the return value, then treat instruction like a normal J
    recJ(code);
}

void DynaRecCPU::recJALR(uint32_t code) {
    recJR(code);

    if (_Rd_) {
        maybeCancelDelayedLoad(_Rd_);
        markConst(_Rd_, m_pc + 4);  // Link
    }
}

void DynaRecCPU::recJR(uint32_t code) {
    m_nextIsDelaySlot = true;
    m_stopCompiling = true;
    m_pcWrittenBack = true;

    if (m_gprs[_Rs_].isConst()) {
        gen.Mov(w0, m_gprs[_Rs_].val & ~3);
        gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));  // force align jump address
        m_linkedPC = m_gprs[_Rs_].val;
    } else {
        allocateReg(_Rs_);
        // PC will get force aligned in the dispatcher since it discards the 2 lower bits
        gen.Str(m_gprs[_Rs_].allocatedReg, MemOperand(contextPointer, PC_OFFSET));
    }
}

void DynaRecCPU::recLUI(uint32_t code) {
    BAILZERO(_Rt_);

    maybeCancelDelayedLoad(_Rt_);
    markConst(_Rt_, m_regs.code << 16);
}

void DynaRecCPU::recLB(uint32_t code) { recompileLoad<8, true>(code); }
void DynaRecCPU::recLBU(uint32_t code) { recompileLoad<8, false>(code); }
void DynaRecCPU::recLH(uint32_t code) { recompileLoad<16, true>(code); }
void DynaRecCPU::recLHU(uint32_t code) { recompileLoad<16, false>(code); }
void DynaRecCPU::recLW(uint32_t code) { recompileLoad<32, true>(code); }

void DynaRecCPU::recLWL(uint32_t code) {
    // The mask to be applied to $rt (top 32 bits) and the shift to be applied to the read memory value (low 32 bits)
    // Depending on the low 3 bits of the unaligned address
    static const uint64_t MASKS_AND_SHIFTS[4] = {0x00FFFFFF00000018, 0x0000FFFF00000010, 0x000000FF00000008, 0};

    // LWL writes $rt straight away rather than going through the delay machinery, so a load still pending on $rt
    // would trample the merged value one instruction later.
    if (_Rt_) {
        maybeCancelDelayedLoad(_Rt_);
    }

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {  // Both previous register value and address are constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWL_MASK[address & 3];
        const auto shift = LWL_SHIFT[address & 3];
        const uint32_t previousValue = m_gprs[_Rt_].val;

        gen.Mov(arg1, address);  // Address in arg1
        call(lwlReadWrapper);    // Read value returned in w0

        if (_Rt_) {
            allocateReg(_Rt_);  // Allocate $rt with writeback
            m_gprs[_Rt_].setWriteback(true);
            gen.Mov(m_gprs[_Rt_].allocatedReg, previousValue & mask);  // Mask the previous $rt value
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg,
                    Operand(w0, LSL, shift));  // Or $rt with shifted value in w0
        }
    } else if (m_gprs[_Rs_].isConst()) {  // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWL_MASK[address & 3];
        const auto shift = LWL_SHIFT[address & 3];

        gen.Mov(arg1, address);  // Address in arg1
        call(lwlReadWrapper);    // Read value returned in w0

        if (_Rt_) {
            allocateReg(_Rt_);  // Allocate $rt with writeback
            m_gprs[_Rt_].setWriteback(true);
            gen.andImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, mask);  // Mask the previous $rt value
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg,
                    Operand(w0, LSL, shift));  // Or $rt with shifted value in w0
        }
    } else if (m_gprs[_Rt_].isConst()) {  // Only previous rt value is constant
        const uint32_t previousValue = m_gprs[_Rt_].val;

        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwlReadWrapper);                                    // Read from the aligned address, result in w0

        if (_Rt_) {
            // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
            alloc_rs_wb_rt(code);
            gen.Mov(m_gprs[_Rt_].allocatedReg, previousValue);     // Flush constant value in $rt
            gen.moveAndAdd(w1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w1 again
            gen.And(w1, w1, 3);                                    // Get the low 2 bits
            gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);  // Form PC-relative address to mask and shift lookup table
            gen.Ldr(x3, MemOperand(x3, x1, LSL, 3));  // Load the mask and shift from LUT by indexing using the bottom 2
                                                      // bits of the unaligned addr.
            gen.Lsl(w0, w0, w3);                      // Shift the read value by the shift amount
            // Mask using the mask in the top 32 bits of x3
            gen.And(m_gprs[_Rt_].allocatedReg.X(), m_gprs[_Rt_].allocatedReg.X(), Operand(x3, LSR, 32));
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, w0);  // Merge with shifted value
        }
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwlReadWrapper);                                    // Read from the aligned address, result in w0

        if (_Rt_) {
            // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
            // $rt is read-modify-write here, so it has to be loaded, not just reserved for writeback
            alloc_rt_rs(code);
            m_gprs[_Rt_].setWriteback(true);
            gen.moveAndAdd(w1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w1
            gen.And(w1, w1, 3);                                    // Get the low 2 bits
            gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);             // Base to mask and shift lookup table in x3
            gen.Ldr(x3, MemOperand(x3, x1, LSL, 3));  // Load the mask and shift from LUT by indexing using the bottom 2
                                                      // bits of the unaligned addr.
            gen.Lsl(w0, w0, w3);
            // Mask using the mask in the top 32 bits of x3
            gen.And(m_gprs[_Rt_].allocatedReg.X(), m_gprs[_Rt_].allocatedReg.X(), Operand(x3, LSR, 32));
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, w0);  // Merge with shifted value
        }
    }
}

void DynaRecCPU::recLWR(uint32_t code) {
    // The mask to be applied to $rt (top 32 bits) and the shift to be applied to the read memory value (low 32 bits)
    // Depending on the low 3 bits of the unaligned address
    static const uint64_t MASKS_AND_SHIFTS[4] = {0, 0xFF00000000000008, 0xFFFF000000000010, 0xFFFFFF0000000018};

    // Same as LWL: cancel a pending load on $rt, since this writes $rt immediately
    if (_Rt_) {
        maybeCancelDelayedLoad(_Rt_);
    }

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {  // Both previous register value and address are constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWR_MASK[address & 3];
        const auto shift = LWR_SHIFT[address & 3];
        const uint32_t previousValue = m_gprs[_Rt_].val;

        gen.Mov(arg1, address);  // Address in arg1 (w0)
        call(lwrReadWrapper);    // Read value returned in w0

        if (_Rt_) {
            allocateReg(_Rt_);  // Allocate $rt with writeback
            m_gprs[_Rt_].setWriteback(true);
            gen.Mov(m_gprs[_Rt_].allocatedReg, previousValue & mask);  // Mask the previous $rt value
            // Shift the read value from aligned address and Or $re with shifted value
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, Operand(w0, LSR, shift));
        }
    } else if (m_gprs[_Rs_].isConst()) {  // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWR_MASK[address & 3];
        const auto shift = LWR_SHIFT[address & 3];

        gen.Mov(arg1, address);  // Address in arg1
        call(lwrReadWrapper);    // Read value returned in w0

        if (_Rt_) {
            allocateReg(_Rt_);  // Allocate $rt with writeback
            m_gprs[_Rt_].setWriteback(true);
            gen.andImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, mask);  // Mask the previous $rt value
            // Shift the read value from aligned address and Or $re with shifted value
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, Operand(w0, LSR, shift));
        }
    } else if (m_gprs[_Rt_].isConst()) {  // Only previous rt value is constant
        const uint32_t previousValue = m_gprs[_Rt_].val;

        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwrReadWrapper);                                    // Read from the aligned address, result in w0

        if (_Rt_) {
            // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
            alloc_rs_wb_rt(code);
            gen.Mov(m_gprs[_Rt_].allocatedReg, previousValue);     // Flush constant value in $rt
            gen.moveAndAdd(w1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w1 again
            gen.And(w1, w1, 3);                                    // Get the low 2 bits
            gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);  // Form PC-relative address to mask and shift lookup table
            gen.Ldr(x3, MemOperand(x3, x1, LSL, 3));  // Load the mask and shift from LUT by indexing using the bottom 2
                                                      // bits of the unaligned addr.
            gen.Lsr(w0, w0, w3);                      // Shift the read value by the shift amount
            // Mask previous $rt value with the mask stored in the top 32 bits of x3
            gen.And(m_gprs[_Rt_].allocatedReg.X(), m_gprs[_Rt_].allocatedReg.X(), Operand(x3, LSR, 32));
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, w0);  // Merge with newly read value
        }
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwrReadWrapper);                                    // Read from the aligned address, result in eax

        if (_Rt_) {
            // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
            // $rt is read-modify-write here, so it has to be loaded, not just reserved for writeback
            alloc_rt_rs(code);
            m_gprs[_Rt_].setWriteback(true);
            gen.moveAndAdd(w1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w1 again
            gen.And(w1, w1, 3);                                    // Get the low 2 bits
            gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);  // Form PC-relative address to mask and shift lookup table
            gen.Ldr(x3, MemOperand(x3, x1, LSL, 3));  // Load the mask and shift from LUT by indexing using the bottom 2
                                                      // bits of the unaligned addr.
            gen.Lsr(w0, w0, w3);                      // Shift the read value by the shift amount
            // Mask previous $rt value with the mask stored in the top 32 bits of X3
            gen.And(m_gprs[_Rt_].allocatedReg.X(), m_gprs[_Rt_].allocatedReg.X(), Operand(x3, LSR, 32));
            gen.Orr(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, w0);  // Merge with newly read value
        }
    }
}

void DynaRecCPU::recMFC0(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);
    allocateRegWithoutLoad(_Rt_);
    m_gprs[_Rt_].setWriteback(true);
    gen.Ldr(m_gprs[_Rt_].allocatedReg, MemOperand(contextPointer, COP0_OFFSET(_Rd_)));
}

// TODO: Constant propagation for MFLO/HI, read the result from register if possible instead of reading memory again
void DynaRecCPU::recMFLO(uint32_t code) {
    BAILZERO(_Rd_);

    maybeCancelDelayedLoad(_Rd_);
    allocateRegWithoutLoad(_Rd_);
    m_gprs[_Rd_].setWriteback(true);

    gen.Ldr(m_gprs[_Rd_].allocatedReg, MemOperand(contextPointer, LO_OFFSET));
}

void DynaRecCPU::recMFHI(uint32_t code) {
    BAILZERO(_Rd_);

    maybeCancelDelayedLoad(_Rd_);
    allocateRegWithoutLoad(_Rd_);
    m_gprs[_Rd_].setWriteback(true);

    gen.Ldr(m_gprs[_Rd_].allocatedReg, MemOperand(contextPointer, HI_OFFSET));
}

// Recompile a load whose result the next instruction actually reads, so the write to $rt has to be held back by
// one instruction. The value goes to memory and either processDelayedLoad or the load delay handler picks it up.
template <int size, bool signExtend>
void DynaRecCPU::recompileLoadWithDelay(uint32_t code, LoadDelayDependencyType type) {
    if (m_gprs[_Rs_].isConst()) {  // Store the address in first argument register
        const uint32_t addr = m_gprs[_Rs_].val + _Imm_;
        gen.Mov(arg1, addr);
        if (size == 32 && usesArgumentRegisters(addr, false)) syncArgumentRegisters();
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);
        if (size == 32) syncArgumentRegisters();
    }

    switch (size) {
        case 8:
            call(read8Wrapper);
            break;
        case 16:
            call(read16Wrapper);
            break;
        case 32:
            call(read32Wrapper);
            break;
        default:
            PCSX::g_system->message("Invalid size for memory load in dynarec. Instruction %08x\n", m_regs.code);
            break;
    }

    if (_Rt_) {
        switch (size) {  // The read result is in w0. 32-bit loads need no extension.
            case 8:
                signExtend ? gen.Sxtb(w0, w0) : gen.Uxtb(w0, w0);
                break;
            case 16:
                signExtend ? gen.Sxth(w0, w0) : gen.Uxth(w0, w0);
                break;
        }

        // Cancel any pending load to the same register, as this load overrides it
        maybeCancelDelayedLoad(_Rt_);
        storeDelayedLoad(_Rt_, type);
    }
}

template <int size, bool signExtend>
void DynaRecCPU::recompileLoad(uint32_t code) {
    const auto loadDelayDependency = getLoadDelayDependencyType(_Rt_);
    if (loadDelayDependency != LoadDelayDependencyType::NoDependency) {
        recompileLoadWithDelay<size, signExtend>(code, loadDelayDependency);
        return;
    }

    // If we won't emulate the load delay, make sure to cancel any pending loads that might trample the value
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst()) {  // Store the address in first argument register
        const uint32_t addr = m_gprs[_Rs_].val + _Imm_;
        const auto pointer = PCSX::g_emulator->m_mem->pointerRead(addr);

        if (pointer != nullptr && (_Rt_) != 0) {
            allocateRegWithoutLoad(_Rt_);
            m_gprs[_Rt_].setWriteback(true);
            load<size, signExtend>(m_gprs[_Rt_].allocatedReg, pointer);
            return;
        }

        gen.Mov(arg1, addr);
        if (size == 32 && usesArgumentRegisters(addr, false)) syncArgumentRegisters();
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);
        if (size == 32) syncArgumentRegisters();
    }

    switch (size) {
        case 8:
            call(read8Wrapper);
            break;
        case 16:
            call(read16Wrapper);
            break;
        case 32:
            call(read32Wrapper);
            break;
        default:
            PCSX::g_system->message("Invalid size for memory load in dynarec. Instruction %08x\n", m_regs.code);
            break;
    }

    if (_Rt_) {
        allocateRegWithoutLoad(_Rt_);  // Allocate $rt after calling the read function, otherwise call() might flush it.
        m_gprs[_Rt_].setWriteback(true);

        switch (size) {
            case 8:
                signExtend ? gen.Sxtb(m_gprs[_Rt_].allocatedReg, w0) : gen.Uxtb(m_gprs[_Rt_].allocatedReg, w0);
                break;
            case 16:
                signExtend ? gen.Sxth(m_gprs[_Rt_].allocatedReg, w0) : gen.Uxth(m_gprs[_Rt_].allocatedReg, w0);
                break;
            case 32:
                gen.Mov(m_gprs[_Rt_].allocatedReg, w0);
                break;
        }
    }
}

// TODO: Handle all COP0 register writes properly. Don't treat read-only field as writeable!
void DynaRecCPU::recMTC0(uint32_t code) {
    if (m_gprs[_Rt_].isConst()) {
        if (_Rd_ == 13) {
            gen.Mov(w0, m_gprs[_Rt_].val & ~0xFC00);
            gen.Str(w0, MemOperand(contextPointer, COP0_OFFSET(_Rd_)));
        } else if (_Rd_ != 6 && _Rd_ != 14 && _Rd_ != 15) {  // Don't write to JUMPDEST, EPC or PRID
            gen.Mov(w0, m_gprs[_Rt_].val);
            gen.Str(w0, MemOperand(contextPointer, COP0_OFFSET(_Rd_)));
        }
    }

    else {
        allocateReg(_Rt_);
        if (_Rd_ == 13) {
            gen.And(w0, m_gprs[_Rt_].allocatedReg, ~0xFC00);
            gen.Str(w0, MemOperand(contextPointer, COP0_OFFSET(_Rd_)));
        } else if (_Rd_ != 6 && _Rd_ != 14 && _Rd_ != 15) {  // Don't write to JUMPDEST, EPC or PRID
            gen.Str(m_gprs[_Rt_].allocatedReg,
                    MemOperand(contextPointer, COP0_OFFSET(_Rd_)));  // Write rt to the cop0 reg
        }
    }

    // Writing to SR/Cause can sometimes forcefully fire an interrupt. So we need to emit extra code to check.
    if (_Rd_ == 12 || _Rd_ == 13) {
        testSoftwareInterrupt<true>();
    }
}

// Checks if a write to SR/CAUSE forcibly triggered an interrupt
// loadSR: Shows if SR is already in w4 or if it should be loaded from memory
template <bool loadSR>
void DynaRecCPU::testSoftwareInterrupt() {
    Label label;
    const auto pc = w3;  // Register to hold temporary PC values in
    const auto sr = w4;  // Register to hold SR in

    if (!m_pcWrittenBack) {
        gen.Mov(pc, m_pc);
        gen.Str(pc, MemOperand(contextPointer, PC_OFFSET));
        m_pcWrittenBack = true;
    }

    m_stopCompiling = true;
    m_linkedPC = std::nullopt;  // The exception replaces the PC that a branch before us linked to
    // The exception call below only runs on one path, but its spill updates the allocator for both. Spill here so
    // the fall-through path writes back the same registers.
    prepareForCall();

    if constexpr (loadSR) {
        gen.Ldr(sr, MemOperand(contextPointer, COP0_OFFSET(12)));  // w4 = SR
    }
    // TODO: Possibly use Tbz or similar here
    gen.Tst(sr, 1);                                              // Check if interrupts are enabled
    gen.bz(label);                                               // If not, skip to the end
    gen.Ldr(arg2, MemOperand(contextPointer, COP0_OFFSET(13)));  // arg2 = CAUSE
    gen.And(sr, sr, arg2);
    gen.Tst(sr, 0x300);  // Check if an interrupt was force-fired
    gen.bz(label);       // Skip to the end if not

    // Fire the interrupt if it was triggered
    // This object in arg1. Exception code is already in arg2 from before (will be masked by exception handler)
    loadThisPointer(arg1.X());
    // The interrupt is taken before the next instruction in execution order, which is the PC written back above, and
    // BD is clear: in a delay slot, the slot completes first and EPC is the branch target.
    gen.Mov(arg3, 0);
    call(exceptionWrapper);  // Call the exception wrapper function

    gen.L(label);  // Execution will jump here if interrupts not enabled
}

void DynaRecCPU::recMTHI(uint32_t code) {
    if (m_gprs[_Rs_].isConst()) {
        gen.Mov(w0, m_gprs[_Rs_].val);
        gen.Str(w0, MemOperand(contextPointer, HI_OFFSET));
    } else {
        allocateReg(_Rs_);
        gen.Str(m_gprs[_Rs_].allocatedReg, MemOperand(contextPointer, HI_OFFSET));
    }
}

void DynaRecCPU::recMTLO(uint32_t code) {
    if (m_gprs[_Rs_].isConst()) {
        gen.Mov(w0, m_gprs[_Rs_].val);
        gen.Str(w0, MemOperand(contextPointer, LO_OFFSET));
    } else {
        allocateReg(_Rs_);
        gen.Str(m_gprs[_Rs_].allocatedReg, MemOperand(contextPointer, LO_OFFSET));
    }
}

void DynaRecCPU::recMULT(uint32_t code) {
    if ((m_gprs[_Rs_].isConst() && m_gprs[_Rs_].val == 0) || (m_gprs[_Rt_].isConst() && m_gprs[_Rt_].val == 0)) {
        gen.Str(xzr, MemOperand(contextPointer, LO_OFFSET));  // Set both LO and HI to 0 in a single 64-bit write
        return;
    }

    if (m_gprs[_Rs_].isConst()) {
        if (m_gprs[_Rt_].isConst()) {
            const uint64_t result = (int64_t)(int32_t)m_gprs[_Rt_].val * (int64_t)(int32_t)m_gprs[_Rs_].val;
            gen.Mov(x0, result);
            gen.Str(x0, MemOperand(contextPointer, LO_OFFSET));
        } else {
            allocateReg(_Rt_);
            gen.Mov(w1, m_gprs[_Rs_].val);
            gen.Smull(x0, w1, m_gprs[_Rt_].allocatedReg);
        }
    } else {
        if (m_gprs[_Rt_].isConst()) {
            allocateReg(_Rs_);
            gen.Mov(w1, m_gprs[_Rt_].val);
            gen.Smull(x0, w1, m_gprs[_Rs_].allocatedReg);
        } else {
            alloc_rt_rs(code);
            gen.Smull(x0, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }
    }

    // Write 64-bit result to lo and hi at the same time
    gen.Str(x0, MemOperand(contextPointer, LO_OFFSET));
}

void DynaRecCPU::recMULTU(uint32_t code) {
    if ((m_gprs[_Rs_].isConst() && m_gprs[_Rs_].val == 0) || (m_gprs[_Rt_].isConst() && m_gprs[_Rt_].val == 0)) {
        gen.Str(xzr, MemOperand(contextPointer, LO_OFFSET));  // Set both LO and HI to 0 in a single 64-bit write
        return;
    }

    if (m_gprs[_Rs_].isConst()) {
        if (m_gprs[_Rt_].isConst()) {
            const uint64_t result = (uint64_t)m_gprs[_Rt_].val * (uint64_t)m_gprs[_Rs_].val;
            gen.Mov(x0, result);
            gen.Str(x0, MemOperand(contextPointer, LO_OFFSET));
        } else {
            allocateReg(_Rt_);
            gen.Mov(w1, m_gprs[_Rs_].val);
            gen.Umull(x0, w1, m_gprs[_Rt_].allocatedReg);
        }
    } else {
        if (m_gprs[_Rt_].isConst()) {
            allocateReg(_Rs_);
            gen.Mov(w1, m_gprs[_Rt_].val);
            gen.Umull(x0, w1, m_gprs[_Rs_].allocatedReg);
        } else {
            alloc_rt_rs(code);
            gen.Umull(x0, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }
    }

    // Write 64-bit result to lo and hi at the same time
    gen.Str(x0, MemOperand(contextPointer, LO_OFFSET));
}

void DynaRecCPU::recNOR(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, ~(m_gprs[_Rs_].val | m_gprs[_Rt_].val));
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.Orr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
        gen.Mvn(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Orr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
        gen.Mvn(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Orr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        gen.Mvn(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg);
    }
}

void DynaRecCPU::recOR(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val | m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.orImm(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.orImm(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Orr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recORI(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (_Rs_ == _Rt_) {
        if (m_gprs[_Rs_].isConst()) {
            m_gprs[_Rt_].val |= _ImmU_;
        } else {
            allocateReg(_Rt_);
            m_gprs[_Rt_].setWriteback(true);
            gen.orImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, _ImmU_);
        }
    } else {
        if (m_gprs[_Rs_].isConst()) {
            markConst(_Rt_, m_gprs[_Rs_].val | _ImmU_);
        } else {
            alloc_rs_wb_rt(code);
            gen.orImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg, _ImmU_);
        }
    }
}

void DynaRecCPU::recREGIMM(uint32_t code) {
    const bool isBGEZ = ((m_regs.code >> 16) & 1) != 0;
    const bool link = ((m_regs.code >> 17) & 0xF) == 8;
    const auto target = _Imm_ * 4 + m_pc;

    m_nextIsDelaySlot = true;

    if (link) {
        maybeCancelDelayedLoad(31);
        markConst(31, m_pc + 4);
    }

    if (target == m_pc + 4) {
        return;
    }

    if (m_gprs[_Rs_].isConst()) {
        if (isBGEZ) {  // BGEZ
            if ((int32_t)m_gprs[_Rs_].val >= 0) {
                m_pcWrittenBack = true;
                m_stopCompiling = true;
                gen.Mov(w0, target);
                gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
                m_linkedPC = target;
            }
        }

        else {  // BLTZ
            if ((int32_t)m_gprs[_Rs_].val < 0) {
                m_pcWrittenBack = true;
                m_stopCompiling = true;
                gen.Mov(w0, target);
                gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
                m_linkedPC = target;
            }
        }

        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    allocateReg(_Rs_);
    gen.Cmp(m_gprs[_Rs_].allocatedReg, 0);
    gen.Mov(w0, target);    // w0 = addr if jump taken
    gen.Mov(w1, m_pc + 4);  // w1 = addr if jump not taken

    if (isBGEZ) {
        gen.Csel(w0, w0, w1, ge);  // if $rs >= 0, move the jump addr into w0
    } else {
        gen.Csel(w0, w0, w1, lt);  // if $rs < 0, move the jump addr into w0
    }

    gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
}

void DynaRecCPU::recRFE(uint32_t code) {
    gen.Ldr(w4, MemOperand(contextPointer, COP0_OFFSET(12)));  // w0 = COP0 status register
    gen.And(w1, w4, 0x3c);                                     // mask out the rest of the SR value
    gen.And(w4, w4, ~0xF);                                     // Clear bottom 4 bits of w4
    gen.Orr(w4, w4, Operand(w1, LSR, 2));                      // Shift bits [5:2] of SR two places to the right
                                                               // & merge the shifted bits into w0
    gen.Str(w4, MemOperand(contextPointer, COP0_OFFSET(12)));  // Write w4 back to SR
    testSoftwareInterrupt<false>();
}

void DynaRecCPU::recSB(uint32_t code) {
    if (m_gprs[_Rs_].isConst()) {
        const uint32_t addr = m_gprs[_Rs_].val + _Imm_;
        const auto pointer = PCSX::g_emulator->m_mem->pointerWrite(addr, 8);

        if (pointer != nullptr) {
            if (m_gprs[_Rt_].isConst()) {
                store<8>(m_gprs[_Rt_].val & 0xFF, pointer);
            } else {
                allocateReg(_Rt_);
                store<8>(m_gprs[_Rt_].allocatedReg, pointer);
            }

            return;
        }

        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg2
            gen.Mov(arg2, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);
        }

        gen.Mov(arg1, addr);  // Address to write to in arg1 TODO: Optimize
        call(write8Wrapper);
    }

    else {
        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg2
            gen.Mov(arg2, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);
        }

        allocateReg(_Rs_);
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address to write to in arg1   TODO: Optimize
        call(write8Wrapper);
    }
}

void DynaRecCPU::recSH(uint32_t code) {
    if (m_gprs[_Rs_].isConst()) {
        const uint32_t addr = m_gprs[_Rs_].val + _Imm_;
        const auto pointer = PCSX::g_emulator->m_mem->pointerWrite(addr, 16);
        if (pointer != nullptr) {
            if (m_gprs[_Rt_].isConst()) {
                store<16>(m_gprs[_Rt_].val & 0xFFFF, pointer);
            } else {
                allocateReg(_Rt_);
                store<16>(m_gprs[_Rt_].allocatedReg, pointer);
            }

            return;
        }

        else if (addr == 0x1f801070) {  // I_STAT
            gen.Mov(x0, (uint64_t)&PCSX::g_emulator->m_mem->m_hard[0x1070]);
            if (m_gprs[_Rt_].isConst()) {
                gen.Ldrh(w1, MemOperand(x0));
                gen.Mov(w2, m_gprs[_Rt_].val & 0xFFFF);
                gen.And(w1, w1, w2);
                gen.Strh(w1, MemOperand(x0));
            } else {
                allocateReg(_Rt_);
                gen.Ldrh(w1, MemOperand(x0));
                gen.And(w1, w1, m_gprs[_Rt_].allocatedReg);
                gen.Strh(w1, MemOperand(x0));
            }

            return;
        }

        else if (addr >= 0x1f801c00 && addr < 0x1f801e00) {  // SPU registers
            gen.Mov(arg1, addr);
            if (m_gprs[_Rt_].isConst()) {
                gen.Mov(arg2, m_gprs[_Rt_].val & 0xFFFF);
            } else {
                allocateReg(_Rt_);
                gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);
            }

            call(SPU_writeRegisterWrapper);
            return;
        }

        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg2
            gen.Mov(arg2, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);
        }

        gen.Mov(arg1, addr);  // Address to write to in arg1   TODO: Optimize
        call(write16Wrapper);
    }

    else {
        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg2
            gen.Mov(arg2, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);
        }

        allocateReg(_Rs_);
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address to write to in arg1   TODO: Optimize
        call(write16Wrapper);
    }
}

void DynaRecCPU::recSW(uint32_t code) {
    if (m_gprs[_Rs_].isConst()) {
        const uint32_t addr = m_gprs[_Rs_].val + _Imm_;
        const auto pointer = PCSX::g_emulator->m_mem->pointerWrite(addr, 32);
        if (pointer != nullptr) {
            if (m_gprs[_Rt_].isConst()) {
                store<32>(m_gprs[_Rt_].val, pointer);
            } else {
                allocateReg(_Rt_);
                store<32>(m_gprs[_Rt_].allocatedReg, pointer);
            }

            return;
        }

        if (m_gprs[_Rt_].isConst()) {  // Value to write in arg2
            gen.Mov(arg2, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);
        }

        gen.Mov(arg1, addr);  // Address to write to in arg1   TODO: Optimize
        if (usesArgumentRegisters(addr, true)) syncArgumentRegisters();
        call(write32Wrapper);
    }

    else {
        if (m_gprs[_Rt_].isConst()) {  // Value to write in arg2
            gen.Mov(arg2, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);
        }

        allocateReg(_Rs_);
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address to write to in arg1   TODO: Optimize
        syncArgumentRegisters();
        call(write32Wrapper);
    }
}

void DynaRecCPU::recSLL(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val << _Sa_);
    } else {
        alloc_rt_wb_rd(code);
        gen.Lsl(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, _Sa_);
    }
}

void DynaRecCPU::recSLLV(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val << (m_gprs[_Rs_].val & 0x1F));
    } else if (m_gprs[_Rs_].isConst()) {
        if (_Rt_ == _Rd_) {
            allocateReg(_Rd_);
            m_gprs[_Rd_].setWriteback(true);
            gen.Lsl(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        } else {
            alloc_rt_wb_rd(code);
            gen.Lsl(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        }
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Mov(w0, m_gprs[_Rt_].val);
        gen.Lsl(m_gprs[_Rd_].allocatedReg, w0, m_gprs[_Rs_].allocatedReg);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Lsl(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recSLT(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, (int32_t)m_gprs[_Rs_].val < (int32_t)m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.Cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
        gen.Cset(m_gprs[_Rd_].allocatedReg, gt);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
        gen.Cset(m_gprs[_Rd_].allocatedReg, lt);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        gen.Cset(m_gprs[_Rd_].allocatedReg, lt);
    }
}

void DynaRecCPU::recSLTI(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst()) {
        markConst(_Rt_, (int32_t)m_gprs[_Rs_].val < _Imm_);
    } else {
        alloc_rs_wb_rt(code);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, _Imm_);
        gen.Cset(m_gprs[_Rt_].allocatedReg, lt);
    }
}

void DynaRecCPU::recSLTIU(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst()) {
        markConst(_Rt_, m_gprs[_Rs_].val < (uint32_t)_Imm_);
    } else {
        alloc_rs_wb_rt(code);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, _Imm_);
        gen.Cset(m_gprs[_Rt_].allocatedReg, cc);
    }
}

void DynaRecCPU::recSLTU(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val < m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.Cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
        gen.Cset(m_gprs[_Rd_].allocatedReg, hi);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
        gen.Cset(m_gprs[_Rd_].allocatedReg, lo);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        gen.Cset(m_gprs[_Rd_].allocatedReg, lo);
    }
}

void DynaRecCPU::recSRA(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, (int32_t)m_gprs[_Rt_].val >> _Sa_);
    } else {
        alloc_rt_wb_rd(code);
        gen.Asr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, _Sa_);
    }
}

void DynaRecCPU::recSRAV(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, (int32_t)m_gprs[_Rt_].val >> (m_gprs[_Rs_].val & 0x1F));
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.Asr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Mov(w0, m_gprs[_Rt_].val);
        gen.Asr(m_gprs[_Rd_].allocatedReg, w0, m_gprs[_Rs_].allocatedReg);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Asr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recSRL(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val >> _Sa_);
    } else {
        alloc_rt_wb_rd(code);
        gen.Lsr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, _Sa_);
    }
}

void DynaRecCPU::recSRLV(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val >> (m_gprs[_Rs_].val & 0x1F));
    } else if (m_gprs[_Rs_].isConst()) {
        if (_Rt_ == _Rd_) {
            allocateReg(_Rd_);
            m_gprs[_Rd_].setWriteback(true);
            gen.Lsr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        } else {
            alloc_rt_wb_rd(code);
            gen.Lsr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        }
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Mov(w0, m_gprs[_Rt_].val);
        gen.Lsr(m_gprs[_Rd_].allocatedReg, w0, m_gprs[_Rs_].allocatedReg);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Lsr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recSUB(uint32_t code) { recSUBU(code); }

void DynaRecCPU::recSUBU(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val - m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.reverseSub(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Sub(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Sub(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].allocatedReg);
    }
}

void DynaRecCPU::recSWL(uint32_t code) {
    // The mask to be applied to $rt (top 32 bits) and the shift to be applied to the read memory value (low 32 bits)
    // Depending on the low 3 bits of the unaligned address
    static const uint64_t MASKS_AND_SHIFTS[4] = {0xFFFFFF0000000018, 0xFFFF000000000010, 0xFF00000000000008, 0};

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {  // Both previous register value and address are constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t alignedAddress = address & ~3;
        const uint32_t mask = SWL_MASK[address & 3];
        const auto shift = SWL_SHIFT[address & 3];

        gen.Mov(arg1, alignedAddress);  // Address in arg1 (w0)
        call(unalignedStoreReadWrapper);
        gen.andImm(arg2, w0, mask);  // Mask read value
        gen.Mov(w2, m_gprs[_Rt_].val >> shift);
        gen.Orr(arg2, arg2, w2);  // Shift $rt and or with read value

        gen.Mov(arg1, address);  // Unaligned address in arg1
        call(swlWriteWrapper);
    } else if (m_gprs[_Rs_].isConst()) {  // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t alignedAddress = address & ~3;
        const uint32_t mask = SWL_MASK[address & 3];
        const auto shift = SWL_SHIFT[address & 3];

        gen.Mov(arg1, alignedAddress);  // Address in arg1 (w0)
        call(unalignedStoreReadWrapper);
        gen.andImm(w0, w0, mask);  // Mask read value

        allocateReg(_Rt_);  // Allocate $rt
        // Value to write back to memory in $arg2
        gen.Orr(arg2, w0, Operand(m_gprs[_Rt_].allocatedReg, LSR, shift));
        gen.Mov(arg1, address);                                  // Unaligned address in arg1
        call(swlWriteWrapper);                                   // Write back
    } else if (m_gprs[_Rt_].isConst()) {                         // Only previous rt value is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.And(arg1, arg1, ~3);                                 // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in w0

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        allocateReg(_Rs_);
        gen.moveAndAdd(w2, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w2 again

        gen.Mov(w5, w2);  // Unaligned address, the write wrapper aligns it

        gen.And(w2, w2, 3);                         // edx = low 2 bits of address
        gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);  // Base to mask and shift lookup table in x3
        gen.Ldr(x3, MemOperand(x3, x2, LSL, 3));    // Load the mask and shift from LUT by indexing using the bottom 2
                                                    // bits of the unaligned addr.
        gen.Mov(arg2, m_gprs[_Rt_].val);            // arg2 = $rt
        gen.Lsr(arg2, arg2, w3);                    // Shift rt value
        gen.And(x0, x0, Operand(x3, LSR, 32));      // Mask read value
        gen.Orr(arg2, arg2, w0);
        gen.Mov(arg1, w5);
        call(swlWriteWrapper);
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.And(arg1, arg1, ~3);                                 // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in w0

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        gen.moveAndAdd(w2, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w2 again

        gen.Mov(w5, w2);  // Unaligned address, the write wrapper aligns it

        gen.And(w2, w2, 3);                         // edx = low 2 bits of address
        gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);  // Base to mask and shift lookup table in x3
        gen.Ldr(x3, MemOperand(x3, x2, LSL, 3));    // Load the mask and shift from LUT by indexing using the bottom 2
                                                    // bits of the unaligned addr.

        gen.Lsr(arg2, m_gprs[_Rt_].allocatedReg, w3);  // arg2 = shifted $rt
        gen.And(x0, x0, Operand(x3, LSR, 32));         // Mask read value
        gen.Orr(arg2, arg2, w0);                       // arg2 = value to write to memory
        gen.Mov(arg1, w5);
        call(swlWriteWrapper);
    }
}

void DynaRecCPU::recSWR(uint32_t code) {
    // The mask to be applied to $rt (top 32 bits) and the shift to be applied to the read memory value (low 32 bits)
    // Depending on the low 3 bits of the unaligned address
    static const uint64_t MASKS_AND_SHIFTS[4] = {0, 0x000000FF00000008, 0x0000FFFF00000010, 0x00FFFFFF00000018};

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {  // Both previous register value and address are constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t alignedAddress = address & ~3;
        const uint32_t mask = SWR_MASK[address & 3];
        const auto shift = SWR_SHIFT[address & 3];

        gen.Mov(arg1, alignedAddress);  // Address in arg1 (w0)
        call(unalignedStoreReadWrapper);
        gen.andImm(arg2, w0, mask);  // Mask read value
        gen.Mov(w2, m_gprs[_Rt_].val << shift);
        gen.Orr(arg2, arg2, w2);  // Shift $rt and or with read value

        gen.Mov(arg1, address);  // Unaligned address in arg1
        call(swrWriteWrapper);
    } else if (m_gprs[_Rs_].isConst()) {  // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t alignedAddress = address & ~3;
        const uint32_t mask = SWR_MASK[address & 3];
        const auto shift = SWR_SHIFT[address & 3];

        gen.Mov(arg1, alignedAddress);  // Address in arg1 (w0)
        call(unalignedStoreReadWrapper);
        gen.andImm(w0, w0, mask);  // Mask read value

        allocateReg(_Rt_);                                       // Allocate $rt
        gen.Lsl(arg2, m_gprs[_Rt_].allocatedReg, shift);         // arg2 = shifted $rt
        gen.Orr(arg2, arg2, w0);                                 // Or with read value
        gen.Mov(arg1, address);                                  // Unaligned address in arg1
        call(swrWriteWrapper);                                   // Write back
    } else if (m_gprs[_Rt_].isConst()) {                         // Only previous rt value is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.And(arg1, arg1, ~3);                                 // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in w0

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        allocateReg(_Rs_);
        gen.moveAndAdd(w2, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w2 again

        gen.Mov(w5, w2);  // Unaligned address, the write wrapper aligns it

        gen.And(w2, w2, 3);                         // w2 = low 2 bits of address
        gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);  // Base to mask and shift lookup table in x3
        gen.Ldr(x3, MemOperand(x3, x2, LSL, 3));    // Load the mask and shift from LUT by indexing using the bottom 2
                                                    // bits of the unaligned addr.

        gen.Mov(arg2, m_gprs[_Rt_].val);        // arg2 = $rt
        gen.Lsl(arg2, arg2, w3);                // Shift rt value
        gen.And(x0, x0, Operand(x3, LSR, 32));  // Mask read value
        gen.Orr(arg2, arg2, w0);
        gen.Mov(arg1, w5);
        call(swrWriteWrapper);
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.And(arg1, arg1, ~3);                                 // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in w0

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        gen.moveAndAdd(w2, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in w2 again

        gen.Mov(w5, w2);  // Unaligned address, the write wrapper aligns it

        gen.And(w2, w2, 3);                         // w2 = low 2 bits of address
        gen.Mov(x3, (uintptr_t)&MASKS_AND_SHIFTS);  // Base to mask and shift lookup table in x3
        gen.Ldr(x3, MemOperand(x3, x2, LSL, 3));    // Load the mask and shift from LUT by indexing using the bottom 2
                                                    // bits of the unaligned addr.

        gen.Mov(arg2, m_gprs[_Rt_].allocatedReg);  // arg2 = $rt
        gen.Lsl(arg2, arg2, w3);                   // Shift rt value
        gen.And(x0, x0, Operand(x3, LSR, 32));     // Mask read value
        gen.Orr(arg2, arg2, w0);                   // Or with read value
        gen.Mov(arg1, w5);
        call(swrWriteWrapper);
    }
}

void DynaRecCPU::recSYSCALL(uint32_t code) { recException(Exception::Syscall); }

void DynaRecCPU::recXOR(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val ^ m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);
        gen.Eor(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);
        gen.Eor(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs_wb_rd(code);
        gen.Eor(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recXORI(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (_Rs_ == _Rt_) {
        if (m_gprs[_Rs_].isConst()) {
            m_gprs[_Rt_].val ^= _ImmU_;
        } else {
            allocateReg(_Rt_);
            m_gprs[_Rt_].setWriteback(true);
            gen.Eor(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, _ImmU_);
        }
    } else {
        if (m_gprs[_Rs_].isConst()) {
            markConst(_Rt_, m_gprs[_Rs_].val ^ _ImmU_);
        } else {
            alloc_rs_wb_rt(code);
            gen.Eor(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg, _ImmU_);
        }
    }
}

void DynaRecCPU::recException(Exception e) {
    m_pcWrittenBack = true;
    m_stopCompiling = true;
    m_linkedPC = std::nullopt;  // The exception replaces the PC that a branch before us linked to

    loadThisPointer(arg1.X());                                                  // Pointer to this object in arg1
    gen.Mov(arg2, static_cast<std::underlying_type<Exception>::type>(e) << 2);  // Exception type in arg2
    gen.Mov(arg3, (int32_t)m_inDelaySlot);  // Store whether we're in a delay slot in arg3
    gen.Mov(w3, exceptionPC());
    gen.Str(w3, MemOperand(contextPointer, PC_OFFSET));  // PC for exception handler to use

    call(exceptionWrapper);  // Call the exception wrapper
}

void DynaRecCPU::recBREAK(uint32_t code) {
    flushRegs();  // For PCDRV support, we need to flush all registers before handling the exception.
    recException(Exception::Break);
}

#undef BAILZERO
#endif  // DYNAREC_AA64
