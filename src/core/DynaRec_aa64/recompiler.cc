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

bool DynaRecCPU::Init() {
    // Initialize recompiler memory
    // Check for 8MB RAM expansion
    const bool ramExpansion = PCSX::g_emulator->settings.get<PCSX::Emulator::Setting8MB>();
    m_ramSize = ramExpansion ? 0x800000 : 0x200000;
    const auto biosSize = 0x80000;

    // The amount of 64KB RAM pages. 0x80 with the ram expansion, 0x20 otherwise
    const int ramPages = m_ramSize >> 16;

    // Split the 32-bit address space into 64KB pages, so 0x10000 pages in total
    m_recompilerLUT = new DynarecCallback*[0x10000]();

    // Instructions need to be on 4-byte boundaries. So the amount of valid block entrypoints
    // in a region of memory is REGION_SIZE / 4
    m_ramBlocks = new DynarecCallback[m_ramSize / 4];
    m_biosBlocks = new DynarecCallback[biosSize / 4];
    m_dummyBlocks = new DynarecCallback[0x10000 / 4];  // Allocate one page worth of dummy blocks

    gen.Reset();  // Reset code generator

    for (int page = 0; page < 0x10000; page++) {  // Default all pages to dummy blocks
        m_recompilerLUT[page] = &m_dummyBlocks[0];
    }

    // For every 64KB page of memory, we can have 64*1024/4 unique blocks = 0x4000
    // Hence the multiplications below
    for (int page = 0; page < ramPages; page++) {          // Map RAM to the recompiler LUT
        const auto pointer = &m_ramBlocks[page * 0x4000];  // Get a pointer to the page of RAM blocks
        m_recompilerLUT[page + 0x0000] = pointer;          // Map KUSEG, KSEG0 and KSEG1 RAM respectively
        m_recompilerLUT[page + 0x8000] = pointer;
        m_recompilerLUT[page + 0xA000] = pointer;
    }

    for (int page = 0; page < 8; page++) {  // Map BIOS to recompiler LUT
        const auto pointer = &m_biosBlocks[page * 0x4000];
        m_recompilerLUT[page + 0x1FC0] = pointer;  // Map KUSEG, KSEG0 and KSEG1 BIOS respectively
        m_recompilerLUT[page + 0x9FC0] = pointer;
        m_recompilerLUT[page + 0xBFC0] = pointer;
    }

#if !defined(__APPLE__)
    if (!gen.setRWX()) {  // Mark code cache as readable/writeable/executable
        PCSX::g_system->message("[Dynarec] Failed to allocate executable memory.\nTry disabling the Dynarec CPU.");
        return false;
    }
#else
    gen.setRW();  // M1 wants buffer marked as readable/writable with mprotect before emitting code
#endif
    emitDispatcher();  // Emit our assembly dispatcher
    uncompileAll();    // Mark all blocks as uncompiled

    for (int i = 0; i < 0x10000 / 4; i++) {  // Mark all dummy blocks as invalid
        m_dummyBlocks[i] = m_invalidBlock;
    }

    m_gprs[0].markConst(0);  // $zero is always zero
    m_currentDelayedLoad = 0;
    m_runtimeLoadDelay.active = false;

#if defined(__APPLE__)
    // Check to make sure code buffer memory was allocated
    if (gen.getCode<void*>() == nullptr) {
        PCSX::g_system->message("[Dynarec] Failed to allocate memory for Dynarec.\nTry disabling the Dynarec CPU.");
        return false;
    }
    gen.setRX();  // Mark code cache as readable/executable before jumping into dispatcher
#endif
    return true;
}

void DynaRecCPU::Reset() {
    R3000Acpu::Reset();  // Reset CPU registers
    Shutdown();          // Deinit and re-init dynarec
    Init();
}

void DynaRecCPU::Shutdown() {
    delete[] m_recompilerLUT;
    delete[] m_ramBlocks;
    delete[] m_biosBlocks;
    delete[] m_dummyBlocks;
}

/// Params: A program counter value
/// Returns: A pointer to the host aa64 code that points to the block that starts from the given PC
DynarecCallback* DynaRecCPU::getBlockPointer(uint32_t pc) {
    const auto base = m_recompilerLUT[pc >> 16];
    const auto offset = (pc & 0xFFFF) >> 2;  // Remove the 2 lower bits, they're guaranteed to be 0

    return &base[offset];
}

void DynaRecCPU::signalShellReached(DynaRecCPU* that) {
    if (!that->m_shellStarted) {
        that->m_shellStarted = true;
        PCSX::g_system->m_eventBus->signal(PCSX::Events::ExecutionFlow::ShellReached{});
    }
}

void DynaRecCPU::error() {
    PCSX::g_system->hardReset();
    PCSX::g_system->pause();
    PCSX::g_system->message("Unrecoverable error while running recompiler\nProgram counter: %08X\n", m_pc);
}

void DynaRecCPU::uncompileAll() {
    constexpr int biosSize = 0x80000;
    for (auto i = 0; i < m_ramSize / 4; i++) {  // Mark all RAM blocks as uncompiled
        m_ramBlocks[i] = m_uncompiledBlock;
    }
    for (auto i = 0; i < biosSize / 4; i++) {  // Mark all BIOS blocks as uncompiled
        m_biosBlocks[i] = m_uncompiledBlock;
    }
}

void DynaRecCPU::flushCache() {
    gen.Reset();       // Reset the emitter's code pointer and code size variables
    emitDispatcher();  // Re-emit dispatcher
    uncompileAll();    // Mark all blocks as uncompiled
}

void DynaRecCPU::emitBlockLookup() {
    gen.Ldr(w4, MemOperand(contextPointer, PC_OFFSET));  // w4 = pc
    // w3 = index into the recompiler LUT page. Calculated like ((pc >> 2) & 0x3fff)
    gen.Ubfx(w3, w4, 2, 14);
    gen.Lsr(w4, w4, 16);  // w4 = pc >> 16

    // Load base pointer to recompiler LUT page in x0
    gen.Mov(x0, (uintptr_t)m_recompilerLUT);
    gen.Ldr(x0, MemOperand(x0, x4, LSL, 3));

    // Load pointer to block in x5 and jump to it
    gen.Ldr(x5, MemOperand(x0, x3, LSL, 3));
    gen.Br(x5);
}

void DynaRecCPU::emitDispatcher() {
    Label done;
    gen.align();
    m_dispatcher = gen.getCurr<DynarecCallback>();

    gen.Str(x30, MemOperand(sp, -16, PreIndex));             // Backup link register
    gen.Str(runningPointer, MemOperand(sp, -16, PreIndex));  // Save runningPointer register in stack
    gen.Str(contextPointer,
            MemOperand(sp, -16, PreIndex));  // Save contextPointer register in stack (also align stack pointer)

    gen.Mov(contextPointer, (uintptr_t)this);  // Load context pointer

    // Back up all our allocateable volatile regs
    static_assert((ALLOCATEABLE_NON_VOLATILE_COUNT & 1) == 0);  // Make sure we've got an even number of regs
    for (auto i = 0; i < ALLOCATEABLE_NON_VOLATILE_COUNT; i += 2) {
        const auto reg = allocateableNonVolatiles[i];
        const auto reg2 = allocateableNonVolatiles[i + 1];
        gen.Stp(reg2.X(), reg.X(), MemOperand(sp, -16, PreIndex));
    }

    emitBlockLookup();  // Look up block

    // Code to be executed after each block.
    gen.align();
    m_returnFromBlock = gen.getCurr<DynarecCallback>();

    loadThisPointer(arg1.X());  // Poll events
    call(recBranchTestWrapper);
    gen.Mov(runningPointer, (uintptr_t)PCSX::g_system->runningPtr());   // Move runningPtr to runningPointer register
    gen.Ldrb(w0, MemOperand(runningPointer));                           // Check if PCSX::g_system->running is true
    gen.Cbz(w0, &done);                                                 // If it's not, return
    gen.Mov(runningPointer, (uintptr_t)PCSX::g_system->quittingPtr());  // Load pointer to "quitting" variable
    gen.Ldrb(w0, MemOperand(runningPointer));                           // Check if PCSX::g_system->quitting is true
    gen.Cbnz(w0, &done);                                                // If it is, return
    emitBlockLookup();                                                  // Otherwise, look up next block

    gen.align();

    // Code for exiting JIT context
    gen.L(done);

    // Restore all non-volatiles
    for (int i = ALLOCATEABLE_NON_VOLATILE_COUNT - 1; i >= 0; i -= 2) {
        const auto reg = allocateableNonVolatiles[i];
        const auto reg2 = allocateableNonVolatiles[i - 1];
        gen.Ldp(reg.X(), reg2.X(), MemOperand(sp, 16, PostIndex));
    }

    gen.Ldr(contextPointer, MemOperand(sp, 16, PostIndex));  // Restore contextPointer register from stack
    gen.Ldr(runningPointer, MemOperand(sp, 16, PostIndex));  // Restore runningPointer register from stack
    gen.Ldr(x30, MemOperand(sp, 16, PostIndex));             // Restore link register before returning
    gen.Ret();

    // Code for when the block to be executed needs to be compiled.
    gen.align();
    m_uncompiledBlock = gen.getCurr<DynarecCallback>();

    loadThisPointer(arg1.X());
    gen.Mov(arg2, 0);  // Do not fully emulate load delays at first
    call(recRecompileWrapper);
    gen.Br(x0);  // Jump to compiled block

    // Code for recompiling the current block with full load delay support.
    gen.align();
    m_needFullLoadDelays = gen.getCurr<DynarecCallback>();

    loadThisPointer(arg1.X());
    gen.Mov(arg2, 1);  // Fully emulate load delays
    call(recRecompileWrapper);
    gen.Br(x0);  // Jump to the newly compiled block

    // Code for committing a load delay inherited from the previous block. Writes the pending value straight into
    // the guest register file. Only touches w4, w5 and x6, none of which the register allocator can hand out, and
    // it is only ever called with the register cache already flushed.
    gen.align();
    m_loadDelayHandler = gen.getCurr<DynarecCallback>();
    {
        const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;
        const auto indexOffset = (uintptr_t)&m_runtimeLoadDelay.index - (uintptr_t)this;
        const auto valueOffset = (uintptr_t)&m_runtimeLoadDelay.value - (uintptr_t)this;

        gen.Ldr(w4, MemOperand(contextPointer, indexOffset));       // Index of the register that needs to be written
        gen.Ldr(w5, MemOperand(contextPointer, valueOffset));       // Value it needs to be written with
        gen.Add(x6, contextPointer, (int64_t)GPR_OFFSET(0));        // Base of the guest register file
        gen.Str(w5, MemOperand(x6, x4, LSL, 2));                    // Write the value
        gen.Strb(wzr, MemOperand(contextPointer, isActiveOffset));  // The load is no longer pending
        gen.Ret();
    }

    // Code for when the block we've jumped to is invalid. Throws an error and exits
    gen.align();
    m_invalidBlock = gen.getCurr<DynarecCallback>();

    loadThisPointer(arg1.X());  // Throw recompiler error
    call(recErrorWrapper);
    gen.B(&done);  // Exit
    gen.ready();   // Ready code buffer before emulator jumps into dispatcher for the first time
}

// Look at the instruction after the load currently being compiled and work out whether it actually reads the
// register the load targets. Most loads are not consumed immediately, so most of them need no delay emulated at all.
DynaRecCPU::LoadDelayDependencyType DynaRecCPU::getLoadDelayDependencyType(int index) {
    // Always emulate load delays when there's a load in a branch delay slot
    if (m_stopCompiling && index != 0) return LoadDelayDependencyType::DependencyAcrossBlocks;

    if (index == 0) {  // Loads to $zero go to the void, so don't bother emulating it as a delayed load
        return LoadDelayDependencyType::NoDependency;
    }

    const uint32_t instruction = PCSX::g_emulator->m_mem->read32(m_pc, PCSX::Memory::ReadType::Instr);
    const auto rt = (instruction >> 16) & 0x1f;
    const auto rs = (instruction >> 21) & 0x1f;
    const auto opcode = instruction >> 26;
    enum : uint8_t { NoDep, DepIfRs, DepIfRt, DepIfRsOrRt };

    // TODO: Handle LWL/LWR/SWL/SWR delay slots properly
    static constexpr uint8_t mainDependencyList[64] = {
        NoDep,   DepIfRs, NoDep,   NoDep,   DepIfRsOrRt, DepIfRsOrRt, DepIfRs, DepIfRs,  // 0x0-0x7
        DepIfRs, DepIfRs, DepIfRs, DepIfRs, DepIfRs,     DepIfRs,     DepIfRs, NoDep,    // 0x8-0xF
        NoDep,   NoDep,   NoDep,   NoDep,   NoDep,       NoDep,       NoDep,   NoDep,    // 0x10-0x17
        NoDep,   NoDep,   NoDep,   NoDep,   NoDep,       NoDep,       NoDep,   NoDep,    // 0x18-0x1F
        DepIfRs, DepIfRs, DepIfRs, DepIfRs, DepIfRs,     DepIfRs,     DepIfRs, NoDep,    // 0x20-0x27
        DepIfRs, DepIfRs, DepIfRs, DepIfRs, NoDep,       NoDep,       DepIfRs, NoDep,    // 0x28-0x2F
        DepIfRs, DepIfRs, DepIfRs, DepIfRs, NoDep,       NoDep,       NoDep,   NoDep,    // 0x30-0x37
        DepIfRs, DepIfRs, DepIfRs, DepIfRs, NoDep,       NoDep,       NoDep,   NoDep,    // 0x38-0x3F
    };

    static constexpr uint8_t specialDependencyList[64] = {
        DepIfRsOrRt, NoDep,       DepIfRt,     DepIfRt,
        DepIfRsOrRt, NoDep,       DepIfRsOrRt, DepIfRsOrRt,  // 0x0-0x7
        NoDep,       NoDep,       NoDep,       NoDep,
        NoDep,       NoDep,       NoDep,       NoDep,  // 0x8-0xF
        NoDep,       DepIfRs,     NoDep,       DepIfRs,
        NoDep,       NoDep,       NoDep,       NoDep,  // 0x10-0x17
        NoDep,       NoDep,       NoDep,       NoDep,
        NoDep,       NoDep,       NoDep,       NoDep,  // 0x18-0x1F
        DepIfRsOrRt, DepIfRsOrRt, DepIfRsOrRt, DepIfRsOrRt,
        DepIfRsOrRt, DepIfRsOrRt, DepIfRsOrRt, DepIfRsOrRt,  // 0x20-0x27
        NoDep,       NoDep,       DepIfRsOrRt, DepIfRsOrRt,
        NoDep,       NoDep,       NoDep,       NoDep,  // 0x28-0x2F
        NoDep,       NoDep,       NoDep,       NoDep,
        NoDep,       NoDep,       NoDep,       NoDep,  // 0x30-0x37
        NoDep,       NoDep,       NoDep,       NoDep,
        NoDep,       NoDep,       NoDep,       NoDep,  // 0x38-0x3F
    };

    uint8_t dependencyType = NoDep;
    switch (opcode) {
        case 0:  // Special instructions
            dependencyType = specialDependencyList[instruction & 0x3F];
            break;
        case 0x10: {  // COP0 instructions also need special handling
            // We need to emulate the delay if the rs field is 4, ie the instruction is MTC0, and "index" is the source
            return (rs == 4 && rt == index) ? LoadDelayDependencyType::DependencyInsideBlock
                                            : LoadDelayDependencyType::NoDependency;
        } break;
        case 0x12: {  // COP2 instructions too
            // Check if the instruction is MFC2 or CFC2 with the source being $rt
            const bool isMove = (instruction & 0x3F) == 0 && (rs == 4 || rs == 6);
            return (isMove && rt == index) ? LoadDelayDependencyType::DependencyInsideBlock
                                           : LoadDelayDependencyType::NoDependency;
            break;
        }
        case 0x20:  // LB
        case 0x21:  // LH
        case 0x23:  // LW
        case 0x24:  // LBU
        case 0x25:  // LHU
            // A load into the same register in the delay slot cancels our pending load, so we emulate the delay and
            // let the next load cancel it. LWL/LWR are excluded as they merge with the pending value instead.
            dependencyType = DepIfRsOrRt;
            break;
        default:
            dependencyType = mainDependencyList[opcode];
            break;
    }

    switch (dependencyType) {
        case NoDep:
            return LoadDelayDependencyType::NoDependency;
        case DepIfRs:
            return (index == rs) ? LoadDelayDependencyType::DependencyInsideBlock
                                 : LoadDelayDependencyType::NoDependency;
        case DepIfRt:
            return (index == rt) ? LoadDelayDependencyType::DependencyInsideBlock
                                 : LoadDelayDependencyType::NoDependency;
        case DepIfRsOrRt:
            return (index == rs || index == rt) ? LoadDelayDependencyType::DependencyInsideBlock
                                                : LoadDelayDependencyType::NoDependency;
    }

    // Unreachable, but returning nothing would technically be UB.
    abort();
    return LoadDelayDependencyType::NoDependency;
}

// Compile a block, write address of compiled code to *callback
// Returns the address of the compiled block
DynarecCallback DynaRecCPU::recompile(DynarecCallback* callback, uint32_t pc, bool align, bool fullLoadDelayEmulation) {
    m_stopCompiling = false;
    m_inDelaySlot = false;
    m_nextIsDelaySlot = false;
    m_delayedLoadInfo[0].active = false;
    m_delayedLoadInfo[1].active = false;
    m_pcWrittenBack = false;
    m_linkedPC = std::nullopt;
    m_pc = pc & ~3;
    m_firstInstruction = true;
    m_fullLoadDelayEmulation = fullLoadDelayEmulation;
    m_gteQueue.count = 0;

    const auto startingPC = m_pc;
    unsigned count = 0;  // How many instructions have we compiled?
    unsigned extra = 0;  // How many instructions from rom?

#if defined(__APPLE__)
    gen.setRW();  // Mark code cache as readable/writeable before emitting code
#endif

    if (align) {
        gen.align();  // Align next block
    }

    if (gen.getSize() > codeCacheSize) {  // Flush JIT cache if we've gone above the acceptable size
        flushCache();
    }

    const auto blockStart = gen.getCurr<DynarecCallback>();
    *callback = blockStart;

    if (!m_fullLoadDelayEmulation) {
        // Check if there's a load delay pending at the start of the block. If so, this block has to be recompiled
        // with full load delay support, because it cannot see the pending load otherwise.
        Label noPendingLoad;
        const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;

        gen.Ldrb(w4, MemOperand(contextPointer, isActiveOffset));
        gen.Cbz(w4, &noPendingLoad);
        jmp((void*)m_needFullLoadDelays);
        gen.L(noPendingLoad);
    }

    handleKernelCall();  // Check if this is a kernel call vector, emit some extra code in that case.

    // GTE writes left in flight by the previous block, the interpreter or a save state land here, up to two
    // instructions early.
    {
        Label noPendingWrites;
        gen.Ldr(w4, MemOperand(contextPointer, (uintptr_t)&m_gtePendingCount - (uintptr_t)this));
        gen.Cbz(w4, &noPendingWrites);
        loadThisPointer(arg1.X());
        call(drainGTEWritesWrapper);
        gen.L(noPendingWrites);
    }

    auto shouldContinue = [&]() {
        if (m_nextIsDelaySlot) {
            return true;
        }
        if (m_stopCompiling) {
            return false;
        }
        // Don't end the block while a load delay is still pending, there'd be nothing left to commit it
        if (count >= MAX_BLOCK_SIZE && !m_delayedLoadInfo[0].active && !m_delayedLoadInfo[1].active) {
            return false;
        }
        return true;
    };

    // Target of the branch whose delay slot is compiled next, when it is known at compile time.
    std::optional<uint32_t> slotBranchTarget;

    // Compile the instruction at m_pc. Returns false if it could not be fetched.
    auto compileInstruction = [&]() {
        m_inDelaySlot = m_nextIsDelaySlot;
        m_nextIsDelaySlot = false;
        m_slotBranchPC = m_nestedSlotBranchPC.value_or(m_pc - 4);
        m_nestedSlotBranchPC = std::nullopt;

        uint32_t* p = PCSX::g_emulator->m_mem->getPointer<uint32_t>(m_pc);
        if (p == nullptr) {  // Error if it can't be fetched
            return false;
        }

        uint32_t code = m_regs.code = *p;  // Actually read the instruction
        m_pc += 4;                         // Increment recompiler PC
        count++;                           // Increment instruction count
        if ((m_pc & 0xffc00000) == 0xbfc00000) extra++;
        m_instructionCount = count;
        m_romInstructionCount = extra;
        slotBranchTarget = staticBranchTarget(code, m_pc);

        const auto func = m_recBSC[code >> 26];  // Look up the opcode in our decoding LUT
        (*this.*func)(code);                     // Jump into the handler to recompile it
        return true;
    };

    // Commit the load that the previous instruction started, if any. The ping-pong index is toggled first, so the
    // slot committed after instruction N is the one instruction N-1 wrote. That is the one instruction of delay.
    const auto processDelayedLoad = [this]() {
        m_currentDelayedLoad ^= 1;
        auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];

        if (delayedLoad.active) {
            delayedLoad.active = false;
            const unsigned index = delayedLoad.index;
            const auto delayedValueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;

            allocateRegWithoutLoad(index);
            m_gprs[index].setWriteback(true);
            gen.Ldr(m_gprs[index].allocatedReg, MemOperand(contextPointer, delayedValueOffset));
        }
    };

    // Commit a load inherited from the previous block. Runs after the first instruction, which is the only one that
    // may not see it yet.
    const auto resolveInitialLoadDelay = [this]() {
        if (!m_fullLoadDelayEmulation) return;
        flushRegs();

        Label noDelayedLoad;
        const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;

        gen.Ldrb(w4, MemOperand(contextPointer, isActiveOffset));
        gen.Cbz(w4, &noDelayedLoad);
        call(m_loadDelayHandler);
        gen.L(noDelayedLoad);
    };

    if (!compileInstruction()) {
        return m_invalidBlock;
    }
    resolveInitialLoadDelay();
    processDelayedLoad();
    tickGTEWrites();
    m_firstInstruction = false;

    // Ends the block: writes back the registers and the PC, adds the cycles, returns to the dispatcher.
    const auto endBlock = [&]() {
        emitGTEFlushQueue(false);
        flushRegs();
        if (!m_pcWrittenBack) {
            gen.Mov(w0, m_pc);
            gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
        }
        gen.Ldr(x0, MemOperand(contextPointer, CYCLE_OFFSET));
        gen.Add(x0, x0, (count + extra * PCSX::Emulator::ROM_EXTRA_BIAS) * PCSX::Emulator::BIAS);
        gen.Str(x0, MemOperand(contextPointer, CYCLE_OFFSET));
        jmp((void*)m_returnFromBlock);
    };

    while (shouldContinue()) {
        // A branch in the delay slot of a branch that may be taken. When the first branch is taken, the
        // second one is relative to the first one's target, and its own delay slot is the instruction at
        // that target. Compile that path separately, then fall through to the not-taken path, which is
        // an ordinary branch with its delay slot after it.
        auto* memory = PCSX::g_emulator->m_mem.get();
        uint32_t* slotPtr =
            m_nextIsDelaySlot && m_pcWrittenBack && slotBranchTarget ? memory->getPointer<uint32_t>(m_pc) : nullptr;
        uint32_t* targetPtr = slotPtr ? memory->getPointer<uint32_t>(*slotBranchTarget) : nullptr;
        if (slotPtr && targetPtr && isBranch(*slotPtr)) {
            const uint32_t firstTarget = *slotBranchTarget;
            const uint32_t slotPC = m_pc;
            const auto savedLoads0 = m_delayedLoadInfo[0];
            const auto savedLoads1 = m_delayedLoadInfo[1];
            const auto savedCurrentLoad = m_currentDelayedLoad;
            const auto savedGTEQueue = m_gteQueue;
            const auto savedCount = count;
            const auto savedExtra = extra;
            Label notTaken;

            flushRegs();
            gen.Ldr(w0, MemOperand(contextPointer, PC_OFFSET));
            gen.Mov(w1, firstTarget);
            gen.Cmp(w0, w1);
            gen.B(&notTaken, ne);

            // Taken: the second branch, based on the first branch's target.
            m_inDelaySlot = true;
            m_nextIsDelaySlot = false;
            m_pcWrittenBack = false;
            m_slotBranchPC = slotPC - 4;
            const uint32_t code = m_regs.code = *slotPtr;
            m_pc = firstTarget;
            count++;
            if (((slotPC + 4) & 0xffc00000) == 0xbfc00000) extra++;
            m_instructionCount = count;
            m_romInstructionCount = extra;
            (*this.*m_recBSC[code >> 26])(code);
            fixDelaySlotBranchLink(code, slotPC + 8);
            if (!m_pcWrittenBack) {
                gen.Mov(w0, firstTarget + 4);
                gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
                m_pcWrittenBack = true;
            }
            processDelayedLoad();
            tickGTEWrites();
            // Its delay slot is the instruction at the first branch's target.
            m_nextIsDelaySlot = true;
            m_nestedSlotBranchPC = slotPC;
            if (!compileInstruction()) {
                return m_invalidBlock;
            }
            processDelayedLoad();
            tickGTEWrites();
            m_pcWrittenBack = true;
            endBlock();

            // Not taken: back to the state before the taken path and compile it the ordinary way.
            gen.L(notTaken);
            m_delayedLoadInfo[0] = savedLoads0;
            m_delayedLoadInfo[1] = savedLoads1;
            m_currentDelayedLoad = savedCurrentLoad;
            m_gteQueue = savedGTEQueue;
            count = savedCount;
            extra = savedExtra;
            m_pc = slotPC;
            m_inDelaySlot = false;
            m_nextIsDelaySlot = true;
            m_pcWrittenBack = true;
            m_stopCompiling = true;
            m_linkedPC = std::nullopt;
        }
        if (!compileInstruction()) {
            return m_invalidBlock;
        }
        processDelayedLoad();
        tickGTEWrites();
    }

    emitGTEFlushQueue(false);
    flushRegs();
    if (!m_pcWrittenBack) {  // Write PC back if needed
        gen.Mov(w0, m_pc);
        gen.Str(w0, MemOperand(contextPointer, PC_OFFSET));
    }

    // If this was the block at 0x8003'0000 (Start of shell), don't link the PC in case we fastboot
    if (startingPC == 0x80030000) {
        m_linkedPC = std::nullopt;
    }

    gen.Ldr(x0, MemOperand(contextPointer, CYCLE_OFFSET));  // Fetch cycle count from memory
    gen.Add(x0, x0, (count + extra * PCSX::Emulator::ROM_EXTRA_BIAS) * PCSX::Emulator::BIAS);  // Add block cycles
    gen.Str(x0, MemOperand(contextPointer, CYCLE_OFFSET));  // Store cycles back to memory

    // Link block else return to dispatcher
    if (m_linkedPC && ENABLE_BLOCK_LINKING && m_linkedPC.value() != startingPC) {
        handleLinking();
    } else {
        jmp((void*)m_returnFromBlock);
    }

    // Clear stale instruction cache contents.
    __builtin___clear_cache(reinterpret_cast<char*>(blockStart), gen.getCurr<char*>());
    gen.ready();
#if defined(__APPLE__)
    gen.setRX();  // Mark code cache as readable/executable before returning to dispatcher
#endif
    // The block might have been invalidated by handleLinking, so re-read the pointer from *callback
    return *callback;
}

// Checks if the block being compiled is one of the kernel call vectors
// If so, emit a call to "InterceptBIOS", which handles the kernel call debugger features
// Also handles fast booting by intercepting the shell reached signal and setting pc to $ra if fastboot is on
void DynaRecCPU::handleKernelCall() {
    if (m_pc == 0x80030000) {
        handleShellReached();
        return;
    }

    const uint32_t pc = m_pc & 0x1fffff;
    const uint32_t base = (m_pc >> 20) & 0xffc;
    if ((base != 0x000) && (base != 0x800) && (base != 0xa00))
        return;  // Mask out the segment, return if not a kernel call vector

    switch (pc) {  // Handle the A0/B0/C0 vectors
        case 0xA0:
            loadThisPointer(arg1.X());
            call(interceptKernelCallWrapper<0xA0>);
            break;

        case 0xB0:
            loadThisPointer(arg1.X());
            call(interceptKernelCallWrapper<0xB0>);
            break;

        case 0xC0:
            loadThisPointer(arg1.X());
            call(interceptKernelCallWrapper<0xC0>);
            break;
    }
}

// Emits a jump to the dispatcher if there's no block to link to.
// Otherwise, handle linking blocks
void DynaRecCPU::handleLinking() {
    vixl::aarch64::Label returnFromBlock;
    // Don't link unless the next PC is valid, and there's over 1MB of free space in the code cache
    if (isPcValid(m_linkedPC.value()) && gen.getRemainingSize() > 0x100000) {
        const auto nextPC = m_linkedPC.value();
        const auto nextBlockPointer = getBlockPointer(nextPC);
        const auto nextBlockOffset = (size_t)nextBlockPointer - (size_t)this;

        if (*nextBlockPointer == m_uncompiledBlock) {  // If the next block hasn't been compiled yet
            // Check that the block hasn't been invalidated/moved
            // The value will be patched later. Since all code is within the same 32MB segment,
            // We can get away with only checking the low 32 bits of the block pointer
            gen.Mov(x0, (uintptr_t)nextBlockPointer);
            gen.Ldr(x0, MemOperand(x0));
            // Move value to compare against into w1
            gen.Mov(w1, 0xcccccccc);
            gen.Cmp(w0, w1);

            const auto pointer = gen.getCurr<uint8_t*>();
            gen.bne(returnFromBlock);  // Return if the block addr changed

            recompile(nextBlockPointer, nextPC, false);  // Fallthrough to next block

            *(uint32_t*)(pointer - 4) = (uint32_t)(uintptr_t)*nextBlockPointer;  // Patch comparison value
        } else {  // If it has already been compiled, link by jumping to the compiled code
            gen.Mov(x0, (uintptr_t)nextBlockPointer);
            gen.Ldr(x0, MemOperand(x0));
            // Move value to compare against into w1
            gen.Mov(w1, (uint32_t)(uintptr_t)*nextBlockPointer);
            gen.Cmp(w0, w1);

            gen.bne(returnFromBlock);       // Return if the block addr changed
            jmp((void*)*nextBlockPointer);  // Jump to linked block otherwise

            gen.L(returnFromBlock);
            jmp((void*)m_returnFromBlock);
        }
    } else {  // Can't link, so return to dispatcher
        jmp((void*)m_returnFromBlock);
    }
}

void DynaRecCPU::handleShellReached() {
    Label alreadyReached;

    gen.Mov(x0, (uintptr_t)&m_shellStarted);  // Check if shell has already been reached
    gen.Ldrb(w0, MemOperand(x0));
    gen.Cbnz(w0, &alreadyReached);  // Skip signalling that we've reached the shell if so

    loadThisPointer(arg1.X());  // Signal that we've reached the shell
    call(signalShellReached);
    jmp((void*)m_returnFromBlock);

    gen.L(alreadyReached);
}

std::unique_ptr<PCSX::R3000Acpu> PCSX::Cpus::getDynaRec() { return std::unique_ptr<PCSX::R3000Acpu>(new DynaRecCPU()); }

#endif  // DYNAREC_AA64
