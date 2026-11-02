/***************************************************************************
 *   Copyright (C) 2023 PCSX-Redux authors                                 *
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

#include "core/callstacks.h"
#include "core/cputrace.h"
#include "core/debug.h"
#include "core/disr3000a.h"
#include "core/gte.h"
#include "core/pgxp_cpu.h"
#include "core/pgxp_debug.h"
#include "core/pgxp_gte.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "tracy/Tracy.hpp"

#undef _PC_
#undef _Op_
#undef _Funct_
#undef _Rd_
#undef _Rt_
#undef _Rs_
#undef _Sa_
#undef _Im_
#undef _Target_
#undef _Imm_
#undef _Target_
#undef _ImmU_
#undef _ImmLU_
#undef _rRs_
#undef _rRt_
#undef _rRd_
#undef _c2dRs_
#undef _c2dRt_
#undef _c2dRd_
#undef _rHi_
#undef _rLo_
#undef _JumpTarget_
#undef _BranchTarget_

#define _PC_ m_regs.pc  // The next PC to be executed

#define _Op_ _fOp_(code)
#define _Funct_ _fFunct_(code)
#define _Rd_ _fRd_(code)
#define _Rt_ _fRt_(code)
#define _Rs_ _fRs_(code)
#define _Sa_ _fSa_(code)
#define _Im_ _fIm_(code)
#define _Target_ _fTarget_(code)

#define _Imm_ _fImm_(code)
#define _ImmU_ _fImmU_(code)
#define _ImmLU_ _fImmLU_(code)

#define _rRs_ m_regs.GPR.r[_Rs_]  // Rs register
#define _rRt_ m_regs.GPR.r[_Rt_]  // Rt register
#define _rRd_ m_regs.GPR.r[_Rd_]  // Rd register

#define _c2dRs_ m_regs.CP2D.r[_Rs_]  // Rs cop2 data register
#define _c2dRt_ m_regs.CP2D.r[_Rt_]  // Rt cop2 data register
#define _c2dRd_ m_regs.CP2D.r[_Rd_]  // Rd cop2 data register

#define _rHi_ m_regs.GPR.n.hi  // The HI register
#define _rLo_ m_regs.GPR.n.lo  // The LO register

#define _JumpTarget_ ((_Target_ * 4) + (_PC_ & 0xf0000000))  // Calculates the target during a jump instruction
// A relative branch adds its offset to the PC register. In the delay slot of a taken branch, the PC
// register already holds the first branch's target, so that is the base.
#define _BranchTarget_ ((int16_t)_Im_ * 4 + branchBase())

class InterpretedCPU final : public PCSX::R3000Acpu {
  public:
    InterpretedCPU() : R3000Acpu("Interpreted") {}

  private:
    // An exception taken in a branch delay slot cancels the pending branch. While the delay slot
    // executes, that branch sits in the other delayed load slot.
    // A BREAK that PCdrv handles takes no exception, so the branch stays pending.
    // Cause.BD and EPC follow the branch whose slot this is, taken or not, and that branch is not
    // always the previous word: a branch in a taken branch's slot owns the target as its slot.
    bool exception(uint32_t code, bool bd, bool cop0 = false) {
        bd = bd || m_inBranchSlot;
        bool taken = R3000Acpu::exception(code, bd, cop0);
        if (taken && bd) {
            m_delayedLoadInfo[m_currentDelayedLoad ^ 1].pcActive = false;
            m_regs.CP0.n.EPC = m_slotBranchPC;
        }
        return taken;
    }
    // Set by every branch and jump as it executes, taken or not; m_inBranchSlot covers exactly the
    // next instruction, m_slotBranchPC is the address of that branch.
    void markBranch() {
        m_nextIsBranchSlot = true;
        m_nextSlotBranchPC = m_regs.pc - 4;
    }
    bool m_inBranchSlot = false;
    uint32_t m_slotBranchPC = 0;
    // A software interrupt raised in a branch slot is taken once the slot completes, at the next
    // instruction in execution order, with BD clear.
    bool m_pendingSWInt = false;
    bool exception(Exception e, bool bd, bool cop0 = false) {
        return exception(static_cast<std::underlying_type<Exception>::type>(e) << 2, bd, cop0);
    }

    virtual bool Implemented() final { return true; }
    virtual bool Init() override;
    virtual void Reset() override;
    virtual void Execute() override;
    // Same arming test Debug::checkBP applies: TR and DE set, plus kernel or user mode enabled.
    bool cop0Armed() const {
        const uint32_t dcic = m_regs.CP0.n.DCIC;
        return ((dcic & 0x80800000) == 0x80800000) && (dcic & 0x60000000);
    }
    virtual RunUntilResult RunUntil(uint32_t stopPC, uint64_t cycleBudget) override;
    virtual void Clear(uint32_t Addr, uint32_t Size) override;
    virtual void Shutdown() override;
    virtual void SetPGXPMode(uint32_t pgxpMode) override;
    virtual bool isDynarec() override { return false; }
    void maybeCancelDelayedLoad(uint32_t index) {
        unsigned other = m_currentDelayedLoad ^ 1;
        if (m_delayedLoadInfo[other].index == index) m_delayedLoadInfo[other].active = false;
    }
    // For the GUI dynarec disassembly widget
    virtual const uint8_t* getBufferPtr() final { return nullptr; }
    virtual const size_t getBufferSize() final { return 0; }

    void psxTestSWInts();

    typedef void (InterpretedCPU::*intFunc_t)(uint32_t code);
    typedef const intFunc_t cIntFunc_t;

    cIntFunc_t* s_pPsxBSC = NULL;
    cIntFunc_t* s_pPsxSPC = NULL;
    cIntFunc_t* s_pPsxREG = NULL;
    cIntFunc_t* s_pPsxCP0 = NULL;
    cIntFunc_t* s_pPsxCP2 = NULL;
    cIntFunc_t* s_pPsxCP2BSC = NULL;

    template <bool debug, bool trace, bool detached = false>
    void execBlock();
    void doBranch(uint32_t target, bool fromLink);
    uint32_t branchBase();

    void MTC0(int reg, uint32_t val);

    /* Arithmetic with immediate operand */
    void psxADDI(uint32_t code);
    void psxADDIU(uint32_t code);
    void psxANDI(uint32_t code);
    void psxORI(uint32_t code);
    void psxXORI(uint32_t code);
    void psxSLTI(uint32_t code);
    void psxSLTIU(uint32_t code);

    /* Register arithmetic */
    void psxADD(uint32_t code);
    void psxADDU(uint32_t code);
    void psxSUB(uint32_t code);
    void psxSUBU(uint32_t code);
    void psxAND(uint32_t code);
    void psxOR(uint32_t code);
    void psxXOR(uint32_t code);
    void psxNOR(uint32_t code);
    void psxSLT(uint32_t code);
    void psxSLTU(uint32_t code);

    /* Register mult/div & Register trap logic */
    void psxDIV(uint32_t code);
    void psxDIVU(uint32_t code);
    void psxMULT(uint32_t code);
    void psxMULTU(uint32_t code);

    /* Register branch logic */
    void psxBGEZ(uint32_t code);
    void psxBGEZAL(uint32_t code);
    void psxBGTZ(uint32_t code);
    void psxBLEZ(uint32_t code);
    void psxBLTZ(uint32_t code);
    void psxBLTZAL(uint32_t code);

    /* Shift arithmetic with constant shift */
    void psxSLL(uint32_t code);
    void psxSRA(uint32_t code);
    void psxSRL(uint32_t code);

    /* Shift arithmetic with variant register shift */
    void psxSLLV(uint32_t code);
    void psxSRAV(uint32_t code);
    void psxSRLV(uint32_t code);

    /* Load higher 16 bits of the first word in GPR with imm */
    void psxLUI(uint32_t code);

    /* Move from HI/LO to GPR */
    void psxMFHI(uint32_t code);
    void psxMFLO(uint32_t code);

    /* Move to GPR to HI/LO & Register jump */
    void psxMTHI(uint32_t code);
    void psxMTLO(uint32_t code);

    /* Special purpose instructions */
    void psxBREAK(uint32_t code);
    void psxSYSCALL(uint32_t code);
    void psxRFE(uint32_t code);

    /* Register branch logic */
    void psxBEQ(uint32_t code);
    void psxBNE(uint32_t code);

    /* Jump to target */
    void psxJ(uint32_t code);
    void psxJAL(uint32_t code);

    /* Register jump */
    void psxJR(uint32_t code);
    void psxJALR(uint32_t code);

    /* Load and store for GPR */
    void psxLB(uint32_t code);
    void psxLBU(uint32_t code);
    void psxLH(uint32_t code);
    void psxLHU(uint32_t code);
    void psxLW(uint32_t code);

  private:
    void psxLWL(uint32_t code);
    void psxLWR(uint32_t code);
    void psxSB(uint32_t code);
    void psxSH(uint32_t code);
    void psxSW(uint32_t code);
    void psxSWL(uint32_t code);
    void psxSWR(uint32_t code);

    /* Moves between GPR and COPx */
    void psxMFC0(uint32_t code);
    void psxCFC0(uint32_t code);
    void psxMTC0(uint32_t code);
    void psxCTC0(uint32_t code);
    void psxMFC2(uint32_t code);
    void psxCFC2(uint32_t code);

    /* Misc */
    void psxNULL(uint32_t code);
    void psxSPECIAL(uint32_t code);
    void psxREGIMM(uint32_t code);
    void psxCOP0(uint32_t code);
    void psxCOP1(uint32_t code);
    void psxCOP2(uint32_t code);
    void psxCOP3(uint32_t code);
    void psxBCz(uint32_t code, unsigned z);
    void gteMove(uint32_t code);

    /* GTE wrappers */
#define GTE_WRAPPER(n) \
    void gte##n(uint32_t code) { PCSX::g_emulator->m_gte->n(code); }
    GTE_WRAPPER(AVSZ3);
    GTE_WRAPPER(AVSZ4);
    GTE_WRAPPER(CC);
    GTE_WRAPPER(CDP);
    GTE_WRAPPER(DCPL);
    GTE_WRAPPER(DPCS);
    GTE_WRAPPER(DPCT);
    GTE_WRAPPER(GPF);
    GTE_WRAPPER(GPL);
    GTE_WRAPPER(INTPL);
    GTE_WRAPPER(MVMVA);
    GTE_WRAPPER(NCCS);
    GTE_WRAPPER(NCCT);
    GTE_WRAPPER(NCDS);
    GTE_WRAPPER(NCDT);
    GTE_WRAPPER(NCLIP);
    GTE_WRAPPER(NCS);
    GTE_WRAPPER(NCT);
    GTE_WRAPPER(OP);
    GTE_WRAPPER(RTPS);
    GTE_WRAPPER(RTPT);
    GTE_WRAPPER(SQR);
#undef GTE_WRAPPER
    void gteSWC2(uint32_t code) {
        gteDrainIfStalling();
        gteStall();
        PCSX::g_emulator->m_gte->SWC2(code);
    }

    // A write to a GTE register (MTC2, CTC2, LWC2) only becomes visible to the third instruction
    // after it: a GTE command, MFC2, CFC2 or SWC2 issued by either of the next two instructions
    // still sees the old value. The write is queued with the value taken at issue and committed
    // through GTE::MTC2 / GTE::CTC2 once two more instructions have run.
    void gteQueueWrite(bool ctrl, unsigned reg, uint32_t value) {
        if (m_gtePendingCount == 3) gteCommitOldest();
        m_gtePending[m_gtePendingCount++] = {value, reg, ctrl, 3};
    }
    void gteCommitOldest() {
        gteCommit(m_gtePending[0]);
        for (unsigned i = 1; i < m_gtePendingCount; i++) m_gtePending[i - 1] = m_gtePending[i];
        m_gtePendingCount--;
    }
    void gteTickWrites() {
        for (unsigned i = 0; i < m_gtePendingCount; i++) m_gtePending[i].countdown--;
        while (m_gtePendingCount && m_gtePending[0].countdown == 0) gteCommitOldest();
    }
    // An access that waits for a running GTE command gives queued writes time to complete.
    void gteDrainIfStalling() {
        if (m_regs.cycle < m_regs.gteReady) {
            while (m_gtePendingCount) gteCommitOldest();
        }
    }
    void gteMTC2(uint32_t code) { gteQueueWrite(false, _Rd_, m_regs.GPR.r[_Rt_]); }
    void gteCTC2(uint32_t code) { gteQueueWrite(true, _Rd_, m_regs.GPR.r[_Rt_]); }
    void gteLWC2(uint32_t code) {
        uint32_t addr = m_regs.GPR.r[_Rs_] + _Imm_;
        if (addr & 3) {
            PCSX::g_emulator->m_gte->LWC2(code);
            return;
        }
        gteQueueWrite(false, _Rt_, PCSX::g_emulator->m_mem->read32(addr));
    }

    static const intFunc_t s_psxBSC[64];
    static const intFunc_t s_psxSPC[64];
    static const intFunc_t s_psxREG[32];
    static const intFunc_t s_psxCP0[32];
    static const intFunc_t s_psxCP2[64];
    static const intFunc_t s_psxCP2BSC[32];

    void pgxpPsxNULL(uint32_t code);
    void pgxpPsxADDI(uint32_t code);
    void pgxpPsxADDIU(uint32_t code);
    void pgxpPsxANDI(uint32_t code);
    void pgxpPsxORI(uint32_t code);
    void pgxpPsxXORI(uint32_t code);
    void pgxpPsxSLTI(uint32_t code);
    void pgxpPsxSLTIU(uint32_t code);
    void pgxpPsxLUI(uint32_t code);
    void pgxpPsxADD(uint32_t code);
    void pgxpPsxADDU(uint32_t code);
    void pgxpPsxSUB(uint32_t code);
    void pgxpPsxSUBU(uint32_t code);
    void pgxpPsxAND(uint32_t code);
    void pgxpPsxOR(uint32_t code);
    void pgxpPsxXOR(uint32_t code);
    void pgxpPsxNOR(uint32_t code);
    void pgxpPsxSLT(uint32_t code);
    void pgxpPsxSLTU(uint32_t code);
    void pgxpPsxMULT(uint32_t code);
    void pgxpPsxMULTU(uint32_t code);
    void pgxpPsxDIV(uint32_t code);
    void pgxpPsxDIVU(uint32_t code);
    void pgxpPsxSB(uint32_t code);
    void pgxpPsxSH(uint32_t code);
    void pgxpPsxSW(uint32_t code);
    void pgxpPsxSWL(uint32_t code);
    void pgxpPsxSWR(uint32_t code);
    void pgxpPsxLWL(uint32_t code);
    void pgxpPsxLW(uint32_t code);
    void pgxpPsxLWR(uint32_t code);
    void pgxpPsxLH(uint32_t code);
    void pgxpPsxLHU(uint32_t code);
    void pgxpPsxLB(uint32_t code);
    void pgxpPsxLBU(uint32_t code);
    void pgxpPsxSLL(uint32_t code);
    void pgxpPsxSRL(uint32_t code);
    void pgxpPsxSRA(uint32_t code);
    void pgxpPsxSLLV(uint32_t code);
    void pgxpPsxSRLV(uint32_t code);
    void pgxpPsxSRAV(uint32_t code);
    void pgxpPsxMFHI(uint32_t code);
    void pgxpPsxMTHI(uint32_t code);
    void pgxpPsxMFLO(uint32_t code);
    void pgxpPsxMTLO(uint32_t code);
    void pgxpPsxMFC2(uint32_t code);
    void pgxpPsxCFC2(uint32_t code);
    void pgxpPsxMTC2(uint32_t code);
    void pgxpPsxCTC2(uint32_t code);
    void pgxpPsxLWC2(uint32_t code);
    void pgxpPsxSWC2(uint32_t code);
    void pgxpPsxMFC0(uint32_t code);
    void pgxpPsxCFC0(uint32_t code);
    void pgxpPsxMTC0(uint32_t code);
    void pgxpPsxCTC0(uint32_t code);
    void pgxpPsxRFE(uint32_t code);

    static const intFunc_t s_pgxpPsxBSC[64];
    static const intFunc_t s_pgxpPsxSPC[64];
    static const intFunc_t s_pgxpPsxCP0[32];
    static const intFunc_t s_pgxpPsxCP2BSC[32];
    static const intFunc_t s_pgxpPsxBSCMem[64];
};

inline uint32_t InterpretedCPU::branchBase() {
    auto& pending = m_delayedLoadInfo[m_currentDelayedLoad ^ 1];
    return pending.pcActive ? pending.pcValue : m_regs.pc;
}

inline void InterpretedCPU::doBranch(uint32_t target, bool fromLink) {
    m_nextIsDelaySlot = true;
    markBranch();
    delayedPCLoad(target, fromLink);
}

/*********************************************************
 * Arithmetic with immediate operand                      *
 * Format:  OP rt, rs, immediate                          *
 *********************************************************/
void InterpretedCPU::psxADDI(uint32_t code) {
    const auto rs = _rRs_;
    const auto imm = _Imm_;
    uint32_t res = rs + imm;

    bool overflow = ((rs ^ res) & (imm ^ res)) >> 31;  // fast signed overflow calculation algorithm
    if (overflow) {                                    // if an overflow occurs, throw an exception
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Signed overflow in ADDI instruction from 0x%08x!\n"), m_regs.pc);
        exception(Exception::ArithmeticOverflow, m_inDelaySlot);
        return;
    }

    if (_Rt_ == 29) {
        if (_Rs_ == 29) {
            PCSX::g_emulator->m_callStacks->offsetSP(rs, imm);
        } else {
            PCSX::g_emulator->m_callStacks->setSP(rs, res);
        }
    }

    if (_Rt_ != 0) {
        maybeCancelDelayedLoad(_Rt_);
        _rRt_ = res;
    }
}

void InterpretedCPU::psxADDIU(uint32_t code) {
    if (!_Rt_) return;
    maybeCancelDelayedLoad(_Rt_);
    uint32_t newValue = _rRs_ + _Imm_;
    if (_Rt_ == 29) {
        if (_Rs_ == 29) {
            PCSX::g_emulator->m_callStacks->offsetSP(_rRt_, _Imm_);
        } else {
            PCSX::g_emulator->m_callStacks->setSP(_rRt_, newValue);
        }
    }

    _rRt_ = newValue;
}  // Rt = Rs + Im
void InterpretedCPU::psxANDI(uint32_t code) {
    if (!_Rt_) return;
    maybeCancelDelayedLoad(_Rt_);
    uint32_t newValue = _rRs_ & _ImmU_;
    if (_Rt_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRt_, newValue);
    }
    _rRt_ = newValue;
}  // Rt = Rs And Im
void InterpretedCPU::psxORI(uint32_t code) {
    if (!_Rt_) return;
    maybeCancelDelayedLoad(_Rt_);
    uint32_t newValue = _rRs_ | _ImmU_;
    if (_Rt_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRt_, newValue);
    }
    _rRt_ = newValue;
}  // Rt = Rs Or  Im
void InterpretedCPU::psxXORI(uint32_t code) {
    if (!_Rt_) return;
    maybeCancelDelayedLoad(_Rt_);
    uint32_t newValue = _rRs_ ^ _ImmU_;
    if (_Rt_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRt_, newValue);
    }
    _rRt_ = newValue;
}  // Rt = Rs Xor Im
void InterpretedCPU::psxSLTI(uint32_t code) {
    if (!_Rt_) return;
    maybeCancelDelayedLoad(_Rt_);
    uint32_t newValue = int32_t(_rRs_) < _Imm_;
    if (_Rt_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRt_, newValue);
    }
    _rRt_ = newValue;
}  // Rt = Rs < Im              (Signed)
void InterpretedCPU::psxSLTIU(uint32_t code) {
    if (!_Rt_) return;
    maybeCancelDelayedLoad(_Rt_);
    uint32_t newValue = _rRs_ < ((uint32_t)_Imm_);
    if (_Rt_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRt_, newValue);
    }
    _rRt_ = newValue;
}  // Rt = Rs < Im              (Unsigned)

/*********************************************************
 * Register arithmetic                                    *
 * Format:  OP rd, rs, rt                                 *
 *********************************************************/
void InterpretedCPU::psxADD(uint32_t code) {
    const auto rs = _rRs_;
    const auto rt = _rRt_;
    uint32_t res = rs + rt;
    bool overflow = ((rs ^ res) & (rt ^ res)) >> 31;  // fast signed overflow calculation algorithm
    if (overflow) {                                   // if an overflow occurs, throw an exception
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Signed overflow in ADD instruction from 0x%08x!\n"), m_regs.pc);
        exception(Exception::ArithmeticOverflow, m_inDelaySlot);
        return;
    }

    if (_Rd_ == 29) {
        if ((_Rs_ == 29) || (_Rt_ == 29)) {
            PCSX::g_emulator->m_callStacks->offsetSP(_rRd_, res - _rRd_);
        } else {
            PCSX::g_emulator->m_callStacks->setSP(_rRd_, res);
        }
    }

    if (_Rd_ != 0) {
        maybeCancelDelayedLoad(_Rd_);
        _rRd_ = res;
    }
}

void InterpretedCPU::psxADDU(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t res = _rRs_ + _rRt_;
    if (_Rd_ == 29) {
        if ((_Rs_ == 29) || (_Rt_ == 29)) {
            PCSX::g_emulator->m_callStacks->offsetSP(_rRd_, res - _rRd_);
        } else {
            PCSX::g_emulator->m_callStacks->setSP(_rRd_, res);
        }
    }
    _rRd_ = res;
}  // Rd = Rs + Rt
void InterpretedCPU::psxSUB(uint32_t code) {
    const auto rs = _rRs_;
    const auto rt = _rRt_;
    uint32_t res = rs - rt;
    bool overflow = ((rs ^ res) & (~rt ^ res)) >> 31;  // fast signed overflow calculation algorithm
    if (overflow) {                                    // if an overflow occurs, throw an exception
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Signed overflow in SUB instruction from 0x%08x!\n"), m_regs.pc);
        exception(Exception::ArithmeticOverflow, m_inDelaySlot);
        return;
    }

    if (_Rd_ == 29) {
        if (_Rs_ == 29) {
            PCSX::g_emulator->m_callStacks->offsetSP(_rRd_, res - _rRd_);
        } else {
            PCSX::g_emulator->m_callStacks->setSP(_rRd_, res);
        }
    }

    if (_Rd_ != 0) {
        maybeCancelDelayedLoad(_Rd_);
        _rRd_ = res;
    }
}  // Rd = Rs - Rt              (Exception on Integer Overflow)
void InterpretedCPU::psxSUBU(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t res = _rRs_ - _rRt_;
    if (_Rd_ == 29) {
        if (_Rs_ == 29) {
            PCSX::g_emulator->m_callStacks->offsetSP(_rRd_, res - _rRd_);
        } else {
            PCSX::g_emulator->m_callStacks->setSP(_rRd_, res);
        }
    }
    _rRd_ = res;
}  // Rd = Rs - Rt
void InterpretedCPU::psxAND(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRs_ & _rRt_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rs And Rt
void InterpretedCPU::psxOR(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRs_ | _rRt_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rs Or  Rt
void InterpretedCPU::psxXOR(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRs_ ^ _rRt_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rs Xor Rt
void InterpretedCPU::psxNOR(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = ~(_rRs_ | _rRt_);
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rs Nor Rt
void InterpretedCPU::psxSLT(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = int32_t(_rRs_) < int32_t(_rRt_);
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rs < Rt              (Signed)
void InterpretedCPU::psxSLTU(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRs_ < _rRt_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rs < Rt              (Unsigned)

/*********************************************************
 * Register mult/div & Register trap logic                *
 * Format:  OP rs, rt                                     *
 *********************************************************/
void InterpretedCPU::psxDIV(uint32_t code) {
    muldivStart(c_divLatency);
    if (_rRt_ == 0) {
        _rHi_ = _rRs_;
        if (_rRs_ & 0x80000000) {
            _rLo_ = 1;
        } else {
            _rLo_ = 0xFFFFFFFF;
        }
    } else if (_rRs_ == 0x80000000 && _rRt_ == 0xFFFFFFFF) {
        _rLo_ = 0x80000000;
        _rHi_ = 0;
    } else {
        _rLo_ = (int32_t)_rRs_ / (int32_t)_rRt_;
        _rHi_ = (int32_t)_rRs_ % (int32_t)_rRt_;
    }
}

void InterpretedCPU::psxDIVU(uint32_t code) {
    muldivStart(c_divLatency);
    if (_rRt_ != 0) {
        _rLo_ = _rRs_ / _rRt_;
        _rHi_ = _rRs_ % _rRt_;
    } else {
        _rLo_ = 0xffffffff;
        _rHi_ = _rRs_;
    }
}

void InterpretedCPU::psxMULT(uint32_t code) {
    muldivStart(multLatency(_rRs_, true));
    uint64_t res = (int64_t)(int32_t)_rRs_ * (int64_t)(int32_t)_rRt_;

    m_regs.GPR.n.lo = (uint32_t)(res & 0xffffffff);
    m_regs.GPR.n.hi = (uint32_t)((res >> 32) & 0xffffffff);
}

void InterpretedCPU::psxMULTU(uint32_t code) {
    muldivStart(multLatency(_rRs_, false));
    uint64_t res = (uint64_t)_rRs_ * (uint64_t)_rRt_;

    m_regs.GPR.n.lo = (uint32_t)(res & 0xffffffff);
    m_regs.GPR.n.hi = (uint32_t)((res >> 32) & 0xffffffff);
}

/*********************************************************
 * Register branch logic                                  *
 * Format:  OP rs, offset                                 *
 *********************************************************/
#define RepZBranchi32(op)                \
    markBranch();                        \
    if (int32_t(_rRs_) op 0) {           \
        doBranch(_BranchTarget_, false); \
    }
#define RepZBranchLinki32(op)                                    \
    {                                                            \
        uint32_t ra = m_regs.pc + 4;                             \
        markBranch();                                            \
        if ((int32_t)_rRs_ op 0) {                               \
            uint32_t sp = m_regs.GPR.n.sp;                       \
            doBranch(_BranchTarget_, true);                      \
            PCSX::g_emulator->m_callStacks->potentialRA(ra, sp); \
        }                                                        \
        m_regs.GPR.r[31] = ra;                                   \
        maybeCancelDelayedLoad(31);                              \
    }

void InterpretedCPU::psxBGEZ(uint32_t code) { RepZBranchi32(>=) }         // Branch if Rs >= 0
void InterpretedCPU::psxBGEZAL(uint32_t code) { RepZBranchLinki32(>=); }  // Branch if Rs >= 0 and link
void InterpretedCPU::psxBGTZ(uint32_t code) { RepZBranchi32(>) }          // Branch if Rs >  0
void InterpretedCPU::psxBLEZ(uint32_t code) { RepZBranchi32(<=) }         // Branch if Rs <= 0
void InterpretedCPU::psxBLTZ(uint32_t code) { RepZBranchi32(<) }          // Branch if Rs <  0
void InterpretedCPU::psxBLTZAL(uint32_t code) { RepZBranchLinki32(<); }   // Branch if Rs <  0 and link
/*********************************************************
 * Shift arithmetic with constant shift                   *
 * Format:  OP rd, rt, sa                                 *
 *********************************************************/
void InterpretedCPU::psxSLL(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRt_ << _Sa_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rt << sa
void InterpretedCPU::psxSRA(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = int32_t(_rRt_) >> _Sa_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rt >> sa (arithmetic)
void InterpretedCPU::psxSRL(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRt_ >> _Sa_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rt >> sa (logical)

/*********************************************************
 * Shift arithmetic with variant register shift           *
 * Format:  OP rd, rt, rs                                 *
 *********************************************************/
void InterpretedCPU::psxSLLV(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRt_ << (_rRs_ & 0x1f);
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rt << rs
void InterpretedCPU::psxSRAV(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = int32_t(_rRt_) >> (_rRs_ & 0x1f);
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rt >> rs (arithmetic)
void InterpretedCPU::psxSRLV(uint32_t code) {
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rRt_ >> (_rRs_ & 0x1f);
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Rt >> rs (logical)

/*********************************************************
 * Load higher 16 bits of the first word in GPR with imm  *
 * Format:  OP rt, immediate                              *
 *********************************************************/
void InterpretedCPU::psxLUI(uint32_t code) {
    if (!_Rt_) return;
    maybeCancelDelayedLoad(_Rt_);
    uint32_t newValue = _ImmLU_;
    if (_Rt_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRt_, newValue);
    }
    _rRt_ = newValue;
}  // Upper halfword of Rt = Im

/*********************************************************
 * Move from HI/LO to GPR                                 *
 * Format:  OP rd                                         *
 *********************************************************/
void InterpretedCPU::psxMFHI(uint32_t code) {
    muldivStall();
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rHi_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Hi
void InterpretedCPU::psxMFLO(uint32_t code) {
    muldivStall();
    if (!_Rd_) return;
    maybeCancelDelayedLoad(_Rd_);
    uint32_t newValue = _rLo_;
    if (_Rd_ == 29) {
        PCSX::g_emulator->m_callStacks->setSP(_rRd_, newValue);
    }
    _rRd_ = newValue;
}  // Rd = Lo

/*********************************************************
 * Move to GPR to HI/LO & Register jump                   *
 * Format:  OP rs                                         *
 *********************************************************/
void InterpretedCPU::psxMTHI(uint32_t code) {
    muldivCancel();
    _rHi_ = _rRs_;  // Hi = Rs
}
void InterpretedCPU::psxMTLO(uint32_t code) {
    muldivCancel();
    _rLo_ = _rRs_;  // Lo = Rs
}

/*********************************************************
 * Special purpose instructions                           *
 * Format:  OP                                            *
 *********************************************************/
void InterpretedCPU::psxBREAK(uint32_t code) {
    m_regs.pc.raw() -= 4;
    exception(Exception::Break, m_inDelaySlot);
}

void InterpretedCPU::psxSYSCALL(uint32_t code) {
    m_regs.pc.raw() -= 4;
    exception(Exception::Syscall, m_inDelaySlot);
}

void InterpretedCPU::psxRFE(uint32_t code) {
    //  PCSX::g_system->log(PCSX::LogClass::CPU, "psxRFE\n");
    m_inISR = false;
    m_regs.CP0.n.Status = (m_regs.CP0.n.Status & 0xfffffff0) | ((m_regs.CP0.n.Status & 0x3c) >> 2);
    psxTestSWInts();
}

/*********************************************************
 * Register branch logic                                  *
 * Format:  OP rs, rt, offset                             *
 *********************************************************/
#define RepBranchi32(op) \
    markBranch();        \
    if ((int32_t)_rRs_ op(int32_t) _rRt_) doBranch(_BranchTarget_, false);

void InterpretedCPU::psxBEQ(uint32_t code) { RepBranchi32(==) }  // Branch if Rs == Rt
void InterpretedCPU::psxBNE(uint32_t code) { RepBranchi32(!=) }  // Branch if Rs != Rt

/*********************************************************
 * Jump to target                                         *
 * Format:  OP target                                     *
 *********************************************************/
void InterpretedCPU::psxJ(uint32_t code) { doBranch(_JumpTarget_, false); }
void InterpretedCPU::psxJAL(uint32_t code) {
    maybeCancelDelayedLoad(31);
    uint32_t ra = m_regs.pc + 4;
    m_regs.GPR.r[31] = ra;
    doBranch(_JumpTarget_, true);
    PCSX::g_emulator->m_callStacks->potentialRA(ra, m_regs.GPR.n.sp);
}

/*********************************************************
 * Register jump                                          *
 * Format:  OP rs, rd                                     *
 *********************************************************/
void InterpretedCPU::psxJR(uint32_t code) {
    // if the jump is unaligned, throw an exception and return
    if (_rRs_ & 3) {
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Attempted unaligned JR to 0x%08x from 0x%08x, firing exception!\n"),
                            _rRs_, m_regs.pc);
        m_regs.CP0.n.BadVAddr = _rRs_;
        exception(Exception::LoadAddressError, m_inDelaySlot);
        return;
    }

    doBranch(_rRs_, false);
}

void InterpretedCPU::psxJALR(uint32_t code) {
    uint32_t temp = _rRs_;
    if (_Rd_) {
        maybeCancelDelayedLoad(_Rd_);
        uint32_t ra = m_regs.pc + 4;
        _rRd_ = ra;
        if (_Rd_ == 31) {
            PCSX::g_emulator->m_callStacks->potentialRA(ra, m_regs.GPR.n.sp);
        }
    }

    // if the address is unaligned, throw an exception and return
    if (temp & 3) {
        // TODO: is Rd modified in this case?
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU,
                            _("Attempted unaligned JALR to 0x%08x from 0x%08x, firing exception!\n"), temp, m_regs.pc);
        m_regs.CP0.n.BadVAddr = temp;
        exception(Exception::LoadAddressError, m_inDelaySlot);
        return;
    }

    doBranch(temp, true);
}

/*********************************************************
 * Load and store for GPR                                 *
 * Format:  OP rt, offset(base)                           *
 *********************************************************/

#define _oB_ (_rRs_ + _Imm_)

void InterpretedCPU::psxLB(uint32_t code) {
    // load delay = 1 latency
    if (_Rt_) {
        maybeCancelDelayedLoad(_Rt_);
        delayedLoadRef<int32_t>(_Rt_) = (int8_t)PCSX::g_emulator->m_mem->read8(_oB_);
    } else {
        PCSX::g_emulator->m_mem->read8(_oB_);
    }
}

void InterpretedCPU::psxLBU(uint32_t code) {
    // load delay = 1 latency
    if (_Rt_) {
        maybeCancelDelayedLoad(_Rt_);
        delayedLoadRef(_Rt_) = PCSX::g_emulator->m_mem->read8(_oB_);
    } else {
        PCSX::g_emulator->m_mem->read8(_oB_);
    }
}

void InterpretedCPU::psxLH(uint32_t code) {
    // load delay = 1 latency
    if (_oB_ & 1) {
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Unaligned address 0x%08x in LH from 0x%08x\n"), _oB_, m_regs.pc);
        m_regs.CP0.n.BadVAddr = _oB_;
        exception(Exception::LoadAddressError, m_inDelaySlot);
        return;
    }

    if (_Rt_) {
        maybeCancelDelayedLoad(_Rt_);
        delayedLoadRef<int32_t>(_Rt_) = (short)PCSX::g_emulator->m_mem->read16(_oB_);
    } else {
        PCSX::g_emulator->m_mem->read16(_oB_);
    }
}

void InterpretedCPU::psxLHU(uint32_t code) {
    // load delay = 1 latency
    if (_oB_ & 1) {
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Unaligned address 0x%08x in LHU from 0x%08x\n"), _oB_, m_regs.pc);
        m_regs.CP0.n.BadVAddr = _oB_;
        exception(Exception::LoadAddressError, m_inDelaySlot);
        return;
    }

    if (_Rt_) {
        maybeCancelDelayedLoad(_Rt_);
        delayedLoadRef(_Rt_) = PCSX::g_emulator->m_mem->read16(_oB_);
    } else {
        PCSX::g_emulator->m_mem->read16(_oB_);
    }
}

void InterpretedCPU::psxLW(uint32_t code) {
    // load delay = 1 latency
    if (_oB_ & 3) {
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Unaligned address 0x%08x in LW from 0x%08x\n"), _oB_, m_regs.pc);
        m_regs.CP0.n.BadVAddr = _oB_;
        exception(Exception::LoadAddressError, m_inDelaySlot);
        return;
    }

    uint32_t val = PCSX::g_emulator->m_mem->read32(_oB_);
    if (_Rt_) {
        maybeCancelDelayedLoad(_Rt_);
        switch (_Rt_) {
            case 29:
                PCSX::g_emulator->m_callStacks->setSP(m_regs.GPR.n.sp, val);
                break;
            case 31:
                if (_Rs_ == 29) {
                    PCSX::g_emulator->m_callStacks->loadRA(_oB_);
                }
                break;
        }
        delayedLoadRef(_Rt_) = val;
    }
}

void InterpretedCPU::psxLWL(uint32_t code) {
    uint32_t addr = _oB_;
    uint32_t shift = addr & 3;
    uint32_t mem = PCSX::g_emulator->m_mem->read32Masked(addr & ~3, PCSX::Memory::leftByteMask(addr));

    // load delay = 1 latency
    if (!_Rt_) return;
    delayedLoadRef(_Rt_, LWL_MASK[shift]) = mem << LWL_SHIFT[shift];

    /*
    Mem = 1234.  Reg = abcd
    0   4bcd   (mem << 24) | (reg & 0x00ffffff)
    1   34cd   (mem << 16) | (reg & 0x0000ffff)
    2   234d   (mem <<  8) | (reg & 0x000000ff)
    3   1234   (mem      ) | (reg & 0x00000000)
    */
}

void InterpretedCPU::psxLWR(uint32_t code) {
    uint32_t addr = _oB_;
    uint32_t shift = addr & 3;
    uint32_t mem = PCSX::g_emulator->m_mem->read32Masked(addr & ~3, PCSX::Memory::rightByteMask(addr));

    // load delay = 1 latency
    if (!_Rt_) return;
    delayedLoadRef(_Rt_, LWR_MASK[shift]) = mem >> LWR_SHIFT[shift];

    /*
    Mem = 1234.  Reg = abcd
    0   1234   (mem      ) | (reg & 0x00000000)
    1   a123   (mem >>  8) | (reg & 0xff000000)
    2   ab12   (mem >> 16) | (reg & 0xffff0000)
    3   abc1   (mem >> 24) | (reg & 0xffffff00)
    */
}

void InterpretedCPU::psxSB(uint32_t code) { PCSX::g_emulator->m_mem->write8(_oB_, _rRt_); }
void InterpretedCPU::psxSH(uint32_t code) {
    if (_oB_ & 1) {
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Unaligned address 0x%08x in SH from 0x%08x\n"), _oB_, m_regs.pc);
        m_regs.CP0.n.BadVAddr = _oB_;
        exception(Exception::StoreAddressError, m_inDelaySlot);
        return;
    }
    PCSX::g_emulator->m_mem->write16(_oB_, _rRt_);
}

void InterpretedCPU::psxSW(uint32_t code) {
    if (_oB_ & 3) {
        m_regs.pc.raw() -= 4;
        PCSX::g_system->log(PCSX::LogClass::CPU, _("Unaligned address 0x%08x in SW from 0x%08x\n"), _oB_, m_regs.pc);
        m_regs.CP0.n.BadVAddr = _oB_;
        exception(Exception::StoreAddressError, m_inDelaySlot);
        return;
    }
    if ((_Rt_ == 31) && (_Rs_ == 29)) {
        PCSX::g_emulator->m_callStacks->storeRA(_oB_, _rRt_);
    }
    PCSX::g_emulator->m_mem->write32(_oB_, _rRt_);
}

void InterpretedCPU::psxSWL(uint32_t code) {
    uint32_t addr = _oB_;
    uint32_t shift = addr & 3;
    addr ^= shift;
    // the preserved bytes aren't consumed, so msan doesn't check any of them
    uint32_t mem = PCSX::g_emulator->m_mem->read32Masked(addr, 0);

    PCSX::g_emulator->m_mem->write32Masked(addr, ((_rRt_) >> SWL_SHIFT[shift]) | (mem & SWL_MASK[shift]),
                                           PCSX::Memory::leftByteMask(shift));
    /*
    Mem = 1234.  Reg = abcd
    0   123a   (reg >> 24) | (mem & 0xffffff00)
    1   12ab   (reg >> 16) | (mem & 0xffff0000)
    2   1abc   (reg >>  8) | (mem & 0xff000000)
    3   abcd   (reg      ) | (mem & 0x00000000)
    */
}

void InterpretedCPU::psxSWR(uint32_t code) {
    uint32_t addr = _oB_;
    uint32_t shift = addr & 3;
    addr ^= shift;
    // the preserved bytes aren't consumed, so msan doesn't check any of them
    uint32_t mem = PCSX::g_emulator->m_mem->read32Masked(addr, 0);

    PCSX::g_emulator->m_mem->write32Masked(addr, ((_rRt_) << SWR_SHIFT[shift]) | (mem & SWR_MASK[shift]),
                                           PCSX::Memory::rightByteMask(shift));

    /*
    Mem = 1234.  Reg = abcd
    0   abcd   (reg      ) | (mem & 0x00000000)
    1   bcd4   (reg <<  8) | (mem & 0x000000ff)
    2   cd34   (reg << 16) | (mem & 0x0000ffff)
    3   d234   (reg << 24) | (mem & 0x00ffffff)
    */
}

/*********************************************************
 * Moves between GPR and COPx                             *
 * Format:  OP rt, fs                                     *
 *********************************************************/
void InterpretedCPU::psxMFC0(uint32_t code) {
    // load delay = 1 latency
    if (!_Rt_) return;
    delayedLoadRef<int32_t>(_Rt_) = (int)m_regs.CP0.r[_Rd_];
}

void InterpretedCPU::psxCFC0(uint32_t code) {
    // load delay = 1 latency
    if (!_Rt_) return;
    delayedLoadRef<int32_t>(_Rt_) = (int)m_regs.CP0.r[_Rd_];
}

void InterpretedCPU::psxTestSWInts() {
    if (m_regs.CP0.n.Cause & m_regs.CP0.n.Status & 0x0300 && m_regs.CP0.n.Status & 0x1) {
        if (m_inBranchSlot) {
            m_pendingSWInt = true;
            return;
        }
        exception(m_regs.CP0.n.Cause, false);
    }
}

inline void InterpretedCPU::MTC0(int reg, uint32_t val) {
    //  PCSX::g_system->log(PCSX::LogClass::CPU, "MTC0 %d: %x\n", reg, val);
    switch (reg) {
        case 12:  // Status
            m_regs.CP0.n.Status = val;
            psxTestSWInts();
            break;

        case 13:  // Cause
            m_regs.CP0.n.Cause = val & ~(0xfc00);
            psxTestSWInts();
            break;

        default:
            m_regs.CP0.r[reg] = val;
            break;
    }
}

void InterpretedCPU::psxMTC0(uint32_t code) { MTC0(_Rd_, _rRt_); }
void InterpretedCPU::psxCTC0(uint32_t code) { MTC0(_Rd_, _rRt_); }

void InterpretedCPU::psxMFC2(uint32_t code) {
    gteDrainIfStalling();
    gteStall();
    // load delay = 1 latency
    if (!_Rt_) return;
    delayedLoadRef(_Rt_) = PCSX::g_emulator->m_gte->MFC2(code);
}

void InterpretedCPU::psxCFC2(uint32_t code) {
    gteDrainIfStalling();
    gteStall();
    // load delay = 1 latency
    if (!_Rt_) return;
    delayedLoadRef(_Rt_) = PCSX::g_emulator->m_gte->CFC2(code);
}

/*********************************************************
 * Unknown instruction (would generate an exception)     *
 *********************************************************/
void InterpretedCPU::psxNULL(uint32_t code) {
    m_regs.pc.raw() -= 4;
    PCSX::g_system->log(PCSX::LogClass::CPU, _("Encountered reserved opcode from 0x%08x, firing an exception\n"),
                        m_regs.pc);
    exception(Exception::ReservedInstruction, m_inDelaySlot);
}

void InterpretedCPU::psxSPECIAL(uint32_t code) { (*this.*(s_pPsxSPC[_Funct_]))(code); }

void InterpretedCPU::psxREGIMM(uint32_t code) { (*this.*(s_pPsxREG[_Rt_]))(code); }

// BCzF / BCzT. None of the coprocessors drives the condition input, so BCzF
// always branches and BCzT never does. Bit 1 of rt is ignored, and there is
// no branch-likely nullification. With SR.CUz clear, the branch raises a
// coprocessor unusable exception, COP0 included, even in kernel mode.
void InterpretedCPU::psxBCz(uint32_t code, unsigned z) {
    if ((m_regs.CP0.n.Status & (0x10000000 << z)) == 0) {
        m_regs.pc.raw() -= 4;
        exception((static_cast<uint32_t>(Exception::CoprocessorUnusable) << 2) | (z << 28), m_inDelaySlot);
        return;
    }
    markBranch();
    if ((_Rt_ & 1) == 0) doBranch(_BranchTarget_, false);
}

void InterpretedCPU::psxCOP0(uint32_t code) {
    if (_Rs_ == 8) {
        psxBCz(code, 0);
        return;
    }
    (*this.*(s_pPsxCP0[_Rs_]))(code);
}

void InterpretedCPU::psxCOP1(uint32_t code) {  // Accesses to the (nonexistent) FPU
    if (_Rs_ == 8) {
        psxBCz(code, 1);
        return;
    }
    // TODO: Verify that COP1 doesn't throw a coprocessor unusable exception
    // Supposedly the COP1/COP3 ops don't fire RI, and they're NOPs
    PCSX::g_system->log(PCSX::LogClass::CPU,
                        _("Attempted to use an invalid floating point instruction from 0x%08x. Ignored.\n"),
                        m_regs.pc - 4);
}

void InterpretedCPU::psxCOP2(uint32_t code) {
    if (_Rs_ == 8) {
        psxBCz(code, 2);
        return;
    }
    if ((m_regs.CP0.n.Status & 0x40000000) == 0) return;

    if ((code & 0x02000000) == 0) {
        (*this.*(s_pPsxCP2[_Funct_]))(code);
        return;
    }
    gteDrainIfStalling();
    gteStart(gteLatency(_Funct_));
    gteRunCommand(code, [](PCSX::R3000Acpu* cpu, uint32_t code) {
        auto self = static_cast<InterpretedCPU*>(cpu);
        (*self.*(self->s_pPsxCP2[_Funct_]))(code);
    });
}

void InterpretedCPU::psxCOP3(uint32_t code) {
    if (_Rs_ == 8) {
        psxBCz(code, 3);
        return;
    }
    PCSX::g_system->log(PCSX::LogClass::CPU, _("Attempted to access COP3 from 0x%08x. Ignored\n"), m_regs.pc - 4);
}

void InterpretedCPU::gteMove(uint32_t code) { (*this.*(s_pPsxCP2BSC[_Rs_]))(code); }

const InterpretedCPU::intFunc_t InterpretedCPU::s_psxBSC[64] = {
    &InterpretedCPU::psxSPECIAL, &InterpretedCPU::psxREGIMM, &InterpretedCPU::psxJ,    &InterpretedCPU::psxJAL,    // 00
    &InterpretedCPU::psxBEQ,     &InterpretedCPU::psxBNE,    &InterpretedCPU::psxBLEZ, &InterpretedCPU::psxBGTZ,   // 04
    &InterpretedCPU::psxADDI,    &InterpretedCPU::psxADDIU,  &InterpretedCPU::psxSLTI, &InterpretedCPU::psxSLTIU,  // 08
    &InterpretedCPU::psxANDI,    &InterpretedCPU::psxORI,    &InterpretedCPU::psxXORI, &InterpretedCPU::psxLUI,    // 0c
    &InterpretedCPU::psxCOP0,    &InterpretedCPU::psxCOP1,   &InterpretedCPU::psxCOP2, &InterpretedCPU::psxCOP3,   // 10
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,   // 14
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,   // 18
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,   // 1c
    &InterpretedCPU::psxLB,      &InterpretedCPU::psxLH,     &InterpretedCPU::psxLWL,  &InterpretedCPU::psxLW,     // 20
    &InterpretedCPU::psxLBU,     &InterpretedCPU::psxLHU,    &InterpretedCPU::psxLWR,  &InterpretedCPU::psxNULL,   // 24
    &InterpretedCPU::psxSB,      &InterpretedCPU::psxSH,     &InterpretedCPU::psxSWL,  &InterpretedCPU::psxSW,     // 28
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::psxSWR,  &InterpretedCPU::psxNULL,   // 2c
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::gteLWC2, &InterpretedCPU::psxNULL,   // 30
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,   // 34
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::gteSWC2, &InterpretedCPU::psxNULL,   // 38
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,   &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,   // 3c
};

const InterpretedCPU::intFunc_t InterpretedCPU::s_psxSPC[64] = {
    &InterpretedCPU::psxSLL,     &InterpretedCPU::psxNULL,  &InterpretedCPU::psxSRL,  &InterpretedCPU::psxSRA,   // 00
    &InterpretedCPU::psxSLLV,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxSRLV, &InterpretedCPU::psxSRAV,  // 04
    &InterpretedCPU::psxJR,      &InterpretedCPU::psxJALR,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 08
    &InterpretedCPU::psxSYSCALL, &InterpretedCPU::psxBREAK, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 0c
    &InterpretedCPU::psxMFHI,    &InterpretedCPU::psxMTHI,  &InterpretedCPU::psxMFLO, &InterpretedCPU::psxMTLO,  // 10
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 14
    &InterpretedCPU::psxMULT,    &InterpretedCPU::psxMULTU, &InterpretedCPU::psxDIV,  &InterpretedCPU::psxDIVU,  // 18
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 1c
    &InterpretedCPU::psxADD,     &InterpretedCPU::psxADDU,  &InterpretedCPU::psxSUB,  &InterpretedCPU::psxSUBU,  // 20
    &InterpretedCPU::psxAND,     &InterpretedCPU::psxOR,    &InterpretedCPU::psxXOR,  &InterpretedCPU::psxNOR,   // 24
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxSLT,  &InterpretedCPU::psxSLTU,  // 28
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 2c
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 30
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 34
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 38
    &InterpretedCPU::psxNULL,    &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 3c
};

// The REGIMM instruction is actually valid for every single value that the rt field might have
// If the lowest bit of the rt field is 1 then the instruction is a BGEZ, otherwise it's a BLTZ.
// If ((_Rt_ >> 4) & 0xF) == 0x8 then the instruction should link the return address to $ra, otherwise not.
const InterpretedCPU::intFunc_t InterpretedCPU::s_psxREG[32] = {
    &InterpretedCPU::psxBLTZ,   &InterpretedCPU::psxBGEZ,   &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ,  // 00
    &InterpretedCPU::psxBLTZ,   &InterpretedCPU::psxBGEZ,   &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ,  // 04
    &InterpretedCPU::psxBLTZ,   &InterpretedCPU::psxBGEZ,   &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ,  // 08
    &InterpretedCPU::psxBLTZ,   &InterpretedCPU::psxBGEZ,   &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ,  // 0c
    &InterpretedCPU::psxBLTZAL, &InterpretedCPU::psxBGEZAL, &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ,  // 10
    &InterpretedCPU::psxBLTZ,   &InterpretedCPU::psxBGEZ,   &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ,  // 14
    &InterpretedCPU::psxBLTZ,   &InterpretedCPU::psxBGEZ,   &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ,  // 18
    &InterpretedCPU::psxBLTZ,   &InterpretedCPU::psxBGEZ,   &InterpretedCPU::psxBLTZ, &InterpretedCPU::psxBGEZ   // 1c
};

const InterpretedCPU::intFunc_t InterpretedCPU::s_psxCP0[32] = {
    &InterpretedCPU::psxMFC0, &InterpretedCPU::psxNULL, &InterpretedCPU::psxCFC0, &InterpretedCPU::psxNULL,  // 00
    &InterpretedCPU::psxMTC0, &InterpretedCPU::psxNULL, &InterpretedCPU::psxCTC0, &InterpretedCPU::psxNULL,  // 04
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 08
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 0c
    &InterpretedCPU::psxRFE,  &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 10
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 14
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 18
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 1c
};

const InterpretedCPU::intFunc_t InterpretedCPU::s_psxCP2[64] = {
    &InterpretedCPU::gteMove, &InterpretedCPU::gteRTPS,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 00
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  &InterpretedCPU::gteNCLIP, &InterpretedCPU::psxNULL,  // 04
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 08
    &InterpretedCPU::gteOP,   &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 0c
    &InterpretedCPU::gteDPCS, &InterpretedCPU::gteINTPL, &InterpretedCPU::gteMVMVA, &InterpretedCPU::gteNCDS,  // 10
    &InterpretedCPU::gteCDP,  &InterpretedCPU::psxNULL,  &InterpretedCPU::gteNCDT,  &InterpretedCPU::psxNULL,  // 14
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::gteNCCS,  // 18
    &InterpretedCPU::gteCC,   &InterpretedCPU::psxNULL,  &InterpretedCPU::gteNCS,   &InterpretedCPU::psxNULL,  // 1c
    &InterpretedCPU::gteNCT,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 20
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 24
    &InterpretedCPU::gteSQR,  &InterpretedCPU::gteDCPL,  &InterpretedCPU::gteDPCT,  &InterpretedCPU::psxNULL,  // 28
    &InterpretedCPU::psxNULL, &InterpretedCPU::gteAVSZ3, &InterpretedCPU::gteAVSZ4, &InterpretedCPU::psxNULL,  // 2c
    &InterpretedCPU::gteRTPT, &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 30
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 34
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  &InterpretedCPU::psxNULL,  // 38
    &InterpretedCPU::psxNULL, &InterpretedCPU::gteGPF,   &InterpretedCPU::gteGPL,   &InterpretedCPU::gteNCCT,  // 3c
};

const InterpretedCPU::intFunc_t InterpretedCPU::s_psxCP2BSC[32] = {
    &InterpretedCPU::psxMFC2, &InterpretedCPU::psxNULL, &InterpretedCPU::psxCFC2, &InterpretedCPU::psxNULL,  // 00
    &InterpretedCPU::gteMTC2, &InterpretedCPU::psxNULL, &InterpretedCPU::gteCTC2, &InterpretedCPU::psxNULL,  // 04
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 08
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 0c
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 10
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 14
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 18
    &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL, &InterpretedCPU::psxNULL,  // 1c
};

/////////////////////////////////////////////
// PGXP wrapper functions
/////////////////////////////////////////////

void InterpretedCPU::pgxpPsxNULL(uint32_t code) {}

#define psxMTC2 gteMTC2
#define psxCTC2 gteCTC2
#define psxLWC2 gteLWC2
#define psxSWC2 gteSWC2

// Choose between debug and direct function
#ifdef PGXP_CPU_DEBUG
#define PGXP_PSX_FUNC_OP(pu, op, nReg) PGXP_psxTraceOp##nReg
#define PGXP_DBG_OP_E(op) DBG_E_##op,
#else
#define PGXP_PSX_FUNC_OP(pu, op, nReg) PGXP_##pu##_##op
#define PGXP_DBG_OP_E(op)
#endif

#define PGXP_INT_FUNC(pu, op)                               \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {       \
        PGXP_PSX_FUNC_OP(pu, op, )(PGXP_DBG_OP_E(op) code); \
        psx##op(code);                                      \
    }

#define PGXP_INT_FUNC_0_1(pu, op, test, nReg, reg1)                        \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {                      \
        if (test) {                                                        \
            psx##op(code);                                                 \
            return;                                                        \
        }                                                                  \
        uint32_t tempInstr = code;                                         \
        psx##op(code);                                                     \
        PGXP_PSX_FUNC_OP(pu, op, nReg)(PGXP_DBG_OP_E(op) tempInstr, reg1); \
    }

#define PGXP_INT_FUNC_1_0(pu, op, test, nReg, reg1)                   \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {                 \
        if (test) {                                                   \
            psx##op(code);                                            \
            return;                                                   \
        }                                                             \
        PGXP_PSX_FUNC_OP(pu, op, nReg)(PGXP_DBG_OP_E(op) code, reg1); \
        psx##op(code);                                                \
    }

#define PGXP_INT_FUNC_1_1(pu, op, test, nReg, reg1, reg2)                         \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {                             \
        if (test) {                                                               \
            psx##op(code);                                                        \
            return;                                                               \
        }                                                                         \
        uint32_t tempInstr = code;                                                \
        uint32_t temp2 = reg2;                                                    \
        const uint32_t pcBefore = m_regs.pc.raw();                                \
        psx##op(code);                                                            \
        if (m_regs.pc.raw() != pcBefore) return;                                  \
        PGXP_PSX_FUNC_OP(pu, op, nReg)(PGXP_DBG_OP_E(op) tempInstr, reg1, temp2); \
    }

#define PGXP_INT_FUNC_0_2(pu, op, test, nReg, reg1, reg2)                        \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {                            \
        if (test) {                                                              \
            psx##op(code);                                                       \
            return;                                                              \
        }                                                                        \
        uint32_t tempInstr = code;                                               \
        psx##op(code);                                                           \
        PGXP_PSX_FUNC_OP(pu, op, nReg)(PGXP_DBG_OP_E(op) tempInstr, reg1, reg2); \
    }

#define PGXP_INT_FUNC_2_0(pu, op, test, nReg, reg1, reg2)                          \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {                              \
        if (test) {                                                                \
            psx##op(code);                                                         \
            return;                                                                \
        }                                                                          \
        uint32_t tempInstr = code;                                                 \
        uint32_t temp1 = reg1;                                                     \
        uint32_t temp2 = reg2;                                                     \
        psx##op(code);                                                             \
        PGXP_PSX_FUNC_OP(pu, op, nReg)(PGXP_DBG_OP_E(op) tempInstr, temp1, temp2); \
    }

#define PGXP_INT_FUNC_2_1(pu, op, test, nReg, reg1, reg2, reg3)                          \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {                                    \
        if (test) {                                                                      \
            psx##op(code);                                                               \
            return;                                                                      \
        }                                                                                \
        uint32_t tempInstr = code;                                                       \
        uint32_t temp2 = reg2;                                                           \
        uint32_t temp3 = reg3;                                                           \
        const uint32_t pcBefore = m_regs.pc.raw();                                       \
        psx##op(code);                                                                   \
        if (m_regs.pc.raw() != pcBefore) return;                                         \
        PGXP_PSX_FUNC_OP(pu, op, nReg)(PGXP_DBG_OP_E(op) tempInstr, reg1, temp2, temp3); \
    }

#define PGXP_INT_FUNC_2_2(pu, op, test, nReg, reg1, reg2, reg3, reg4)                          \
    void InterpretedCPU::pgxpPsx##op(uint32_t code) {                                          \
        if (test) {                                                                            \
            psx##op(code);                                                                     \
            return;                                                                            \
        }                                                                                      \
        uint32_t tempInstr = code;                                                             \
        uint32_t temp3 = reg3;                                                                 \
        uint32_t temp4 = reg4;                                                                 \
        psx##op(code);                                                                         \
        PGXP_PSX_FUNC_OP(pu, op, nReg)(PGXP_DBG_OP_E(op) tempInstr, reg1, reg2, temp3, temp4); \
    }

// Rt = Rs op imm
PGXP_INT_FUNC_1_1(CPU, ADDI, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_1_1(CPU, ADDIU, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_1_1(CPU, ANDI, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_1_1(CPU, ORI, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_1_1(CPU, XORI, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_1_1(CPU, SLTI, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_1_1(CPU, SLTIU, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])

// Rt = imm
PGXP_INT_FUNC_0_1(CPU, LUI, !_Rt_, 1, m_regs.GPR.r[_Rt_])

// Rd = Rs op Rt
PGXP_INT_FUNC_2_1(CPU, ADD, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, ADDU, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, SUB, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, SUBU, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, AND, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, OR, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, XOR, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, NOR, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, SLT, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_1(CPU, SLTU, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])

// Hi/Lo = Rs op Rt
PGXP_INT_FUNC_2_2(CPU, MULT, 0, 4, m_regs.GPR.n.hi, m_regs.GPR.n.lo, m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_2(CPU, MULTU, 0, 4, m_regs.GPR.n.hi, m_regs.GPR.n.lo, m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_2(CPU, DIV, 0, 4, m_regs.GPR.n.hi, m_regs.GPR.n.lo, m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_2_2(CPU, DIVU, 0, 4, m_regs.GPR.n.hi, m_regs.GPR.n.lo, m_regs.GPR.r[_Rs_], m_regs.GPR.r[_Rt_])

// Mem[addr] = Rt
PGXP_INT_FUNC_1_1(CPU, SB, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, SH, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, SW, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, SWL, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, SWR, 0, 2, m_regs.GPR.r[_Rt_], _oB_)

// Rt = Mem[addr]
PGXP_INT_FUNC_1_1(CPU, LWL, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, LW, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, LWR, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, LH, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, LHU, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, LB, 0, 2, m_regs.GPR.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(CPU, LBU, 0, 2, m_regs.GPR.r[_Rt_], _oB_)

// Rd = Rt op Sa
PGXP_INT_FUNC_1_1(CPU, SLL, !_Rd_, 2, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_1_1(CPU, SRL, !_Rd_, 2, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_1_1(CPU, SRA, !_Rd_, 2, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rt_])

// Rd = Rt op Rs
PGXP_INT_FUNC_2_1(CPU, SLLV, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_2_1(CPU, SRLV, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])
PGXP_INT_FUNC_2_1(CPU, SRAV, !_Rd_, 3, m_regs.GPR.r[_Rd_], m_regs.GPR.r[_Rt_], m_regs.GPR.r[_Rs_])

PGXP_INT_FUNC_1_1(CPU, MFHI, !_Rd_, 2, m_regs.GPR.r[_Rd_], m_regs.GPR.n.hi)
PGXP_INT_FUNC_1_1(CPU, MTHI, 0, 2, m_regs.GPR.n.hi, m_regs.GPR.r[_Rd_])
PGXP_INT_FUNC_1_1(CPU, MFLO, !_Rd_, 2, m_regs.GPR.r[_Rd_], m_regs.GPR.n.lo)
PGXP_INT_FUNC_1_1(CPU, MTLO, 0, 2, m_regs.GPR.n.lo, m_regs.GPR.r[_Rd_])

// COP2 (GTE)
PGXP_INT_FUNC_1_1(GTE, MFC2, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.CP2D.r[_Rd_])
PGXP_INT_FUNC_1_1(GTE, CFC2, !_Rt_, 2, m_regs.GPR.r[_Rt_], m_regs.CP2C.r[_Rd_])
PGXP_INT_FUNC_1_1(GTE, MTC2, 0, 2, m_regs.CP2D.r[_Rd_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_1_1(GTE, CTC2, 0, 2, m_regs.CP2C.r[_Rd_], m_regs.GPR.r[_Rt_])

PGXP_INT_FUNC_1_1(GTE, LWC2, 0, 2, m_regs.CP2D.r[_Rt_], _oB_)
PGXP_INT_FUNC_1_1(GTE, SWC2, 0, 2, m_regs.CP2D.r[_Rt_], _oB_)

// COP0
PGXP_INT_FUNC_1_1(CP0, MFC0, !_Rd_, 2, m_regs.GPR.r[_Rt_], m_regs.CP0.r[_Rd_])
PGXP_INT_FUNC_1_1(CP0, CFC0, !_Rd_, 2, m_regs.GPR.r[_Rt_], m_regs.CP0.r[_Rd_])
PGXP_INT_FUNC_1_1(CP0, MTC0, !_Rt_, 2, m_regs.CP0.r[_Rd_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC_1_1(CP0, CTC0, !_Rt_, 2, m_regs.CP0.r[_Rd_], m_regs.GPR.r[_Rt_])
PGXP_INT_FUNC(CP0, RFE)

// end of PGXP

// Trace all functions using PGXP
const InterpretedCPU::intFunc_t InterpretedCPU::s_pgxpPsxBSC[64] = {
    &InterpretedCPU::psxSPECIAL,  &InterpretedCPU::psxREGIMM,     // 00
    &InterpretedCPU::psxJ,        &InterpretedCPU::psxJAL,        // 02
    &InterpretedCPU::psxBEQ,      &InterpretedCPU::psxBNE,        // 04
    &InterpretedCPU::psxBLEZ,     &InterpretedCPU::psxBGTZ,       // 06
    &InterpretedCPU::pgxpPsxADDI, &InterpretedCPU::pgxpPsxADDIU,  // 08
    &InterpretedCPU::pgxpPsxSLTI, &InterpretedCPU::pgxpPsxSLTIU,  // 0a
    &InterpretedCPU::pgxpPsxANDI, &InterpretedCPU::pgxpPsxORI,    // 0c
    &InterpretedCPU::pgxpPsxXORI, &InterpretedCPU::pgxpPsxLUI,    // 0e
    &InterpretedCPU::psxCOP0,     &InterpretedCPU::psxCOP1,       // 10
    &InterpretedCPU::psxCOP2,     &InterpretedCPU::psxCOP3,       // 12
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 14
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 16
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 18
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 1a
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 1c
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 1e
    &InterpretedCPU::pgxpPsxLB,   &InterpretedCPU::pgxpPsxLH,     // 20
    &InterpretedCPU::pgxpPsxLWL,  &InterpretedCPU::pgxpPsxLW,     // 22
    &InterpretedCPU::pgxpPsxLBU,  &InterpretedCPU::pgxpPsxLHU,    // 24
    &InterpretedCPU::pgxpPsxLWR,  &InterpretedCPU::pgxpPsxNULL,   // 26
    &InterpretedCPU::pgxpPsxSB,   &InterpretedCPU::pgxpPsxSH,     // 28
    &InterpretedCPU::pgxpPsxSWL,  &InterpretedCPU::pgxpPsxSW,     // 2a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 2c
    &InterpretedCPU::pgxpPsxSWR,  &InterpretedCPU::pgxpPsxNULL,   // 2e
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 30
    &InterpretedCPU::pgxpPsxLWC2, &InterpretedCPU::psxNULL,       // 32
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 34
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 36
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 38
    &InterpretedCPU::pgxpPsxSWC2, &InterpretedCPU::psxNULL,       // 3a
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 3c
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 3e
};

const InterpretedCPU::intFunc_t InterpretedCPU::s_pgxpPsxSPC[64] = {
    &InterpretedCPU::pgxpPsxSLL,  &InterpretedCPU::pgxpPsxNULL,   // 00
    &InterpretedCPU::pgxpPsxSRL,  &InterpretedCPU::pgxpPsxSRA,    // 02
    &InterpretedCPU::pgxpPsxSLLV, &InterpretedCPU::pgxpPsxNULL,   // 04
    &InterpretedCPU::pgxpPsxSRLV, &InterpretedCPU::pgxpPsxSRAV,   // 06
    &InterpretedCPU::psxJR,       &InterpretedCPU::psxJALR,       // 08
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 0a
    &InterpretedCPU::psxSYSCALL,  &InterpretedCPU::psxBREAK,      // 0c
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,       // 0e
    &InterpretedCPU::pgxpPsxMFHI, &InterpretedCPU::pgxpPsxMTHI,   // 10
    &InterpretedCPU::pgxpPsxMFLO, &InterpretedCPU::pgxpPsxMTLO,   // 12
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 14
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 16
    &InterpretedCPU::pgxpPsxMULT, &InterpretedCPU::pgxpPsxMULTU,  // 18
    &InterpretedCPU::pgxpPsxDIV,  &InterpretedCPU::pgxpPsxDIVU,   // 1a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 1c
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 1e
    &InterpretedCPU::pgxpPsxADD,  &InterpretedCPU::pgxpPsxADDU,   // 20
    &InterpretedCPU::pgxpPsxSUB,  &InterpretedCPU::pgxpPsxSUBU,   // 22
    &InterpretedCPU::pgxpPsxAND,  &InterpretedCPU::pgxpPsxOR,     // 24
    &InterpretedCPU::pgxpPsxXOR,  &InterpretedCPU::pgxpPsxNOR,    // 26
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 28
    &InterpretedCPU::pgxpPsxSLT,  &InterpretedCPU::pgxpPsxSLTU,   // 2a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 2c
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 2e
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 30
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 32
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 34
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 36
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 38
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 3a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 3c
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,   // 3e
};

const InterpretedCPU::intFunc_t InterpretedCPU::s_pgxpPsxCP0[32] = {
    &InterpretedCPU::pgxpPsxMFC0, &InterpretedCPU::pgxpPsxNULL,  // 00
    &InterpretedCPU::pgxpPsxCFC0, &InterpretedCPU::pgxpPsxNULL,  // 02
    &InterpretedCPU::pgxpPsxMTC0, &InterpretedCPU::pgxpPsxNULL,  // 04
    &InterpretedCPU::pgxpPsxCTC0, &InterpretedCPU::pgxpPsxNULL,  // 06
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 08
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 0a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 0c
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 0e
    &InterpretedCPU::pgxpPsxRFE,  &InterpretedCPU::pgxpPsxNULL,  // 10
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 12
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 14
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 16
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 18
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 1a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 1c
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 1e
};

const InterpretedCPU::intFunc_t InterpretedCPU::s_pgxpPsxCP2BSC[32] = {
    &InterpretedCPU::pgxpPsxMFC2, &InterpretedCPU::pgxpPsxNULL,  // 00
    &InterpretedCPU::pgxpPsxCFC2, &InterpretedCPU::pgxpPsxNULL,  // 02
    &InterpretedCPU::pgxpPsxMTC2, &InterpretedCPU::pgxpPsxNULL,  // 04
    &InterpretedCPU::pgxpPsxCTC2, &InterpretedCPU::pgxpPsxNULL,  // 06
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 08
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 0a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 0c
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 0e
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 10
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 12
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 14
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 16
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 18
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 1a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 1c
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 1e
};

// Trace memory functions only
const InterpretedCPU::intFunc_t InterpretedCPU::s_pgxpPsxBSCMem[64] = {
    &InterpretedCPU::psxSPECIAL,  &InterpretedCPU::psxREGIMM,    // 00
    &InterpretedCPU::psxJ,        &InterpretedCPU::psxJAL,       // 02
    &InterpretedCPU::psxBEQ,      &InterpretedCPU::psxBNE,       // 04
    &InterpretedCPU::psxBLEZ,     &InterpretedCPU::psxBGTZ,      // 06
    &InterpretedCPU::psxADDI,     &InterpretedCPU::psxADDIU,     // 08
    &InterpretedCPU::psxSLTI,     &InterpretedCPU::psxSLTIU,     // 0a
    &InterpretedCPU::psxANDI,     &InterpretedCPU::psxORI,       // 0c
    &InterpretedCPU::psxXORI,     &InterpretedCPU::psxLUI,       // 0e
    &InterpretedCPU::psxCOP0,     &InterpretedCPU::psxCOP1,      // 10
    &InterpretedCPU::psxCOP2,     &InterpretedCPU::psxCOP3,      // 12
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 14
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 16
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 18
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 1a
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 1c
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 1e
    &InterpretedCPU::pgxpPsxLB,   &InterpretedCPU::pgxpPsxLH,    // 20
    &InterpretedCPU::pgxpPsxLWL,  &InterpretedCPU::pgxpPsxLW,    // 22
    &InterpretedCPU::pgxpPsxLBU,  &InterpretedCPU::pgxpPsxLHU,   // 24
    &InterpretedCPU::pgxpPsxLWR,  &InterpretedCPU::pgxpPsxNULL,  // 26
    &InterpretedCPU::pgxpPsxSB,   &InterpretedCPU::pgxpPsxSH,    // 28
    &InterpretedCPU::pgxpPsxSWL,  &InterpretedCPU::pgxpPsxSW,    // 2a
    &InterpretedCPU::pgxpPsxNULL, &InterpretedCPU::pgxpPsxNULL,  // 2c
    &InterpretedCPU::pgxpPsxSWR,  &InterpretedCPU::pgxpPsxNULL,  // 2e
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 30
    &InterpretedCPU::pgxpPsxLWC2, &InterpretedCPU::psxNULL,      // 32
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 34
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 36
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 38
    &InterpretedCPU::pgxpPsxSWC2, &InterpretedCPU::psxNULL,      // 3a
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 3c
    &InterpretedCPU::psxNULL,     &InterpretedCPU::psxNULL,      // 3e
};

///////////////////////////////////////////

bool InterpretedCPU::Init() { return true; }
void InterpretedCPU::Reset() {
    R3000Acpu::Reset();
    m_nextIsDelaySlot = false;
    m_inDelaySlot = false;
    m_nextIsBranchSlot = false;
    m_inBranchSlot = false;
    m_pendingSWInt = false;
    m_delayedLoadInfo[0].active = false;
    m_delayedLoadInfo[1].active = false;
    m_delayedLoadInfo[0].pcActive = false;
    m_delayedLoadInfo[1].pcActive = false;
}
void InterpretedCPU::Execute() {
    ZoneScoped;
    while (hasToRun()) {
        const bool& debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                                .get<PCSX::Emulator::DebugSettings::Debug>();
        const bool& trace = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                                .get<PCSX::Emulator::DebugSettings::Trace>();
        const bool& skipISR = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                                  .get<PCSX::Emulator::DebugSettings::SkipISR>();
        // The cop0 debug unit is always live on silicon: a guest that arms DCIC gets the debug
        // path whether or not the debugger is enabled.
        if (debug || cop0Armed()) {
            if (!trace || (skipISR && m_inISR)) {
                execBlock<true, false>();
            } else {
                execBlock<true, true>();
            }
        } else {
            if (!trace || (skipISR && m_inISR)) {
                execBlock<false, false>();
            } else {
                execBlock<false, true>();
            }
        }
    }
}

PCSX::R3000Acpu::RunUntilResult InterpretedCPU::RunUntil(uint32_t stopPC, uint64_t cycleBudget) {
    ZoneScoped;
    /* Nesting would re-enter execBlock on top of a live one and shred the delayed-load
       ping-pong underneath it. The dangerous caller is a breakpoint invoker, which runs from
       Debug::process in the middle of execBlock's loop body; an ExecutionFlow event listener
       is fine, since those fire from hasToRun() between blocks, which is the same point
       UI::shellReached does its own wholesale register surgery from. */
    if (m_inRunUntil) return RunUntilResult::Reentered;
    m_inRunUntil = true;
    /* Even at a clean boundary a branch may have left a delay slot pending, and the callee
       must not inherit it. Park the bookkeeping and hand it back untouched. */
    const bool savedNextIsDelaySlot = m_nextIsDelaySlot;
    const bool savedInDelaySlot = m_inDelaySlot;
    const bool savedNextIsBranchSlot = m_nextIsBranchSlot;
    const uint32_t savedNextSlotBranchPC = m_nextSlotBranchPC;
    const unsigned savedCurrentDelayedLoad = m_currentDelayedLoad;
    const auto savedDelayedLoad0 = m_delayedLoadInfo[0];
    const auto savedDelayedLoad1 = m_delayedLoadInfo[1];
    m_nextIsDelaySlot = false;
    m_inDelaySlot = false;
    m_nextIsBranchSlot = false;
    m_delayedLoadInfo[0].active = false;
    m_delayedLoadInfo[1].active = false;
    m_delayedLoadInfo[0].pcActive = false;
    m_delayedLoadInfo[1].pcActive = false;

    /* exception() honours the first-chance mask by printing a line and calling
       g_system->pause(true), which stops the WHOLE emulator - including a -run that some
       other part of the harness is relying on. A call that faults is a result here, not an
       incident: we report the code, EPC and BadVAddr in the return value and leave the
       machine alone. Park the mask for the duration. */
    auto& debugSettings = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>();
    const uint32_t savedFirstChance = debugSettings.get<PCSX::Emulator::DebugSettings::FirstChanceException>();
    debugSettings.get<PCSX::Emulator::DebugSettings::FirstChanceException>() = 0;

    const uint64_t deadline = m_regs.cycle + cycleBudget;
    /* m_inISR is how we hear about an exception: exception() sets it unconditionally, and
       it covers the cop0 vectors too, which enumerating the four vector addresses wouldn't.
       We own it for the duration and hand it back the way we found it. */
    const bool wasInISR = m_inISR;
    m_inISR = false;
    /* Handed back on every way out, including an exception thrown by a Lua memory callback
       from inside execBlock, or the next call would see m_inRunUntil still set. */
    struct HandBack {
        std::function<void()> f;
        ~HandBack() { f(); }
    } handBack{[&]() {
        debugSettings.get<PCSX::Emulator::DebugSettings::FirstChanceException>() = savedFirstChance;
        m_inISR = wasInISR;
        m_nextIsDelaySlot = savedNextIsDelaySlot;
        m_inDelaySlot = savedInDelaySlot;
        m_nextIsBranchSlot = savedNextIsBranchSlot;
        m_nextSlotBranchPC = savedNextSlotBranchPC;
        m_currentDelayedLoad = savedCurrentDelayedLoad;
        m_delayedLoadInfo[0] = savedDelayedLoad0;
        m_delayedLoadInfo[1] = savedDelayedLoad1;
        m_inRunUntil = false;
    }};
    auto result = RunUntilResult::OutOfCycles;
    while (true) {
        if (m_regs.pc == stopPC) {
            /* A load in the return jump's delay slot is still in flight here; retire it, or
               the caller reads the register before it lands and the hand back drops it. */
            m_currentDelayedLoad ^= 1;
            flushCurrentDelayedLoad();
            result = RunUntilResult::Reached;
            break;
        }
        if (m_regs.cycle >= deadline) break;
        execBlock<false, false, true>();
        if (m_inISR) {
            result = RunUntilResult::Exception;
            break;
        }
    }
    return result;
}

void InterpretedCPU::Clear(uint32_t Addr, uint32_t Size) {
    for (auto i = 0; i < Size; i += 4) {
        flushICacheLine(Addr);
        Addr += 16;
    }
}

void InterpretedCPU::Shutdown() {}
// interpreter execution
template <bool debug, bool trace, bool detached>
inline void InterpretedCPU::execBlock() {
    bool ranDelaySlot = false;
    bool stackTripped = false;
    do {
        if (m_nextIsDelaySlot) {
            m_inDelaySlot = true;
            m_nextIsDelaySlot = false;
        }
        m_inBranchSlot = m_nextIsBranchSlot;
        m_slotBranchPC = m_nextSlotBranchPC;
        m_nextIsBranchSlot = false;
        // TODO: throw an exception here if pc is out of range
        const uint32_t pc = m_regs.pc;
        // TODO: throw an exception here if we don't have a pointer
        uint32_t code = readICache(pc);

        m_regs.code = code;

        if constexpr (trace) {
            PCSX::g_emulator->m_cpuTrace->capture(pc, code);
        }

        m_regs.pc.increment();
        m_regs.cycle += PCSX::Emulator::BIAS;
        if ((m_regs.pc & 0xffc00000) == 0xbfc00000) m_regs.cycle += PCSX::Emulator::BIAS * 10;

        cIntFunc_t func = s_pPsxBSC[code >> 26];
        (*this.*func)(code);

        m_currentDelayedLoad ^= 1;
        flushCurrentDelayedLoad();
        // Checked here rather than in the instructions that write $sp so that
        // every write is covered, delayed loads included. Exception handlers
        // run on their own stacks, so nothing is checked until the rfe.
        if (m_stackBounds.active && !m_inISR) [[unlikely]] {
            const uint32_t sp = m_regs.GPR.n.sp;
            if (sp != m_stackBounds.lastSP) {
                m_stackBounds.lastSP = sp;
                if ((sp < m_stackBounds.low) || (sp > m_stackBounds.high)) {
                    stackBoundsViolation(pc, sp);
                    stackTripped = true;
                }
            }
        }
        if (m_gtePendingCount) gteTickWrites();
        auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
        bool fromLink = false;
        if (delayedLoad.pcActive) {
            m_regs.pc.raw() = delayedLoad.pcValue;
            fromLink = delayedLoad.fromLink;
            delayedLoad.pcActive = false;
            delayedLoad.fromLink = false;
        }
        m_inBranchSlot = false;
        if (m_pendingSWInt) {
            m_pendingSWInt = false;
            psxTestSWInts();
        }
        if (m_inDelaySlot) {
            m_inDelaySlot = false;
            ranDelaySlot = true;
            InterceptBIOS<true>(m_regs.pc);
            /* A detached block runs the cpu with nothing else attached to it: no counters,
               no scheduled interrupts, and in particular no Counters::update() -> SPU
               waitForGoal(), which is a real-time sleep. RunUntil() wants the cpu alone. */
            if constexpr (!detached) branchTest();
        }
        if constexpr (debug) {
            uint32_t newPC = m_regs.pc;
            uint32_t newCode = readICache(newPC);
            PCSX::g_emulator->m_debug->process(pc, newPC, code, newCode, fromLink);
        }
        /* A detached block returns per instruction, like a debug one. Block granularity
           looks tempting here and is wrong: this loop only ends on a retired delay slot, so
           straight-line code with no branches never yields. Guest code that runs off into
           blank RAM executes nops until it happens to meet a branch, which measured at
           262113 instructions in one call - long past any cycle budget, and long past the
           exception that sent it there. */
        if constexpr (debug || detached) break;
        // Stop on the offending instruction, as a breakpoint would: the paused cpu then sits right
        // after it, and nothing further in the block can report again.
        if (stackTripped) [[unlikely]] break;
    } while (!ranDelaySlot && !cop0Armed());
}

void InterpretedCPU::SetPGXPMode(uint32_t pgxpMode) {
    switch (pgxpMode) {
        case 0:  // PGXP_MODE_DISABLED:
            s_pPsxBSC = s_psxBSC;
            s_pPsxSPC = s_psxSPC;
            s_pPsxREG = s_psxREG;
            s_pPsxCP0 = s_psxCP0;
            s_pPsxCP2 = s_psxCP2;
            s_pPsxCP2BSC = s_psxCP2BSC;
            break;
        case 1:  // PGXP_MODE_MEM:
            s_pPsxBSC = s_pgxpPsxBSCMem;
            s_pPsxSPC = s_psxSPC;
            s_pPsxREG = s_psxREG;
            s_pPsxCP0 = s_pgxpPsxCP0;
            s_pPsxCP2 = s_psxCP2;
            s_pPsxCP2BSC = s_pgxpPsxCP2BSC;
            break;
        case 2:  // PGXP_MODE_FULL:
            s_pPsxBSC = s_pgxpPsxBSC;
            s_pPsxSPC = s_pgxpPsxSPC;
            s_pPsxREG = s_psxREG;
            s_pPsxCP0 = s_pgxpPsxCP0;
            s_pPsxCP2 = s_psxCP2;
            s_pPsxCP2BSC = s_pgxpPsxCP2BSC;
            break;
    }

    // reset to ensure new func tables are used
    InterpretedCPU::Reset();
}

std::unique_ptr<PCSX::R3000Acpu> PCSX::Cpus::getInterpreted() {
    return std::unique_ptr<PCSX::R3000Acpu>(new InterpretedCPU());
}
