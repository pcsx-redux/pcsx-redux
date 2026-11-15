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
#include <array>

#include "core/gte.h"
#define COP2_CONTROL_OFFSET(reg) ((uintptr_t) & m_regs.CP2C.r[(reg)] - (uintptr_t)this)
#define COP2_DATA_OFFSET(reg) ((uintptr_t) & m_regs.CP2D.r[(reg)] - (uintptr_t)this)

#define GTE_WRITE_VALUE_OFFSET(slot) ((uintptr_t) & m_gteWriteValues[(slot)] - (uintptr_t)this)
#define GTE_PENDING_OFFSET(i, field) ((uintptr_t) & m_gtePending[(i)].field - (uintptr_t)this)
#define GTE_PENDING_COUNT_OFFSET ((uintptr_t) & m_gtePendingCount - (uintptr_t)this)

void DynaRecCPU::drainGTEWrites() {
    for (unsigned i = 0; i < m_gtePendingCount; i++) gteCommit(m_gtePending[i]);
    m_gtePendingCount = 0;
}

// Writes w0 to a GTE register the way GTE::MTC2 and GTE::CTC2 do. Clobbers w0 to w3.
void DynaRecCPU::emitGTEWrite(bool ctrl, unsigned reg) {
    if (ctrl) {
        switch (reg) {
            case 4:  // Signed 16-bit registers, read back sign-extended
            case 12:
            case 20:
            case 26:
            case 27:
            case 29:
            case 30:
                gen.Sxth(w0, w0);
                break;

            case 31:  // FLAG: low 12 bits read as 0, bit 31 is the OR of the error bits
                gen.And(w0, w0, 0x7ffff000);
                gen.Orr(w1, w0, 0x80000000);
                gen.Mov(w2, 0x7f87e000);
                gen.Tst(w0, w2);
                gen.Csel(w0, w1, w0, ne);
                break;
        }
        gen.Str(w0, MemOperand(contextPointer, COP2_CONTROL_OFFSET(reg)));
        return;
    }

    switch (reg) {
        case 15:                                                            // SXYP
            gen.Ldr(x1, MemOperand(contextPointer, COP2_DATA_OFFSET(13)));  // SXY0 = SXY1 and SXY1 = SXY2
            gen.Str(x1, MemOperand(contextPointer, COP2_DATA_OFFSET(12)));
            break;

        case 28:  // IRGB sets IR1-IR3
            gen.And(w1, w0, 0x1f);
            gen.Lsl(w1, w1, 7);
            gen.Str(w1, MemOperand(contextPointer, COP2_DATA_OFFSET(9)));
            gen.Lsl(w1, w0, 2);
            gen.And(w1, w1, 0xf80);
            gen.Str(w1, MemOperand(contextPointer, COP2_DATA_OFFSET(10)));
            gen.Lsr(w1, w0, 3);
            gen.And(w1, w1, 0xf80);
            gen.Str(w1, MemOperand(contextPointer, COP2_DATA_OFFSET(11)));
            break;

        case 30:  // LZCS sets LZCR
            gen.Eor(w1, w0, Operand(w0, ASR, 31));
            gen.Clz(w1, w1);
            gen.Str(w1, MemOperand(contextPointer, COP2_DATA_OFFSET(31)));
            break;

        case 31:  // LZCR is read-only
            return;
    }
    // SXYP itself is stored as SXY2
    gen.Str(w0, MemOperand(contextPointer, COP2_DATA_OFFSET(reg == 15 ? 14 : reg)));
}

void DynaRecCPU::emitGTECommit(const GTEQueuedWrite& write) {
    if (write.isConst) {
        gen.Mov(w0, write.value);
    } else {
        gen.Ldr(w0, MemOperand(contextPointer, GTE_WRITE_VALUE_OFFSET(write.slot)));
    }
    emitGTEWrite(write.ctrl, write.reg);
}

// At most two writes are in flight when a new one issues, so this only commits on a path the tick did not cover.
void DynaRecCPU::makeRoomForGTEWrite() {
    auto& q = m_gteQueue;
    if (q.count < 3) return;
    emitGTECommit(q.writes[0]);
    for (unsigned i = 1; i < q.count; i++) q.writes[i - 1] = q.writes[i];
    q.count--;
}

void DynaRecCPU::queueGTEWrite(bool ctrl, unsigned reg, bool isConst, uint32_t value, Register src) {
    auto& q = m_gteQueue;
    unsigned used = 0;
    for (unsigned i = 0; i < q.count; i++) used |= 1 << q.writes[i].slot;
    uint8_t slot = 0;
    while (used & (1 << slot)) slot++;
    if (!isConst) gen.Str(src, MemOperand(contextPointer, GTE_WRITE_VALUE_OFFSET(slot)));
    q.writes[q.count++] = {value, uint8_t(reg), slot, 3, ctrl, isConst};
}

// Runs after each compiled instruction, like the interpreter's tick after each executed one.
void DynaRecCPU::tickGTEWrites() {
    auto& q = m_gteQueue;
    for (unsigned i = 0; i < q.count; i++) q.writes[i].countdown--;
    while (q.count && q.writes[0].countdown == 0) {
        emitGTECommit(q.writes[0]);
        for (unsigned i = 1; i < q.count; i++) q.writes[i - 1] = q.writes[i];
        q.count--;
    }
}

// Copies the writes in flight to m_gtePending, starting at index 0.
void DynaRecCPU::emitGTESpillQueue() {
    const auto& q = m_gteQueue;
    for (unsigned i = 0; i < q.count; i++) {
        const auto& w = q.writes[i];
        if (w.isConst) {
            gen.Mov(w0, w.value);
        } else {
            gen.Ldr(w0, MemOperand(contextPointer, GTE_WRITE_VALUE_OFFSET(w.slot)));
        }
        gen.Str(w0, MemOperand(contextPointer, GTE_PENDING_OFFSET(i, value)));
        gen.Mov(w0, w.reg);
        gen.Str(w0, MemOperand(contextPointer, GTE_PENDING_OFFSET(i, reg)));
        gen.Mov(w0, w.ctrl ? 1 : 0);
        gen.Strb(w0, MemOperand(contextPointer, GTE_PENDING_OFFSET(i, ctrl)));
        gen.Mov(w0, w.countdown);
        gen.Str(w0, MemOperand(contextPointer, GTE_PENDING_OFFSET(i, countdown)));
    }
    gen.Mov(w0, q.count);
    gen.Str(w0, MemOperand(contextPointer, GTE_PENDING_COUNT_OFFSET));
}

// Leaves the writes still in flight in m_gtePending for whatever runs next, or commits them all with commitAll,
// up to two instructions early.
void DynaRecCPU::emitGTEFlushQueue(bool commitAll) {
    const auto& q = m_gteQueue;
    if (q.count == 0) return;
    if (commitAll) {
        for (unsigned i = 0; i < q.count; i++) emitGTECommit(q.writes[i]);
    } else {
        emitGTESpillQueue();
    }
}

void DynaRecCPU::recCOP2(uint32_t code) {
    if (_Rs_ == 8) {
        recBCz(code);
        return;
    }
    emitCu2Check();
    const auto func = m_recGTE[code & 0x3f];  // Look up the opcode in our decoding LUT
    auto& q = m_gteQueue;
    if ((code & 0x02000000) == 0 || q.count == 0 || func == &DynaRecCPU::recUnknown) {
        (*this.*func)(code);  // Jump into the handler to recompile it
        return;
    }

    // A GTE command with writes in flight: hand them to m_gtePending and run the command through the shared model
    emitGTESpillQueue();
    q.count = 0;
    loadThisPointer(arg1.X());
    gen.Mov(arg2, code);
    call(gteCommandWrapper);
}

namespace {

using GTECommand = void (PCSX::GTE::*)(uint32_t);
constexpr auto c_gteCommands = [] {
    std::array<GTECommand, 64> t{};
    t[0x01] = &PCSX::GTE::RTPS;
    t[0x06] = &PCSX::GTE::NCLIP;
    t[0x0c] = &PCSX::GTE::OP;
    t[0x10] = &PCSX::GTE::DPCS;
    t[0x11] = &PCSX::GTE::INTPL;
    t[0x12] = &PCSX::GTE::MVMVA;
    t[0x13] = &PCSX::GTE::NCDS;
    t[0x14] = &PCSX::GTE::CDP;
    t[0x16] = &PCSX::GTE::NCDT;
    t[0x1b] = &PCSX::GTE::NCCS;
    t[0x1c] = &PCSX::GTE::CC;
    t[0x1e] = &PCSX::GTE::NCS;
    t[0x20] = &PCSX::GTE::NCT;
    t[0x28] = &PCSX::GTE::SQR;
    t[0x29] = &PCSX::GTE::DCPL;
    t[0x2a] = &PCSX::GTE::DPCT;
    t[0x2d] = &PCSX::GTE::AVSZ3;
    t[0x2e] = &PCSX::GTE::AVSZ4;
    t[0x30] = &PCSX::GTE::RTPT;
    t[0x3d] = &PCSX::GTE::GPF;
    t[0x3e] = &PCSX::GTE::GPL;
    t[0x3f] = &PCSX::GTE::NCCT;
    return t;
}();

}  // namespace

void DynaRecCPU::gteCommandWrapper(DynaRecCPU* that, uint32_t code) {
    that->gteRunCommand(code, [](PCSX::R3000Acpu*, uint32_t code) {
        auto command = c_gteCommands[code & 0x3f];
        if (command) (PCSX::g_emulator->m_gte.get()->*command)(code);
    });
}

void DynaRecCPU::recGTEMove(uint32_t code) {
    switch (_Rs_) {
        case 0:
            recMFC2(code);
            break;
        case 2:
            recCFC2(code);
            break;
        case 4:
            recMTC2(code);
            break;
        case 6:
            recCTC2(code);
            break;
        default:
            recUnknown(code);
            break;
    }
}

void DynaRecCPU::recCTC2(uint32_t code) {
    makeRoomForGTEWrite();
    if (m_gprs[_Rt_].isConst()) {
        queueGTEWrite(true, _Rd_, true, m_gprs[_Rt_].val, w0);
    } else {
        allocateReg(_Rt_);
        queueGTEWrite(true, _Rd_, false, 0, m_gprs[_Rt_].allocatedReg);
    }
}

void DynaRecCPU::recMTC2(uint32_t code) {
    makeRoomForGTEWrite();
    if (m_gprs[_Rt_].isConst()) {
        queueGTEWrite(false, _Rd_, true, m_gprs[_Rt_].val, w0);
    } else {
        allocateReg(_Rt_);
        queueGTEWrite(false, _Rd_, false, 0, m_gprs[_Rt_].allocatedReg);
    }
}

static uint32_t MFC2Wrapper(int reg) { return PCSX::g_emulator->m_gte->MFC2(reg); }

// Read a COP2 data register into "dest", applying the sign/zero extension quirks the register file has.
// Note that this can emit a call for IRGB/ORGB, so "dest" must not be a register the allocator might spill.
void DynaRecCPU::loadGTEDataRegister(Register dest, int index) {
    switch (index) {
        case 1:
        case 3:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
            gen.Ldrsh(dest, MemOperand(contextPointer, COP2_DATA_OFFSET(index)));
            break;

        case 7:
        case 16:
        case 17:
        case 18:
        case 19:
            gen.Ldrh(dest, MemOperand(contextPointer, COP2_DATA_OFFSET(index)));
            break;

        case 15:  // Return SXY2 from SXYP
            gen.Ldr(dest, MemOperand(contextPointer, COP2_DATA_OFFSET(14)));
            break;

        case 28:
        case 29:  // Fallback for IRGB/ORGB
            gen.Mov(arg1, index);
            call(MFC2Wrapper);  // result in w0
            gen.Mov(dest, w0);
            break;

        default:
            gen.Ldr(dest, MemOperand(contextPointer, COP2_DATA_OFFSET(index)));
            break;
    }
}

void DynaRecCPU::recMFC2(uint32_t code) {
    if (!_Rt_) {
        // Reading IRGB/ORGB has side effects, so it still has to happen even with the result thrown away
        if (_Rd_ == 28 || _Rd_ == 29) {
            gen.Mov(arg1, _Rd_);
            call(MFC2Wrapper);
        }
        return;
    }

    const auto loadDelayDependency = getLoadDelayDependencyType(_Rt_);
    if (loadDelayDependency != LoadDelayDependencyType::NoDependency) {
        loadGTEDataRegister(w0, _Rd_);
        storeDelayedLoad(_Rt_, loadDelayDependency);
        return;
    }

    // If we won't emulate the load delay, make sure to cancel any pending loads that might trample the value
    maybeCancelDelayedLoad(_Rt_);
    allocateRegWithoutLoad(_Rt_);
    m_gprs[_Rt_].setWriteback(true);

    switch (_Rd_) {
        // Fallback for IRGB/ORGB. Can't use loadGTEDataRegister for these because the call might unallocate $rt
        case 28:
        case 29:
            gen.Mov(arg1, _Rd_);
            call(MFC2Wrapper);  // result in w0

            allocateRegWithoutLoad(_Rt_);  // Reallocate the reg in case the call thrashed it
            m_gprs[_Rt_].setWriteback(true);
            gen.Mov(m_gprs[_Rt_].allocatedReg, w0);
            break;

        default:
            loadGTEDataRegister(m_gprs[_Rt_].allocatedReg, _Rd_);
            break;
    }
}

void DynaRecCPU::recCFC2(uint32_t code) {
    if (!_Rt_) return;

    const auto loadDelayDependency = getLoadDelayDependencyType(_Rt_);
    if (loadDelayDependency != LoadDelayDependencyType::NoDependency) {
        gen.Ldr(w0, MemOperand(contextPointer, COP2_CONTROL_OFFSET(_Rd_)));
        storeDelayedLoad(_Rt_, loadDelayDependency);
        return;
    }

    // If we won't emulate the load delay, make sure to cancel any pending loads that might trample the value
    maybeCancelDelayedLoad(_Rt_);
    allocateRegWithoutLoad(_Rt_);
    m_gprs[_Rt_].setWriteback(true);
    gen.Ldr(m_gprs[_Rt_].allocatedReg, MemOperand(contextPointer, COP2_CONTROL_OFFSET(_Rd_)));
}

void DynaRecCPU::recLWC2(uint32_t code) {
    emitCu2Check();
    makeRoomForGTEWrite();
    if (m_gprs[_Rs_].isConst()) {  // Store address in arg1
        gen.Mov(arg1, m_gprs[_Rs_].val + _Imm_);
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);
    }

    call(read32Wrapper);
    queueGTEWrite(false, _Rt_, false, 0, w0);
}

void DynaRecCPU::recSWC2(uint32_t code) {
    emitCu2Check();
    gen.Mov(arg1, _Rt_);
    call(MFC2Wrapper);  // Fetch the COP2 data reg in w0
    gen.Mov(arg2, w0);  // Value to write in arg2

    // Address in arg1
    if (m_gprs[_Rs_].isConst()) {
        gen.Mov(arg1, m_gprs[_Rs_].val + _Imm_);
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg1, m_gprs[_Rs_].allocatedReg, _Imm_);
    }

    call(write32Wrapper);
}

#define GTE_FALLBACK(name)                                                                          \
    static void name##Wrapper(uint32_t instruction) { PCSX::g_emulator->m_gte->name(instruction); } \
                                                                                                    \
    void DynaRecCPU::rec##name(uint32_t code) {                                                     \
        gen.Mov(arg1, m_regs.code);                                                                 \
        call(name##Wrapper);                                                                        \
    }

GTE_FALLBACK(AVSZ3);
GTE_FALLBACK(AVSZ4);
GTE_FALLBACK(CC);
GTE_FALLBACK(CDP);
GTE_FALLBACK(DCPL);
GTE_FALLBACK(DPCS);
GTE_FALLBACK(DPCT);
GTE_FALLBACK(GPF);
GTE_FALLBACK(GPL);
GTE_FALLBACK(INTPL);
GTE_FALLBACK(MVMVA);
GTE_FALLBACK(NCCS);
GTE_FALLBACK(NCCT);
GTE_FALLBACK(NCDS);
GTE_FALLBACK(NCDT);
GTE_FALLBACK(NCLIP);
GTE_FALLBACK(NCS);
GTE_FALLBACK(NCT);
GTE_FALLBACK(OP);
GTE_FALLBACK(RTPS);
GTE_FALLBACK(RTPT);
GTE_FALLBACK(SQR);

#endif  // DYNAREC_X86_64
