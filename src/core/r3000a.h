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

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <type_traits>

#include "core/kernel.h"
#include "core/psxcounters.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "mips-common/util/mips.hh"
#include "support/file.h"
#include "support/hashtable.h"

#if defined(__i386__) || defined(_M_IX86)
#define DYNAREC_NONE  // Hahano
#elif defined(__x86_64) || defined(_M_AMD64)
#define DYNAREC_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_ARCH_ISA_A64)
#define DYNAREC_AA64
#elif defined(__arm__) || defined(_M_ARM)
#define DYNAREC_NONE  // Placeholder for AA32
#elif defined(__powerpc__) || defined(_M_PPC)
#define DYNAREC_NONE  // Placeholder for PPC
#else
#define DYNAREC_NONE
#endif

// Disable AA64 JIT on Windows for now
#if defined(DYNAREC_AA64) && (defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__))
#undef DYNAREC_AA64
#define DYNAREC_NONE
#endif

namespace PCSX {

typedef union {
#if defined(__BIGENDIAN__)
    struct {
        uint8_t h3, h2, h, l;
    } b;
    struct {
        int8_t h3, h2, h, l;
    } sb;
    struct {
        uint16_t h, l;
    } w;
    struct {
        int16_t h, l;
    } sw;
#else
    struct {
        uint8_t l, h, h2, h3;
    } b;
    struct {
        uint16_t l, h;
    } w;
    struct {
        int8_t l, h, h2, h3;
    } sb;
    struct {
        int16_t l, h;
    } sw;
#endif
    uint32_t d;
    int32_t sd;
} PAIR;

using psxGPRRegs = Mips::GPRRegs;

typedef union {
    struct {
        uint32_t Index, Random, EntryLo0, BPC, Context, BDA, PIDMask, DCIC;
        uint32_t BadVAddr, BDAM, EntryHi, BPCM, Status, Cause, EPC, PRid;
        uint32_t Config, LLAddr, WatchLO, WatchHI, XContext, Reserved1, Reserved2, Reserved3;
        uint32_t Reserved4, Reserved5, ECC, CacheErr, TagLo, TagHi, ErrorEPC, Reserved6;
    } n;
    uint32_t r[32];
} psxCP0Regs;

// The SR bits that keep a written value. The rest read back as 0 whatever is written: bits 6-7, PE, TS,
// bits 23-24, RE and bits 26-27.
static constexpr uint32_t c_cop0StatusWritableMask = 0xf04fff3f;

typedef struct {
    int16_t x, y;
} SVector2D;

typedef struct {
    int16_t z, unused;
} SVector2Dz;

typedef struct {
    int16_t x, y, z, unused;
} SVector3D;

typedef struct {
    int16_t x, y, z, unused;
} LVector3D;

typedef struct {
    uint8_t r, g, b, c;
} CBGR;

typedef struct {
    int16_t m11, m12, m13, m21, m22, m23, m31, m32, m33, unused;
} SMatrix3D;

typedef union {
    struct {
        SVector3D v0, v1, v2;
        CBGR rgb;
        int32_t otz;
        int32_t ir0, ir1, ir2, ir3;
        SVector2D sxy0, sxy1, sxy2, sxyp;
        SVector2Dz sz0, sz1, sz2, sz3;
        CBGR rgb0, rgb1, rgb2;
        int32_t reserved;
        int32_t mac0, mac1, mac2, mac3;
        uint32_t irgb, orgb;
        int32_t lzcs, lzcr;
    } n;
    uint32_t r[32];
    PAIR p[32];
} psxCP2Data;

typedef union {
    struct {
        SMatrix3D rMatrix;
        int32_t trX, trY, trZ;
        SMatrix3D lMatrix;
        int32_t rbk, gbk, bbk;
        SMatrix3D cMatrix;
        int32_t rfc, gfc, bfc;
        int32_t ofx, ofy;
        int32_t h;
        int32_t dqa, dqb;
        int32_t zsf3, zsf4;
        int32_t flag;
    } n;
    uint32_t r[32];
    PAIR p[32];
} psxCP2Ctrl;

enum class Schedule : unsigned {
    SIO = 0,
    SIO1,
    CDRFIFO,
    CDRCOMMANDS,
    CDREAD,
    GPUDMA,
    MDECOUTDMA,
    SPUDMA,
    MDECINDMA,
    GPUOTCDMA,
    CDRDMA,
};

// The program counter. Reads convert to uint32_t. Outside the cpu core, writes go through
// R3000Acpu::setPC(), which also drops a branch still pending in a delay slot. raw() is for
// the core itself and for the paths that save and restore the whole cpu state.
class PCRegister {
  public:
    operator uint32_t() const { return m_value; }
    uint32_t &raw() { return m_value; }
    // Steps to the next instruction. Same caveat as raw(): no pending branch is touched.
    void increment() { m_value += 4; }

  private:
    uint32_t m_value = 0;
};
// Lets fmt, printf-style formats included, take it as the uint32_t it holds.
inline uint32_t format_as(PCRegister pc) { return pc; }
static_assert(sizeof(PCRegister) == sizeof(uint32_t));
static_assert(std::is_standard_layout_v<PCRegister>);

struct psxRegisters {
    psxGPRRegs GPR;   // General Purpose Registers
    psxCP0Regs CP0;   // COP0 Registers
    psxCP2Data CP2D;  // COP2 data registers
    psxCP2Ctrl CP2C;  // COP2 control registers
    PCRegister pc;    // Program counter
    uint32_t code;    // The current instruction
    uint64_t cycle;
    uint64_t previousCycles;
    uint32_t scheduleMask;
    std::atomic<bool> spuInterrupt;
    uint64_t scheduleTargets[32];
    uint64_t lowestTarget;
    uint64_t muldivReady;  // cycle at which hi/lo can be read without stalling
    uint64_t gteReady;     // cycle at which the GTE can be accessed without stalling
    uint8_t iCacheAddr[0x1000];
    uint8_t iCacheCode[0x1000];
    uint32_t getFutureCycle(std::chrono::nanoseconds delay) const { return cycle + durationToCycles(delay); }
    std::chrono::nanoseconds getFutureTime(uint32_t futureCycle) const {
        return std::chrono::nanoseconds(int64_t(int32_t(futureCycle - cycle)) * 1'000'000'000 /
                                        Emulator::m_psxClockSpeed);
    }
    static constexpr uint32_t durationToCycles(std::chrono::nanoseconds duration) {
        return duration.count() * Emulator::m_psxClockSpeed / 1'000'000'000;
    }
};

// R3000A Instruction Macros
#define _PC_ PCSX::g_emulator->m_cpu->m_regs.pc  // The next PC to be executed

#define _fOp_(code) ((code >> 26))           // The opcode part of the instruction register
#define _fFunct_(code) ((code) & 0x3F)       // The funct part of the instruction register
#define _fRd_(code) ((code >> 11) & 0x1F)    // The rd part of the instruction register
#define _fRt_(code) ((code >> 16) & 0x1F)    // The rt part of the instruction register
#define _fRs_(code) ((code >> 21) & 0x1F)    // The rs part of the instruction register
#define _fSa_(code) ((code >> 6) & 0x1F)     // The sa part of the instruction register
#define _fIm_(code) ((uint16_t)code)         // The immediate part of the instruction register
#define _fTarget_(code) (code & 0x03ffffff)  // The target part of the instruction register

#define _fImm_(code) ((int16_t)code)   // sign-extended immediate
#define _fImmU_(code) (code & 0xffff)  // zero-extended immediate
#define _fImmLU_(code) (code << 16)    // LUI

#define _Op_ _fOp_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _Funct_ _fFunct_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _Rd_ _fRd_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _Rt_ _fRt_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _Rs_ _fRs_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _Sa_ _fSa_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _Im_ _fIm_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _Target_ _fTarget_(PCSX::g_emulator->m_cpu->m_regs.code)

#define _Imm_ _fImm_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _ImmU_ _fImmU_(PCSX::g_emulator->m_cpu->m_regs.code)
#define _ImmLU_ _fImmLU_(PCSX::g_emulator->m_cpu->m_regs.code)

#define _rRs_ PCSX::g_emulator->m_cpu->m_regs.GPR.r[_Rs_]  // Rs register
#define _rRt_ PCSX::g_emulator->m_cpu->m_regs.GPR.r[_Rt_]  // Rt register
#define _rRd_ PCSX::g_emulator->m_cpu->m_regs.GPR.r[_Rd_]  // Rd register

#define _c2dRs_ PCSX::g_emulator->m_cpu->m_regs.CP2D.r[_Rs_]  // Rs cop2 data register
#define _c2dRt_ PCSX::g_emulator->m_cpu->m_regs.CP2D.r[_Rt_]  // Rt cop2 data register
#define _c2dRd_ PCSX::g_emulator->m_cpu->m_regs.CP2D.r[_Rd_]  // Rd cop2 data register

#define _rHi_ PCSX::g_emulator->m_cpu->m_regs.GPR.n.hi  // The HI register
#define _rLo_ PCSX::g_emulator->m_cpu->m_regs.GPR.n.lo  // The LO register

#define _JumpTarget_ ((_Target_ * 4) + (_PC_ & 0xf0000000))  // Calculates the target during a jump instruction
#define _BranchTarget_ ((int16_t)_Im_ * 4 + _PC_)            // Calculates the target during a branch instruction

class R3000Acpu {
  public:
    virtual ~R3000Acpu() { m_pcdrvFiles.destroyAll(); };
    R3000Acpu() {
        for (unsigned i = 0; i < 65536; i++) {
            m_availableFDs.push_back(i);
        }
    }
    virtual bool Init() { return false; }
    virtual void Execute() = 0; /* executes up to a debug break */

    enum class RunUntilResult {
        Reached,     /* pc landed on stopPC */
        OutOfCycles, /* the budget ran out first */
        Exception,   /* the guest took an exception */
        Unsupported, /* this cpu can't do it */
        Reentered,   /* called from inside another RunUntil */
    };
    /* Runs until pc reaches stopPC, the cycle budget is spent, or an exception is taken.
       Unlike Execute(), this doesn't run branchTest(), so counters, scheduled interrupts
       and the audio-driven frame throttle are all inert for the duration - the caller gets
       the cpu on its own, with no wall clock attached. Only the interpreter implements it;
       the recompilers can't stop on an arbitrary pc, since they emit no per-instruction
       checks and linked blocks jump straight past the dispatcher. */
    virtual RunUntilResult RunUntil(uint32_t stopPC, uint64_t cycleBudget) { return RunUntilResult::Unsupported; }

    virtual void Clear(uint32_t Addr, uint32_t Size) = 0;
    virtual void Shutdown() = 0;
    virtual void SetPGXPMode(uint32_t pgxpMode) = 0;
    virtual bool Implemented() = 0;
    // For the GUI dynarec disassembly widget
    virtual const uint8_t *getBufferPtr() = 0;
    virtual const size_t getBufferSize() = 0;

    const std::string &getName() { return m_name; }

    std::map<uint32_t, std::string> m_symbols;

    std::pair<const uint32_t, std::string> *findContainingSymbol(uint32_t addr);
    std::string *getSymbolAt(uint32_t addr);

    static int psxInit();
    virtual bool isDynarec() = 0;
    void psxReset();
    void psxShutdown();

    enum class Exception : uint32_t {
        Interrupt = 0,
        LoadAddressError = 4,
        StoreAddressError = 5,
        InstructionBusError = 6,
        DataBusError = 7,
        Syscall = 8,
        Break = 9,
        ReservedInstruction = 10,
        CoprocessorUnusable = 11,
        ArithmeticOverflow = 12,
    };
    bool exception(Exception e, bool bd, bool cop0 = false) {
        return exception(static_cast<std::underlying_type<Exception>::type>(e) << 2, bd, cop0);
    }
    bool exception(uint32_t code, bool bd, bool cop0 = false);
    void branchTest();

  private:
    void branchTestEvents();

  public:
    void psxSetPGXPMode(uint32_t pgxpMode);

    void schedule(Schedule s_, uint32_t eCycle) {
        unsigned s = static_cast<unsigned>(s_);
        PSXIRQ_LOG("Scheduling callback %08x at %08x\n", s, eCycle);
        const uint64_t cycle = m_regs.cycle;
        uint64_t target = cycle + uint64_t(double(eCycle) * m_scheduleScales[s]);
        m_regs.scheduleMask |= (1 << s);
        m_regs.scheduleTargets[s] = target;
        int64_t lowest = m_regs.lowestTarget - cycle;
        int64_t maybeNewLowest = target - cycle;
        if (maybeNewLowest < lowest) m_regs.lowestTarget = target;
    }

    void unschedule(Schedule s_) {
        unsigned s = static_cast<unsigned>(s_);
        PSXIRQ_LOG("Unscheduling callback %08x\n", s);
        m_regs.scheduleMask &= ~(1 << s);
    }

    bool isScheduled(Schedule s_) {
        unsigned s = static_cast<unsigned>(s_);
        return (m_regs.scheduleMask & (1 << s)) != 0;
    }

    // GTE command execution time, indexed by the command's function field.
    // Zero for the function codes that aren't documented commands.
    static uint32_t gteLatency(uint32_t funct) {
        static constexpr uint8_t latencies[64] = {
            0,  15, 0, 0,  0,  0, 8,  0, 0, 0, 0,  0,  6,  0, 0,  0,   // 00
            8,  8,  8, 19, 13, 0, 44, 0, 0, 0, 0,  17, 11, 0, 14, 0,   // 10
            30, 0,  0, 0,  0,  0, 0,  0, 5, 8, 17, 0,  0,  5, 6,  0,   // 20
            23, 0,  0, 0,  0,  0, 0,  0, 0, 0, 0,  0,  0,  5, 5,  39,  // 30
        };
        return latencies[funct & 0x3f];
    }
    // Called when a GTE command issues, after the cycle counter was advanced
    // for it. A new command waits for the previous one to finish.
    void gteStart(uint32_t latency) {
        gteStall();
        m_regs.gteReady = m_regs.cycle + latency + PCSX::Emulator::BIAS;
    }
    // Called by mfc2, cfc2 and swc2, which wait for a running command to
    // finish whichever register they access. mtc2, ctc2 and lwc2 don't wait.
    void gteStall() {
        if (m_regs.cycle < m_regs.gteReady) m_regs.cycle = m_regs.gteReady;
    }

    // Multiplier and divider result latency. A multiply takes 6, 9 or 13 cycles
    // depending on the magnitude of rs only; a divide always takes 36.
    static uint32_t multLatency(uint32_t rs, bool isSigned) {
        if (isSigned && (int32_t(rs) < 0)) rs = ~rs;
        if (rs < 0x800) return 6;
        if (rs < 0x100000) return 9;
        return 13;
    }
    static constexpr uint32_t c_divLatency = 36;
    // Called when a mul/div issues, after the cycle counter was advanced for it.
    // The result can be read by an instruction issuing latency cycles later.
    // A mul/div issued while the unit is busy does not wait: it replaces the
    // running operation.
    void muldivStart(uint32_t latency) { m_regs.muldivReady = m_regs.cycle + latency + PCSX::Emulator::BIAS; }
    // Called when hi/lo is read: waits for the running operation to complete.
    void muldivStall() {
        if (m_regs.cycle < m_regs.muldivReady) m_regs.cycle = m_regs.muldivReady;
    }
    // Called by mthi/mtlo, which abort a running multiply and leave the other
    // register partially computed; the partial value is not modelled. A
    // running divide is cancelled the same way, which hasn't been measured.
    void muldivCancel() { m_regs.muldivReady = 0; }

    psxRegisters m_regs;
    float m_scheduleScales[static_cast<unsigned>(Schedule::CDRDMA) + 1] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                                           1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    bool m_shellStarted = false;
    // Set by Emulator::vsync(), consumed by hasToRun(). It makes Execute()
    // RETURN once per emulated frame rather than only when the emulator is
    // paused. Frames were always rendered on this boundary - what was missing
    // is control coming back to the caller on it, which is what an
    // emscripten_set_main_loop callback has to do so the browser can run its
    // own event loop between frames.
    bool m_frameDone = false;

    virtual void Reset() {
        invalidateCache();
        m_regs.scheduleMask = 0;
        m_gtePendingCount = 0;
    }
    bool m_inISR = false;
    bool m_inRunUntil = false;

    // Guest-registered stack bounds, set through the guest command port and
    // checked by the interpreter each time $sp changes value.
    struct StackBounds {
        uint32_t low = 0;
        uint32_t high = 0;
        // $sp as of the last check; only a change against it is checked.
        uint32_t lastSP = 0;
        bool active = false;
    };
    StackBounds m_stackBounds;
    static constexpr int c_stackBoundsExitCode = 251;
    // Low and high both zero clears the bounds. Constant time, no allocation:
    // guests issue this on every context switch.
    void setStackBounds(uint32_t low, uint32_t high) {
        m_stackBounds.low = low;
        m_stackBounds.high = high;
        m_stackBounds.lastSP = m_regs.GPR.n.sp;
        m_stackBounds.active = (low != 0) || (high != 0);
    }
    void clearStackBounds() { m_stackBounds = {}; }
    // Called with $sp already updated; pc is the instruction that moved it.
    [[gnu::cold]] void stackBoundsViolation(uint32_t pc, uint32_t sp);
    // Called on a $sp change that stayed inside the bounds, when an allocator has
    // registered its metadata through pcsx_registerHeapMetadata. Returns true and
    // stops like stackBoundsViolation when that allocator has handed out memory
    // inside the bounds.
    bool checkHeapAgainstStack(uint32_t pc);
    bool m_nextIsDelaySlot = false;
    bool m_inDelaySlot = false;
    // The next instruction is the delay slot of the branch at m_nextSlotBranchPC, taken or not.
    // Only the interpreter sets these.
    bool m_nextIsBranchSlot = false;
    uint32_t m_nextSlotBranchPC = 0;
    struct {
        uint32_t index = 0;
        uint32_t value = 0;
        uint32_t mask = 0;
        uint32_t pcValue = 0;
        uint32_t branchPC = 0;
        bool active = false;
        bool pcActive = false;
        bool fromLink = false;
    } m_delayedLoadInfo[2];
    unsigned m_currentDelayedLoad = 0;
    // GTE register writes not yet visible to the GTE, oldest first.
    struct GTEPendingWrite {
        uint32_t value;
        uint32_t reg;
        bool ctrl;
        uint32_t countdown;
    };
    GTEPendingWrite m_gtePending[3] = {};
    uint32_t m_gtePendingCount = 0;
    // Lands a write through GTE::MTC2 / GTE::CTC2.
    void gteCommit(const GTEPendingWrite &w);
    // Runs a GTE command with the writes in m_gtePending, which it empties. execute runs the
    // command itself on the registers as the command reads them.
    void gteRunCommand(uint32_t code, void (*execute)(R3000Acpu *cpu, uint32_t code));
    template <typename T = uint32_t>
    T &delayedLoadRef(unsigned reg, uint32_t mask = 0) {
        if (reg >= 32) abort();
        auto &delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
        delayedLoad.active = true;
        delayedLoad.index = reg;
        delayedLoad.mask = mask;
        return reinterpret_cast<T &>(delayedLoad.value);
    }
    void delayedLoad(unsigned reg, uint32_t value, uint32_t mask = 0) {
        auto &ref = delayedLoadRef(reg, mask);
        ref = value;
    }
    // Moves pc from outside the instruction stream. Paused in a delay slot, the branch is
    // still pending and would take the instruction at the new pc as its delay slot, then
    // jump to its own target anyway.
    void setPC(uint32_t value) {
        m_regs.pc.raw() = value;
        m_nextIsDelaySlot = false;
        m_inDelaySlot = false;
        m_nextIsBranchSlot = false;
        for (auto &delayedLoad : m_delayedLoadInfo) {
            delayedLoad.pcActive = false;
            delayedLoad.fromLink = false;
        }
    }
    // Writes a register from outside the instruction stream. Paused right after a load, the
    // loaded value is still pending and would land over the new one on the next step.
    void setGPR(unsigned index, uint32_t value) {
        if (index >= 34) return;
        m_regs.GPR.r[index] = index == 0 ? 0 : value;
        for (auto &delayedLoad : m_delayedLoadInfo) {
            if (delayedLoad.index == index) delayedLoad.active = false;
        }
    }
    void delayedPCLoad(uint32_t value, bool fromLink) {
        auto &delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
        delayedLoad.pcActive = true;
        delayedLoad.pcValue = value;
        delayedLoad.branchPC = m_regs.pc - 4;
        delayedLoad.fromLink = fromLink;
    }
    void flushCurrentDelayedLoad() {
        auto &delayedLoad = m_delayedLoadInfo[m_currentDelayedLoad];
        if (delayedLoad.active) {
            uint32_t reg = m_regs.GPR.r[delayedLoad.index];
            reg &= delayedLoad.mask;
            reg |= delayedLoad.value;
            m_regs.GPR.r[delayedLoad.index] = reg;
            delayedLoad.active = false;
        }
    }

  protected:
    R3000Acpu(const std::string &name) : m_name(name) {}
    static inline const uint32_t MASKS[7] = {0, 0xffffff, 0xffff, 0xff, 0xff000000, 0xffff0000, 0xffffff00};
    static inline const uint32_t LWL_MASK[4] = {0xffffff, 0xffff, 0xff, 0};
    static inline const uint32_t LWL_MASK_INDEX[4] = {1, 2, 3, 0};
    static inline const uint32_t LWL_SHIFT[4] = {24, 16, 8, 0};
    static inline const uint32_t LWR_MASK[4] = {0, 0xff000000, 0xffff0000, 0xffffff00};
    static inline const uint32_t LWR_MASK_INDEX[4] = {0, 4, 5, 6};
    static inline const uint32_t LWR_SHIFT[4] = {0, 8, 16, 24};
    static inline const uint32_t SWL_MASK[4] = {0xffffff00, 0xffff0000, 0xff000000, 0};
    static inline const uint32_t SWL_SHIFT[4] = {24, 16, 8, 0};
    static inline const uint32_t SWR_MASK[4] = {0, 0xff, 0xffff, 0xffffff};
    static inline const uint32_t SWR_SHIFT[4] = {0, 8, 16, 24};
    inline bool hasToRun() {
        if (!m_shellStarted) {
            uint32_t &pc = m_regs.pc.raw();
            if (pc == 0x80030000) {
                m_shellStarted = true;
                g_system->m_eventBus->signal(Events::ExecutionFlow::ShellReached{});
            }
        }
        // Consumed HERE, on the way out, rather than anywhere that runs every
        // call: hasToRun() is evaluated once per block - roughly every fifty
        // instructions and around jumps, on the interpreter and both dynarecs
        // alike - so clearing it on entry would wipe it microseconds after
        // vsync() set it and the loop would never see it.
        if (m_frameDone) {
            m_frameDone = false;
            return false;
        }
        return g_system->running();
    }
    void processA0KernelCall(uint32_t call);
    void processB0KernelCall(uint32_t call);
    void logA0KernelCall(uint32_t call);
    void logB0KernelCall(uint32_t call);
    void logC0KernelCall(uint32_t call);

  public:
    template <bool checkPC = true>
    inline void InterceptBIOS(uint32_t currentPC) {
        const uint32_t pc = currentPC & g_emulator->getRamMask();

        if constexpr (checkPC) {
            const uint32_t base = (currentPC >> 20) & 0xffc;
            if ((base != 0x000) && (base != 0x800) && (base != 0xa00)) return;
        }

        auto r = m_regs.GPR.n;

        // Intercepts write, puts, putc, and putchar.
        // The BIOS doesn't have the TTY output set up by default,
        // so this hack enables us to properly display printfs. However,
        // sometimes, games will fully redirect printf's output, so it
        // will stop calling putchar. We'd need to also intercept
        // printf, but interpreting it is awful. The hope is it'd
        // eventually call one of these 4 functions.
        const uint32_t call = r.t1 & 0xff;
        switch (pc) {
            case 0xa0:
                processA0KernelCall(call);
                break;
            case 0xb0:
                processB0KernelCall(call);
                break;
        }

        if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::KernelLog>()) {
            switch (pc) {
                case 0xa0:
                    logA0KernelCall(call);
                    break;
                case 0xb0:
                    logB0KernelCall(call);
                    break;
                case 0xc0:
                    logC0KernelCall(call);
                    break;
            }
        }
    }

    /*
Formula One 2001
- Use old CPU cache code when the RAM location is
  updated with new code (affects in-game racing)
*/

    virtual void invalidateCache() {
        memset(m_regs.iCacheAddr, 0xff, sizeof(m_regs.iCacheAddr));
        memset(m_regs.iCacheCode, 0xff, sizeof(m_regs.iCacheCode));
    }

    /* The i-cache is 256 lines of four words. Each line has one tag holding the physical page
       (bits 31-12) of the code it caches and a valid bit per word (bits 3-0); the tag sits in the
       first word of the line's slot in iCacheAddr, the other three are unused. A fetch hits when
       the page matches and the word is valid. A miss on another page replaces the tag and clears
       every valid bit. The refill runs from the missed word to the end of the line, or to the end
       of its two-word half when BIU_CONFIG.IBLKSZ (bit 8) is clear, and sets the valid bits of the
       words it fetched. A fill entered at word 0 with the tag matching and some words invalid is
       a full-line refill, as the nugget tests/cpu/icache suite measures; a tag-matching miss at words
       1 to 3 refills from that word on, which is the same rule and is not measured on its own. */
    static constexpr uint32_t c_iCachePageMask = 0x1ffff000;
    uint32_t *iCacheTag(uint32_t addr) { return reinterpret_cast<uint32_t *>(m_regs.iCacheAddr) + (addr & 0xff0) / 4; }
    uint32_t *iCacheLine(uint32_t addr) {
        return reinterpret_cast<uint32_t *>(m_regs.iCacheCode) + (addr & 0xff0) / 4;
    }

    // Used by DMA into RAM, which drops the lines it overwrites. CPU stores do not get here.
    inline void flushICacheLine(uint32_t pc) {
        uint32_t pcBank = pc >> 24;
        if (pcBank == 0x00 || pcBank == 0x80) {
            uint32_t *tag = iCacheTag(pc);
            *tag = SWAP_LE32(SWAP_LE32(*tag) & ~0xf);
        }
    }

    inline uint32_t readICache(uint32_t pc) {
        uint32_t pcBank = pc >> 24;

        // cached - RAM
        if (pcBank == 0x00 || pcBank == 0x80) {
            uint32_t *tagPtr = iCacheTag(pc);
            uint32_t *line = iCacheLine(pc);
            const uint32_t word = (pc >> 2) & 3;
            const uint32_t page = pc & c_iCachePageMask;
            uint32_t tag = SWAP_LE32(*tagPtr);
            if ((tag & ~0xfff) != page) {
                tag = page;
            } else if (tag & (1 << word)) {
                return SWAP_LE32(line[word]);
            }
            const uint32_t last = (g_emulator->m_mem->getBIU() & 0x100) ? 3 : (word | 1);
            const uint32_t base = pc & ~0xf;
            for (uint32_t w = word; w <= last; w++) {
                line[w] = SWAP_LE32(g_emulator->m_mem->read32(base + w * 4, Memory::ReadType::Instr));
                tag |= 1 << w;
            }
            *tagPtr = SWAP_LE32(tag);
            return SWAP_LE32(line[word]);
        }

        // default
        return g_emulator->m_mem->read32(pc, Memory::ReadType::Instr);
    }

    /* A 32-bit load or store with COP0 SR.IsC set and BIU_CONFIG.IS1 set reaches the i-cache
       instead of memory. With BIU_CONFIG.TAG (bit 2) set it reaches the line's tag: a store sets
       the tag to the store address' page and the low four bits of the data, and a load returns the
       valid bits, bit 4 set when the tag's page matches the load address' page, and the upper bits
       of the addressed code word. Without TAG it reaches the addressed code word, and a store
       leaves the valid bits alone. */
    uint32_t readICacheIsolated(uint32_t addr, bool tagMode) {
        const uint32_t code = SWAP_LE32(iCacheLine(addr)[(addr >> 2) & 3]);
        if (!tagMode) return code;
        const uint32_t tag = SWAP_LE32(*iCacheTag(addr));
        const uint32_t match = (tag & ~0xfff) == (addr & c_iCachePageMask) ? 0x10 : 0;
        return (code & ~0x1f) | match | (tag & 0xf);
    }
    void writeICacheIsolated(uint32_t addr, uint32_t value, bool tagMode) {
        if (tagMode) {
            *iCacheTag(addr) = SWAP_LE32((addr & c_iCachePageMask) | (value & 0xf));
        } else {
            iCacheLine(addr)[(addr >> 2) & 3] = SWAP_LE32(value);
        }
    }

    /* Save states from before the per-line tag kept an address per word. Word 0 of a line held the
       line's address once all four words were fetched, 0xff once DMA had dropped the line, or
       0xffffffff. Keep the filled lines and their code, drop the rest. */
    void convertLegacyICache() {
        uint32_t *tags = reinterpret_cast<uint32_t *>(m_regs.iCacheAddr);
        for (unsigned line = 0; line < 256; line++) {
            const uint32_t old = SWAP_LE32(tags[line * 4]);
            const uint32_t tag = (old & 0xf) == 0 ? (old & c_iCachePageMask) | 0xf : 0xffffffff;
            for (unsigned w = 0; w < 4; w++) tags[line * 4 + w] = SWAP_LE32(w == 0 ? tag : 0xffffffff);
        }
    }

    // A CPU store reached RAM. The i-cache ignores stores; a recompiler drops the blocks it built
    // from that word.
    virtual void ClearStore(uint32_t addr) { Clear(addr, 1); }
    // The guest wrote one of the BIU_CONFIG values cache flush routines use. The recompilers do not
    // run code through the i-cache model and drop all their blocks here instead.
    virtual void biuCacheFlush() { invalidateCache(); }

  private:
    const std::string m_name;

    struct PCdrvFile;
    typedef Intrusive::HashTable<uint32_t, PCdrvFile> PCdrvFiles;
    struct PCdrvFile : public IO<File>, public PCdrvFiles::Node {
        PCdrvFile(const std::filesystem::path &filename) : IO<File>(new PosixFile(filename)) {}
        PCdrvFile(const std::filesystem::path &filename, FileOps::ReadWrite)
            : IO<File>(new PosixFile(filename, FileOps::READWRITE)) {}
        PCdrvFile(const std::filesystem::path &filename, FileOps::Truncate)
            : IO<File>(new PosixFile(filename, FileOps::TRUNCATE)) {}
        PCdrvFile(const std::filesystem::path &filename, FileOps::Create)
            : IO<File>(new PosixFile(filename, FileOps::CREATE)) {}
        // A TCP stream (tcp://host:port) or the networking probe (probe:net). Neither is a
        // real file: they are not saved in save states, and lseek on them fails.
        enum class Kind { File, Socket, Probe };
        PCdrvFile(IO<File> file, Kind kind) : IO<File>(std::move(file)), m_kind(kind) {}
        virtual ~PCdrvFile();
        bool refusesWrite() {
            if (m_kind == Kind::Probe) return true;
            return (m_kind == Kind::Socket) && (*this)->failed();
        }
        std::string m_relativeFilename;
        const Kind m_kind = Kind::File;
    };
    PCdrvFiles m_pcdrvFiles;
    std::list<uint16_t> m_availableFDs;
    // PCopen and PCread on network names. Both set v0 and v1.
    void pcdrvOpenNet(const std::string &filename);
    void pcdrvReadNet(PCdrvFile &file, IO<File> &memFile);

  public:
    void closeAllPCdrvFiles() {
        m_pcdrvFiles.destroyAll();
        m_availableFDs.clear();
        for (unsigned i = 0; i < 65536; i++) {
            m_availableFDs.push_back(i);
        }
    }
    void listAllPCdrvFiles(std::function<void(uint16_t, std::filesystem::path, bool, size_t, size_t)> walker) {
        for (auto iter = m_pcdrvFiles.begin(); iter != m_pcdrvFiles.end(); iter++) {
            if (iter->m_kind != PCdrvFile::Kind::File) continue;
            walker(iter->getKey(), iter->m_relativeFilename, (*iter)->writable(), (*iter)->rTell(), (*iter)->wTell());
        }
    }
    // A negative position means the end of the file.
    void restorePCdrvFile(const std::filesystem::path &path, uint16_t fd, bool writable, ssize_t readPosition,
                          ssize_t writePosition);
};

class Cpus {
  public:
    static std::unique_ptr<R3000Acpu> Interpreted();
    static std::unique_ptr<R3000Acpu> DynaRec();

  private:
    static std::unique_ptr<R3000Acpu> getDynaRec();
    static std::unique_ptr<R3000Acpu> getInterpreted();
};

}  // namespace PCSX
