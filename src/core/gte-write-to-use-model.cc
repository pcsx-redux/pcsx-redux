/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
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

#include <algorithm>
#include <array>

#include "core/gte-write-to-use.h"
#include "core/gte.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"

namespace {

// Registers are numbered 0-31 for data and 32-63 for control.
constexpr unsigned c_gteFlag = 32 + 31;

constexpr auto c_gteNeedTable = [] {
    std::array<std::array<uint8_t, 64>, 64> t{};
    for (auto &n : PCSX::GTEWriteToUse::c_needs) t[n.funct][n.reg] = n.need;
    return t;
}();

// How many instructions must separate a write to reg from the command for the command to read
// the new value.
unsigned gteWriteNeed(uint32_t code, unsigned reg) {
    using namespace PCSX::GTEWriteToUse;
    unsigned funct = code & 0x3f;
    if (funct != 0x12) return c_gteNeedTable[funct][reg];
    // MVMVA, by the role the register plays in this encoding.
    unsigned mx = (code >> 17) & 3;
    unsigned v = (code >> 15) & 3;
    unsigned cv = (code >> 13) & 3;
    if (reg >= 32) {
        unsigned c = reg - 32;
        if (mx < 3 && c >= mx * 8 && c < mx * 8 + 5) return c_mvmvaMatrix[c - mx * 8];
        if (cv < 3 && c >= 5 + cv * 8 && c < 5 + cv * 8 + 3) return c_mvmvaTranslation[c - 5 - cv * 8];
        return 0;
    }
    if (v == 3) return (reg >= 9 && reg <= 11) ? c_mvmvaVector[reg - 9] : 0;
    if (reg == v * 2) return std::max(c_mvmvaVector[0], c_mvmvaVector[1]);
    if (reg == v * 2 + 1) return c_mvmvaVector[2];
    return 0;
}

constexpr uint64_t gteBits(unsigned first, unsigned last) {
    uint64_t r = 0;
    for (unsigned i = first; i <= last; i++) r |= uint64_t(1) << i;
    return r;
}

// The registers a command writes.
uint64_t gteCommandOutputs(uint32_t code) {
    constexpr uint64_t flag = uint64_t(1) << c_gteFlag;
    constexpr uint64_t mac0 = uint64_t(1) << 24;
    constexpr uint64_t vector = gteBits(9, 11) | gteBits(25, 27);  // IR1-IR3, MAC1-MAC3
    constexpr uint64_t color = vector | gteBits(20, 22);           // and the RGB FIFO
    switch (code & 0x3f) {
        case 0x01:  // RTPS
        case 0x30:  // RTPT
            return flag | gteBits(8, 19) | gteBits(24, 27);
        case 0x06:  // NCLIP
            return flag | mac0;
        case 0x0c:  // OP
        case 0x12:  // MVMVA
        case 0x28:  // SQR
            return flag | vector;
        case 0x2d:  // AVSZ3
        case 0x2e:  // AVSZ4
            return flag | mac0 | (uint64_t(1) << 7);
        case 0x10:  // DPCS
        case 0x11:  // INTPL
        case 0x13:  // NCDS
        case 0x14:  // CDP
        case 0x16:  // NCDT
        case 0x1b:  // NCCS
        case 0x1c:  // CC
        case 0x1e:  // NCS
        case 0x20:  // NCT
        case 0x29:  // DCPL
        case 0x2a:  // DPCT
        case 0x3d:  // GPF
        case 0x3e:  // GPL
        case 0x3f:  // NCCT
            return flag | color;
    }
    return 0;
}

// The registers a write sets: a data write to SXYP shifts SXY0-SXY2, one to IRGB sets IR1-IR3,
// one to LZCS sets LZCR, and LZCR itself is read only.
uint64_t gteWriteTargets(bool ctrl, unsigned reg) {
    if (ctrl) return uint64_t(1) << (32 + reg);
    switch (reg) {
        case 15:
            return gteBits(12, 15);
        case 28:
            return gteBits(9, 11) | (uint64_t(1) << 28);
        case 30:
            return gteBits(30, 31);
        case 31:
            return 0;
    }
    return uint64_t(1) << reg;
}

}  // namespace

void PCSX::R3000Acpu::gteCommit(const GTEPendingWrite &w) {
    if (w.ctrl) {
        g_emulator->m_gte->CTC2(w.value, w.reg);
    } else {
        g_emulator->m_gte->MTC2(w.value, w.reg);
    }
}

// A queued write reaches a command if enough instructions separate the two, which depends on the
// command and the register (nugget tests/gte/write-to-use, see gte-write-to-use.h). One that does
// not is read by the command as its old value and lands while the command runs: a write by the
// instruction before the command lands after the command's results, so it overwrites a register
// the command also writes; an older one lands before them, so the command's result stays (nugget
// tests/gte/store-delay, SQR on IR1). Registers the command does not write take the written value.
void PCSX::R3000Acpu::gteRunCommand(uint32_t code, void (*execute)(R3000Acpu *cpu, uint32_t code)) {
    GTEPendingWrite late[3];
    unsigned nLate = 0;
    for (unsigned i = 0; i < m_gtePendingCount; i++) {
        auto &w = m_gtePending[i];
        // countdown is 2 for a write by the previous instruction, 1 for the one before.
        unsigned distance = 2 - w.countdown;
        if (distance >= gteWriteNeed(code, w.ctrl ? 32 + w.reg : w.reg)) {
            gteCommit(w);
        } else {
            late[nLate++] = w;
        }
    }
    m_gtePendingCount = 0;
    if (nLate == 0) {
        execute(this, code);
        return;
    }
    uint32_t before[64], written[64];
    std::copy_n(m_regs.CP2D.r, 32, before);
    std::copy_n(m_regs.CP2C.r, 32, before + 32);
    uint64_t targets = 0;
    uint64_t overResults = 0;
    for (unsigned i = 0; i < nLate; i++) {
        gteCommit(late[i]);
        uint64_t t = gteWriteTargets(late[i].ctrl, late[i].reg);
        targets |= t;
        if (late[i].countdown == 2) overResults |= t;
    }
    std::copy_n(m_regs.CP2D.r, 32, written);
    std::copy_n(m_regs.CP2C.r, 32, written + 32);
    std::copy_n(before, 32, m_regs.CP2D.r);
    std::copy_n(before + 32, 32, m_regs.CP2C.r);
    execute(this, code);
    targets &= ~gteCommandOutputs(code) | overResults;
    for (unsigned r = 0; r < 32; r++) {
        if (targets & (uint64_t(1) << r)) m_regs.CP2D.r[r] = written[r];
        if (targets & (uint64_t(1) << (32 + r))) m_regs.CP2C.r[r] = written[32 + r];
    }
}
