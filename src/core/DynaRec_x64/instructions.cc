/***************************************************************************
 *   Copyright (C) 2021 PCSX-Redux authors                                 *
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
#if defined(DYNAREC_X86_64)
#define BAILZERO(x) \
    if (!(x)) {     \
        return;     \
    }

void DynaRecCPU::recUnknown(uint32_t code) {
    PCSX::g_system->message("Unknown instruction for dynarec - address %08x, instruction %08x\n", m_pc, m_regs.code);
    recException(Exception::ReservedInstruction);
}

void DynaRecCPU::recLUI(uint32_t code) {
    BAILZERO(_Rt_);

    maybeCancelDelayedLoad(_Rt_);
    markConst(_Rt_, m_regs.code << 16);
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

        if (_Rt_ == _Rd_) {
            switch (m_gprs[_Rs_].val) {
                case 1:
                    gen.inc(m_gprs[_Rd_].allocatedReg);
                    break;
                case 0xFFFFFFFF:
                    gen.dec(m_gprs[_Rd_].allocatedReg);
                    break;
                default:
                    gen.add(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val);
            }
        } else {
            gen.moveAndAdd(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
        }
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        if (_Rs_ == _Rd_) {
            switch (m_gprs[_Rt_].val) {
                case 1:
                    gen.inc(m_gprs[_Rd_].allocatedReg);
                    break;
                case 0xFFFFFFFF:
                    gen.dec(m_gprs[_Rd_].allocatedReg);
                    break;
                default:
                    gen.add(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            }
        } else {
            gen.moveAndAdd(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
        }
    } else {
        alloc_rt_rs_wb_rd(code);

        if (_Rs_ == _Rd_) {  // Rd+= Rt
            gen.add(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        } else if (_Rt_ == _Rd_) {  // Rd+= Rs
            gen.add(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {  // Rd = Rs + Rt
            gen.lea(m_gprs[_Rd_].allocatedReg,
                    dword[m_gprs[_Rs_].allocatedReg.cvt64() + m_gprs[_Rt_].allocatedReg.cvt64()]);
        }
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
            switch (_Imm_) {
                case 1:
                    gen.inc(m_gprs[_Rt_].allocatedReg);
                    break;
                case -1:
                    gen.dec(m_gprs[_Rt_].allocatedReg);
                    break;
                default:
                    gen.add(m_gprs[_Rt_].allocatedReg, _Imm_);
                    break;
            }
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

        if (_Rs_ == _Rd_) {
            switch (m_gprs[_Rt_].val) {
                case 1:
                    gen.dec(m_gprs[_Rd_].allocatedReg);
                    break;
                case 0xFFFFFFFF:
                    gen.inc(m_gprs[_Rd_].allocatedReg);
                    break;
                default:
                    gen.sub(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            }
        } else {
            gen.moveAndAdd(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, 0u - m_gprs[_Rt_].val);
        }
    } else {
        alloc_rt_rs_wb_rd(code);

        if (_Rs_ == _Rd_) {  // Rd -= Rt
            gen.sub(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        } else {
            gen.mov(eax, m_gprs[_Rs_].allocatedReg);
            gen.sub(eax, m_gprs[_Rt_].allocatedReg);
            gen.mov(m_gprs[_Rd_].allocatedReg, eax);
        }
    }
}

void DynaRecCPU::recSLTI(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst()) {
        markConst(_Rt_, (int32_t)m_gprs[_Rs_].val < _Imm_);
    } else {
        alloc_rs_wb_rt(code);
        gen.setLess(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg, _Imm_);
    }
}

void DynaRecCPU::recSLTIU(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst()) {
        markConst(_Rt_, m_gprs[_Rs_].val < (uint32_t)_Imm_);
    } else {
        alloc_rs_wb_rt(code);

        gen.cmp(m_gprs[_Rs_].allocatedReg, _Imm_);
        gen.setb(m_gprs[_Rt_].allocatedReg.cvt8());
        gen.movzx(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg.cvt8());
    }
}

void DynaRecCPU::recSLTU(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val < m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);

        gen.cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
        gen.seta(m_gprs[_Rd_].allocatedReg.cvt8());
        gen.movzx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg.cvt8());
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        gen.cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
        gen.setb(m_gprs[_Rd_].allocatedReg.cvt8());
        gen.movzx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg.cvt8());
    } else {
        alloc_rt_rs_wb_rd(code);

        gen.cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        gen.setb(m_gprs[_Rd_].allocatedReg.cvt8());
        gen.movzx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg.cvt8());
    }
}

void DynaRecCPU::recSLT(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, (int32_t)m_gprs[_Rs_].val < (int32_t)m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);

        gen.cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
        gen.setg(m_gprs[_Rd_].allocatedReg.cvt8());
        gen.movzx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg.cvt8());
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        gen.setLess(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs_wb_rd(code);

        gen.cmp(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        gen.setl(m_gprs[_Rd_].allocatedReg.cvt8());
        gen.movzx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg.cvt8());
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

        if (_Rd_ == _Rs_) {
            gen.and_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        } else if (_Rd_ == _Rt_) {
            gen.and_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            gen.and_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }
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

void DynaRecCPU::recNOR(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, ~(m_gprs[_Rs_].val | m_gprs[_Rt_].val));
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);

        gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        gen.orImm(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val);
        gen.not_(m_gprs[_Rd_].allocatedReg);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        gen.orImm(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
        gen.not_(m_gprs[_Rd_].allocatedReg);
    } else {
        alloc_rt_rs_wb_rd(code);

        if (_Rd_ == _Rs_) {
            gen.or_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        } else if (_Rd_ == _Rt_) {
            gen.or_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            gen.or_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }

        gen.not_(m_gprs[_Rd_].allocatedReg);
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

        if (_Rd_ == _Rs_) {
            gen.or_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        } else if (_Rd_ == _Rt_) {
            gen.or_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            gen.or_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }
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
            gen.or_(m_gprs[_Rt_].allocatedReg, _ImmU_);
        }
    } else {
        if (m_gprs[_Rs_].isConst()) {
            markConst(_Rt_, m_gprs[_Rs_].val | _ImmU_);
        } else {
            alloc_rs_wb_rt(code);
            gen.mov(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
            gen.orImm(m_gprs[_Rt_].allocatedReg, _ImmU_);
        }
    }
}

void DynaRecCPU::recXOR(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rs_].val ^ m_gprs[_Rt_].val);
    } else if (m_gprs[_Rs_].isConst()) {
        alloc_rt_wb_rd(code);

        gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        gen.xor_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        gen.xor_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs_wb_rd(code);

        if (_Rd_ == _Rs_) {
            gen.xor_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        } else if (_Rd_ == _Rt_) {
            gen.xor_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            gen.xor_(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }
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
            gen.xor_(m_gprs[_Rt_].allocatedReg, _ImmU_);
        }
    } else {
        if (m_gprs[_Rs_].isConst()) {
            markConst(_Rt_, m_gprs[_Rs_].val ^ _ImmU_);
        } else {
            alloc_rs_wb_rt(code);
            gen.mov(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
            if (_ImmU_) {
                gen.xor_(m_gprs[_Rt_].allocatedReg, _ImmU_);
            }
        }
    }
}

void DynaRecCPU::recSLL(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val << _Sa_);
    } else {
        alloc_rt_wb_rd(code);
        gen.shlImm(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, _Sa_);
    }
}

// Note: This code doesn't mask the shift amount to 32 bits, as x86 processors do that implicitly
void DynaRecCPU::recSLLV(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val << (m_gprs[_Rs_].val & 0x1F));
    } else if (m_gprs[_Rs_].isConst()) {
        if (_Rt_ == _Rd_) {
            allocateReg(_Rd_);
            m_gprs[_Rd_].setWriteback(true);
            gen.shl(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        } else {
            alloc_rt_wb_rd(code);
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            gen.shl(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        }
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        if (gen.hasBMI2 && (_Rd_ != _Rs_)) {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            gen.shlx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }

        else {
            gen.mov(ecx, m_gprs[_Rs_].allocatedReg);  // Shift amount in ecx
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            gen.shl(m_gprs[_Rd_].allocatedReg, cl);
        }
    } else {
        alloc_rt_rs_wb_rd(code);

        if (gen.hasBMI2) {
            gen.shlx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(ecx, m_gprs[_Rs_].allocatedReg);  // Shift amount in ecx
            if (_Rt_ != _Rd_) {
                gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            }
            gen.shl(m_gprs[_Rd_].allocatedReg, cl);
        }
    }
}

void DynaRecCPU::recSRA(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, (int32_t)m_gprs[_Rt_].val >> _Sa_);
    } else {
        alloc_rt_wb_rd(code);

        if (_Rd_ != _Rt_) {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        }

        if (_Sa_) {
            gen.sar(m_gprs[_Rd_].allocatedReg, _Sa_);
        }
    }
}

// Note: This code doesn't mask the shift amount to 32 bits, as x86 processors do that implicitly
void DynaRecCPU::recSRAV(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, (int32_t)m_gprs[_Rt_].val >> (m_gprs[_Rs_].val & 0x1F));
    } else if (m_gprs[_Rs_].isConst()) {
        if (_Rt_ == _Rd_) {
            allocateReg(_Rd_);
            m_gprs[_Rd_].setWriteback(true);
            gen.sar(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        } else {
            alloc_rt_wb_rd(code);
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            gen.sar(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        }
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        if (gen.hasBMI2 && (_Rd_ != _Rs_)) {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            gen.sarx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(ecx, m_gprs[_Rs_].allocatedReg);  // Shift amount in ecx
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            gen.sar(m_gprs[_Rd_].allocatedReg, cl);
        }
    } else {
        alloc_rt_rs_wb_rd(code);

        if (gen.hasBMI2) {
            gen.sarx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(ecx, m_gprs[_Rs_].allocatedReg);  // Shift amount in ecx
            if (_Rt_ != _Rd_) {
                gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            }
            gen.sar(m_gprs[_Rd_].allocatedReg, cl);
        }
    }
}

void DynaRecCPU::recSRL(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val >> _Sa_);
    } else {
        alloc_rt_wb_rd(code);

        if (_Rd_ != _Rt_) {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
        }

        if (_Sa_) {
            gen.shr(m_gprs[_Rd_].allocatedReg, _Sa_);
        }
    }
}
// Note: This code doesn't mask the shift amount to 32 bits, as x86 processors do that implicitly
void DynaRecCPU::recSRLV(uint32_t code) {
    BAILZERO(_Rd_);
    maybeCancelDelayedLoad(_Rd_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {
        markConst(_Rd_, m_gprs[_Rt_].val >> (m_gprs[_Rs_].val & 0x1F));
    } else if (m_gprs[_Rs_].isConst()) {
        if (_Rt_ == _Rd_) {
            allocateReg(_Rd_);
            m_gprs[_Rd_].setWriteback(true);
            gen.shr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        } else {
            alloc_rt_wb_rd(code);
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            gen.shr(m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].val & 0x1F);
        }
    } else if (m_gprs[_Rt_].isConst()) {
        alloc_rs_wb_rd(code);

        if (gen.hasBMI2 && (_Rd_ != _Rs_)) {
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            gen.shrx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rd_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        }

        else {
            gen.mov(ecx, m_gprs[_Rs_].allocatedReg);  // Shift amount in ecx
            gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].val);
            gen.shr(m_gprs[_Rd_].allocatedReg, cl);
        }
    } else {
        alloc_rt_rs_wb_rd(code);

        if (gen.hasBMI2) {
            gen.shrx(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
        } else {
            gen.mov(ecx, m_gprs[_Rs_].allocatedReg);  // Shift amount in ecx
            if (_Rt_ != _Rd_) {
                gen.mov(m_gprs[_Rd_].allocatedReg, m_gprs[_Rt_].allocatedReg);
            }
            gen.shr(m_gprs[_Rd_].allocatedReg, cl);
        }
    }
}

// Waits for the previous mul/div to complete. Block cycles are only added to
// the cycle counter at the end of a block, so the current cycle is computed
// from the position of this instruction in the block. Leaves it in rax.
void DynaRecCPU::emitMuldivStall() {
    Label noStall;
    gen.mov(rax, qword[contextPointer + CYCLE_OFFSET]);
    gen.add(rax, m_instructionCount * PCSX::Emulator::BIAS);
    gen.mov(rcx, qword[contextPointer + MULDIV_READY_OFFSET]);
    gen.cmp(rax, rcx);
    gen.jae(noStall);
    gen.sub(rcx, rax);
    gen.add(qword[contextPointer + CYCLE_OFFSET], rcx);
    gen.add(rax, rcx);
    gen.L(noStall);
}

// Leaves the cycle at which the current instruction issues in rax.
void DynaRecCPU::emitMuldivNow() {
    gen.mov(rax, qword[contextPointer + CYCLE_OFFSET]);
    gen.add(rax, m_instructionCount * PCSX::Emulator::BIAS);
}

// A mul/div issued while the unit is busy replaces the running operation, so
// there is no stall here.
void DynaRecCPU::emitMuldivStart(uint32_t latency) {
    emitMuldivNow();
    gen.add(rax, latency + PCSX::Emulator::BIAS);
    gen.mov(qword[contextPointer + MULDIV_READY_OFFSET], rax);
}

void DynaRecCPU::emitMultStart(uint32_t code, bool isSigned) {
    if (m_gprs[_Rs_].isConst()) {
        emitMuldivStart(multLatency(m_gprs[_Rs_].val, isSigned));
        return;
    }

    Label done;
    allocateReg(_Rs_);
    emitMuldivNow();
    gen.mov(edx, m_gprs[_Rs_].allocatedReg);
    if (isSigned) {
        gen.mov(ecx, edx);
        gen.sar(ecx, 31);
        gen.xor_(edx, ecx);
    }
    gen.cmp(edx, 0x800);
    gen.mov(ecx, 6 + PCSX::Emulator::BIAS);
    gen.jb(done);
    gen.cmp(edx, 0x100000);
    gen.mov(ecx, 9 + PCSX::Emulator::BIAS);
    gen.jb(done);
    gen.mov(ecx, 13 + PCSX::Emulator::BIAS);
    gen.L(done);
    gen.add(rax, rcx);
    gen.mov(qword[contextPointer + MULDIV_READY_OFFSET], rax);
}

void DynaRecCPU::recMULT(uint32_t code) {
    emitMultStart(code, true);
    if ((m_gprs[_Rs_].isConst() && m_gprs[_Rs_].val == 0) || (m_gprs[_Rt_].isConst() && m_gprs[_Rt_].val == 0)) {
        gen.mov(qword[contextPointer + LO_OFFSET], 0);  // Set both LO and HI to 0 in a single 64-bit write
        return;
    }

    if (m_gprs[_Rs_].isConst()) {
        if (m_gprs[_Rt_].isConst()) {
            const uint64_t result = (int64_t)(int32_t)m_gprs[_Rt_].val * (int64_t)(int32_t)m_gprs[_Rs_].val;
            if ((int64_t)result == (int32_t)result) {
                // A 64-bit store sign-extends its 32-bit immediate, which writes HI in the same go.
                gen.mov(qword[contextPointer + LO_OFFSET], result);
            } else {
                gen.mov(dword[contextPointer + LO_OFFSET], (uint32_t)result);
                gen.mov(dword[contextPointer + HI_OFFSET], (uint32_t)(result >> 32));
            }
            return;
        } else {
            allocateReg(_Rt_);
            gen.movsxd(rax, m_gprs[_Rt_].allocatedReg);
            gen.imul(rax, rax, m_gprs[_Rs_].val);
        }
    } else {
        if (m_gprs[_Rt_].isConst()) {
            allocateReg(_Rs_);
            gen.movsxd(rax, m_gprs[_Rs_].allocatedReg);
            gen.imul(rax, rax, m_gprs[_Rt_].val);
        } else {
            alloc_rt_rs(code);
            gen.movsxd(rax, m_gprs[_Rs_].allocatedReg);
            gen.movsxd(rcx, m_gprs[_Rt_].allocatedReg);
            gen.imul(rax, rcx);
        }
    }

    // Write 64-bit result to lo and hi at the same time
    gen.mov(qword[contextPointer + LO_OFFSET], rax);
}

// TODO: Add a static_assert that makes sure address_of_hi == address_of_lo + 4
void DynaRecCPU::recMULTU(uint32_t code) {
    emitMultStart(code, false);
    if ((m_gprs[_Rs_].isConst() && m_gprs[_Rs_].val == 0) || (m_gprs[_Rt_].isConst() && m_gprs[_Rt_].val == 0)) {
        gen.mov(qword[contextPointer + LO_OFFSET], 0);  // Set both LO and HI to 0 in a single 64-bit write
        return;
    }

    if (m_gprs[_Rs_].isConst()) {
        gen.mov(eax, m_gprs[_Rs_].val);

        if (m_gprs[_Rt_].isConst()) {
            gen.mov(edx, m_gprs[_Rt_].val);
            gen.mul(edx);
        } else {
            allocateReg(_Rt_);
            gen.mul(m_gprs[_Rt_].allocatedReg);
        }
    } else {
        if (m_gprs[_Rt_].isConst()) {
            allocateReg(_Rs_);
            gen.mov(eax, m_gprs[_Rt_].val);
            gen.mul(m_gprs[_Rs_].allocatedReg);
        } else {
            alloc_rt_rs(code);
            gen.mov(eax, m_gprs[_Rs_].allocatedReg);
            gen.mul(m_gprs[_Rt_].allocatedReg);
        }
    }

    gen.mov(dword[contextPointer + LO_OFFSET], eax);
    gen.mov(dword[contextPointer + HI_OFFSET], edx);
}

template <int size, bool signExtend>
void DynaRecCPU::recompileLoadWithDelay(uint32_t code, LoadDelayDependencyType type) {
    if (m_gprs[_Rs_].isConst()) {
        const uint32_t addr = m_gprs[_Rs_].val + _Imm_;
        gen.mov(arg2, addr);
        if (size == 32 && usesArgumentRegisters(addr, false)) syncArgumentRegisters();
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg2, m_gprs[_Rs_].allocatedReg, _Imm_);
        if (size == 32) syncArgumentRegisters();
    }

    switch (size) {
        case 8:
            callMemoryFunc(&PCSX::Memory::read8);
            break;
        case 16:
            callMemoryFunc(&PCSX::Memory::read16);
            break;
        case 32:
            callMemoryFunc(&PCSX::Memory::read32);
            break;
    }

    if (_Rt_) {
        // Cancel any pending load to the same register, as this load overrides it
        maybeCancelDelayedLoad(_Rt_);
        m_delayedLoadInfo[m_currentDelayedLoad].active = true;

        switch (size) {
            case 8:
                signExtend ? gen.movsx(eax, al) : gen.movzx(eax, al);
                break;
            case 16:
                signExtend ? gen.movsx(eax, ax) : gen.movzx(eax, ax);
                break;
        }

        if (type == LoadDelayDependencyType::DependencyAcrossBlocks) {
            const auto delayedLoadValueOffset = (uintptr_t)&m_runtimeLoadDelay.value - (uintptr_t)this;
            const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
            const auto indexOffset = (uintptr_t)&m_runtimeLoadDelay.index - (uintptr_t)this;
            gen.mov(dword[contextPointer + delayedLoadValueOffset], eax);
            gen.mov(Xbyak::util::byte[contextPointer + isActiveOffset], 1);
            gen.mov(dword[contextPointer + indexOffset], _Rt_);
        } else {
            auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
            const auto delayedLoadValueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;
            delayedLoad.index = _Rt_;
            gen.mov(dword[contextPointer + delayedLoadValueOffset], eax);
        }
    }
}

template <int size, bool signExtend>
void DynaRecCPU::recompileLoad(uint32_t code) {
    static_assert(size == 8 || size == 16 || size == 32);

    const auto loadDelayDependency = getLoadDelayDependencyType(_Rt_);
    if (loadDelayDependency != LoadDelayDependencyType::NoDependency) {
        recompileLoadWithDelay<size, signExtend>(code, loadDelayDependency);
        return;
    }

    // If we won't emulate the load delay, make sure to cancel any pending loads that might trample the value
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst()) {  // Store the address in arg2
        const uint32_t addr = m_gprs[_Rs_].val + _Imm_;
        const auto pointer = PCSX::g_emulator->m_mem->pointerRead(addr);

        if (pointer != nullptr && (_Rt_) != 0) {
            allocateRegWithoutLoad(_Rt_);
            m_gprs[_Rt_].setWriteback(true);
            load<size, signExtend>(m_gprs[_Rt_].allocatedReg, pointer);
            return;
        }

        gen.mov(arg2, addr);
        if (size == 32 && usesArgumentRegisters(addr, false)) syncArgumentRegisters();
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg2, m_gprs[_Rs_].allocatedReg, _Imm_);
        if (size == 32) syncArgumentRegisters();
    }

    switch (size) {
        case 8:
            callMemoryFunc(&PCSX::Memory::read8);
            break;
        case 16:
            callMemoryFunc(&PCSX::Memory::read16);
            break;
        case 32:
            callMemoryFunc(&PCSX::Memory::read32);
            break;
    }

    if (_Rt_) {
        allocateRegWithoutLoad(_Rt_);  // Allocate $rt after calling the read function, otherwise call() might flush it.
        m_gprs[_Rt_].setWriteback(true);

        switch (size) {
            case 8:
                signExtend ? gen.movsx(m_gprs[_Rt_].allocatedReg, al) : gen.movzx(m_gprs[_Rt_].allocatedReg, al);
                break;
            case 16:
                signExtend ? gen.movsx(m_gprs[_Rt_].allocatedReg, ax) : gen.movzx(m_gprs[_Rt_].allocatedReg, ax);
                break;
            case 32:
                gen.mov(m_gprs[_Rt_].allocatedReg, eax);
                break;
        }
    }
}

void DynaRecCPU::recLB(uint32_t code) { recompileLoad<8, true>(code); }
void DynaRecCPU::recLBU(uint32_t code) { recompileLoad<8, false>(code); }
void DynaRecCPU::recLH(uint32_t code) { recompileLoad<16, true>(code); }
void DynaRecCPU::recLHU(uint32_t code) { recompileLoad<16, false>(code); }
void DynaRecCPU::recLW(uint32_t code) { recompileLoad<32, true>(code); }

void DynaRecCPU::recLWL(uint32_t code) {
    if (_Rt_ == 0) {  // If $rt == 0, just execute the read in case it has side-effects, then return
        if (m_gprs[_Rs_].isConst()) {
            const uint32_t address = m_gprs[_Rs_].val + _Imm_;
            gen.mov(arg1, address);  // Address in arg1
        } else {
            allocateReg(_Rs_);                                       // Allocate address reg
            gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        }

        call(lwlReadWrapper);  // Read from the aligned address
        return;
    }

    // The mask to be applied to $rt (top 32 bits) and the shift to be applied to the read memory value (low 32 bits)
    // Depending on the low 3 bits of the unaligned address
    static const uint64_t MASKS_AND_SHIFTS[4] = {0x00FFFFFF00000018, 0x0000FFFF00000010, 0x000000FF00000008, 0};
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {  // Both previous register value and address are constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWL_MASK[address & 3];
        const auto shift = LWL_SHIFT[address & 3];
        const uint32_t previousValue = m_gprs[_Rt_].val;

        gen.mov(arg1, address);  // Address in arg1
        call(lwlReadWrapper);

        allocateReg(_Rt_);  // Allocate $rt with writeback
        m_gprs[_Rt_].setWriteback(true);
        gen.mov(m_gprs[_Rt_].allocatedReg, previousValue & mask);  // Mask the previous $rt value
        gen.shlImm(eax, shift);                                    // Shift the value read from the aligned address
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);                   // Or $rt with shifted value
    } else if (m_gprs[_Rs_].isConst()) {                           // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWL_MASK[address & 3];
        const auto shift = LWL_SHIFT[address & 3];

        gen.mov(arg1, address);  // Address in arg1
        call(lwlReadWrapper);

        allocateReg(_Rt_);  // Allocate $rt with writeback
        m_gprs[_Rt_].setWriteback(true);
        gen.andImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, mask);  // Mask the previous $rt value
        gen.shlImm(eax, shift);                   // Shift the value read from the aligned address
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);  // Or $rt with shifted value
    } else if (m_gprs[_Rt_].isConst()) {          // Only previous rt value is constant
        const uint32_t previousValue = m_gprs[_Rt_].val;

        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwlReadWrapper);                                    // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        m_gprs[_Rt_].setWriteback(true);

        gen.mov(m_gprs[_Rt_].allocatedReg, previousValue);      // Flush constant value in $rt
        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again
        gen.and_(edx, 3);                                       // Get the low 2 bits
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);            // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.
        gen.shl(eax, cl);  // Shift the read value by the shift amount (This relies on x86 masking shift behavior)
        gen.shr(rcx, 32);  // ecx = mask now
        gen.and_(m_gprs[_Rt_].allocatedReg, ecx);                // Mask previous $rt value
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);                 // Merge with newly read value
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwlReadWrapper);                                    // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        m_gprs[_Rt_].setWriteback(true);

        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again
        gen.and_(edx, 3);                                       // Get the low 2 bits
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);            // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.
        gen.shl(eax, cl);  // Shift the read value by the shift amount (This relies on x86 masking shift behavior)
        gen.shr(rcx, 32);  // ecx = mask now
        gen.and_(m_gprs[_Rt_].allocatedReg, ecx);  // Mask previous $rt value
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);   // Merge with newly read value
    }
}

void DynaRecCPU::recLWR(uint32_t code) {
    if (_Rt_ == 0) {  // If $rt == 0, just execute the read in case it has side-effects, then return
        if (m_gprs[_Rs_].isConst()) {
            const uint32_t address = m_gprs[_Rs_].val + _Imm_;
            gen.mov(arg1, address);  // Address in arg1
        } else {
            allocateReg(_Rs_);                                       // Allocate address reg
            gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        }

        call(lwrReadWrapper);  // Read from the aligned address
        return;
    }

    // The mask to be applied to $rt (top 32 bits) and the shift to be applied to the read memory value (low 32 bits)
    // Depending on the low 3 bits of the unaligned address
    static const uint64_t MASKS_AND_SHIFTS[4] = {0, 0xFF00000000000008, 0xFFFF000000000010, 0xFFFFFF0000000018};
    maybeCancelDelayedLoad(_Rt_);

    if (m_gprs[_Rs_].isConst() && m_gprs[_Rt_].isConst()) {  // Both previous register value and address are constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWR_MASK[address & 3];
        const auto shift = LWR_SHIFT[address & 3];
        const uint32_t previousValue = m_gprs[_Rt_].val;

        gen.mov(arg1, address);  // Address in arg1
        call(lwrReadWrapper);

        allocateReg(_Rt_);  // Allocate $rt with writeback
        m_gprs[_Rt_].setWriteback(true);
        gen.mov(m_gprs[_Rt_].allocatedReg, previousValue & mask);  // Mask the previous $rt value
        gen.shr(eax, shift);                                       // Shift the value read from the aligned address
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);                   // Or $rt with shifted value
    } else if (m_gprs[_Rs_].isConst()) {                           // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t mask = LWR_MASK[address & 3];
        const auto shift = LWR_SHIFT[address & 3];

        gen.mov(arg1, address);  // Address in arg1
        call(lwrReadWrapper);

        allocateReg(_Rt_);  // Allocate $rt with writeback
        m_gprs[_Rt_].setWriteback(true);
        gen.andImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rt_].allocatedReg, mask);  // Mask the previous $rt value
        gen.shr(eax, shift);                      // Shift the value read from the aligned address
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);  // Or $rt with shifted value
    } else if (m_gprs[_Rt_].isConst()) {          // Only previous rt value is constant
        const uint32_t previousValue = m_gprs[_Rt_].val;

        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwrReadWrapper);                                    // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        m_gprs[_Rt_].setWriteback(true);

        gen.mov(m_gprs[_Rt_].allocatedReg, previousValue);      // Flush constant value in $rt
        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again
        gen.and_(edx, 3);                                       // Get the low 2 bits
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);            // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.
        gen.shr(eax, cl);  // Shift the read value by the shift amount (This relies on x86 masking shift behavior)
        gen.shr(rcx, 32);  // ecx = mask now
        gen.and_(m_gprs[_Rt_].allocatedReg, ecx);                // Mask previous $rt value
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);                 // Merge with newly read value
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        call(lwrReadWrapper);                                    // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        m_gprs[_Rt_].setWriteback(true);

        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again
        gen.and_(edx, 3);                                       // Get the low 2 bits
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);            // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.
        gen.shr(eax, cl);  // Shift the read value by the shift amount (This relies on x86 masking shift behavior)
        gen.shr(rcx, 32);  // ecx = mask now
        gen.and_(m_gprs[_Rt_].allocatedReg, ecx);  // Mask previous $rt value
        gen.or_(m_gprs[_Rt_].allocatedReg, eax);   // Merge with newly read value
    }
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
                store<8>(m_gprs[_Rt_].allocatedReg.cvt8(), pointer);
            }

            return;
        }

        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg3
            gen.moveImm(arg3, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.mov(arg3, m_gprs[_Rt_].allocatedReg);
        }

        gen.mov(arg2, addr);  // Address to write to in arg2 TODO: Optimize
        callMemoryFunc(&PCSX::Memory::write8);
    }

    else {
        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg3
            gen.moveImm(arg3, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.mov(arg3, m_gprs[_Rt_].allocatedReg);
        }

        allocateReg(_Rs_);
        gen.moveAndAdd(arg2, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address to write to in arg2   TODO: Optimize
        callMemoryFunc(&PCSX::Memory::write8);
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
                store<16>(m_gprs[_Rt_].allocatedReg.cvt16(), pointer);
            }

            return;
        }

        else if (addr == 0x1f801070) {  // I_STAT
            gen.mov(rax, (uint64_t)&PCSX::g_emulator->m_mem->m_hard[0x1070]);
            if (m_gprs[_Rt_].isConst()) {
                // Doing an AND directly seems to make Xbyak throw an exception due to the immediate being too big.
                // Seems to be an xbyak bug? Affects Fromage, and potentially other titles.
                gen.mov(cx, m_gprs[_Rt_].val & 0xFFFF);
                gen.and_(word[rax], cx);
            } else {
                allocateReg(_Rt_);
                gen.and_(word[rax], m_gprs[_Rt_].allocatedReg.cvt16());
            }

            return;
        }

        else if (addr >= 0x1f801c00 && addr < 0x1f801e00) {  // SPU registers
            gen.mov(arg1, addr);
            if (m_gprs[_Rt_].isConst()) {
                gen.moveImm(arg2, m_gprs[_Rt_].val & 0xFFFF);
            } else {
                allocateReg(_Rt_);
                gen.mov(arg2, m_gprs[_Rt_].allocatedReg);
            }

            call(SPU_writeRegisterWrapper);
            return;
        }

        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg3
            gen.moveImm(arg3, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.mov(arg3, m_gprs[_Rt_].allocatedReg);
        }

        gen.mov(arg2, addr);  // Address to write to in arg2   TODO: Optimize
        callMemoryFunc(&PCSX::Memory::write16);
    }

    else {
        if (m_gprs[_Rt_].isConst()) {  // Full 32-bit value to write in arg3
            gen.moveImm(arg3, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.mov(arg3, m_gprs[_Rt_].allocatedReg);
        }

        allocateReg(_Rs_);
        gen.moveAndAdd(arg2, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address to write to in arg2   TODO: Optimize
        callMemoryFunc(&PCSX::Memory::write16);
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

        if (m_gprs[_Rt_].isConst()) {  // Value to write in arg3
            gen.moveImm(arg3, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.mov(arg3, m_gprs[_Rt_].allocatedReg);
        }

        gen.mov(arg2, addr);  // Address to write to in arg2   TODO: Optimize
        if (usesArgumentRegisters(addr, true)) syncArgumentRegisters();
        callMemoryFunc(&PCSX::Memory::write32);
    }

    else {
        if (m_gprs[_Rt_].isConst()) {  // Value to write in arg3
            gen.moveImm(arg3, m_gprs[_Rt_].val);
        } else {
            allocateReg(_Rt_);
            gen.mov(arg3, m_gprs[_Rt_].allocatedReg);
        }

        allocateReg(_Rs_);
        gen.moveAndAdd(arg2, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address to write to in arg2   TODO: Optimize
        syncArgumentRegisters();
        callMemoryFunc(&PCSX::Memory::write32);
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

        gen.mov(arg1, alignedAddress);  // Address in arg1
        call(unalignedStoreReadWrapper);
        gen.andImm(eax, eax, mask);               // Mask read value
        gen.or_(eax, m_gprs[_Rt_].val >> shift);  // Shift $rt and or with read value

        gen.mov(arg1, address);  // Unaligned address in arg1
        gen.mov(arg2, eax);      // Address to write to in arg2
        call(swlWriteWrapper);
    } else if (m_gprs[_Rs_].isConst()) {  // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t alignedAddress = address & ~3;
        const uint32_t mask = SWL_MASK[address & 3];
        const auto shift = SWL_SHIFT[address & 3];

        gen.mov(arg1, alignedAddress);  // Address in arg1
        call(unalignedStoreReadWrapper);
        gen.andImm(eax, eax, mask);  // Mask read value

        gen.mov(arg1, address);                                  // Unaligned address in arg1
        allocateReg(_Rt_);                                       // Allocate $rt
        gen.mov(arg2, m_gprs[_Rt_].allocatedReg);                // Move rt to arg2
        gen.shr(arg2, shift);                                    // Shift rt value
        gen.or_(arg2, eax);                                      // Or with read value
        call(swlWriteWrapper);                                   // Write back
    } else if (m_gprs[_Rt_].isConst()) {                         // Only previous rt value is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.and_(arg1, ~3);                                      // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        allocateReg(_Rs_);
        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again

        if constexpr (isWindows()) {  // On Windows, we'll have to corrupt arg1 (ecx) to do a variable-amount shift
            gen.mov(arg4, edx);       // So we use arg4 as a temporary (r9d)
        } else {
            gen.mov(arg1, edx);  // On System V, we can just use arg1 (edi)
        }

        gen.and_(edx, 3);                             // edx = low 2 bits of address
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);  // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.

        gen.mov(arg2, m_gprs[_Rt_].val);  // arg2 = $rt
        gen.shr(arg2, cl);                // Shift rt value
        gen.shr(rcx, 32);                 // rcx = mask now
        gen.and_(eax, ecx);               // Mask read value
        gen.or_(arg2, eax);

        if constexpr (isWindows()) {
            gen.mov(arg1, arg4);
        }

        call(swlWriteWrapper);
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.and_(arg1, ~3);                                      // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again

        if constexpr (isWindows()) {  // On Windows, we'll have to corrupt arg1 (ecx) to do a variable-amount shift
            gen.mov(arg4, edx);       // So we use arg4 as a temporary (r9d)
        } else {
            gen.mov(arg1, edx);  // On System V, we can just use arg1 (edi)
        }

        gen.and_(edx, 3);                             // edx = low 2 bits of address
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);  // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.

        gen.mov(arg2, m_gprs[_Rt_].allocatedReg);  // arg2 = $rt
        gen.shr(arg2, cl);                         // Shift rt value
        gen.shr(rcx, 32);                          // rcx = mask now
        gen.and_(eax, ecx);                        // Mask read value
        gen.or_(arg2, eax);

        if constexpr (isWindows()) {
            gen.mov(arg1, arg4);
        }

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

        gen.mov(arg1, alignedAddress);  // Address in arg1
        call(unalignedStoreReadWrapper);
        gen.andImm(eax, eax, mask);               // Mask read value
        gen.or_(eax, m_gprs[_Rt_].val << shift);  // Shift $rt and or with read value

        gen.mov(arg1, address);  // Unaligned address in arg1
        gen.mov(arg2, eax);      // Address to write to in arg2
        call(swrWriteWrapper);
    } else if (m_gprs[_Rs_].isConst()) {  // Only address is constant
        const uint32_t address = m_gprs[_Rs_].val + _Imm_;
        const uint32_t alignedAddress = address & ~3;
        const uint32_t mask = SWR_MASK[address & 3];
        const auto shift = SWR_SHIFT[address & 3];

        gen.mov(arg1, alignedAddress);  // Address in arg1
        call(unalignedStoreReadWrapper);
        gen.andImm(eax, eax, mask);  // Mask read value

        gen.mov(arg1, address);                                  // Unaligned address in arg1
        allocateReg(_Rt_);                                       // Allocate $rt
        gen.mov(arg2, m_gprs[_Rt_].allocatedReg);                // Move rt to arg2
        gen.shlImm(arg2, shift);                                 // Shift rt value
        gen.or_(arg2, eax);                                      // Or with read value
        call(swrWriteWrapper);                                   // Write back
    } else if (m_gprs[_Rt_].isConst()) {                         // Only previous rt value is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.and_(arg1, ~3);                                      // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        allocateReg(_Rs_);
        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again

        if constexpr (isWindows()) {  // On Windows, we'll have to corrupt arg1 (ecx) to do a variable-amount shift
            gen.mov(arg4, edx);       // So we use arg4 as a temporary (r9d)
        } else {
            gen.mov(arg1, edx);  // On System V, we can just use arg1 (edi)
        }

        gen.and_(edx, 3);                             // edx = low 2 bits of address
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);  // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.

        gen.mov(arg2, m_gprs[_Rt_].val);  // edx = $rt
        gen.shl(arg2, cl);                // Shift rt value
        gen.shr(rcx, 32);                 // rcx = mask now
        gen.and_(eax, ecx);               // Mask read value
        gen.or_(arg2, eax);

        if constexpr (isWindows()) {
            gen.mov(arg1, arg4);
        }

        call(swrWriteWrapper);
    } else {                                                     // Nothing is constant
        allocateReg(_Rs_);                                       // Allocate address reg
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in arg1
        gen.and_(arg1, ~3);                                      // Force align it
        call(unalignedStoreReadWrapper);                         // Read from the aligned address, result in eax

        // The call might have flushed $rs, so we need to allocate it again, and also allocate $rt
        alloc_rt_rs(code);
        gen.moveAndAdd(edx, m_gprs[_Rs_].allocatedReg, _Imm_);  // Address in edx again

        if constexpr (isWindows()) {  // On Windows, we'll have to corrupt arg1 (ecx) to do a variable-amount shift
            gen.mov(arg4, edx);       // So we use arg4 as a temporary (r9d)
        } else {
            gen.mov(arg1, edx);  // On System V, we can just use arg1 (edi)
        }

        gen.and_(edx, 3);                             // edx = low 2 bits of address
        gen.lea(rcx, qword[rip + MASKS_AND_SHIFTS]);  // Base to mask and shift lookup table in rcx
        gen.mov(rcx, qword[rcx + rdx * 8]);  // Load the mask and shift from LUT by indexing using the bottom 2 bits of
                                             // the unaligned addr.

        gen.mov(arg2, m_gprs[_Rt_].allocatedReg);  // edx = $rt
        gen.shl(arg2, cl);                         // Shift rt value
        gen.shr(rcx, 32);                          // rcx = mask now
        gen.and_(eax, ecx);                        // Mask read value
        gen.or_(arg2, eax);

        if constexpr (isWindows()) {
            gen.mov(arg1, arg4);
        }

        call(swrWriteWrapper);
    }
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
    gen.test(dword[contextPointer + COP0_OFFSET(12)], 0x10000000 << z);
    gen.jnz(usable, Xbyak::CodeGenerator::T_NEAR);

    // The load started by the previous instruction still lands
    if (m_firstInstruction && m_fullLoadDelayEmulation) {
        Label noDelayedLoad;
        const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
        gen.cmp(Xbyak::util::byte[contextPointer + isActiveOffset], 0);
        gen.je(noDelayedLoad);
        gen.call((void*)m_loadDelayHandler);
        gen.L(noDelayedLoad);
    }
    const auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad ^ 1];
    if (delayedLoad.active && delayedLoad.index != 0) {
        const auto delayedValueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;
        gen.mov(eax, dword[contextPointer + delayedValueOffset]);
        gen.mov(dword[contextPointer + GPR_OFFSET(delayedLoad.index)], eax);
    }
    emitGTEFlushQueue(true);

    loadThisPointer(arg1.cvt64());
    gen.moveImm(arg2, (static_cast<uint32_t>(Exception::CoprocessorUnusable) << 2) | (z << 28));
    gen.moveImm(arg3, (int32_t)m_inDelaySlot);
    gen.mov(dword[contextPointer + PC_OFFSET], exceptionPC());
    call(exceptionWrapper);
    if constexpr (ENABLE_PROFILER) {
        endProfiling();
    }
    gen.add(qword[contextPointer + CYCLE_OFFSET],
            (m_instructionCount + m_romInstructionCount * PCSX::Emulator::ROM_EXTRA_BIAS) * PCSX::Emulator::BIAS);
    gen.jmp((void*)m_returnFromBlock);

    gen.L(usable);
    m_nextIsDelaySlot = true;

    if ((_Rt_ & 1) || target == m_pc + 4) {
        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;
    gen.mov(dword[contextPointer + PC_OFFSET], target);
    m_linkedPC = target;
}

void DynaRecCPU::recMFC0(uint32_t code) {
    BAILZERO(_Rt_);
    maybeCancelDelayedLoad(_Rt_);
    allocateRegWithoutLoad(_Rt_);
    m_gprs[_Rt_].setWriteback(true);

    gen.mov(m_gprs[_Rt_].allocatedReg, dword[contextPointer + COP0_OFFSET(_Rd_)]);
}

// TODO: Handle all COP0 register writes properly. Don't treat read-only field as writeable!
void DynaRecCPU::recMTC0(uint32_t code) {
    if (m_gprs[_Rt_].isConst()) {
        if (_Rd_ == 13) {
            gen.mov(dword[contextPointer + COP0_OFFSET(_Rd_)], m_gprs[_Rt_].val & ~0xFC00);
        } else if (_Rd_ != 6 && _Rd_ != 14 && _Rd_ != 15) {  // Don't write to JUMPDEST, EPC or PRID
            gen.mov(dword[contextPointer + COP0_OFFSET(_Rd_)], m_gprs[_Rt_].val);
        }
    }

    else {
        allocateReg(_Rt_);
        if (_Rd_ == 13) {
            gen.mov(eax, m_gprs[_Rt_].allocatedReg);
            gen.and_(eax, ~0xFC00);
            gen.mov(dword[contextPointer + COP0_OFFSET(_Rd_)], eax);
        } else if (_Rd_ != 6 && _Rd_ != 14 && _Rd_ != 15) {  // Don't write to JUMPDEST, EPC or PRID
            gen.mov(dword[contextPointer + COP0_OFFSET(_Rd_)], m_gprs[_Rt_].allocatedReg);  // Write rt to the cop0 reg
        }
    }

    // Writing to SR/Cause can sometimes forcefully fire an interrupt. So we need to emit extra code to check.
    if (_Rd_ == 12 || _Rd_ == 13) {
        testSoftwareInterrupt<true>();
    }
}

void DynaRecCPU::recRFE(uint32_t code) {
    gen.mov(eax, dword[contextPointer + COP0_OFFSET(12)]);  // eax = COP0 status register
    gen.mov(ecx, eax);                                      // Copy to ecx
    gen.and_(eax, ~0xF);                                    // Clear bottom 4 bits of eax
    gen.and_(
        ecx,
        0x3c);  // Shift bits [5:2] of previous SR two places to the right, mask out the rest of the cached SR value
    gen.shr(ecx, 2);
    gen.or_(eax, ecx);                                      // Merge the shifted bits into eax
    gen.mov(dword[contextPointer + COP0_OFFSET(12)], eax);  // Write eax back to SR
    testSoftwareInterrupt<false>();
}

// Checks if a write to SR/CAUSE forcibly triggered an interrupt
// loadSR: Shows if SR is already in eax or if it should be loaded from memory
template <bool loadSR>
void DynaRecCPU::testSoftwareInterrupt() {
    Label label;
    if (!m_pcWrittenBack) {
        gen.mov(dword[contextPointer + PC_OFFSET], m_pc);
        m_pcWrittenBack = true;
    }

    m_stopCompiling = true;
    m_linkedPC = std::nullopt;  // The exception replaces the PC that a branch before us linked to
    // The exception call below only runs on one path, but its spill updates the allocator for both. Spill here so
    // the fall-through path writes back the same registers.
    prepareForCall();

    if constexpr (loadSR) {
        gen.mov(eax, dword[contextPointer + COP0_OFFSET(12)]);  // eax = SR
    }
    gen.test(eax, 1);  // Check if interrupts are enabled
    gen.jz(label);     // If not, skip to the end

    gen.mov(arg2, dword[contextPointer + COP0_OFFSET(13)]);  // arg2 = CAUSE
    gen.and_(eax, arg2);
    gen.test(eax, 0x300);  // Check if an interrupt was force-fired
    gen.jz(label);         // Skip to the end if not

    // Fire the interrupt if it was triggered
    // This object in arg1. Exception code is already in arg2 from before (will be masked by exception handler)
    loadThisPointer(arg1.cvt64());
    // The interrupt is taken before the next instruction in execution order, which is the PC written back above, and
    // BD is clear: in a delay slot, the slot completes first and EPC is the branch target.
    gen.moveImm(arg3, 0);
    call(exceptionWrapper);  // Call the exception wrapper function

    gen.L(label);
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
            gen.mov(dword[contextPointer + PC_OFFSET], target);
            m_linkedPC = target;
        }
        return;
    } else if (m_gprs[_Rs_].isConst()) {
        allocateReg(_Rt_);
        gen.cmpEqImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        allocateReg(_Rs_);
        gen.cmpEqImm(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs(code);
        gen.cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    gen.mov(ecx, target);    // ecx = addr if jump taken
    gen.mov(eax, m_pc + 4);  // eax = addr if jump not taken
    gen.cmovne(eax, ecx);    // if not equal, move the jump addr into eax
    gen.mov(dword[contextPointer + PC_OFFSET], eax);
}

void DynaRecCPU::recJ(uint32_t code) {
    const uint32_t target = (m_pc & 0xf0000000) | (_Target_ << 2);
    m_nextIsDelaySlot = true;
    m_stopCompiling = true;
    m_pcWrittenBack = true;

    gen.mov(dword[contextPointer + PC_OFFSET], target);  // Write PC
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
        gen.mov(dword[contextPointer + PC_OFFSET], m_gprs[_Rs_].val & ~3);  // force align jump address
        m_linkedPC = m_gprs[_Rs_].val;
    } else {
        allocateReg(_Rs_);
        // PC will get force aligned in the dispatcher since it discards the 2 lower bits
        gen.mov(dword[contextPointer + PC_OFFSET], m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recREGIMM(uint32_t code) {
    const bool isBGEZ = ((code >> 16) & 1) != 0;
    const bool link = ((code >> 17) & 0xF) == 8;
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

                gen.mov(dword[contextPointer + PC_OFFSET], target);
                m_linkedPC = target;
            }
        }

        else {  // BLTZ
            if ((int32_t)m_gprs[_Rs_].val < 0) {
                m_pcWrittenBack = true;
                m_stopCompiling = true;

                gen.mov(dword[contextPointer + PC_OFFSET], target);
                m_linkedPC = target;
            }
        }

        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    allocateReg(_Rs_);
    gen.test(m_gprs[_Rs_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    gen.mov(ecx, target);    // ecx = addr if jump taken
    gen.mov(eax, m_pc + 4);  // eax = addr if jump not taken

    if (isBGEZ) {  // We're lazy so we can handle the difference between bgez/bltz by just emitting a different form of
                   // cmov
        gen.cmovns(eax, ecx);  // if $rs >= 0, move the jump addr into eax
    } else {
        gen.cmovs(eax, ecx);  // if $rs < 0, move the jump addr into eax
    }

    gen.mov(dword[contextPointer + PC_OFFSET], eax);
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
            gen.mov(dword[contextPointer + PC_OFFSET], target);

            m_linkedPC = target;
        }
        return;
    } else if (m_gprs[_Rs_].isConst()) {
        allocateReg(_Rt_);
        gen.cmpEqImm(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].val);
    } else if (m_gprs[_Rt_].isConst()) {
        allocateReg(_Rs_);
        gen.cmpEqImm(m_gprs[_Rs_].allocatedReg, m_gprs[_Rt_].val);
    } else {
        alloc_rt_rs(code);
        gen.cmp(m_gprs[_Rt_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    gen.mov(ecx, target);    // ecx = addr if jump taken
    gen.mov(eax, m_pc + 4);  // eax = addr if jump not taken
    gen.cmove(eax, ecx);     // if equal, move the jump addr into eax
    gen.mov(dword[contextPointer + PC_OFFSET], eax);
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
            gen.mov(dword[contextPointer + PC_OFFSET], target);
            m_linkedPC = target;
        }
        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    if (m_gprs[_Rs_].isAllocated()) {  // Don't bother allocating Rs unless it's already allocated
        gen.test(m_gprs[_Rs_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    } else {
        gen.cmp(dword[contextPointer + GPR_OFFSET(_Rs_)], 0);
    }

    gen.mov(eax, m_pc + 4);  // eax = addr if jump not taken
    gen.mov(ecx, target);    // ecx = addr if jump is taken
    gen.cmovg(eax, ecx);     // if taken, move the jump addr into eax
    gen.mov(dword[contextPointer + PC_OFFSET], eax);
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
            gen.mov(dword[contextPointer + PC_OFFSET], target);
            m_linkedPC = target;
        }
        return;
    }

    m_pcWrittenBack = true;
    m_stopCompiling = true;

    if (m_gprs[_Rs_].isAllocated()) {  // Don't bother allocating Rs unless it's already allocated
        gen.test(m_gprs[_Rs_].allocatedReg, m_gprs[_Rs_].allocatedReg);
    } else {
        gen.cmp(dword[contextPointer + GPR_OFFSET(_Rs_)], 0);
    }

    gen.mov(eax, m_pc + 4);  // eax = addr if jump not taken
    gen.mov(ecx, target);    // ecx = addr if jump is taken
    gen.cmovle(eax, ecx);    // if taken, move the jump addr into eax
    gen.mov(dword[contextPointer + PC_OFFSET], eax);
}

void DynaRecCPU::recDIV(uint32_t code) {
    Label notIntMin, divisionByZero, end;
    bool emitIntMinCheck = true;
    emitMuldivStart(c_divLatency);

    if (m_gprs[_Rt_].isConst()) {     // Check divisor if constant
        if (m_gprs[_Rt_].val == 0) {  // Handle case where divisor is 0
            if (m_gprs[_Rs_].isConst()) {
                gen.mov(dword[contextPointer + LO_OFFSET],
                        m_gprs[_Rs_].val & 0x80000000 ? 1 : -1);  // LO = 1 or -1 depending on the sign of $rs
                gen.mov(dword[contextPointer + HI_OFFSET], m_gprs[_Rs_].val);  // HI = $rs
            }

            else {
                allocateReg(_Rs_);
                gen.mov(dword[contextPointer + HI_OFFSET], m_gprs[_Rs_].allocatedReg);  // Set hi to $rs
                gen.mov(eax, m_gprs[_Rs_].allocatedReg);
                gen.shr(eax, 31);
                gen.lea(eax, dword[rax + rax - 1]);
                gen.mov(dword[contextPointer + LO_OFFSET], eax);  // Set lo to 1 or -1 depending on the sign of $rs
            }

            return;
        } else if (m_gprs[_Rt_].val != 0xffffffff) {
            emitIntMinCheck = false;
        }

        if (m_gprs[_Rs_].isConst()) {
            if (m_gprs[_Rs_].val == 0x80000000 && m_gprs[_Rt_].val == 0xffffffff) {
                gen.mov(dword[contextPointer + LO_OFFSET], 0x80000000);
                gen.mov(dword[contextPointer + HI_OFFSET], 0);
            } else {
                gen.mov(dword[contextPointer + LO_OFFSET], (int32_t)m_gprs[_Rs_].val / (int32_t)m_gprs[_Rt_].val);
                gen.mov(dword[contextPointer + HI_OFFSET], (int32_t)m_gprs[_Rs_].val % (int32_t)m_gprs[_Rt_].val);
            }
            return;
        }

        allocateReg(_Rs_);
        gen.mov(eax, m_gprs[_Rs_].allocatedReg);
        gen.mov(ecx, m_gprs[_Rt_].val);  // Divisor in ecx
    } else {                             // non-constant divisor
        if (m_gprs[_Rs_].isConst()) {
            allocateReg(_Rt_);
            gen.mov(eax, m_gprs[_Rs_].val);  // Dividend in eax
            emitIntMinCheck = m_gprs[_Rs_].val == 0x80000000;
        }

        else {
            alloc_rt_rs(code);
            gen.mov(eax, m_gprs[_Rs_].allocatedReg);  // Dividend in eax
        }

        gen.mov(ecx, m_gprs[_Rt_].allocatedReg);  // Divisor in ecx
        gen.test(ecx, ecx);                       // Check if divisor is 0
        gen.jz(divisionByZero);                   // Jump to divisionByZero label if so
    }

    if (emitIntMinCheck) {
        gen.cmp(eax, 0x80000000);  // Check if dividend is INT_MIN
        gen.jne(notIntMin);        // Bail if not
        gen.cmp(ecx, 0xffffffff);  // Check if divisor is -1
        gen.jne(notIntMin);        // Bail if not

        // Handle INT_MIN / -1
        gen.mov(eax, 0x80000000);  // Set lo to INT_MIN
        gen.xor_(edx, edx);        // Set hi to 0
        gen.jmp(end);
    }

    gen.L(notIntMin);
    gen.cdq();      // Sign extend dividend to 64 bits in edx:eax
    gen.idiv(ecx);  // Signed division by divisor

    if (!m_gprs[_Rt_].isConst()) {  // Emit a division by 0 handler if the divisor is unknown at compile time
        gen.jmp(end);               // skip to the end if not a div by zero
        gen.L(divisionByZero);      // Here starts our division by 0 handler

        gen.mov(edx, eax);  // Set hi to $rs
        gen.shr(eax, 31);
        gen.lea(eax, dword[rax + rax - 1]);  // Set lo to 1 or -1 depending on the sign of $rs
    }

    gen.L(end);

    gen.mov(dword[contextPointer + LO_OFFSET], eax);  // Lo = quotient
    gen.mov(dword[contextPointer + HI_OFFSET], edx);  // Hi = remainder
}

void DynaRecCPU::recDIVU(uint32_t code) {
    Label divisionByZero;
    emitMuldivStart(c_divLatency);

    if (m_gprs[_Rt_].isConst()) {                            // Check divisor if constant
        if (m_gprs[_Rt_].val == 0) {                         // Handle case where divisor is 0
            gen.mov(dword[contextPointer + LO_OFFSET], -1);  // Set lo to -1

            if (m_gprs[_Rs_].isConst()) {
                gen.mov(dword[contextPointer + HI_OFFSET], m_gprs[_Rs_].val);  // HI = $rs
            }

            else {
                allocateReg(_Rs_);
                gen.mov(dword[contextPointer + HI_OFFSET], m_gprs[_Rs_].allocatedReg);  // Set hi to $rs
            }

            return;
        }

        if (m_gprs[_Rs_].isConst()) {
            gen.mov(dword[contextPointer + LO_OFFSET], m_gprs[_Rs_].val / m_gprs[_Rt_].val);
            gen.mov(dword[contextPointer + HI_OFFSET], m_gprs[_Rs_].val % m_gprs[_Rt_].val);
            return;
        }

        allocateReg(_Rs_);
        gen.mov(eax, m_gprs[_Rs_].allocatedReg);
        gen.mov(ecx, m_gprs[_Rt_].val);  // Divisor in ecx
    } else {                             // non-constant divisor
        if (m_gprs[_Rs_].isConst()) {
            allocateReg(_Rt_);
            gen.mov(eax, m_gprs[_Rs_].val);  // Dividend in eax
        }

        else {
            alloc_rt_rs(code);
            gen.mov(eax, m_gprs[_Rs_].allocatedReg);  // Dividend in eax
        }

        gen.mov(ecx, m_gprs[_Rt_].allocatedReg);  // Divisor in ecx
        gen.test(ecx, ecx);                       // Check if divisor is 0
        gen.jz(divisionByZero);                   // Jump to divisionByZero label if so
    }

    gen.xor_(edx, edx);  // Set top 32 bits of dividend to 0
    gen.div(ecx);        // Unsigned division by divisor

    if (!m_gprs[_Rt_].isConst()) {  // Emit a division by 0 handler if the divisor is unknown at compile time
        Label end;
        gen.jmp(end);           // skip to the end if not a div by zero
        gen.L(divisionByZero);  // Here starts our division by 0 handler

        gen.mov(edx, eax);  // Set hi to $rs
        gen.mov(eax, -1);   // Set lo to -1

        gen.L(end);
    }

    gen.mov(dword[contextPointer + LO_OFFSET], eax);  // Lo = quotient
    gen.mov(dword[contextPointer + HI_OFFSET], edx);  // Hi = remainder
}

// TODO: Constant propagation for MFLO/HI, read the result from eax/edx if possible instead of reading memory again
void DynaRecCPU::recMFLO(uint32_t code) {
    emitMuldivStall();
    BAILZERO(_Rd_);

    maybeCancelDelayedLoad(_Rd_);
    allocateRegWithoutLoad(_Rd_);
    m_gprs[_Rd_].setWriteback(true);

    gen.mov(m_gprs[_Rd_].allocatedReg, dword[contextPointer + LO_OFFSET]);
}

// TODO: Constant propagation for MFLO/HI, read the result from eax/edx if possible instead of reading memory again
void DynaRecCPU::recMFHI(uint32_t code) {
    emitMuldivStall();
    BAILZERO(_Rd_);

    maybeCancelDelayedLoad(_Rd_);
    allocateRegWithoutLoad(_Rd_);
    m_gprs[_Rd_].setWriteback(true);

    gen.mov(m_gprs[_Rd_].allocatedReg, dword[contextPointer + HI_OFFSET]);
}

void DynaRecCPU::recMTLO(uint32_t code) {
    gen.mov(qword[contextPointer + MULDIV_READY_OFFSET], 0);  // mthi/mtlo abort a running mul/div
    if (m_gprs[_Rs_].isConst()) {
        gen.mov(dword[contextPointer + LO_OFFSET], m_gprs[_Rs_].val);
    } else {
        allocateReg(_Rs_);
        gen.mov(dword[contextPointer + LO_OFFSET], m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recMTHI(uint32_t code) {
    gen.mov(qword[contextPointer + MULDIV_READY_OFFSET], 0);  // mthi/mtlo abort a running mul/div
    if (m_gprs[_Rs_].isConst()) {
        gen.mov(dword[contextPointer + HI_OFFSET], m_gprs[_Rs_].val);
    } else {
        allocateReg(_Rs_);
        gen.mov(dword[contextPointer + HI_OFFSET], m_gprs[_Rs_].allocatedReg);
    }
}

void DynaRecCPU::recException(Exception e) {
    m_pcWrittenBack = true;
    m_stopCompiling = true;
    m_linkedPC = std::nullopt;  // The exception replaces the PC that a branch before us linked to

    loadThisPointer(arg1.cvt64());                                                  // Pointer to this object in arg1
    gen.moveImm(arg2, static_cast<std::underlying_type<Exception>::type>(e) << 2);  // Exception type in arg2
    gen.moveImm(arg3, (int32_t)m_inDelaySlot);                  // Store whether we're in a delay slot in arg3
    gen.mov(dword[contextPointer + PC_OFFSET], exceptionPC());  // PC for exception handler to use

    call(exceptionWrapper);  // Call the exception wrapper
}

void DynaRecCPU::recSYSCALL(uint32_t code) { recException(Exception::Syscall); }

void DynaRecCPU::recBREAK(uint32_t code) {
    flushRegs();  // For PCDRV support, we need to flush all registers before handling the exception.
    recException(Exception::Break);
}

#undef BAILZERO
#endif  // DYNAREC_X86_64
