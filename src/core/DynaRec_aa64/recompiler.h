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

#pragma once
#include "core/r3000a.h"

#if defined(DYNAREC_AA64)
#include <array>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include "emitter.h"
#include "fmt/format.h"
#include "regAllocation.h"
#include "spu/interface.h"
#include "tracy/Tracy.hpp"

#define HOST_REG_CACHE_OFFSET(x) ((uintptr_t) & m_hostRegisterCache[(x)] - (uintptr_t)this)
#define GPR_OFFSET(x) ((uintptr_t) & m_regs.GPR.r[(x)] - (uintptr_t)this)
#define COP0_OFFSET(x) ((uintptr_t) & m_regs.CP0.r[(x)] - (uintptr_t)this)
#define PC_OFFSET ((uintptr_t) & m_regs.pc - (uintptr_t)this)
#define LO_OFFSET ((uintptr_t) & m_regs.GPR.n.lo - (uintptr_t)this)
#define HI_OFFSET ((uintptr_t) & m_regs.GPR.n.hi - (uintptr_t)this)
#define CYCLE_OFFSET ((uintptr_t) & m_regs.cycle - (uintptr_t)this)

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

static uint8_t read8Wrapper(uint32_t address) { return PCSX::g_emulator->m_mem->read8(address); }
static uint16_t read16Wrapper(uint32_t address) { return PCSX::g_emulator->m_mem->read16(address); }
static uint32_t read32Wrapper(uint32_t address) { return PCSX::g_emulator->m_mem->read32(address); }

static void SPU_writeRegisterWrapper(uint32_t addr, uint16_t value) {
    PCSX::g_emulator->m_spu->writeRegister(addr, value);
}

static void write8Wrapper(uint32_t address, uint32_t value) { PCSX::g_emulator->m_mem->write8(address, value); }
static void write16Wrapper(uint32_t address, uint32_t value) { PCSX::g_emulator->m_mem->write16(address, value); }
static void write32Wrapper(uint32_t address, uint32_t value) { PCSX::g_emulator->m_mem->write32(address, value); }
// LWL/LWR/SWL/SWR: msan only considers the bytes of the aligned word the instruction consumes or overwrites
static uint32_t lwlReadWrapper(uint32_t address) {
    return PCSX::g_emulator->m_mem->read32Masked(address & ~3, PCSX::Memory::leftByteMask(address));
}
static uint32_t lwrReadWrapper(uint32_t address) {
    return PCSX::g_emulator->m_mem->read32Masked(address & ~3, PCSX::Memory::rightByteMask(address));
}
static uint32_t unalignedStoreReadWrapper(uint32_t address) {
    return PCSX::g_emulator->m_mem->read32Masked(address, 0);
}
static void swlWriteWrapper(uint32_t address, uint32_t value) {
    PCSX::g_emulator->m_mem->write32Masked(address & ~3, value, PCSX::Memory::leftByteMask(address));
}
static void swrWriteWrapper(uint32_t address, uint32_t value) {
    PCSX::g_emulator->m_mem->write32Masked(address & ~3, value, PCSX::Memory::rightByteMask(address));
}

using DynarecCallback = void (*)();  // A function pointer to JIT-emitted code

class DynaRecCPU final : public PCSX::R3000Acpu {
    using recompilationFunc = void (DynaRecCPU::*)(uint32_t code);  // A function pointer to a dynarec member function

  private:
    uint64_t m_hostRegisterCache[16];  // An array to backup non-volatile regs temporarily

    DynarecCallback** m_recompilerLUT;
    DynarecCallback* m_ramBlocks;   // Pointers to compiled RAM blocks (If nullptr then this block needs to be compiled)
    DynarecCallback* m_biosBlocks;  // Pointers to compiled BIOS blocks
    DynarecCallback* m_dummyBlocks;  // This is where invalid pages will point

    // Functions written in raw assembly
    DynarecCallback m_dispatcher;       // Pointer to our assembly dispatcher
    DynarecCallback m_returnFromBlock;  // Pointer to the code that will be executed when returning from a block
    DynarecCallback m_uncompiledBlock;  // Pointer to the code that will be executed when jumping to an uncompiled block
    DynarecCallback m_invalidBlock;     // Pointer to the code that will be executed the PC is invalid
    DynarecCallback m_loadDelayHandler;  // Pointer to the code that will handle load delays at the start of a block
    // Pointer to the code that will be executed when a block needs to be recompiled with full load delay support
    DynarecCallback m_needFullLoadDelays;

    uint32_t m_pc;
    unsigned m_instructionCount;     // Instructions compiled so far in the current block
    unsigned m_romInstructionCount;  // How many of those were fetched from ROM
    Emitter gen;

    bool m_stopCompiling;  // Should we stop compiling code?
    bool m_pcWrittenBack;  // Has the PC been written back already by a jump?
    uint32_t m_ramSize;    // RAM is 2MB on retail units, 8MB on some DTL units (Can be toggled in GUI)
    const int MAX_BLOCK_SIZE = 50;

    bool m_firstInstruction;        // Is the instruction being compiled the first one of the block?
    bool m_fullLoadDelayEmulation;  // Should we emulate load delays that cross block boundaries?

    // Used to hold info when we've got a load delay between the end of a block and the start of another
    // For example, when there's an lw instruction in the delay slot of a branch
    struct {
        bool active;
        int index;
        uint32_t value;
    } m_runtimeLoadDelay;

    // GTE register writes (MTC2, CTC2, LWC2) compiled in the current block that have not reached the GTE yet,
    // oldest first. A write lands once two more instructions have run, as in the interpreter. Between blocks,
    // writes still in flight live in R3000Acpu::m_gtePending.
    struct GTEQueuedWrite {
        uint32_t value;  // The value, when isConst
        uint8_t reg;
        uint8_t slot;  // Index into m_gteWriteValues
        uint8_t countdown;
        bool ctrl;
        bool isConst;
    };
    struct {
        GTEQueuedWrite writes[3];
        unsigned count;
    } m_gteQueue;
    uint32_t m_gteWriteValues[3];  // Values of the queued writes, captured at issue

    enum class LoadDelayDependencyType { NoDependency, DependencyInsideBlock, DependencyAcrossBlocks };

    enum class RegState { Unknown, Constant };
    enum class LoadingMode { DoNotLoad, Load };
    // Renamed temporarily to Reg since it collides with Register in vixl::aarch64 namespace
    struct Reg {
        uint32_t val = 0;                    // The register's cached value used for constant propagation
        RegState state = RegState::Unknown;  // Is this register's value a constant, or an unknown value?

        bool allocated = false;  // Has this guest register been allocated to a host reg?
        bool writeback = false;  // Does this register need to be written back to memory at the end of the block?
        Register allocatedReg;   // If a host reg has been allocated to this register, which reg is it?
        int allocatedRegIndex = 0;

        inline bool isConst() { return state == RegState::Constant; }
        inline bool isAllocated() { return allocated; }
        inline void markConst(uint32_t value) {
            val = value;
            state = RegState::Constant;
            writeback = false;  // Disable writeback in case the reg was previously allocated with writeback
            allocated = false;  // Unallocate register
        }

        // Note: It's important that markUnknown does not modify the val field as that would mess up codegen
        inline void markUnknown() { state = RegState::Unknown; }

        inline void setWriteback(bool wb) { writeback = wb; }
    };

    inline void markConst(int index, uint32_t value) {
        m_gprs[index].markConst(value);
        if (m_hostRegs[m_gprs[index].allocatedRegIndex].mappedReg == index) {
            m_hostRegs[m_gprs[index].allocatedRegIndex].mappedReg =
                std::nullopt;  // Unmap the register on the host reg side too
        }
    }

    struct HostRegister {
        std::optional<int> mappedReg = std::nullopt;  // The register this is allocated to, if any
    };

    Reg m_gprs[32];
    std::array<HostRegister, ALLOCATEABLE_REG_COUNT> m_hostRegs;
    std::optional<uint32_t> m_linkedPC = std::nullopt;
    // Address of the branch whose delay slot is being compiled. It is the previous word, except for the
    // instruction at a taken branch's target when that branch had another branch in its slot: that
    // second branch owns it, and an exception there reports it in EPC.
    uint32_t m_slotBranchPC = 0;
    std::optional<uint32_t> m_nestedSlotBranchPC = std::nullopt;
    // The PC to hand the exception handler: the excepting instruction, or the branch it is the delay slot of plus 4
    // so that the handler's EPC = PC - 4 for BD lands on that branch.
    uint32_t exceptionPC() const { return m_inDelaySlot ? m_slotBranchPC + 4 : m_pc - 4; }

    template <LoadingMode mode = LoadingMode::Load>
    void reserveReg(int index);
    void allocateReg(int reg);
    void allocateRegWithoutLoad(int reg);

    template <int T, int U>
    void allocateRegisters(std::array<int, T> regsWithoutWb, std::array<int, U> regsWithWb);
    void alloc_rt_rs(uint32_t code);
    void alloc_rt_wb_rd(uint32_t code);
    void alloc_rs_wb_rd(uint32_t code);
    void alloc_rs_wb_rt(uint32_t code);
    void alloc_rt_rs_wb_rd(uint32_t code);

    void flushRegs();
    void spillRegisterCache();
    void prepareForCall();
    // The msan hardware registers take their arguments in $a0/$a1 (see psxhw.cc), so a 32-bit access to
    // them, or to an address only known at runtime, needs syncArgumentRegisters first.
    static bool usesArgumentRegisters(uint32_t addr, bool write) {
        return addr == 0x1f802094 || (!write && (addr == 0x1f80208c || addr == 0x1f802090));
    }
    void syncArgumentRegisters();
    unsigned int m_allocatedRegisters = 0;  // how many registers have been allocated in this block?

    // Check if we're executing from valid memory
    inline bool isPcValid(uint32_t addr) { return m_recompilerLUT[addr >> 16] != m_dummyBlocks; }

    // True for every branch and jump: REGIMM, J, JAL, BEQ, BNE, BLEZ, BGTZ, JR and JALR.
    static bool isBranch(uint32_t code) {
        const uint32_t op = code >> 26;
        if (op == 0) return (code & 0x3e) == 0x08;  // JR, JALR
        return op >= 1 && op <= 7;
    }

    // Target of the branch or jump in `code` if it is taken, when it is known at compile time.
    // `pc` is the address after the branch, which is the base of a relative branch.
    std::optional<uint32_t> staticBranchTarget(uint32_t code, uint32_t pc) {
        const uint32_t op = code >> 26;
        if (op == 1 || (op >= 4 && op <= 7)) return (uint32_t)((int16_t)code * 4) + pc;
        if (op == 2 || op == 3) return (pc & 0xf0000000) | ((code & 0x03ffffff) << 2);
        if (op == 0 && (code & 0x3e) == 0x08) {
            const unsigned rs = (code >> 21) & 0x1f;
            if (m_gprs[rs].isConst()) return m_gprs[rs].val & ~3;
        }
        return std::nullopt;
    }

    // A linking branch compiled with a moved base still links to its own address + 8.
    void fixDelaySlotBranchLink(uint32_t code, uint32_t link) {
        const uint32_t op = code >> 26;
        if (op == 3 || (op == 1 && ((code >> 17) & 0xf) == 8)) {
            markConst(31, link);
        } else if (op == 0 && (code & 0x3f) == 0x09) {
            const unsigned rd = (code >> 11) & 0x1f;
            if (rd) markConst(rd, link);
        }
    }

    DynarecCallback* getBlockPointer(uint32_t pc);
    DynarecCallback recompile(DynarecCallback* callback, uint32_t pc, bool align = true,
                              bool fullLoadDelayEmulation = false);
    LoadDelayDependencyType getLoadDelayDependencyType(int index);
    void error();
    void flushCache();
    void handleLinking();
    void handleShellReached();
    void handleKernelCall();
    void emitDispatcher();
    void emitBlockLookup();
    void uncompileAll();

    // Class Wrapper Functions
    static void exceptionWrapper(DynaRecCPU* that, int32_t e, int32_t bd) { that->exception(e, bd); }
    static void recClearWrapper(DynaRecCPU* that, uint32_t address) { that->Clear(address, 1); }
    static void recBranchTestWrapper(DynaRecCPU* that) { that->branchTest(); }
    static void recErrorWrapper(DynaRecCPU* that) { that->error(); }

    static void signalShellReached(DynaRecCPU* that);
    static DynarecCallback recRecompileWrapper(DynaRecCPU* that, bool fullLoadDelayEmulation) {
        const uint32_t pc = that->m_regs.pc;
        return that->recompile(that->getBlockPointer(pc), pc, true, fullLoadDelayEmulation);
    }

    template <uint32_t pc>
    static void interceptKernelCallWrapper(DynaRecCPU* that) {
        that->InterceptBIOS<false>(pc);
    }

    // TODO: This is currently un-unsed in x64 DynaRec. Check this.
    void inlineClear(uint32_t address) {
        if (isPcValid(address & ~3)) {
            loadThisPointer(arg1.X());
            gen.Mov(arg2, address & ~3);
            call(recClearWrapper);
        }
    }

  public:
    DynaRecCPU() : R3000Acpu("Dynarec (arm64)") {}
    virtual bool Implemented() final { return true; }
    virtual bool Init() final;
    virtual void Reset() final;
    virtual void Execute() final {
        ZoneScoped;         // Tell the Tracy profiler to do its thing
        (*m_dispatcher)();  // Jump to assembly dispatcher
    }
    virtual void Clear(uint32_t Addr, uint32_t Size) final {
        auto pointer = getBlockPointer(Addr);
        for (auto i = 0; i < Size; i++) {
            *pointer++ = m_uncompiledBlock;
        }
    }
    virtual void Shutdown() final;
    virtual bool isDynarec() final { return true; }

    virtual void invalidateCache() override final {
        memset(m_regs.iCacheAddr, 0xff, sizeof(m_regs.iCacheAddr));
        memset(m_regs.iCacheCode, 0xff, sizeof(m_regs.iCacheCode));
        uncompileAll();  // Mark all blocks as uncompiled
    }

    virtual void SetPGXPMode(uint32_t pgxpMode) final {
        if (pgxpMode != 0) {
            throw std::runtime_error("PGXP not supported in x64 JIT");
        }
    }

    // For the GUI dynarec disassembly widget
    virtual const uint8_t* getBufferPtr() final { return gen.getCode<const uint8_t*>(); }
    virtual const size_t getBufferSize() final { return gen.getSize(); }

  private:
    // Calculate the number of instructions between the current PC and the branch target
    // Returns a negative number for backwards jumps
    int64_t getPCOffset(const void* current, const void* target) {
        return (int64_t)((ptrdiff_t)target - (ptrdiff_t)current) >> 2;
    }

    // Load a pointer to the JIT object in "reg" using lea with the context pointer
    void loadThisPointer(Register reg) { gen.Mov(reg, contextPointer); }

    // Loads a value into dest from the given pointer.
    template <int size, bool signExtend>
    void load(Register dest, const void* pointer) {
        //        const auto distance = (intptr_t)pointer - (intptr_t)this;
        gen.Mov(x4, (uintptr_t)pointer);
        switch (size) {
            case 8:
                signExtend ? gen.Ldrsb(dest, MemOperand(x4)) : gen.Ldrb(dest, MemOperand(x4));
                break;
            case 16:
                signExtend ? gen.Ldrsh(dest, MemOperand(x4)) : gen.Ldrh(dest, MemOperand(x4));
                break;
            case 32:
                gen.Ldr(dest, MemOperand(x4));
                break;
        }
    }

    // Stores a value of "size" bits from "source" to the given pointer
    template <int size, typename T>
    void store(T source, const void* pointer) {
        gen.Mov(x4, (uintptr_t)pointer);
        gen.Mov(w5, source);
        switch (size) {
            case 8:
                gen.Strb(w5, MemOperand(x4));
                break;
            case 16:
                gen.Strh(w5, MemOperand(x4));
                break;
            case 32:
                gen.Str(w5, MemOperand(x4));
                break;
        }
    }

    // Prepare for a call to a C++ function and then actually emit it
    template <typename T>
    void call(T& func) {
        prepareForCall();
        const auto ptr = reinterpret_cast<const void*>(func);
        const int64_t disp = getPCOffset(gen.getCurr<const void*>(), ptr);

        // If the displacement can fit in a 26-bit int, that means we can emit a direct call to the address
        // Otherwise, load the address into a register and emit a blr
        const bool canDoDirectCall = vixl::IsInt26(disp);

        if (canDoDirectCall) {
            gen.bl(disp);
        } else {
            gen.Mov(x4, (uintptr_t)ptr);
            gen.Blr(x4);
        }
    }

    // jmp function using same method as call to attempt direct jump
    void jmp(void* pointer) {
        const int64_t disp = getPCOffset(gen.getCurr<const void*>(), pointer);

        // If the displacement can fit in a 26-bit int, that means we can emit a direct call to the address
        // Otherwise, load the address into a register and emit a br
        const bool canDoDirectJump = vixl::IsInt26(disp);

        if (canDoDirectJump) {
            gen.b(disp);
        } else {
            gen.Mov(x4, (uintptr_t)pointer);
            gen.Br(x4);
        }
    }

    void maybeCancelDelayedLoad(uint32_t index) {
        // A load delay inherited from the previous block is only visible to the first instruction of this one, so
        // that is the only place where it can be cancelled at runtime.
        if (m_fullLoadDelayEmulation && m_firstInstruction) {
            const auto indexOffset = (uintptr_t)&m_runtimeLoadDelay.index - (uintptr_t)this;
            const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
            Label noDelayedLoad;

            gen.Ldr(w4, MemOperand(contextPointer, indexOffset));
            gen.Cmp(w4, index);
            gen.B(&noDelayedLoad, ne);
            gen.Strb(wzr, MemOperand(contextPointer, isActiveOffset));
            gen.L(noDelayedLoad);
        }

        const unsigned other = m_currentDelayedLoad ^ 1;
        if (m_delayedLoadInfo[other].index == index) {
            m_delayedLoadInfo[other].active = false;
        }
    }

    // Park the value currently in w0 as a load delayed by one instruction, targeting guest register "index".
    // Either processDelayedLoad picks it back up next instruction, or the load delay handler does at the start of
    // the next block. Clobbers w4.
    void storeDelayedLoad(unsigned index, LoadDelayDependencyType type) {
        m_delayedLoadInfo[m_currentDelayedLoad].active = true;

        if (type == LoadDelayDependencyType::DependencyAcrossBlocks) {
            const auto valueOffset = (uintptr_t)&m_runtimeLoadDelay.value - (uintptr_t)this;
            const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
            const auto indexOffset = (uintptr_t)&m_runtimeLoadDelay.index - (uintptr_t)this;

            gen.Str(w0, MemOperand(contextPointer, valueOffset));
            gen.Mov(w4, 1);
            gen.Strb(w4, MemOperand(contextPointer, isActiveOffset));
            gen.Mov(w4, index);
            gen.Str(w4, MemOperand(contextPointer, indexOffset));
        } else {
            auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
            const auto valueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;

            delayedLoad.index = index;
            gen.Str(w0, MemOperand(contextPointer, valueOffset));
        }
    }

    // Instruction definitions
    void recUnknown(uint32_t code);
    void recSpecial(uint32_t code);

    void recADD(uint32_t code);
    void recADDIU(uint32_t code);
    void recADDU(uint32_t code);
    void recAND(uint32_t code);
    void recANDI(uint32_t code);
    void recBEQ(uint32_t code);
    void recBGTZ(uint32_t code);
    void recBLEZ(uint32_t code);
    void recBNE(uint32_t code);
    void recBREAK(uint32_t code);
    void recCFC2(uint32_t code);
    void recCOP0(uint32_t code);
    void recCOP2(uint32_t code);
    void recCOP1or3(uint32_t code);
    void recBCz(uint32_t code);
    void recCTC2(uint32_t code);
    void recDIV(uint32_t code);
    void recDIVU(uint32_t code);
    void recJ(uint32_t code);
    void recJAL(uint32_t code);
    void recJALR(uint32_t code);
    void recJR(uint32_t code);
    void recLB(uint32_t code);
    void recLBU(uint32_t code);
    void recLH(uint32_t code);
    void recLHU(uint32_t code);
    void recLUI(uint32_t code);
    void recLW(uint32_t code);
    void recLWC2(uint32_t code);
    void recLWL(uint32_t code);
    void recLWR(uint32_t code);
    void recMFC0(uint32_t code);
    void recMFC2(uint32_t code);
    void recMFHI(uint32_t code);
    void recMFLO(uint32_t code);
    void recMTC0(uint32_t code);
    void recMTC2(uint32_t code);
    void recMTHI(uint32_t code);
    void recMTLO(uint32_t code);
    void recMULT(uint32_t code);
    void recMULTU(uint32_t code);
    void recNOR(uint32_t code);
    void recOR(uint32_t code);
    void recORI(uint32_t code);
    void recREGIMM(uint32_t code);
    void recRFE(uint32_t code);
    void recSB(uint32_t code);
    void recSH(uint32_t code);
    void recSLL(uint32_t code);
    void recSLLV(uint32_t code);
    void recSLT(uint32_t code);
    void recSLTI(uint32_t code);
    void recSLTIU(uint32_t code);
    void recSLTU(uint32_t code);
    void recSRA(uint32_t code);
    void recSRAV(uint32_t code);
    void recSRL(uint32_t code);
    void recSRLV(uint32_t code);
    void recSUB(uint32_t code);
    void recSUBU(uint32_t code);
    void recSW(uint32_t code);
    void recSWC2(uint32_t code);
    void recSWL(uint32_t code);
    void recSWR(uint32_t code);
    void recSYSCALL(uint32_t code);
    void recXOR(uint32_t code);
    void recXORI(uint32_t code);
    void recException(Exception e);

    // GTE instructions
    void recGTEMove(uint32_t code);
    void recAVSZ3(uint32_t code);
    void recAVSZ4(uint32_t code);
    void recCC(uint32_t code);
    void recCDP(uint32_t code);
    void recDCPL(uint32_t code);
    void recDPCS(uint32_t code);
    void recDPCT(uint32_t code);
    void recGPF(uint32_t code);
    void recGPL(uint32_t code);
    void recINTPL(uint32_t code);
    void recMVMVA(uint32_t code);
    void recNCCS(uint32_t code);
    void recNCCT(uint32_t code);
    void recNCDS(uint32_t code);
    void recNCDT(uint32_t code);
    void recNCLIP(uint32_t code);
    void recNCS(uint32_t code);
    void recNCT(uint32_t code);
    void recOP(uint32_t code);
    void recRTPS(uint32_t code);
    void recRTPT(uint32_t code);
    void recSQR(uint32_t code);

    template <bool isAVSZ4>
    void recAVSZ(uint32_t code);

    template <bool loadSR>
    void testSoftwareInterrupt();

    void loadGTEDataRegister(Register dest, int index);
    void emitGTEWrite(bool ctrl, unsigned reg);
    void emitGTECommit(const GTEQueuedWrite& write);
    void emitGTESpillQueue();
    void emitGTEFlushQueue(bool commitAll);
    void makeRoomForGTEWrite();
    void queueGTEWrite(bool ctrl, unsigned reg, bool isConst, uint32_t value, Register src);
    void tickGTEWrites();
    static void gteCommandWrapper(DynaRecCPU* that, uint32_t code);
    void drainGTEWrites();
    static void drainGTEWritesWrapper(DynaRecCPU* that) { that->drainGTEWrites(); }

    template <int size, bool signExtend>
    void recompileLoad(uint32_t code);

    template <int size, bool signExtend>
    void recompileLoadWithDelay(uint32_t code, LoadDelayDependencyType type);

    const recompilationFunc m_recBSC[64] = {
        &DynaRecCPU::recSpecial, &DynaRecCPU::recREGIMM,  &DynaRecCPU::recJ,       &DynaRecCPU::recJAL,      // 00
        &DynaRecCPU::recBEQ,     &DynaRecCPU::recBNE,     &DynaRecCPU::recBLEZ,    &DynaRecCPU::recBGTZ,     // 04
        &DynaRecCPU::recADDIU,   &DynaRecCPU::recADDIU,   &DynaRecCPU::recSLTI,    &DynaRecCPU::recSLTIU,    // 08
        &DynaRecCPU::recANDI,    &DynaRecCPU::recORI,     &DynaRecCPU::recXORI,    &DynaRecCPU::recLUI,      // 0c
        &DynaRecCPU::recCOP0,    &DynaRecCPU::recCOP1or3, &DynaRecCPU::recCOP2,    &DynaRecCPU::recCOP1or3,  // 10
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 14
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 18
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 1c
        &DynaRecCPU::recLB,      &DynaRecCPU::recLH,      &DynaRecCPU::recLWL,     &DynaRecCPU::recLW,       // 20
        &DynaRecCPU::recLBU,     &DynaRecCPU::recLHU,     &DynaRecCPU::recLWR,     &DynaRecCPU::recUnknown,  // 24
        &DynaRecCPU::recSB,      &DynaRecCPU::recSH,      &DynaRecCPU::recSWL,     &DynaRecCPU::recSW,       // 28
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recSWR,     &DynaRecCPU::recUnknown,  // 2c
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recLWC2,    &DynaRecCPU::recUnknown,  // 30
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 34
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recSWC2,    &DynaRecCPU::recUnknown,  // 38
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 3c
    };

    const recompilationFunc m_recSPC[64] = {
        &DynaRecCPU::recSLL,     &DynaRecCPU::recUnknown, &DynaRecCPU::recSRL,     &DynaRecCPU::recSRA,      // 00
        &DynaRecCPU::recSLLV,    &DynaRecCPU::recUnknown, &DynaRecCPU::recSRLV,    &DynaRecCPU::recSRAV,     // 04
        &DynaRecCPU::recJR,      &DynaRecCPU::recJALR,    &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 08
        &DynaRecCPU::recSYSCALL, &DynaRecCPU::recBREAK,   &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 0c
        &DynaRecCPU::recMFHI,    &DynaRecCPU::recMTHI,    &DynaRecCPU::recMFLO,    &DynaRecCPU::recMTLO,     // 10
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 14
        &DynaRecCPU::recMULT,    &DynaRecCPU::recMULTU,   &DynaRecCPU::recDIV,     &DynaRecCPU::recDIVU,     // 18
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 1c
        &DynaRecCPU::recADD,     &DynaRecCPU::recADDU,    &DynaRecCPU::recSUB,     &DynaRecCPU::recSUBU,     // 20
        &DynaRecCPU::recAND,     &DynaRecCPU::recOR,      &DynaRecCPU::recXOR,     &DynaRecCPU::recNOR,      // 24
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recSLT,     &DynaRecCPU::recSLTU,     // 28
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 2c
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 30
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 34
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 38
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 3c
    };

    const recompilationFunc m_recGTE[64] = {
        &DynaRecCPU::recGTEMove, &DynaRecCPU::recRTPS,    &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 00
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recNCLIP,   &DynaRecCPU::recUnknown,  // 04
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 08
        &DynaRecCPU::recOP,      &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 0c
        &DynaRecCPU::recDPCS,    &DynaRecCPU::recINTPL,   &DynaRecCPU::recMVMVA,   &DynaRecCPU::recNCDS,     // 10
        &DynaRecCPU::recCDP,     &DynaRecCPU::recUnknown, &DynaRecCPU::recNCDT,    &DynaRecCPU::recUnknown,  // 14
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recNCCS,     // 18
        &DynaRecCPU::recCC,      &DynaRecCPU::recUnknown, &DynaRecCPU::recNCS,     &DynaRecCPU::recUnknown,  // 1c
        &DynaRecCPU::recNCT,     &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 20
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 24
        &DynaRecCPU::recSQR,     &DynaRecCPU::recDCPL,    &DynaRecCPU::recDPCT,    &DynaRecCPU::recUnknown,  // 28
        &DynaRecCPU::recUnknown, &DynaRecCPU::recAVSZ3,   &DynaRecCPU::recAVSZ4,   &DynaRecCPU::recUnknown,  // 2c
        &DynaRecCPU::recRTPT,    &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 30
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 34
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 38
        &DynaRecCPU::recUnknown, &DynaRecCPU::recGPF,     &DynaRecCPU::recGPL,     &DynaRecCPU::recNCCT,     // 3c
    };

    static constexpr bool ENABLE_BLOCK_LINKING = false;
};

#endif  // DYNAREC_AA64
