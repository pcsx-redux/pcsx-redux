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

#pragma once
#include "core/r3000a.h"

#if defined(DYNAREC_X86_64)
#include <array>
#include <cassert>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/gpu.h"
#include "emitter.h"
#include "fmt/format.h"
#include "profiler.h"
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
#define MULDIV_READY_OFFSET ((uintptr_t) & m_regs.muldivReady - (uintptr_t)this)
#define GTE_READY_OFFSET ((uintptr_t) & m_regs.gteReady - (uintptr_t)this)

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

inline uint32_t read32Wrapper(uint32_t address) { return PCSX::g_emulator->m_mem->read32(address); }
inline void write32Wrapper(uint32_t address, uint32_t value) { PCSX::g_emulator->m_mem->write32(address, value); }
// LWL/LWR/SWL/SWR: msan only considers the bytes of the aligned word the instruction consumes or overwrites
inline uint32_t lwlReadWrapper(uint32_t address) {
    return PCSX::g_emulator->m_mem->read32Masked(address & ~3, PCSX::Memory::leftByteMask(address));
}
inline uint32_t lwrReadWrapper(uint32_t address) {
    return PCSX::g_emulator->m_mem->read32Masked(address & ~3, PCSX::Memory::rightByteMask(address));
}
inline uint32_t unalignedStoreReadWrapper(uint32_t address) {
    return PCSX::g_emulator->m_mem->read32Masked(address, 0);
}
inline void swlWriteWrapper(uint32_t address, uint32_t value) {
    PCSX::g_emulator->m_mem->write32Masked(address & ~3, value, PCSX::Memory::leftByteMask(address));
}
inline void swrWriteWrapper(uint32_t address, uint32_t value) {
    PCSX::g_emulator->m_mem->write32Masked(address & ~3, value, PCSX::Memory::rightByteMask(address));
}
inline void SPU_writeRegisterWrapper(uint32_t addr, uint16_t value) {
    PCSX::g_emulator->m_spu->writeRegister(addr, value);
}

using DynarecCallback = void (*)();  // A function pointer to JIT-emitted code
using namespace Xbyak;
using namespace Xbyak::util;

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
    DynarecCallback m_invalidateBlocks;  // Pointer to the code that will invalidate all RAM code blocks
    DynarecCallback m_loadDelayHandler;  // Pointer to the code that will handle load delays at the start of a block
    // Pointer to the code that will be executed when a block needs to be recompiled with full load delay support
    DynarecCallback m_needFullLoadDelays;

    Emitter gen;
    uint32_t m_pc;                   // Recompiler PC
    unsigned m_instructionCount;     // Instructions compiled so far in the current block
    unsigned m_romInstructionCount;  // How many of those were fetched from ROM

    bool m_stopCompiling;  // Should we stop compiling code?
    // Has this block checked SR.CU2 yet? SR cannot change inside a block (an MTC0 to it ends the block), so one
    // check before the first COP2 opcode covers the rest.
    bool m_cu2Checked;
    bool m_pcWrittenBack;  // Has the PC been written back already by a jump?
    bool m_firstInstruction;
    bool m_fullLoadDelayEmulation;
    uint32_t m_ramSize;  // RAM is 2MB on retail units, 8MB on some DTL units (Can be toggled in GUI)

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
        uint8_t slot;  // Index into m_gteWriteValues and bit in m_gteWriteLive
        uint8_t countdown;
        bool ctrl;
        bool isConst;
        // A stall may already have committed it at runtime, which its bit in m_gteWriteLive tells
        bool uncertain;
    };
    struct {
        GTEQueuedWrite writes[3];
        unsigned count;
    } m_gteQueue;
    uint32_t m_gteWriteValues[3];  // Values of the queued writes, captured at issue
    uint8_t m_gteWriteLive;        // Bit n set: the write in slot n is still in flight

    const int MAX_BLOCK_SIZE = 50;

    enum class RegState { Unknown, Constant };
    enum class LoadingMode { DoNotLoad, Load };
    enum class LoadDelayDependencyType { NoDependency, DependencyInsideBlock, DependencyAcrossBlocks };

    struct Register {
        uint32_t val = 0;                    // The register's cached value used for constant propagation
        RegState state = RegState::Unknown;  // Is this register's value a constant, or an unknown value?

        bool allocated = false;  // Has this guest register been allocated to a host reg?
        bool writeback = false;  // Does this register need to be written back to memory at the end of the block?
        Reg32 allocatedReg;      // If a host reg has been allocated to this register, which reg is it?
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

    Register m_gprs[32];
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
    unsigned int m_allocatedRegisters = 0;  // how many registers have been allocated in this block?

    void prepareForCall();
    // The msan hardware registers take their arguments in $a0/$a1 (see psxhw.cc), so a 32-bit access to
    // them, or to an address only known at runtime, needs syncArgumentRegisters first.
    static bool usesArgumentRegisters(uint32_t addr, bool write) {
        return addr == 0x1f802094 || (!write && (addr == 0x1f80208c || addr == 0x1f802090));
    }
    void syncArgumentRegisters();
    void handleKernelCall();
    void emitDispatcher();
    void uncompileAll();

  public:
    DynaRecCPU() : R3000Acpu("Dynarec (x86-64)") {}

    virtual bool Implemented() final { return true; }
    virtual bool Init() final;
    virtual void Reset() final;
    virtual void Shutdown() final;
    virtual bool isDynarec() final { return true; }
    virtual void Execute() final {
        ZoneScoped;         // Tell the Tracy profiler to do its thing
        (*m_dispatcher)();  // Jump to assembly dispatcher
    }
    // For the GUI dynarec disassembly widget
    virtual const uint8_t* getBufferPtr() final { return gen.getCode<const uint8_t*>(); }
    virtual const size_t getBufferSize() final { return gen.getSize(); }

    // TODO: Make it less slow and bad
    // Possibly clear blocks more aggressively
    // Note: This relies on the behavior in psxmem.cc which calls Clear after force-aligning the address
    virtual void Clear(uint32_t addr, uint32_t size) final {
        auto pointer = getBlockPointer(addr);
        for (auto i = 0; i < size; i++) {
            *pointer++ = m_uncompiledBlock;
        }
    }

    virtual void invalidateCache() override final {
        memset(m_regs.iCacheAddr, 0xff, sizeof(m_regs.iCacheAddr));
        memset(m_regs.iCacheCode, 0xff, sizeof(m_regs.iCacheCode));
        m_invalidateBlocks();
    }

    virtual void SetPGXPMode(uint32_t pgxpMode) final {
        if (pgxpMode != 0) {
            throw std::runtime_error("PGXP not supported in x64 JIT");
        }
    }

    void dumpBuffer() const {
        std::ofstream file("DynarecOutput.dump", std::ios::binary);  // Make a file for our dump
        file.write(gen.getCode<const char*>(), gen.getSize());       // Write the code buffer to the dump
    }

  private:
    // Sets dest to "pointer"
    void loadAddress(Xbyak::Reg64 dest, void* pointer) { gen.mov(dest, (uintptr_t)pointer); }

    // Loads a value into dest from the given pointer.
    // Tries to use base pointer relative addressing, otherwise uses movabs
    template <int size, bool signExtend>
    void load(Xbyak::Reg32 dest, const void* pointer) {
        const auto distance = (intptr_t)pointer - (intptr_t)this;

        if (Xbyak::inner::IsInInt32(distance)) {
            switch (size) {
                case 8:
                    signExtend ? gen.movsx(dest, Xbyak::util::byte[contextPointer + distance])
                               : gen.movzx(dest, Xbyak::util::byte[contextPointer + distance]);
                    break;
                case 16:
                    signExtend ? gen.movsx(dest, word[contextPointer + distance])
                               : gen.movzx(dest, word[contextPointer + distance]);
                    break;
                case 32:
                    gen.mov(dest, dword[contextPointer + distance]);
                    break;
            }
        } else {
            gen.mov(rax, (uintptr_t)pointer);
            switch (size) {
                case 8:
                    signExtend ? gen.movsx(dest, Xbyak::util::byte[rax]) : gen.movzx(dest, Xbyak::util::byte[rax]);
                    break;
                case 16:
                    signExtend ? gen.movsx(dest, word[rax]) : gen.movzx(dest, word[rax]);
                    break;
                case 32:
                    gen.mov(dest, dword[rax]);
                    break;
            }
        }
    }

    // Stores a value of "size" bits from "source" to the given pointer
    // Tries to use base pointer relative addressing, otherwise uses movabs
    template <int size, typename T>
    void store(T source, const void* pointer) {
        const auto distance = (intptr_t)pointer - (intptr_t)this;

        if (Xbyak::inner::IsInInt32(distance)) {
            switch (size) {
                case 8:
                    gen.mov(Xbyak::util::byte[contextPointer + distance], source);
                    break;
                case 16:
                    gen.mov(word[contextPointer + distance], source);
                    break;
                case 32:
                    gen.mov(dword[contextPointer + distance], source);
                    break;
            }
        } else {
            gen.mov(rax, (uintptr_t)pointer);
            switch (size) {
                case 8:
                    gen.mov(Xbyak::util::byte[rax], source);
                    break;
                case 16:
                    gen.mov(word[rax], source);
                    break;
                case 32:
                    gen.mov(dword[rax], source);
                    break;
            }
        }
    }

    // Emit a call to a class member function, passing "thisObject" (+ an adjustment if necessary)
    // As the function's "this" pointer. Only works with classes with single, non-virtual inheritance
    // Hence the static asserts. Those are all we need though, thankfully.
    template <typename T>
    void emitMemberFunctionCall(T func, void* thisObject) {
        void* functionPtr;
        uintptr_t thisPtr = reinterpret_cast<uintptr_t>(thisObject);

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
        static_assert(sizeof(T) == 8, "[x64 JIT] Invalid size for member function pointer");
        std::memcpy(&functionPtr, &func, sizeof(T));
#else
        static_assert(sizeof(T) == 16, "[x64 JIT] Invalid size for member function pointer");
        uintptr_t arr[2];
        std::memcpy(arr, &func, sizeof(T));
        // First 8 bytes correspond to the actual pointer to the function
        functionPtr = reinterpret_cast<void*>(arr[0]);
        // Next 8 bytes correspond to the "this" pointer adjustment
        thisPtr += arr[1];
#endif

        // Load this pointer to arg1
        if (thisPtr == reinterpret_cast<uintptr_t>(this)) {
            loadThisPointer(arg1.cvt64());
        } else {
            loadAddress(arg1.cvt64(), reinterpret_cast<void*>(thisPtr));
        }

        gen.call(functionPtr);
    }

    template <typename T>
    void callMemoryFunc(T func) {
        void* object = PCSX::g_emulator->m_mem.get();
        prepareForCall();
        emitMemberFunctionCall(func, object);
    }

    template <typename T>
    void callGTEFunc(T func) {
        void* object = PCSX::g_emulator->m_gte.get();
        prepareForCall();
        emitMemberFunctionCall(func, object);
    }

    static void exceptionWrapper(DynaRecCPU* that, int32_t e, int32_t bd) { that->exception(e, bd); }
    static void recErrorWrapper(DynaRecCPU* that) { that->error(); }

    static void signalShellReached(DynaRecCPU* that);
    static DynarecCallback recRecompileWrapper(DynaRecCPU* that, bool fullLoadDelayEmulation) {
        return that->recompile(that->m_regs.pc, fullLoadDelayEmulation);
    }

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
    DynarecCallback recompile(uint32_t pc, bool fullLoadDelayEmulation, bool align = true);
    void error();
    void flushCache();
    void handleLinking();
    void handleShellReached();
    void emitBlockLookup();

    std::string m_symbols;
    RecompilerProfiler<10000000> m_profiler;

    void makeSymbols();
    bool startProfiling(uint32_t pc);
    void endProfiling();
    void dumpProfileData();

    void maybeCancelDelayedLoad(int index) {
        if (m_fullLoadDelayEmulation && m_firstInstruction) {
            const auto& delay = m_runtimeLoadDelay;
            const auto indexOffset = (uintptr_t)&delay.index - (uintptr_t)this;
            const auto isActiveOffset = (uintptr_t)&delay.active - (uintptr_t)this;

            Label(noDelayedLoad);
            gen.cmp(Xbyak::util::dword[contextPointer + indexOffset], index);  // Check if there's an active delay
            gen.jne(noDelayedLoad);
            gen.mov(Xbyak::util::byte[contextPointer + isActiveOffset], 0);
            gen.L(noDelayedLoad);
        }

        const unsigned other = m_currentDelayedLoad ^ 1;
        if (m_delayedLoadInfo[other].index == index) {
            m_delayedLoadInfo[other].active = false;
        }
    }
    LoadDelayDependencyType getLoadDelayDependencyType(int index);

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
    void recLSCz(uint32_t code);
    void emitCopUnusableCheck(unsigned z, bool userModeOnly = false);
    void emitCu2Check() {
        if (m_cu2Checked) return;
        emitCopUnusableCheck(2);
        m_cu2Checked = true;
    }
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
    void emitGTEStall();
    void emitGTEWrite(bool ctrl, unsigned reg);
    void emitGTECommit(const GTEQueuedWrite& write, bool checkLive);
    void emitGTEFlushQueue(bool commitAll);
    void makeRoomForGTEWrite();
    void queueGTEWrite(bool ctrl, unsigned reg, bool isConst, uint32_t value, Xbyak::Reg32 src);
    void tickGTEWrites();
    static void gteCommandWrapper(DynaRecCPU* that, uint32_t code, uint32_t liveBits);
    void drainGTEWrites();
    static void drainGTEWritesWrapper(DynaRecCPU* that) { that->drainGTEWrites(); }
    void emitMuldivStall();
    void emitMuldivNow();
    void emitMuldivStart(uint32_t latency);
    void emitMultStart(uint32_t code, bool isSigned);
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
    void loadGTEDataRegister(Reg32 dest, int index);

    template <bool readSR>
    void testSoftwareInterrupt();

    // Prepare for a call to a C++ function and then actually emit it
    template <typename T>
    void call(T& func) {
        prepareForCall();
        gen.callFunc(func);
    }

    // Load a pointer to the JIT object in "reg"
    void loadThisPointer(Xbyak::Reg64 reg) { gen.mov(reg, contextPointer); }

    template <int size, bool signExtend>
    void recompileLoad(uint32_t code);
    template <int size, bool signExtend>
    void recompileLoadWithDelay(uint32_t code, LoadDelayDependencyType dependencyType);
    template <bool left>
    void recompileUnalignedLoad(uint32_t code);

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
        &DynaRecCPU::recLSCz,    &DynaRecCPU::recLSCz,    &DynaRecCPU::recLWC2,    &DynaRecCPU::recLSCz,     // 30
        &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown, &DynaRecCPU::recUnknown,  // 34
        &DynaRecCPU::recLSCz,    &DynaRecCPU::recLSCz,    &DynaRecCPU::recSWC2,    &DynaRecCPU::recLSCz,     // 38
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

    static constexpr bool ENABLE_BLOCK_LINKING = true;
    static constexpr bool ENABLE_PROFILER = false;
    static constexpr bool ENABLE_SYMBOLS = false;
};
#endif  // DYNAREC_X86_64
