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

#include "core/disr3000a.h"

#include <stdarg.h>
#include <string.h>

#include "core/psxemulator.h"
#include "core/r3000a.h"

// Names of registers
const char *PCSX::Disasm::s_disRNameGPR[] = {
    "r0", "at", "v0", "v1", "a0", "a1", "a2", "a3",  // 00
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",  // 08
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",  // 10
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",  // 18
};

const char *PCSX::Disasm::s_disRNameCP2D[] = {
    "vxy0", "vz0",  "vxy1", "vz1",  "vxy2", "vz2",  "rgb",  "otz",   // 00
    "ir0",  "ir1",  "ir2",  "ir3",  "sxy0", "sxy1", "sxy2", "sxyp",  // 08
    "sz0",  "sz1",  "sz2",  "sz3",  "rgb0", "rgb1", "rgb2", "res1",  // 10
    "mac0", "mac1", "mac2", "mac3", "irgb", "orgb", "lzcs", "lzcr",  // 18
};

const char *PCSX::Disasm::s_disRNameCP2C[] = {
    "r11r12", "r13r21", "r22r23", "r31r32", "r33", "trx",  "try",  "trz",   // 00
    "l11l12", "l13l21", "l22l23", "l31l32", "l33", "rbk",  "gbk",  "bbk",   // 08
    "lr1lr2", "lr3lg1", "lg2lg3", "lb1lb2", "lb3", "rfc",  "gfc",  "bfc",   // 10
    "ofx",    "ofy",    "h",      "dqa",    "dqb", "zsf3", "zsf4", "flag",  // 18
};

const char *PCSX::Disasm::s_disRNameCP0[] = {
    "Index",    "Random", "EntryLo0", "BPC",       // 00
    "Context",  "BDA",    "PIDMask",  "DCIC",      // 04
    "BadVAddr", "BDAM",   "EntryHi",  "BPCM",      // 08
    "Status",   "Cause",  "EPC",      "PRid",      // 0c
    "Config",   "LLAddr", "WatchLo",  "WatchHi",   // 10
    "XContext", "*RES*",  "*RES*",    "*RES*",     // 14
    "*RES*",    "*RES*",  "ECC",      "CacheErr",  // 18
    "TagLo",    "TagHi",  "ErrorEPC", "*RES*",     // 1c
};

const char *PCSX::Disasm::getSyscallName(uint32_t index) {
    switch (index) {
        case 0:
            return "NoFunction";
        case 1:
            return "EnterCriticalSection";
        case 2:
            return "ExitCriticalSection";
        case 3:
            return "ChangeThreadSubFunction";
        default:
            return "DeliverEvent";
    }
}

#undef declare
#undef _Funct_
#undef _Rd_
#undef _Rt_
#undef _Rs_
#undef _Sa_
#undef _Im_
#undef _Target_

#define declare(n) \
    void PCSX::Disasm::n(uint32_t code, uint32_t nextCode, uint32_t pc, bool *skipNext, bool *delaySlotNext)
#define _Funct_ ((code) & 0x3F)     // The funct part of the instruction register
#define _Rd_ ((code >> 11) & 0x1F)  // The rd part of the instruction register
#define _Rt_ ((code >> 16) & 0x1F)  // The rt part of the instruction register
#define _Rs_ ((code >> 21) & 0x1F)  // The rs part of the instruction register
#define _Sa_ ((code >> 6) & 0x1F)   // The sa part of the instruction register
#define _Im_ (code & 0xFFFF)        // The immediate part of the instruction register

#define _Target_ ((pc & 0xf0000000) + ((code & 0x03ffffff) * 4))
#define _Branch_ (pc + 4 + ((short)_Im_ * 4))
#define _OfB_ _Im_, _nRs_

namespace {
// Reads operand values straight from the running emulator. This is the default
// source and matches the historical "with values" behavior exactly: register
// reads come from the live register file, memory peeks go through pointerRead
// (side-effect-free, never touches the bus / MMIO).
struct LiveValueSource : public PCSX::Disasm::ValueSource {
    const uint8_t *ptr(uint32_t addr) {
        auto p = static_cast<const uint8_t *>(PCSX::g_emulator->m_mem->pointerRead(addr));
        if (p != nullptr) {
            return p;
        } else {
            static uint8_t dummy[4] = {0, 0, 0, 0};
            return dummy;
        }
    }
    uint32_t gpr(uint8_t reg) override { return PCSX::g_emulator->m_cpu->m_regs.GPR.r[reg]; }
    uint32_t cp0(uint8_t reg) override { return PCSX::g_emulator->m_cpu->m_regs.CP0.r[reg]; }
    uint32_t cp2d(uint8_t reg) override { return PCSX::g_emulator->m_cpu->m_regs.CP2D.r[reg]; }
    uint32_t cp2c(uint8_t reg) override { return PCSX::g_emulator->m_cpu->m_regs.CP2C.r[reg]; }
    uint32_t hi() override { return PCSX::g_emulator->m_cpu->m_regs.GPR.n.hi; }
    uint32_t lo() override { return PCSX::g_emulator->m_cpu->m_regs.GPR.n.lo; }
    uint8_t mem8(uint32_t addr) override { return *ptr(addr); }
    uint16_t mem16(uint32_t addr) override {
        int16_t v;
        memcpy(&v, ptr(addr), sizeof(v));
        return SWAP_LE16(v);
    }
    uint32_t mem32(uint32_t addr) override {
        int32_t v;
        memcpy(&v, ptr(addr), sizeof(v));
        return SWAP_LE32(v);
    }
};

struct StringDisasm : public PCSX::Disasm {
    void append(const char *str, ...) {
        va_list va;
        va_start(va, str);
        char buf[64];
        std::vsnprintf(buf, 64, str, va);
        va_end(va);
        size_t len = strlen(buf);
        memcpy(m_buf + m_len, buf, len + 1);
        m_len += len;
    }
    void comma() {
        if (m_gotArg) append(", ");
        m_gotArg = true;
    }
    virtual void Invalid() final { strcpy(m_buf, "*** Bad OP ***"); }
    virtual void OpCode(std::string_view name) final {
        std::sprintf(m_buf, "%-7s", name.data());
        m_gotArg = false;
        m_len = 7;
    }
    virtual void GPR(uint8_t reg) final {
        comma();
        append("$");
        append(s_disRNameGPR[reg]);
        if (m_withValues) {
            append("(%08x)", m_values->gpr(reg));
        }
    }
    virtual void CP0(uint8_t reg) final {
        comma();
        append("$");
        append(s_disRNameCP0[reg]);
        if (m_withValues) {
            append("(%08x)", m_values->cp0(reg));
        }
    }
    virtual void CP2D(uint8_t reg) final {
        comma();
        append("$");
        append(s_disRNameCP2D[reg]);
        if (m_withValues) {
            append("(%08x)", m_values->cp2d(reg));
        }
    }
    virtual void CP2C(uint8_t reg) final {
        comma();
        append("$");
        append(s_disRNameCP2C[reg]);
        if (m_withValues) {
            append("(%08x)", m_values->cp2c(reg));
        }
    }
    virtual void HI() final {
        comma();
        append("$hi");
        if (m_withValues) {
            append("(%08x)", m_values->hi());
        }
    }
    virtual void LO() final {
        comma();
        append("$lo");
        if (m_withValues) {
            append("(%08x)", m_values->lo());
        }
    }
    virtual void Imm16(int16_t value) final {
        comma();
        if (value < 0) {
            append("-0x%4.4x", -value);
        } else {
            append("0x%4.4x", value);
        }
    }
    virtual void Imm16u(uint16_t value) final {
        comma();
        append("0x%4.4x", value);
    }
    virtual void Imm32(uint32_t value) final {
        comma();
        append("0x%8.8x", value);
    }
    virtual void Target(uint32_t value) final {
        comma();
        append("0x%8.8x", value);
    }
    virtual void Sa(uint8_t value) final {
        comma();
        append("0x%2.2x", value);
    }
    virtual void OfB(int16_t offset, uint8_t reg, int size) {
        comma();
        if (offset < 0) {
            append("-0x%4.4x(%s)", -offset, s_disRNameGPR[reg]);
        } else {
            append("0x%4.4x(%s)", offset, s_disRNameGPR[reg]);
        }
        if (m_withValues) {
            uint32_t addr = m_values->gpr(reg) + offset;
            switch (size) {
                case 1:
                    append("([%8.8x] = %2.2x)", addr, m_values->mem8(addr));
                    break;
                case 2:
                    append("([%8.8x] = %4.4x)", addr, m_values->mem16(addr));
                    break;
                case 4:
                    append("([%8.8x] = %8.8x)", addr, m_values->mem32(addr));
                    break;
            }
        }
    }
    virtual void BranchDest(uint32_t value) final {
        comma();
        append("0x%8.8x", value);
    }
    virtual void Offset(uint32_t addr, int size) final {
        comma();
        append("0x%8.8x", addr);
        if (m_withValues) {
            switch (size) {
                case 1:
                    append("([%8.8x] = %2.2x)", addr, m_values->mem8(addr));
                    break;
                case 2:
                    append("([%8.8x] = %4.4x)", addr, m_values->mem16(addr));
                    break;
                case 4:
                    append("([%8.8x] = %8.8x)", addr, m_values->mem32(addr));
                    break;
            }
        }
    }
    virtual void SyscallName(const char *name) final { append(" %s", name); }
    virtual void reset() final {
        m_buf[0] = 0;
        m_len = 0;
    }
    char m_buf[512];
    size_t m_len = 0;
    bool m_gotArg = false;
    bool m_withValues = false;
    PCSX::Disasm::ValueSource *m_values = &PCSX::Disasm::liveValueSource();

  public:
    std::string get() { return m_buf; }
    void setValues(bool withValues) { m_withValues = withValues; }
    void setSource(PCSX::Disasm::ValueSource *values) { m_values = values ? values : &PCSX::Disasm::liveValueSource(); }
};
}  // namespace

#define dOpCodeGTE(i)                                                                                        \
    do {                                                                                                     \
        reset();                                                                                             \
        unsigned sf = (code >> 19) & 1;                                                                      \
        unsigned lm = (code >> 10) & 1;                                                                      \
        unsigned mxi = (code >> 17) & 3;                                                                     \
        unsigned vi = (code >> 15) & 3;                                                                      \
        unsigned cvi = (code >> 13) & 3;                                                                     \
        static constexpr const char *mx[] = {"rt", "ll", "lc", "??"};                                        \
        static constexpr const char *v[] = {"v0", "v1", "v2", "v3"};                                         \
        static constexpr const char *cv[] = {"tr", "bk", "fc", "zr"};                                        \
        std::string s = fmt::format("{}<sf:{},lm:{},mx:{},v:{},cv:{}>", i, sf, lm, mx[mxi], v[vi], cv[cvi]); \
        OpCode(s);                                                                                           \
    } while (0)
#define dOpCode(i) \
    do {           \
        reset();   \
        OpCode(i); \
    } while (0)
#define dImm16() Imm16(_Im_)
#define dImm16u() Imm16u(_Im_)
#define dTarget() Target(_Target_)
#define dSa() Sa(_Sa_)
#define dOfB(size) OfB(_Im_, _Rs_, size)
#define dBranch() BranchDest(_Branch_)

/*********************************************************
 * Arithmetic with immediate operand                      *
 * Format:  OP rt, rs, immediate                          *
 *********************************************************/
declare(disADDI) {
    // addi technically cannot be used for a 'move' pseudo-instruction
    dOpCode("addi");
    GPR(_Rt_);
    if (_Rt_ != _Rs_) GPR(_Rs_);
    dImm16();
}
declare(disADDIU) {
    if (_Rs_ == 0) {
        // this is the common pseudo-instruction to load an immediate 16 bits value
        disLI(_Rt_, _Im_, false, nextCode, skipNext);
        return;
    }
    dOpCode("addiu");
    GPR(_Rt_);
    if (_Rt_ != _Rs_) GPR(_Rs_);
    dImm16();
}
declare(disANDI) {
    dOpCode("andi");
    GPR(_Rt_);
    if (_Rt_ != _Rs_) GPR(_Rs_);
    dImm16u();
}
declare(disORI) {
    if (_Rs_ == 0) {
        // while rare, this can also be used to load an immediate 16-bits value
        disLI(_Rt_, _Im_, true, nextCode, skipNext);
        return;
    }
    dOpCode("ori");
    GPR(_Rt_);
    if (_Rt_ != _Rs_) GPR(_Rs_);
    dImm16u();
}
declare(disSLTI) {
    dOpCode("slti");
    GPR(_Rt_);
    if (_Rt_ != _Rs_) GPR(_Rs_);
    dImm16();
}
declare(disSLTIU) {
    dOpCode("sltiu");
    GPR(_Rt_);
    if (_Rt_ != _Rs_) GPR(_Rs_);
    dImm16u();
}
declare(disXORI) {
    dOpCode("xori");
    GPR(_Rt_);
    if (_Rt_ != _Rs_) GPR(_Rs_);
    dImm16u();
}

/*********************************************************
 * Register arithmetic                                    *
 * Format:  OP rd, rs, rt                                 *
 *********************************************************/
declare(disADD) {
    if (_Rt_ == 0) {
        dOpCode("move");
        GPR(_Rd_);
        GPR(_Rs_);
    } else if (_Rs_ == 0) {
        dOpCode("move");
        GPR(_Rd_);
        GPR(_Rt_);
    } else {
        dOpCode("add");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
        GPR(_Rt_);
    }
}
declare(disADDU) {
    if (_Rt_ == 0) {
        dOpCode("move");
        GPR(_Rd_);
        GPR(_Rs_);
    } else if (_Rs_ == 0) {
        dOpCode("move");
        GPR(_Rd_);
        GPR(_Rt_);
    } else {
        dOpCode("addu");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
        GPR(_Rt_);
    }
}
declare(disAND) {
    dOpCode("and");
    GPR(_Rd_);
    if (_Rd_ != _Rs_) GPR(_Rs_);
    GPR(_Rt_);
}
declare(disNOR) {
    if (_Rt_ == 0) {
        dOpCode("not");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
    } else if (_Rs_ == 0) {
        dOpCode("not");
        GPR(_Rd_);
        if (_Rd_ != _Rt_) GPR(_Rt_);
    } else {
        dOpCode("nor");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
        GPR(_Rt_);
    }
}
declare(disOR) {
    if ((_Rs_ == _Rt_) || (_Rt_ == 0)) {
        dOpCode("move");
        GPR(_Rd_);
        GPR(_Rs_);
    } else if (_Rs_ == 0) {
        dOpCode("move");
        GPR(_Rd_);
        GPR(_Rt_);
    } else {
        dOpCode("or");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
        GPR(_Rt_);
    }
}
declare(disSLT) {
    uint8_t nextIns = nextCode >> 26;
    uint8_t nextRt = (nextCode >> 16) & 0x1f;
    uint8_t nextRs = (nextCode >> 21) & 0x1f;
    int16_t nextImm = nextCode & 0xffff;
    if (skipNext && (nextIns == 0x05) && (_Rd_ == nextRs) && (_Rd_ == 1) && (nextRt == 0)) {
        // (slt $at, x, y) + (bne $at, $0, dest) = (blt x, y, dest)
        dOpCode("blt");
        GPR(_Rs_);
        GPR(_Rt_);
        BranchDest(pc + 4 + nextImm * 4);
        *skipNext = true;
        if (delaySlotNext) *delaySlotNext = true;
    } else {
        dOpCode("slt");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
        GPR(_Rt_);
    }
}
declare(disSLTU) {
    uint8_t nextIns = nextCode >> 26;
    uint8_t nextRt = (nextCode >> 16) & 0x1f;
    uint8_t nextRs = (nextCode >> 21) & 0x1f;
    int16_t nextImm = nextCode & 0xffff;
    if (skipNext && (nextIns == 0x05) && (_Rd_ == nextRs) && (_Rd_ == 1) && (nextRt == 0)) {
        // (sltu $at, x, y) + (bne $at, $0, dest) = (bltu x, y, dest)
        dOpCode("bltu");
        GPR(_Rs_);
        GPR(_Rt_);
        BranchDest(pc + 4 + nextImm * 4);
        *skipNext = true;
        if (delaySlotNext) *delaySlotNext = true;
    } else {
        dOpCode("sltu");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
        GPR(_Rt_);
    }
}
declare(disSUB) {
    if (_Rs_ == 0) {
        dOpCode("neg");
        GPR(_Rd_);
        if (_Rd_ != _Rt_) GPR(_Rt_);
    } else {
        dOpCode("sub");
        GPR(_Rd_);
        if (_Rd_ != _Rs_) GPR(_Rs_);
        GPR(_Rt_);
    }
}
declare(disSUBU) {
    dOpCode("subu");
    GPR(_Rd_);
    if (_Rd_ != _Rs_) GPR(_Rs_);
    GPR(_Rt_);
}
declare(disXOR) {
    dOpCode("xor");
    GPR(_Rd_);
    if (_Rd_ != _Rs_) GPR(_Rs_);
    GPR(_Rt_);
}

/*********************************************************
 * Register arithmetic & Register trap logic              *
 * Format:  OP rs, rt                                     *
 *********************************************************/
declare(disDIV) {
    dOpCode("div");
    GPR(_Rs_);
    GPR(_Rt_);
}
declare(disDIVU) {
    dOpCode("divu");
    GPR(_Rs_);
    GPR(_Rt_);
}
declare(disMULT) {
    dOpCode("mult");
    GPR(_Rs_);
    GPR(_Rt_);
}
declare(disMULTU) {
    dOpCode("multu");
    GPR(_Rs_);
    GPR(_Rt_);
}

/*********************************************************
 * Register branch logic                                  *
 * Format:  OP rs, offset                                 *
 *********************************************************/
declare(disBGEZ) {
    if (delaySlotNext) *delaySlotNext = true;
    if (_Rs_ == 0) {
        dOpCode("b");
    } else {
        dOpCode("bgez");
        GPR(_Rs_);
    }
    dBranch();
}
declare(disBGEZAL) {
    if (delaySlotNext) *delaySlotNext = true;
    if (_Rs_ == 0) {
        dOpCode("bal");
    } else {
        dOpCode("bgezal");
        GPR(_Rs_);
    }
    dBranch();
}
declare(disBGTZ) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("bgtz");
    GPR(_Rs_);
    dBranch();
}
declare(disBLEZ) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("blez");
    GPR(_Rs_);
    dBranch();
}
declare(disBLTZ) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("bltz");
    GPR(_Rs_);
    dBranch();
}
declare(disBLTZAL) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("bltzal");
    GPR(_Rs_);
    dBranch();
}

/*********************************************************
 * Shift arithmetic with constant shift                   *
 * Format:  OP rd, rt, sa                                 *
 *********************************************************/
declare(disSLL) {
    if (code) {
        dOpCode("sll");
        GPR(_Rd_);
        if (_Rd_ != _Rt_) GPR(_Rt_);
        dSa();
    } else {
        dOpCode("nop");
    }
}
declare(disSRA) {
    dOpCode("sra");
    GPR(_Rd_);
    if (_Rd_ != _Rt_) GPR(_Rt_);
    dSa();
}
declare(disSRL) {
    dOpCode("srl");
    GPR(_Rd_);
    if (_Rd_ != _Rt_) GPR(_Rt_);
    dSa();
}

/*********************************************************
 * Shift arithmetic with variant register shift           *
 * Format:  OP rd, rt, rs                                 *
 *********************************************************/
declare(disSLLV) {
    dOpCode("sllv");
    GPR(_Rd_);
    if (_Rd_ != _Rt_) GPR(_Rt_);
    GPR(_Rs_);
}
declare(disSRAV) {
    dOpCode("srav");
    GPR(_Rd_);
    if (_Rd_ != _Rt_) GPR(_Rt_);
    GPR(_Rs_);
}
declare(disSRLV) {
    dOpCode("srlv");
    GPR(_Rd_);
    if (_Rd_ != _Rt_) GPR(_Rt_);
    GPR(_Rs_);
}

/*********************************************************
 * Load higher 16 bits of the first word in GPR with imm  *
 * Format:  OP rt, immediate                              *
 *********************************************************/
declare(disLUI) {
    uint8_t nextIns = nextCode >> 26;
    uint8_t nextRt = (nextCode >> 16) & 0x1f;
    uint8_t nextRs = (nextCode >> 21) & 0x1f;
    uint16_t nextImm = nextCode & 0xffff;
    uint32_t imm = static_cast<uint32_t>(static_cast<int16_t>(nextImm)) + (static_cast<uint32_t>(_Im_) << 16);
    if (skipNext && (nextIns == 0x09) && (_Rt_ == nextRt) && (nextRt == nextRs)) {
        // next = addiu
        // usually, lui + addiu = la, for address relocations
        // we *could* mark it as an Offset, instead
        dOpCode("la");
        GPR(_Rt_);
        Imm32(imm);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x0d) && (_Rt_ == nextRt) && (nextRt == nextRs)) {
        // next = ori
        dOpCode("li");
        GPR(_Rt_);
        imm = static_cast<uint32_t>(nextImm) | (static_cast<uint32_t>(_Im_) << 16);
        Imm32(imm);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x20) && (_Rt_ == nextRs) && ((_Rt_ == 1) || (_Rt_ == nextRt))) {
        // next = lb
        // if using $at with load or stores, it's a simple full load or store, as
        // we're supposed to be guaranteed that $at is assembler-specific
        // if the load happens into the same register as the indirection, this is also
        // fine, since we know the register is clobbered
        dOpCode("lb");
        GPR(nextRt);
        Offset(imm, 1);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x21) && (_Rt_ == nextRs) && ((_Rt_ == 1) || (_Rt_ == nextRt))) {
        // next = lh
        dOpCode("lh");
        GPR(nextRt);
        Offset(imm, 2);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x23) && (_Rt_ == nextRs) && ((_Rt_ == 1) || (_Rt_ == nextRt))) {
        // next = lw
        dOpCode("lw");
        GPR(nextRt);
        Offset(imm, 4);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x24) && (_Rt_ == nextRs) && ((_Rt_ == 1) || (_Rt_ == nextRt))) {
        // next = lbu
        dOpCode("lbu");
        GPR(nextRt);
        Offset(imm, 1);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x25) && (_Rt_ == nextRs) && ((_Rt_ == 1) || (_Rt_ == nextRt))) {
        // next = lhu
        dOpCode("lhu");
        GPR(nextRt);
        Offset(imm, 2);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x28) && (_Rt_ == nextRs) && ((_Rt_ == 1) || (_Rt_ == nextRt))) {
        // next = sb
        dOpCode("sb");
        GPR(nextRt);
        Offset(imm, 1);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x29) && (_Rt_ == nextRs) && (_Rt_ == 1)) {
        // compiler can reuse the Rt register, so we can't make the same contraction
        // when the Rt registers are the same
        // next = sh
        dOpCode("sh");
        GPR(nextRt);
        Offset(imm, 2);
        *skipNext = true;
    } else if (skipNext && (nextIns == 0x2b) && (_Rt_ == nextRs) && (_Rt_ == 1)) {
        // next = sw
        dOpCode("sw");
        GPR(nextRt);
        Offset(imm, 4);
        *skipNext = true;
    } else {
        // normal lui, or something we couldn't properly infer as a contraction
        // note that it's not unusual for compilers to issue multiple lui, then
        // issue the second part of the pseudo-instruction, but then this is too
        // complex for a simpe disassembler such as this
        dOpCode("lui");
        GPR(_Rt_);
        dImm16u();
    }
}

void PCSX::Disasm::disLI(uint8_t reg, uint16_t imm, bool zeroExtend, uint32_t nextCode, bool *skipNext) {
    // Merge syscalls, which usually have the pattern
    // li $a0, syscallNumber
    // syscall 0x0000
    // A syscall with a nonzero code field is left alone, so its code still shows.
    static constexpr uint32_t c_plainSyscall = 0x0000000c;
    const bool liA0BeforeSyscall = (reg == 4) && (nextCode == c_plainSyscall);
    if (skipNext && liA0BeforeSyscall) {
        dOpCode("syscall");
        SyscallName(getSyscallName(imm));
        *skipNext = true;
        return;
    }
    dOpCode("li");
    GPR(reg);
    if (zeroExtend) {
        Imm16u(imm);
    } else {
        Imm16(imm);
    }
}

/*********************************************************
 * Move from HI/LO to GPR                                 *
 * Format:  OP rd                                         *
 *********************************************************/
declare(disMFHI) {
    dOpCode("mfhi");
    GPR(_Rd_);
    HI();
}
declare(disMFLO) {
    dOpCode("mflo");
    GPR(_Rd_);
    LO();
}

/*********************************************************
 * Move from GPR to HI/LO                                 *
 * Format:  OP rd                                         *
 *********************************************************/
declare(disMTHI) {
    dOpCode("mthi");
    HI();
    GPR(_Rs_);
}
declare(disMTLO) {
    dOpCode("mtlo");
    LO();
    GPR(_Rs_);
}

/*********************************************************
 * Special purpose instructions                           *
 * Format:  OP                                            *
 *********************************************************/
declare(disBREAK) {
    dOpCode("break");
    unsigned category = (code >> 16) & 0x3ff;
    unsigned exCode = (code >> 6) & 0x3ff;
    Imm16(category);
    if (exCode != 0) {
        Imm16(exCode);
    }
}
declare(disRFE) { dOpCode("rfe"); }
declare(disSYSCALL) {
    dOpCode("syscall");
    Imm32((code >> 6) & 0xfffff);
}

declare(disRTPS) { dOpCodeGTE("gte::rtps"); }
declare(disOP) { dOpCodeGTE("gte::op"); }
declare(disNCLIP) { dOpCodeGTE("gte::nclip"); }
declare(disDPCS) { dOpCodeGTE("gte::dpcs"); }
declare(disINTPL) { dOpCodeGTE("gte::intpl"); }
declare(disMVMVA) { dOpCodeGTE("gte::mvmva"); }
declare(disNCDS) { dOpCodeGTE("gte::ncds"); }
declare(disCDP) { dOpCodeGTE("gte::cdp"); }
declare(disNCDT) { dOpCodeGTE("gte::ncdt"); }
declare(disNCCS) { dOpCodeGTE("gte::nccs"); }
declare(disCC) { dOpCodeGTE("gte::cc"); }
declare(disNCS) { dOpCodeGTE("gte::ncs"); }
declare(disNCT) { dOpCodeGTE("gte::nct"); }
declare(disSQR) { dOpCodeGTE("gte::sqr"); }
declare(disDCPL) { dOpCodeGTE("gte::dcpl"); }
declare(disDPCT) { dOpCodeGTE("gte::dpct"); }
declare(disAVSZ3) { dOpCodeGTE("gte::avsz3"); }
declare(disAVSZ4) { dOpCodeGTE("gte::avsz4"); }
declare(disRTPT) { dOpCodeGTE("gte::rtpt"); }
declare(disGPF) { dOpCodeGTE("gte::gpf"); }
declare(disGPL) { dOpCodeGTE("gte::gpl"); }
declare(disNCCT) { dOpCodeGTE("gte::ncct"); }

declare(disMFC2) {
    dOpCode("mfc2");
    GPR(_Rt_);
    CP2D(_Rd_);
}
declare(disMTC2) {
    dOpCode("mtc2");
    CP2D(_Rd_);
    GPR(_Rt_);
}
declare(disCFC2) {
    dOpCode("cfc2");
    GPR(_Rt_);
    CP2C(_Rd_);
}
declare(disCTC2) {
    dOpCode("ctc2");
    CP2C(_Rd_);
    GPR(_Rt_);
}

/*********************************************************
 * Register branch logic                                  *
 * Format:  OP rs, rt, offset                             *
 *********************************************************/
declare(disBEQ) {
    if (delaySlotNext) *delaySlotNext = true;
    if (_Rs_ == _Rt_) {
        dOpCode("b");
        dBranch();
    } else if (_Rs_ == 0) {
        dOpCode("beqz");
        GPR(_Rt_);
        dBranch();
    } else if (_Rt_ == 0) {
        dOpCode("beqz");
        GPR(_Rs_);
        dBranch();
    } else {
        dOpCode("beq");
        GPR(_Rs_);
        GPR(_Rt_);
        dBranch();
    }
}
declare(disBNE) {
    if (delaySlotNext) *delaySlotNext = true;
    if (_Rs_ == 0) {
        dOpCode("bnez");
        GPR(_Rt_);
        dBranch();
    } else if (_Rt_ == 0) {
        dOpCode("bnez");
        GPR(_Rs_);
        dBranch();
    } else {
        dOpCode("bne");
        GPR(_Rs_);
        GPR(_Rt_);
        dBranch();
    }
}

/*********************************************************
 * Jump to target                                         *
 * Format:  OP target                                     *
 *********************************************************/
declare(disJ) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("j");
    dTarget();
}
declare(disJAL) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("jal");
    dTarget();
}

/*********************************************************
 * Register jump                                          *
 * Format:  OP rs, rd                                     *
 *********************************************************/
declare(disJR) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("jr");
    GPR(_Rs_);
}
declare(disJALR) {
    if (delaySlotNext) *delaySlotNext = true;
    dOpCode("jalr");
    GPR(_Rs_);
    if (_Rd_ != 31) GPR(_Rd_);
}

/*********************************************************
 * Load and store for GPR                                 *
 * Format:  OP rt, offset(base)                           *
 *********************************************************/
declare(disLB) {
    dOpCode("lb");
    GPR(_Rt_);
    dOfB(1);
}
declare(disLBU) {
    dOpCode("lbu");
    GPR(_Rt_);
    dOfB(1);
}
declare(disLH) {
    dOpCode("lh");
    GPR(_Rt_);
    dOfB(2);
}
declare(disLHU) {
    dOpCode("lhu");
    GPR(_Rt_);
    dOfB(2);
}
declare(disLW) {
    dOpCode("lw");
    GPR(_Rt_);
    dOfB(4);
}
declare(disLWL) {
    uint8_t nextIns = nextCode >> 26;
    uint8_t nextRt = (nextCode >> 16) & 0x1f;
    uint8_t nextRs = (nextCode >> 21) & 0x1f;
    uint16_t nextIm = nextCode & 0xffff;
    if (skipNext && (nextIns == 0x26) && (_Rt_ == nextRt) && (_Rs_ == nextRs) && (_Im_ == (nextIm + 3))) {
        dOpCode("ulw");
        *skipNext = true;
        GPR(_Rt_);
        OfB(_Im_ - 3, _Rs_, 4);
    } else {
        dOpCode("lwl");
        GPR(_Rt_);
        dOfB(4);
    }
}
declare(disLWR) {
    dOpCode("lwr");
    GPR(_Rt_);
    dOfB(4);
}
declare(disLWC2) {
    dOpCode("lwc2");
    CP2D(_Rt_);
    dOfB(4);
}
declare(disSB) {
    dOpCode("sb");
    GPR(_Rt_);
    dOfB(1);
}
declare(disSH) {
    dOpCode("sh");
    GPR(_Rt_);
    dOfB(2);
}
declare(disSW) {
    dOpCode("sw");
    GPR(_Rt_);
    dOfB(4);
}
declare(disSWL) {
    uint8_t nextIns = nextCode >> 26;
    uint8_t nextRt = (nextCode >> 16) & 0x1f;
    uint8_t nextRs = (nextCode >> 21) & 0x1f;
    uint16_t nextIm = nextCode & 0xffff;
    if (skipNext && (nextIns == 0x2e) && (_Rt_ == nextRt) && (_Rs_ == nextRs) && (_Im_ == (nextIm + 3))) {
        dOpCode("usw");
        *skipNext = true;
        GPR(_Rt_);
        OfB(_Im_ - 3, _Rs_, 4);
    } else {
        dOpCode("swl");
        GPR(_Rt_);
        dOfB(4);
    }
}
declare(disSWR) {
    dOpCode("swr");
    GPR(_Rt_);
    dOfB(4);
}
declare(disSWC2) {
    dOpCode("swc2");
    CP2D(_Rt_);
    dOfB(4);
}

/*********************************************************
 * Moves between GPR and COPx                             *
 * Format:  OP rt, fs                                     *
 *********************************************************/
declare(disMFC0) {
    dOpCode("mfc0");
    GPR(_Rt_);
    CP0(_Rd_);
}
declare(disMTC0) {
    dOpCode("mtc0");
    CP0(_Rd_);
    GPR(_Rt_);
}
declare(disCFC0) {
    dOpCode("cfc0");
    GPR(_Rt_);
    CP0(_Rd_);
}
declare(disCTC0) {
    dOpCode("ctc0");
    CP0(_Rd_);
    GPR(_Rt_);
}

/*********************************************************
 * Coprocessor branches                                   *
 * Format:  OP offset                                     *
 *********************************************************/
declare(disBCz) {
    if (delaySlotNext) *delaySlotNext = true;
    unsigned z = (code >> 26) & 3;
    dOpCode(fmt::format("bc{}{}", z, (_Rt_ & 1) ? 't' : 'f'));
    dBranch();
}

/*********************************************************
 * Unknown instruction (would generate an exception)      *
 * Format:  ?                                             *
 *********************************************************/
declare(disNULL) {
    reset();
    Invalid();
}

const PCSX::Disasm::TdisR3000AF PCSX::Disasm::s_disR3000A_SPECIAL[] = {
    // Subset of disSPECIAL
    &Disasm::disSLL,     &Disasm::disNULL,  &Disasm::disSRL,  &Disasm::disSRA,   // 00
    &Disasm::disSLLV,    &Disasm::disNULL,  &Disasm::disSRLV, &Disasm::disSRAV,  // 04
    &Disasm::disJR,      &Disasm::disJALR,  &Disasm::disNULL, &Disasm::disNULL,  // 08
    &Disasm::disSYSCALL, &Disasm::disBREAK, &Disasm::disNULL, &Disasm::disNULL,  // 0c
    &Disasm::disMFHI,    &Disasm::disMTHI,  &Disasm::disMFLO, &Disasm::disMTLO,  // 10
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,  // 14
    &Disasm::disMULT,    &Disasm::disMULTU, &Disasm::disDIV,  &Disasm::disDIVU,  // 18
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,  // 1c
    &Disasm::disADD,     &Disasm::disADDU,  &Disasm::disSUB,  &Disasm::disSUBU,  // 20
    &Disasm::disAND,     &Disasm::disOR,    &Disasm::disXOR,  &Disasm::disNOR,   // 24
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disSLT,  &Disasm::disSLTU,  // 28
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,  // 2c
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,  // 30
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,  // 34
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,  // 38
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,  // 3c
};

declare(disSPECIAL) {
    cTdisR3000AF ptr = s_disR3000A_SPECIAL[_Funct_];
    (*this.*ptr)(code, nextCode, pc, skipNext, delaySlotNext);
}

const PCSX::Disasm::TdisR3000AF PCSX::Disasm::s_disR3000A_BCOND[] = {
    // Subset of disBCOND
    &Disasm::disBLTZ,   &Disasm::disBGEZ,   &Disasm::disNULL, &Disasm::disNULL,  // 00
    &Disasm::disNULL,   &Disasm::disNULL,   &Disasm::disNULL, &Disasm::disNULL,  // 04
    &Disasm::disNULL,   &Disasm::disNULL,   &Disasm::disNULL, &Disasm::disNULL,  // 08
    &Disasm::disNULL,   &Disasm::disNULL,   &Disasm::disNULL, &Disasm::disNULL,  // 0c
    &Disasm::disBLTZAL, &Disasm::disBGEZAL, &Disasm::disNULL, &Disasm::disNULL,  // 10
    &Disasm::disNULL,   &Disasm::disNULL,   &Disasm::disNULL, &Disasm::disNULL,  // 14
    &Disasm::disNULL,   &Disasm::disNULL,   &Disasm::disNULL, &Disasm::disNULL,  // 18
    &Disasm::disNULL,   &Disasm::disNULL,   &Disasm::disNULL, &Disasm::disNULL,  // 1c
};

declare(disBCOND) {
    cTdisR3000AF ptr = s_disR3000A_BCOND[_Rt_];
    (*this.*ptr)(code, nextCode, pc, skipNext, delaySlotNext);
}

const PCSX::Disasm::TdisR3000AF PCSX::Disasm::s_disR3000A_COP0[] = {
    // Subset of disCOP0
    &Disasm::disMFC0, &Disasm::disNULL, &Disasm::disCFC0, &Disasm::disNULL,  // 00
    &Disasm::disMTC0, &Disasm::disNULL, &Disasm::disCTC0, &Disasm::disNULL,  // 04
    &Disasm::disBCz,  &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 08
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 0c
    &Disasm::disRFE,  &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 10
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 14
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 18
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 1c
};

declare(disCOP0) {
    cTdisR3000AF ptr = s_disR3000A_COP0[_Rs_];
    (*this.*ptr)(code, nextCode, pc, skipNext, delaySlotNext);
}

// COP1 and COP3 have no registers on the PS1; only their branches decode.
declare(disCOPn) {
    if (_Rs_ == 8) {
        disBCz(code, nextCode, pc, skipNext, delaySlotNext);
    } else {
        disNULL(code, nextCode, pc, skipNext, delaySlotNext);
    }
}

const PCSX::Disasm::TdisR3000AF PCSX::Disasm::s_disR3000A_BASIC[] = {
    // Subset of disBASIC (based on rs)
    &Disasm::disMFC2, &Disasm::disNULL, &Disasm::disCFC2, &Disasm::disNULL,  // 00
    &Disasm::disMTC2, &Disasm::disNULL, &Disasm::disCTC2, &Disasm::disNULL,  // 04
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 08
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 0c
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 10
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 14
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 18
    &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL, &Disasm::disNULL,  // 1c
};

declare(disBASIC) {
    cTdisR3000AF ptr = s_disR3000A_BASIC[_Rs_];
    (*this.*ptr)(code, nextCode, pc, skipNext, delaySlotNext);
}

const PCSX::Disasm::TdisR3000AF PCSX::Disasm::s_disR3000A_COP2[] = {
    // Subset of disR3000F_COP2 (based on funct)
    &Disasm::disBASIC, &Disasm::disRTPS,  &Disasm::disNULL,  &Disasm::disNULL,  // 00
    &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNCLIP, &Disasm::disNULL,  // 04
    &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  // 08
    &Disasm::disOP,    &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  // 0c
    &Disasm::disDPCS,  &Disasm::disINTPL, &Disasm::disMVMVA, &Disasm::disNCDS,  // 10
    &Disasm::disCDP,   &Disasm::disNULL,  &Disasm::disNCDT,  &Disasm::disNULL,  // 14
    &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNCCS,  // 18
    &Disasm::disCC,    &Disasm::disNULL,  &Disasm::disNCS,   &Disasm::disNULL,  // 1c
    &Disasm::disNCT,   &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  // 20
    &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  // 24
    &Disasm::disSQR,   &Disasm::disDCPL,  &Disasm::disDPCT,  &Disasm::disNULL,  // 28
    &Disasm::disNULL,  &Disasm::disAVSZ3, &Disasm::disAVSZ4, &Disasm::disNULL,  // 2c
    &Disasm::disRTPT,  &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  // 30
    &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  // 34
    &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  &Disasm::disNULL,  // 38
    &Disasm::disNULL,  &Disasm::disGPF,   &Disasm::disGPL,   &Disasm::disNCCT,  // 3c
};

declare(disCOP2) {
    if (_Rs_ == 8) {
        disBCz(code, nextCode, pc, skipNext, delaySlotNext);
        return;
    }
    cTdisR3000AF ptr = s_disR3000A_COP2[_Funct_];
    (*this.*ptr)(code, nextCode, pc, skipNext, delaySlotNext);
}

const PCSX::Disasm::TdisR3000AF PCSX::Disasm::s_disR3000A[] = {
    &Disasm::disSPECIAL, &Disasm::disBCOND, &Disasm::disJ,    &Disasm::disJAL,    // 00
    &Disasm::disBEQ,     &Disasm::disBNE,   &Disasm::disBLEZ, &Disasm::disBGTZ,   // 04
    &Disasm::disADDI,    &Disasm::disADDIU, &Disasm::disSLTI, &Disasm::disSLTIU,  // 08
    &Disasm::disANDI,    &Disasm::disORI,   &Disasm::disXORI, &Disasm::disLUI,    // 0c
    &Disasm::disCOP0,    &Disasm::disCOPn,  &Disasm::disCOP2, &Disasm::disCOPn,   // 10
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,   // 14
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,   // 18
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,   // 1c
    &Disasm::disLB,      &Disasm::disLH,    &Disasm::disLWL,  &Disasm::disLW,     // 20
    &Disasm::disLBU,     &Disasm::disLHU,   &Disasm::disLWR,  &Disasm::disNULL,   // 24
    &Disasm::disSB,      &Disasm::disSH,    &Disasm::disSWL,  &Disasm::disSW,     // 28
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disSWR,  &Disasm::disNULL,   // 2c
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disLWC2, &Disasm::disNULL,   // 30
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,   // 34
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disSWC2, &Disasm::disNULL,   // 38
    &Disasm::disNULL,    &Disasm::disNULL,  &Disasm::disNULL, &Disasm::disNULL,   // 3c
};

PCSX::Disasm::ValueSource &PCSX::Disasm::liveValueSource() {
    static LiveValueSource s_live;
    return s_live;
}

std::string PCSX::Disasm::asString(uint32_t code, uint32_t nextCode, uint32_t pc, bool *skipNext, bool withValues,
                                   ValueSource *values) {
    StringDisasm strd;
    strd.setValues(withValues);
    strd.setSource(values);
    strd.process(code, nextCode, pc, skipNext);
    char buf[64];
    snprintf(buf, 64, "%8.8x %8.8x: ", pc, code);
    std::string ret = buf + strd.get();
    strd.reset();
    return ret;
}
