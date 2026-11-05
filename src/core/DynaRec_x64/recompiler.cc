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
#include <cassert>
#include <cstring>

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

    gen.reset();

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

    if (!gen.setRWX()) {
        PCSX::g_system->message("[Dynarec] Failed to allocate executable memory.\nTry disabling the Dynarec CPU.");
        return false;
    }
    emitDispatcher();  // Emit our assembly dispatcher
    uncompileAll();    // Mark all blocks as uncompiled

    for (int i = 0; i < 0x10000 / 4; i++) {  // Mark all dummy blocks as invalid
        m_dummyBlocks[i] = m_invalidBlock;
    }

    if constexpr (ENABLE_SYMBOLS) {
        makeSymbols();
    }

    if constexpr (ENABLE_PROFILER) {
        m_profiler.init();
    }

    m_gprs[0].markConst(0);  // $zero is always zero
    m_currentDelayedLoad = 0;
    m_runtimeLoadDelay.active = false;
    return true;
}

void DynaRecCPU::Shutdown() {
    delete[] m_recompilerLUT;
    delete[] m_ramBlocks;
    delete[] m_biosBlocks;
    delete[] m_dummyBlocks;

    if constexpr (ENABLE_SYMBOLS) {
        std::ofstream out("DynarecOutput.map");
        out << m_symbols;
        m_symbols.clear();
        dumpBuffer();
    }

    if constexpr (ENABLE_PROFILER) {
        dumpProfileData();
    }
}

void DynaRecCPU::Reset() {
    R3000Acpu::Reset();  // Reset CPU registers
    Shutdown();          // Deinit and re-init dynarec
    Init();
}

std::unique_ptr<PCSX::R3000Acpu> PCSX::Cpus::getDynaRec() { return std::unique_ptr<PCSX::R3000Acpu>(new DynaRecCPU()); }

/// Params: A program counter value
/// Returns: A pointer to the host x64 code that points to the block that starts from the given PC
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
    gen.reset();       // Reset the emitter's code pointer and code size variables
    emitDispatcher();  // Re-emit dispatcher
    uncompileAll();    // Mark all blocks as uncompiled
}

void DynaRecCPU::emitBlockLookup() {
    const auto lutOffset = (size_t)m_recompilerLUT - (size_t)this;

    gen.mov(ecx, dword[contextPointer + PC_OFFSET]);  // ecx = pc
    gen.mov(edx, ecx);                                // edx = pc
    gen.shr(ecx, 16);                                 // ecx = pc >> 16
    gen.and_(edx, 0xfffc);                            // edx = index into the recompiler LUT page, multiplied by 4

    // Load base pointer to recompiler LUT page in rax
    // Using a single mov if possible
    if (Xbyak::inner::IsInInt32(lutOffset)) {
        gen.mov(rax, qword[contextPointer + rcx * 8 + lutOffset]);
    } else {
        loadAddress(rax, m_recompilerLUT);
        gen.mov(rax, qword[rax + rcx * 8]);
    }
    gen.jmp(ptr[rax + rdx * 2]);  // Jump to block
}

void DynaRecCPU::emitDispatcher() {
    Xbyak::Label done;

    gen.align(16);
    m_dispatcher = gen.getCurr<DynarecCallback>();
    gen.push(contextPointer);                  // Save context pointer register in stack (also align stack pointer)
    gen.mov(contextPointer, (uintptr_t)this);  // Load context pointer

    // Back up all our allocateable volatile regs
    static_assert((ALLOCATEABLE_NON_VOLATILE_COUNT & 1) == 0);  // Make sure we've got an even number of regs
    for (auto i = 0; i < ALLOCATEABLE_NON_VOLATILE_COUNT; i++) {
        const auto reg = allocateableNonVolatiles[i];
        gen.push(reg.cvt64());
    }
    gen.mov(qword[contextPointer + HOST_REG_CACHE_OFFSET(0)], runningPointer);  // Backup running pointer

    // Allocate shadow stack space on Windows
    if constexpr (isWindows()) {
        gen.sub(rsp, 32);
    }

    emitBlockLookup();  // Look up block

    // Code to be executed after each block
    // Blocks will jmp to here
    gen.align(16);
    m_returnFromBlock = gen.getCurr<DynarecCallback>();

    // Poll events
    emitMemberFunctionCall(&PCSX::R3000Acpu::branchTest, this);
    gen.mov(runningPointer, (uintptr_t)PCSX::g_system->runningPtr());   // Load pointer to "running" variable
    gen.test(Xbyak::util::byte[runningPointer], 1);                     // Check if PCSX::g_system->running is true
    gen.jz(done);                                                       // If it's not, return
    gen.mov(runningPointer, (uintptr_t)PCSX::g_system->quittingPtr());  // Load pointer to "quitting" variable
    gen.test(Xbyak::util::byte[runningPointer], 1);                     // Check if PCSX::g_system->running is true
    gen.jnz(done);                                                      // If it is, return
    emitBlockLookup();                                                  // Otherwise, look up next block

    gen.align(16);
    // Code for exiting JIT context
    gen.L(done);

    // Deallocate shadow stack space on Windows
    if constexpr (isWindows()) {
        gen.add(rsp, 32);
    }

    // Restore all non-volatiles
    for (int i = ALLOCATEABLE_NON_VOLATILE_COUNT - 1; i >= 0; i--) {
        const auto reg = allocateableNonVolatiles[i];
        gen.pop(reg.cvt64());
    }
    gen.mov(runningPointer, qword[contextPointer + HOST_REG_CACHE_OFFSET(0)]);

    gen.pop(contextPointer);  // Restore our context pointer
    gen.ret();                // Return

    // Code for when the block to be executed needs to be compiled.
    gen.align(16);
    m_uncompiledBlock = gen.getCurr<DynarecCallback>();

    loadThisPointer(arg1.cvt64());
    gen.xor_(arg2, arg2);               // Do not fully emulate load delays at first
    gen.callFunc(recRecompileWrapper);  // Call recompilation function. Returns pointer to emitted code
    gen.jmp(rax);

    // Code for when the block we've jumped to is invalid. Throws an error and exits
    gen.align(16);
    m_invalidBlock = gen.getCurr<DynarecCallback>();

    loadThisPointer(arg1.cvt64());  // Throw recompiler error
    gen.callFunc(recErrorWrapper);
    gen.jmp(done);  // Exit

    // Code that will invalidate all RAM blocks when FlushCache is called
    gen.align(16);
    m_invalidateBlocks = gen.getCurr<DynarecCallback>();

    const uint32_t blockCount = m_ramSize / 4;  // Each 4 bytes correspond to 1 block
    gen.mov(rax, (uintptr_t)m_ramBlocks);       // rax = pointer to the blocks we'll be invalidating
    gen.xor_(edx, edx);                         // edx = iteration counter
    Label literalPool;

    if (gen.hasAVX) {                                 // AVX version
        gen.vmovdqa(ymm0, yword[rip + literalPool]);  // Broadcast the pointer in ymm0 four times over
        Label loop;
        gen.L(loop);                     // Memset loop
        for (auto i = 0; i < 16; i++) {  // Unroll 16 iterations of the loop
            gen.vmovdqu(yword[rax + rdx * 8 + i * 32], ymm0);
        }
        gen.add(edx, 16 * 4);  // We cleared 64 blocks in total
        gen.cmp(edx, blockCount);
        gen.jb(loop);
        gen.vzeroupper();  // Exit AVX context
        gen.ret();
    } else {  // SSE version
        // Store the pointer in xmm0 twice over, so we can write it twice in 1 128-bit write
        gen.movdqa(xmm0, xword[rip + literalPool]);
        Label loop;
        gen.L(loop);                     // Memset loop
        for (auto i = 0; i < 16; i++) {  // Unroll 16 iterations of the loop
            gen.movdqu(xword[rax + rdx * 8 + i * 16], xmm0);
        }
        gen.add(edx, 16 * 2);  // We cleared 32 blocks in total
        gen.cmp(edx, blockCount);
        gen.jb(loop);
        gen.ret();
    }

    // Code for handling load delays at the beginning of a block
    {
        gen.align(16);
        const auto& delay = m_runtimeLoadDelay;
        const auto isActiveOffset = (uintptr_t)&delay.active - (uintptr_t)this;
        const auto indexOffset = (uintptr_t)&delay.index - (uintptr_t)this;
        const auto valueOffset = (uintptr_t)&delay.value - (uintptr_t)this;
        const auto registerArrayOffset = (uintptr_t)&m_regs.GPR.r[0] - (uintptr_t)this;

        m_loadDelayHandler = gen.getCurr<DynarecCallback>();
        gen.mov(ecx, dword[contextPointer + indexOffset]);  // Index of the register that needs to be loaded to
        gen.mov(edx, dword[contextPointer + valueOffset]);  // Value of the register that needs to be loaded to
        gen.mov(dword[contextPointer + rcx * 4 + registerArrayOffset], edx);  // Write the value
        gen.mov(Xbyak::util::byte[contextPointer + isActiveOffset], 0);       // Load is no longer active
        gen.ret();
    }

    // Literal pool containing the pointer to our uncompiled block handler four times.
    // Used for our SSE/AVX code for block invalidaton
    gen.align(32);
    gen.L(literalPool);
    for (int i = 0; i < 4; i++) {
        gen.dq((uintptr_t)m_uncompiledBlock);
    }

    // Code to recompile the current block with full load delay emulation if necessary
    gen.align(16);
    m_needFullLoadDelays = gen.getCurr<DynarecCallback>();
    loadThisPointer(arg1.cvt64());
    gen.mov(arg2, 1);                   // Fully emulate load delays
    gen.callFunc(recRecompileWrapper);  // Call recompilation function. Returns pointer to emitted code
    gen.jmp(rax);
}

// Compile a block, write address of compiled code to *callback
// Returns the address of the compiled block
DynarecCallback DynaRecCPU::recompile(uint32_t pc, bool fullLoadDelayEmulation, bool align) {
    m_stopCompiling = false;
    m_inDelaySlot = false;
    m_nextIsDelaySlot = false;
    m_pcWrittenBack = false;
    m_linkedPC = std::nullopt;
    m_delayedLoadInfo[0].active = false;
    m_delayedLoadInfo[1].active = false;
    m_pc = pc & ~3;
    m_firstInstruction = true;
    m_fullLoadDelayEmulation = fullLoadDelayEmulation;
    m_gteQueue.count = 0;
    auto& memory = PCSX::g_emulator->m_mem;

    // If we somehow ended up compiling a block at an invalid PC, throw an error.
    if (!isPcValid(m_pc)) return m_invalidBlock;

    const auto startingPC = m_pc;
    unsigned count = 0;                                 // How many instructions have we compiled?
    unsigned extra = 0;                                 // How many instructions in rom?
    DynarecCallback* callback = getBlockPointer(m_pc);  // Pointer to where we'll store the addr of the emitted code

    if (align) {
        gen.align(16);  // Align next block
    }

    if (gen.getSize() > codeCacheSize) {  // Flush JIT cache if we've gone above the acceptable size
        flushCache();
    }

    if constexpr (ENABLE_SYMBOLS) {
        m_symbols += fmt::format("{} recompile_{:08X}\n", gen.getCurr<void*>(), m_pc);
        // This is unnecessary, but it acts as a hint to the decompiler about the context pointer's value
        gen.mov(contextPointer, (uintptr_t)this);
    }

    *callback = gen.getCurr<DynarecCallback>();  // Pointer to emitted code
    if constexpr (ENABLE_PROFILER) {
        if (startProfiling(m_pc)) {  // Uncompile all blocks if the profiler data overflower
            uncompileAll();
        }
    }

    if (!m_fullLoadDelayEmulation) {
        const auto isActiveOffset = (uintptr_t)&m_runtimeLoadDelay.active - (uintptr_t)this;

        // Check if there's a pending load at the start of the block. If so we need to
        // Recompile the block with full load delay support
        gen.cmp(Xbyak::util::byte[contextPointer + isActiveOffset], 0);
        gen.jne((void*)m_needFullLoadDelays);
    }
    handleKernelCall();  // Check if this is a kernel call vector, emit some extra code in that case.

    // GTE writes left in flight by the previous block, the interpreter or a save state land here, up to two
    // instructions early.
    {
        Label noPendingWrites;
        gen.cmp(dword[contextPointer + ((uintptr_t)&m_gtePendingCount - (uintptr_t)this)], 0);
        gen.je(noPendingWrites);
        loadThisPointer(arg1.cvt64());
        call(drainGTEWritesWrapper);
        gen.L(noPendingWrites);
    }

    const auto shouldContinue = [this, &count]() {
        if (m_nextIsDelaySlot) {
            return true;
        }
        if (m_stopCompiling) {
            return false;
        }
        if (count >= MAX_BLOCK_SIZE && !m_delayedLoadInfo[0].active && !m_delayedLoadInfo[1].active) {
            return false;
        }
        return true;
    };

    const auto processDelayedLoad = [this]() {
        m_currentDelayedLoad ^= 1;
        auto& delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];

        if (delayedLoad.active) {
            delayedLoad.active = false;
            const unsigned index = delayedLoad.index;
            const auto delayedValueOffset = (uintptr_t)&delayedLoad.value - (uintptr_t)this;
            const auto delayedLoadActiveOffset = (uintptr_t)&delayedLoad.active - (uintptr_t)this;
            allocateRegWithoutLoad(index);
            m_gprs[index].setWriteback(true);
            gen.mov(m_gprs[index].allocatedReg, dword[contextPointer + delayedValueOffset]);
        }
    };

    // Target of the branch whose delay slot is compiled next, when it is known at compile time.
    std::optional<uint32_t> slotBranchTarget;

    const auto compileInstruction = [this, &count, &memory, &extra, &slotBranchTarget]() -> bool {
        m_inDelaySlot = m_nextIsDelaySlot;
        m_nextIsDelaySlot = false;
        m_slotBranchPC = m_nestedSlotBranchPC.value_or(m_pc - 4);
        m_nestedSlotBranchPC = std::nullopt;

        // Fetch instruction. We make sure this function is called with a valid PC, otherwise it will crash
        uint32_t* ptr = memory->getPointer<uint32_t>(m_pc);
        if (!ptr) return false;
        uint32_t code = m_regs.code = *ptr;
        m_pc += 4;  // Increment recompiler PC
        count++;    // Increment instruction count
        if ((m_pc & 0xffc00000) == 0xbfc00000) extra++;
        m_instructionCount = count;
        m_romInstructionCount = extra;
        slotBranchTarget = staticBranchTarget(code, m_pc);

        const auto func = m_recBSC[code >> 26];  // Look up the opcode in our decoding LUT
        (*this.*func)(code);                     // Jump into the handler to recompile it
        return true;
    };

    const auto resolveInitialLoadDelay = [this]() {
        if (!m_fullLoadDelayEmulation) return;
        flushRegs();

        Label noDelayedLoad;
        const auto& delay = m_runtimeLoadDelay;
        const auto isActiveOffset = (uintptr_t)&delay.active - (uintptr_t)this;

        gen.cmp(Xbyak::util::byte[contextPointer + isActiveOffset], 0);  // Check if there's an active delay
        gen.je(noDelayedLoad);
        gen.call((void*)m_loadDelayHandler);
        gen.L(noDelayedLoad);
    };

    // For the first instruction in the block: Check if there's a pending load as well
    if (!compileInstruction()) {
        return m_invalidBlock;
    }
    resolveInitialLoadDelay();
    processDelayedLoad();
    tickGTEWrites();
    m_firstInstruction = false;

    // Ends the block: writes back the registers and the PC, adds the cycles, returns to the dispatcher.
    const auto endBlock = [this, &count, &extra]() {
        emitGTEFlushQueue(false);
        flushRegs();
        if (!m_pcWrittenBack) {
            gen.mov(dword[contextPointer + PC_OFFSET], m_pc);
        }
        if constexpr (ENABLE_PROFILER) {
            endProfiling();
        }
        gen.add(qword[contextPointer + CYCLE_OFFSET],
                (count + extra * PCSX::Emulator::ROM_EXTRA_BIAS) * PCSX::Emulator::BIAS);
        gen.jmp((void*)m_returnFromBlock);
    };

    while (shouldContinue()) {
        // A branch in the delay slot of a branch that may be taken. When the first branch is taken, the
        // second one is relative to the first one's target, and its own delay slot is the instruction at
        // that target. Compile that path separately, then fall through to the not-taken path, which is
        // an ordinary branch with its delay slot after it.
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
            gen.cmp(dword[contextPointer + PC_OFFSET], firstTarget);
            gen.jne(notTaken, Xbyak::CodeGenerator::T_NEAR);

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
                gen.mov(dword[contextPointer + PC_OFFSET], firstTarget + 4);
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
    if (!m_pcWrittenBack) {
        gen.mov(dword[contextPointer + PC_OFFSET], m_pc);
    }

    // If this was the block at 0x8003'0000 (Start of shell), don't link the PC in case we fastboot
    if (startingPC == 0x80030000) {
        m_linkedPC = std::nullopt;
    }
    if constexpr (ENABLE_PROFILER) {
        endProfiling();
    }

    gen.add(qword[contextPointer + CYCLE_OFFSET],
            (count + extra * PCSX::Emulator::ROM_EXTRA_BIAS) * PCSX::Emulator::BIAS);  // Add block cycles;
    if (m_linkedPC && ENABLE_BLOCK_LINKING && m_linkedPC.value() != startingPC) {
        handleLinking();
    } else {
        gen.jmp((void*)m_returnFromBlock);
    }

    // Block linking might have invalidated this block, so don't cache the pointer to the invalidated block.
    // Instead, read the callback address again
    return *callback;
}

void DynaRecCPU::recSpecial(uint32_t code) {
    const auto func = m_recSPC[code & 0x3f];  // Look up the opcode in our decoding LUT
    (*this.*func)(code);                      // Jump into the handler to recompile it
}

// Checks if the block being compiled is one of the kernel call vectors
// If so, emit a call to "InterceptBIOS", which handles the kernel call debugger features
// Also handles fast booting by intercepting the shell reached signal and setting pc to $ra if fastboot is on
void DynaRecCPU::handleKernelCall() {
    if (m_pc == 0x80030000) {
        handleShellReached();
        return;
    }

    const uint32_t pc = m_pc & PCSX::g_emulator->getRamMask();
    const uint32_t base = (m_pc >> 20) & 0xffc;
    if ((base != 0x000) && (base != 0x800) && (base != 0xa00))
        return;  // Mask out the segment, return if not a kernel call vector

    if (pc == 0xA0 || pc == 0xB0 || pc == 0xC0) {
        gen.mov(arg2, m_pc);
        emitMemberFunctionCall(&PCSX::R3000Acpu::InterceptBIOS<false>, this);
    }
}

// Emits a jump to the dispatcher if there's no block to link to.
// Otherwise, handle linking blocks
void DynaRecCPU::handleLinking() {
    // Don't link unless the next PC is valid, and there's over 1MB of free space in the code cache
    if (isPcValid(m_linkedPC.value()) && gen.getRemainingSize() > 0x100000) {
        const auto nextPC = m_linkedPC.value();
        const auto nextBlockPointer = getBlockPointer(nextPC);
        const auto nextBlockOffset = (size_t)nextBlockPointer - (size_t)this;

        if (*nextBlockPointer == m_uncompiledBlock) {  // If the next block hasn't been compiled yet
            // Check that the block hasn't been invalidated/moved
            // The value will be patched later. Since all code is within the same 32MB segment,
            // We can get away with only checking the low 32 bits of the block pointer
            if (Xbyak::inner::IsInInt32(nextBlockOffset)) {
                gen.cmp(dword[contextPointer + nextBlockOffset], 0xcccccccc);
            } else {
                loadAddress(rax, nextBlockPointer);
                gen.cmp(dword[rax], 0xcccccccc);
            }

            const auto pointer = gen.getCurr<uint8_t*>();
            gen.jne((void*)m_returnFromBlock);  // Return if the block addr changed
            recompile(nextPC, false);           // Fallthrough to next block

            const uint32_t patchValue = (uint32_t)(uintptr_t)*nextBlockPointer;
            std::memcpy(pointer - 4, &patchValue, sizeof(patchValue));  // Patch comparison value
        } else {  // If it has already been compiled, link by jumping to the compiled code
            if (Xbyak::inner::IsInInt32(nextBlockOffset)) {
                gen.cmp(dword[contextPointer + nextBlockOffset], (uint32_t)(uintptr_t)*nextBlockPointer);
            } else {
                loadAddress(rax, nextBlockPointer);
                gen.cmp(dword[rax], (uint32_t)(uintptr_t)*nextBlockPointer);
            }

            gen.jne((void*)m_returnFromBlock);  // Return if the block addr changed
            gen.jmp((void*)*nextBlockPointer);  // Jump to linked block otherwise
        }
    } else {  // Can't link, so return to dispatcher
        gen.jmp((void*)m_returnFromBlock);
    }
}

void DynaRecCPU::handleShellReached() {
    Xbyak::Label alreadyReached;

    loadAddress(rax, &m_shellStarted);  // Check if shell has already been reached
    gen.cmp(Xbyak::util::byte[rax], 0);
    gen.jne(alreadyReached);  // Skip signalling that we've reached the shell if so

    loadThisPointer(arg1.cvt64());  // Signal that we've reached the shell
    call(signalShellReached);

    gen.jmp((void*)m_returnFromBlock);
    gen.L(alreadyReached);
}

// Peek at the next instruction to see if it has a read dependency on register "index"
// If it does, we need to emulate the load delay
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
#endif  // DYNAREC_X86_64
