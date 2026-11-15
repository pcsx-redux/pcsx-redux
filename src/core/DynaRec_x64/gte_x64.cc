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
#include <array>

#include "core/gte.h"
#define COP2_CONTROL_OFFSET(reg) ((uintptr_t) & m_regs.CP2C.r[(reg)] - (uintptr_t)this)
#define COP2_DATA_OFFSET(reg) ((uintptr_t) & m_regs.CP2D.r[(reg)] - (uintptr_t)this)

#define GTE_WRITE_VALUE_OFFSET(slot) ((uintptr_t) & m_gteWriteValues[(slot)] - (uintptr_t)this)
#define GTE_WRITE_LIVE_OFFSET ((uintptr_t) & m_gteWriteLive - (uintptr_t)this)
#define GTE_PENDING_OFFSET(i, field) ((uintptr_t) & m_gtePending[(i)].field - (uintptr_t)this)
#define GTE_PENDING_COUNT_OFFSET ((uintptr_t) & m_gtePendingCount - (uintptr_t)this)

void DynaRecCPU::drainGTEWrites() {
    for (unsigned i = 0; i < m_gtePendingCount; i++) gteCommit(m_gtePending[i]);
    m_gtePendingCount = 0;
}

// Writes eax to a GTE register the way GTE::MTC2 and GTE::CTC2 do. Clobbers rax, rcx and rdx.
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
                gen.movsx(eax, ax);
                break;

            case 31:  // FLAG: low 12 bits read as 0, bit 31 is the OR of the error bits
                gen.and_(eax, 0x7ffff000);
                gen.mov(ecx, eax);
                gen.or_(ecx, 0x80000000);
                gen.test(eax, 0x7f87e000);
                gen.cmovnz(eax, ecx);
                break;
        }
        gen.mov(dword[contextPointer + COP2_CONTROL_OFFSET(reg)], eax);
        return;
    }

    switch (reg) {
        case 15:                                                         // SXYP
            gen.mov(rcx, qword[contextPointer + COP2_DATA_OFFSET(13)]);  // SXY0 = SXY1 and SXY1 = SXY2
            gen.mov(qword[contextPointer + COP2_DATA_OFFSET(12)], rcx);
            gen.mov(dword[contextPointer + COP2_DATA_OFFSET(14)], eax);  // SXY2 = val
            break;

        case 28:  // IRGB sets IR1-IR3
            gen.mov(ecx, eax);
            gen.and_(ecx, 0x1f);
            gen.shl(ecx, 7);
            gen.mov(dword[contextPointer + COP2_DATA_OFFSET(9)], ecx);
            gen.lea(ecx, dword[4 * rax]);
            gen.and_(ecx, 0xf80);
            gen.mov(dword[contextPointer + COP2_DATA_OFFSET(10)], ecx);
            gen.mov(ecx, eax);
            gen.shr(ecx, 3);
            gen.and_(ecx, 0xf80);
            gen.mov(dword[contextPointer + COP2_DATA_OFFSET(11)], ecx);
            break;

        case 30:  // LZCS sets LZCR
            gen.mov(ecx, eax);
            gen.mov(edx, eax);
            gen.sar(edx, 31);
            gen.xor_(ecx, edx);
            if (gen.hasLZCNT) {
                gen.lzcnt(ecx, ecx);
            } else {
                gen.bsr(ecx, ecx);
                gen.mov(edx, 63);
                gen.cmovz(ecx, edx);
                gen.xor_(ecx, 31);
            }
            gen.mov(dword[contextPointer + COP2_DATA_OFFSET(31)], ecx);
            break;

        case 31:  // LZCR is read-only
            return;
    }
    gen.mov(dword[contextPointer + COP2_DATA_OFFSET(reg)], eax);
}

// Commits a queued write. With checkLive, only if a stall has not committed it already.
void DynaRecCPU::emitGTECommit(const GTEQueuedWrite& write, bool checkLive) {
    Label skip;
    if (checkLive) {
        gen.test(Xbyak::util::byte[contextPointer + GTE_WRITE_LIVE_OFFSET], 1 << write.slot);
        gen.jz(skip, Xbyak::CodeGenerator::T_NEAR);
    }
    if (write.isConst) {
        gen.mov(eax, write.value);
    } else {
        gen.mov(eax, dword[contextPointer + GTE_WRITE_VALUE_OFFSET(write.slot)]);
    }
    emitGTEWrite(write.ctrl, write.reg);
    gen.L(skip);
}

// At most two writes are in flight when a new one issues, so this only commits on a path the tick did not cover.
void DynaRecCPU::makeRoomForGTEWrite() {
    auto& q = m_gteQueue;
    if (q.count < 3) return;
    emitGTECommit(q.writes[0], q.writes[0].uncertain);
    for (unsigned i = 1; i < q.count; i++) q.writes[i - 1] = q.writes[i];
    q.count--;
}

void DynaRecCPU::queueGTEWrite(bool ctrl, unsigned reg, bool isConst, uint32_t value, Xbyak::Reg32 src) {
    auto& q = m_gteQueue;
    unsigned used = 0;
    for (unsigned i = 0; i < q.count; i++) used |= 1 << q.writes[i].slot;
    uint8_t slot = 0;
    while (used & (1 << slot)) slot++;
    if (!isConst) gen.mov(dword[contextPointer + GTE_WRITE_VALUE_OFFSET(slot)], src);
    q.writes[q.count++] = {value, uint8_t(reg), slot, 3, ctrl, isConst, false};
}

// Runs after each compiled instruction, like the interpreter's tick after each executed one.
void DynaRecCPU::tickGTEWrites() {
    auto& q = m_gteQueue;
    for (unsigned i = 0; i < q.count; i++) q.writes[i].countdown--;
    while (q.count && q.writes[0].countdown == 0) {
        emitGTECommit(q.writes[0], q.writes[0].uncertain);
        for (unsigned i = 1; i < q.count; i++) q.writes[i - 1] = q.writes[i];
        q.count--;
    }
}

// Leaves the writes still in flight in m_gtePending for whatever runs next. A write whose liveness depends on a
// stall, and every write when commitAll is set, is committed instead, up to two instructions early.
void DynaRecCPU::emitGTEFlushQueue(bool commitAll) {
    const auto& q = m_gteQueue;
    unsigned queued = 0;
    for (unsigned i = 0; i < q.count; i++) {
        const auto& w = q.writes[i];
        if (commitAll || w.uncertain) {
            emitGTECommit(w, w.uncertain);
            continue;
        }
        if (w.isConst) {
            gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(queued, value)], w.value);
        } else {
            gen.mov(eax, dword[contextPointer + GTE_WRITE_VALUE_OFFSET(w.slot)]);
            gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(queued, value)], eax);
        }
        gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(queued, reg)], w.reg);
        gen.mov(Xbyak::util::byte[contextPointer + GTE_PENDING_OFFSET(queued, ctrl)], w.ctrl ? 1 : 0);
        gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(queued, countdown)], w.countdown);
        queued++;
    }
    if (queued) gen.mov(dword[contextPointer + GTE_PENDING_COUNT_OFFSET], queued);
}

// Waits for the running GTE command. Block cycles are only added to the cycle counter at the end of a block, so
// the current cycle is computed from the position of this instruction in the block. Leaves it in rax. A wait gives
// the writes in flight time to land, so they are committed on that path.
void DynaRecCPU::emitGTEStall() {
    auto& q = m_gteQueue;
    uint8_t certain = 0;
    for (unsigned i = 0; i < q.count; i++) {
        if (!q.writes[i].uncertain) certain |= 1 << q.writes[i].slot;
    }
    if (certain) gen.or_(Xbyak::util::byte[contextPointer + GTE_WRITE_LIVE_OFFSET], certain);

    Label noStall;
    gen.mov(rax, qword[contextPointer + CYCLE_OFFSET]);
    gen.add(rax, m_instructionCount * PCSX::Emulator::BIAS);
    gen.mov(rcx, qword[contextPointer + GTE_READY_OFFSET]);
    gen.cmp(rax, rcx);
    gen.jae(noStall, Xbyak::CodeGenerator::T_NEAR);
    gen.sub(rcx, rax);
    gen.add(qword[contextPointer + CYCLE_OFFSET], rcx);
    if (q.count == 0) {
        gen.add(rax, rcx);
    } else {
        for (unsigned i = 0; i < q.count; i++) emitGTECommit(q.writes[i], q.writes[i].uncertain);
        gen.mov(Xbyak::util::byte[contextPointer + GTE_WRITE_LIVE_OFFSET], 0);
        gen.mov(rax, qword[contextPointer + GTE_READY_OFFSET]);
        for (unsigned i = 0; i < q.count; i++) q.writes[i].uncertain = true;
    }
    gen.L(noStall);
}

void DynaRecCPU::recCOP2(uint32_t code) {
    if (_Rs_ == 8) {
        recBCz(code);
        return;
    }
    emitCu2Check();
    if ((code & 0x02000000) == 0) {
        const auto func = m_recGTE[m_regs.code & 0x3F];  // Look up the opcode in our decoding LUT
        (*this.*func)(code);                             // Jump into the handler to recompile it
        return;
    }

    // GTE command: wait for the previous one, then mark the GTE busy
    emitGTEStall();
    gen.add(rax, gteLatency(code & 0x3f) + PCSX::Emulator::BIAS);
    gen.mov(qword[contextPointer + GTE_READY_OFFSET], rax);

    const auto func = m_recGTE[m_regs.code & 0x3F];
    auto& q = m_gteQueue;
    if (q.count == 0 || func == &DynaRecCPU::recUnknown) {
        (*this.*func)(code);
        return;
    }

    // Writes in flight: hand them to m_gtePending and run the command through the shared model. A write a
    // stall may have committed already only counts if its bit in m_gteWriteLive is still set.
    uint32_t liveBits = 0;
    for (unsigned i = 0; i < q.count; i++) {
        const auto& w = q.writes[i];
        if (w.isConst) {
            gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(i, value)], w.value);
        } else {
            gen.mov(eax, dword[contextPointer + GTE_WRITE_VALUE_OFFSET(w.slot)]);
            gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(i, value)], eax);
        }
        gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(i, reg)], w.reg);
        gen.mov(Xbyak::util::byte[contextPointer + GTE_PENDING_OFFSET(i, ctrl)], w.ctrl ? 1 : 0);
        gen.mov(dword[contextPointer + GTE_PENDING_OFFSET(i, countdown)], w.countdown);
        if (w.uncertain) liveBits |= (1u << w.slot) << (i * 8);
    }
    gen.mov(dword[contextPointer + GTE_PENDING_COUNT_OFFSET], q.count);
    q.count = 0;

    prepareForCall();
    loadThisPointer(arg1.cvt64());
    gen.mov(arg2, code);
    gen.mov(arg3, liveBits);
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

void DynaRecCPU::gteCommandWrapper(DynaRecCPU* that, uint32_t code, uint32_t liveBits) {
    unsigned kept = 0;
    for (unsigned i = 0; i < that->m_gtePendingCount; i++) {
        const uint8_t bit = liveBits >> (i * 8);
        if (bit && !(that->m_gteWriteLive & bit)) continue;
        that->m_gtePending[kept++] = that->m_gtePending[i];
    }
    that->m_gtePendingCount = kept;
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
        queueGTEWrite(true, _Rd_, true, m_gprs[_Rt_].val, eax);
    } else {
        allocateReg(_Rt_);
        queueGTEWrite(true, _Rd_, false, 0, m_gprs[_Rt_].allocatedReg);
    }
}

void DynaRecCPU::recMTC2(uint32_t code) {
    makeRoomForGTEWrite();
    if (m_gprs[_Rt_].isConst()) {
        queueGTEWrite(false, _Rd_, true, m_gprs[_Rt_].val, eax);
    } else {
        allocateReg(_Rt_);
        queueGTEWrite(false, _Rd_, false, 0, m_gprs[_Rt_].allocatedReg);
    }
}

static uint32_t MFC2Wrapper(int reg) { return PCSX::g_emulator->m_gte->MFC2(reg); }

// Note: For IRGB/ORGB this will generate a call instruction. Only use if you do not care about a call happening
void DynaRecCPU::loadGTEDataRegister(Reg32 dest, int index) {
    switch (index) {
        case 1:
        case 3:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
            gen.movsx(dest, word[contextPointer + COP2_DATA_OFFSET(index)]);
            break;

        case 7:
        case 16:
        case 17:
        case 18:
        case 19:
            gen.movzx(dest, word[contextPointer + COP2_DATA_OFFSET(index)]);
            break;

        case 15:  // Return SXY2 from SXYP
            gen.mov(dest, dword[contextPointer + COP2_DATA_OFFSET(14)]);
            break;

        case 28:
        case 29:  // Fallback for IRGB/ORGB
            gen.mov(arg1, index);
            call(MFC2Wrapper);
            gen.mov(dest, eax);
            break;

        default:
            gen.mov(dest, dword[contextPointer + COP2_DATA_OFFSET(index)]);
            break;
    }
}

void DynaRecCPU::recMFC2(uint32_t code) {
    emitGTEStall();
    if (!_Rt_) return;

    const auto loadDelayDependency = getLoadDelayDependencyType(_Rt_);
    if (loadDelayDependency != LoadDelayDependencyType::NoDependency) {
        loadGTEDataRegister(eax, _Rd_);

        if (loadDelayDependency == LoadDelayDependencyType::DependencyAcrossBlocks) {
            const auto delayedLoadValueOffset = (uintptr_t)&m_runtimeLoadDelay.value - (uintptr_t)this;
            const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
            const auto indexOffset = (uintptr_t)&m_runtimeLoadDelay.index - (uintptr_t)this;
            gen.mov(dword[contextPointer + delayedLoadValueOffset], eax);
            gen.mov(Xbyak::util::byte[contextPointer + isActiveOffset], 1);
            gen.mov(dword[contextPointer + indexOffset], _Rt_);
        } else {
            auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
            const auto delayedLoadValueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;
            delayedLoad.active = true;
            delayedLoad.index = _Rt_;
            gen.mov(dword[contextPointer + delayedLoadValueOffset], eax);
        }
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
            gen.mov(arg1, _Rd_);
            call(MFC2Wrapper);

            allocateRegWithoutLoad(_Rt_);  // Reallocate the reg in case the call thrashed it
            m_gprs[_Rt_].setWriteback(true);
            gen.mov(m_gprs[_Rt_].allocatedReg, eax);
            break;

        default:
            loadGTEDataRegister(m_gprs[_Rt_].allocatedReg, _Rd_);
            break;
    }
}

void DynaRecCPU::recCFC2(uint32_t code) {
    emitGTEStall();
    if (!_Rt_) return;

    const auto loadDelayDependency = getLoadDelayDependencyType(_Rt_);
    if (loadDelayDependency != LoadDelayDependencyType::NoDependency) {
        gen.mov(eax, dword[contextPointer + COP2_CONTROL_OFFSET(_Rd_)]);

        if (loadDelayDependency == LoadDelayDependencyType::DependencyAcrossBlocks) {
            const auto delayedLoadValueOffset = (uintptr_t)&m_runtimeLoadDelay.value - (uintptr_t)this;
            const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
            const auto indexOffset = (uintptr_t)&m_runtimeLoadDelay.index - (uintptr_t)this;
            gen.mov(dword[contextPointer + delayedLoadValueOffset], eax);
            gen.mov(Xbyak::util::byte[contextPointer + isActiveOffset], 1);
            gen.mov(dword[contextPointer + indexOffset], _Rt_);
        } else {
            auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
            const auto delayedLoadValueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;
            delayedLoad.active = true;
            delayedLoad.index = _Rt_;
            gen.mov(dword[contextPointer + delayedLoadValueOffset], eax);
        }
    } else {
        // If we won't emulate the load delay, make sure to cancel any pending loads that might trample the value
        maybeCancelDelayedLoad(_Rt_);
        allocateRegWithoutLoad(_Rt_);
        m_gprs[_Rt_].setWriteback(true);

        gen.mov(m_gprs[_Rt_].allocatedReg, dword[contextPointer + COP2_CONTROL_OFFSET(_Rd_)]);
    }
}

void DynaRecCPU::recLWC2(uint32_t code) {
    emitCu2Check();
    makeRoomForGTEWrite();
    if (m_gprs[_Rs_].isConst()) {  // Store address in arg2
        gen.mov(arg2, m_gprs[_Rs_].val + _Imm_);
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg2, m_gprs[_Rs_].allocatedReg, _Imm_);
    }

    callMemoryFunc(&PCSX::Memory::read32);
    queueGTEWrite(false, _Rt_, false, 0, eax);
}

void DynaRecCPU::recSWC2(uint32_t code) {
    emitCu2Check();
    emitGTEStall();
    loadGTEDataRegister(arg3, _Rt_);  // Load the register we'll write to memory in arg3

    // Address in arg2
    if (m_gprs[_Rs_].isConst()) {
        gen.mov(arg2, m_gprs[_Rs_].val + _Imm_);
    } else {
        allocateReg(_Rs_);
        gen.moveAndAdd(arg2, m_gprs[_Rs_].allocatedReg, _Imm_);
    }

    callMemoryFunc(&PCSX::Memory::write32);
}

template <bool isAVSZ4>
void DynaRecCPU::recAVSZ(uint32_t code) {
    Xbyak::Label noOverflow, label1, end, checkIfBelowLim, notBelowLim;

    constexpr Reg32 flag = arg1;  // Register for FLAG
    const Reg64 scaleFactor = arg2.cvt64();

    gen.xor_(flag, flag);  // Set FLAG to 0

    if constexpr (isAVSZ4) {  // Load SZF4 into scaleFactor if this is AVSZ4
        gen.movsx(scaleFactor, word[contextPointer + COP2_CONTROL_OFFSET(30)]);
    } else {  // Otherwise, load SZF3
        gen.movsx(scaleFactor, word[contextPointer + COP2_CONTROL_OFFSET(29)]);
    }

    // eax = SZ1 + SZ2 + SZ3
    gen.movzx(eax, word[contextPointer + COP2_DATA_OFFSET(17)]);
    gen.movzx(arg3, word[contextPointer + COP2_DATA_OFFSET(18)]);
    gen.add(rax, arg3.cvt64());
    gen.movzx(arg3, word[contextPointer + COP2_DATA_OFFSET(19)]);
    gen.add(rax, arg3.cvt64());

    // eax += SZ0 for AVSZ4
    if constexpr (isAVSZ4) {
        gen.movzx(arg3.cvt64(), word[contextPointer + COP2_DATA_OFFSET(16)]);
        gen.add(rax, arg3.cvt64());
    }

    // rax = (Sum of Z values) * scaleFactor
    gen.imul(rax, scaleFactor);
    // Set MAC0
    gen.mov(dword[contextPointer + COP2_DATA_OFFSET(24)], eax);
    // Calculate flags if MAC0 result is larger than 31 bits
    gen.cmp(rax, 0x7fffffff);
    gen.jle(label1);
    gen.mov(flag, (1 << 31) | (1 << 16));
    gen.jmp(noOverflow);

    gen.L(label1);
    gen.cmp(rax, 0x80000000);
    gen.jge(noOverflow);
    gen.mov(flag, (1 << 31) | (1 << 15));
    gen.L(noOverflow);

    gen.shr(rax, 12);
    // Saturate eax to [0, 0xffff] and set OTZ to the saturated value

    gen.cmp(eax, 0x10000);    // Check if above 0xffff
    gen.jl(checkIfBelowLim);  // If not, check if below
    // Set FLAG, set OTZ to 0xffff is MAC0 > 0xffff
    gen.or_(flag, (1 << 31) | (1 << 18));
    gen.mov(word[contextPointer + COP2_DATA_OFFSET(7)], 0xffff);
    gen.jmp(end);

    gen.L(checkIfBelowLim);  // Check if below 0
    gen.test(eax, eax);
    gen.jns(notBelowLim);
    // Set FLAG, and set OTZ to 0 if MAC0 < 0
    gen.or_(flag, (1 << 31) | (1 << 18));
    gen.mov(word[contextPointer + COP2_DATA_OFFSET(7)], 0);
    gen.jmp(end);

    gen.L(notBelowLim);  // handle the case where eax doesn't need to be saturated
    gen.mov(word[contextPointer + COP2_DATA_OFFSET(7)], ax);
    gen.L(end);
    gen.mov(dword[contextPointer + COP2_CONTROL_OFFSET(31)], flag);  // Writeback FLAG
}

void DynaRecCPU::recAVSZ3(uint32_t code) { recAVSZ<false>(code); }
void DynaRecCPU::recAVSZ4(uint32_t code) { recAVSZ<true>(code); }

#define GTE_FALLBACK(name)                      \
    void DynaRecCPU::rec##name(uint32_t code) { \
        gen.mov(arg2, code);                    \
        callGTEFunc(&PCSX::GTE::name);          \
    }

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

#undef GTE_FALLBACK
#endif  // DYNAREC_X86_64
