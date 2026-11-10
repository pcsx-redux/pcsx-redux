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

#include "core/callstacks.h"

#include "core/callprofiler.h"
#include "core/logger.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"

#if 0
template <typename... Args>
static void debugLog(const char* format, const Args&... args) {
    PCSX::g_system->log(PCSX::LogClass::SYSTEM, "%08x: ", PCSX::g_emulator->m_cpu->m_regs.pc);
    PCSX::g_system->log(PCSX::LogClass::SYSTEM, format, args...);
}
#else
#define debugLog(...)
#endif

template <typename... Args>
static void normalLog(const char* format, const Args&... args) {
    PCSX::g_system->log(PCSX::LogClass::SYSTEM, "%08x: ", PCSX::g_emulator->m_cpu->m_regs.pc);
    PCSX::g_system->log(PCSX::LogClass::SYSTEM, format, args...);
}

void PCSX::CallStacks::setSP(uint32_t oldSP, uint32_t newSP) {
    debugLog("[CSDBG] setSP(0x%08x, 0x%08x)\n", oldSP, newSP);
    m_current = nullptr;
    auto callstack = m_callstacks.find(newSP, TreeType::INTERVAL_SEARCH);
    ListType todelete;
    m_currentSP = newSP;
    while (callstack != m_callstacks.end()) {
        if (callstack->getLow() == newSP) {
            debugLog("[CSDBG] setSP: switching callstack to 0x%08x - 0x%08x\n", callstack->getLow(),
                     callstack->getHigh());
            m_current = &*callstack;
        } else {
            // Don't nuke a callstack on a spurious lui
            if (newSP & 0xffff) {
                debugLog("[CSDBG] setSP: deleting obsolete callstack 0x%08x - 0x%08x\n", callstack->getLow(),
                         callstack->getHigh());
                todelete.push_back(&*callstack);
            }
        }
        callstack++;
    }
    todelete.destroyAll();
    if (m_pathTracking) refreshPath();
}

void PCSX::CallStacks::offsetSP(uint32_t oldSP, int32_t offset) {
    uint32_t lowSP = oldSP + offset;
    uint32_t highSP = oldSP;
    m_currentSP = lowSP;
    debugLog("[CSDBG] offsetSP: moving stack from 0x%08x to 0x%08x\n", oldSP, lowSP);
    if (!m_current) {
        debugLog("[CSDBG] offsetSP: no current stack, creating a new one\n");
        m_current = new CallStack();
    } else {
        highSP = m_current->getHigh();
        debugLog("[CSDBG] offsetSP: adjusting high pointer to 0x%08x\n", highSP);
    }
    if (lowSP > highSP) {
        debugLog("[CSDBG] inconsistent stack offset; adjusting (low = 0x%08x, high = 0x%08x)\n", lowSP, highSP);
        highSP = lowSP;
    }
    m_callstacks.unlink(m_current);
    auto callstacks = m_callstacks.find(lowSP, highSP);
    ListType todelete;
    while (callstacks != m_callstacks.end()) {
        debugLog("[CSDBG] deleting intersecting stack\n", callstacks->getLow(), callstacks->getHigh());
        todelete.push_back(&*callstacks);
        callstacks++;
    }
    todelete.destroyAll();
    m_callstacks.insert(lowSP, highSP, m_current);
    auto& calls = m_current->calls;
    CallStack::Call* maybeShadow = nullptr;
    unsigned popped = 0;
    uint32_t poppedRA = 0;
    while (true) {
        if ((calls.size() == 0) || ((calls.size() == 1) && maybeShadow)) break;
        auto last = --calls.end();
        if (maybeShadow) last--;
        if (last->sp >= lowSP) break;
        if (maybeShadow) {
            debugLog("[CSDBG] offsetSP: deleting shadow space call to 0x%08x from 0x%08x\n", maybeShadow->ra,
                     maybeShadow->sp);
            popped++;
            poppedRA = maybeShadow->ra;
            delete maybeShadow;
            maybeShadow = nullptr;
        }
        if (last->shadow) {
            maybeShadow = &*last;
        } else {
            debugLog("[CSDBG] offsetSP: deleting call to 0x%08x from 0x%08x\n", last->ra, last->sp);
            popped++;
            poppedRA = last->ra;
            delete &*last;
            m_current->ra = 0;
            m_current->fp = 0;
        }
    }
    if (m_pathTracking) {
        // A frame popped by its own epilogue still has a jr $ra and maybe a delay slot to run,
        // and those belong to the callee, so it goes back to pending until it returns. Several
        // frames going at once is an unwind, not a return, and nothing is pending after it.
        if (popped == 1) {
            m_current->pendingReturn = poppedRA;
        } else if (popped > 1) {
            m_current->pendingReturn = 0;
        }
        // A surviving shadow call may have lost its predecessor.
        if (maybeShadow) rechain(m_current);
        refreshPath();
    }
}

void PCSX::CallStacks::storeRA(uint32_t sp, uint32_t ra) {
    if (!m_current) {
        normalLog("[CS] Got 0x%08x written to 0x%08x, but we don't have a callstack for it.\n", ra, sp);
        return;
    }
    uint32_t low = m_current->getLow();
    uint32_t high = m_current->getHigh();
    bool shadow = false;
    if ((high < sp) || (low > sp)) {
        if ((low - 16) > sp) {
            g_system->log(
                LogClass::SYSTEM,
                "[CS] Got 0x%08x written to 0x%08x, but it's out of bounds of our current stack (0x%08x - 0x%08x).\n",
                ra, sp, low, high);
            return;
        }
        shadow = true;
    }
    uint32_t fp = 0;
    if (m_current->ra == ra) {
        fp = m_current->fp;
    }
    m_current->ra = 0;
    m_current->fp = 0;
    if (shadow) {
        debugLog("[CSDBG] storeRA: creating shadow space call to 0x%08x from 0x%08x on stack 0x%08x\n", ra, sp, high);
    } else {
        debugLog("[CSDBG] storeRA: creating call to 0x%08x from 0x%08x on stack 0x%08x\n", ra, sp, high);
    }
    auto call = new CallStack::Call(sp, fp, ra, shadow);
    if (m_pathTracking) {
        call->path = internPath(lastPath(m_current), ra);
        m_current->pendingReturn = 0;
    }
    m_current->calls.push_back(call);
    if (m_pathTracking) refreshPath();
}

void PCSX::CallStacks::loadRA(uint32_t sp) {
    if (!m_current) {
        debugLog("[CSDBG] Got a RA load from 0x%08x, but we don't have any active stack.\n", sp);
        return;
    }
    auto& calls = m_current->calls;
    if (calls.size() == 0) {
        debugLog("[CSDBG] Got a RA load from 0x%08x, but current stack is empty.\n", sp);
        return;
    }
    auto last = --calls.end();
    if (last->sp != sp) {
        debugLog("[CSDBG] Got a RA load from 0x%08x, but the active stack's at 0x%08x (ra: 0x%08x, size = %i)\n", sp,
                 last->sp, last->ra, calls.size());
    }
}

void PCSX::CallStacks::potentialRA(uint32_t ra, uint32_t sp, uint32_t target) {
    if (!m_current && m_currentSP) {
        offsetSP(m_currentSP, 0);
    }
    if (m_current) {
        debugLog("[CSDBG] potentialRA: to 0x%08x\n", ra);
        m_current->ra = ra;
        m_current->fp = sp;
        if (m_pathTracking) {
            m_pathBeforeCall = m_currentPath;
            m_delaySlotPC = ra - 4;
            m_current->pendingReturn = ra;
            refreshPath();
            g_emulator->m_profiler->countCall(m_currentPath, target);
        }
    }
}

uint32_t PCSX::CallStacks::internPath(uint32_t parent, uint32_t ra) {
    uint64_t key = (uint64_t(parent) << 32) | ra;
    auto [it, inserted] = m_pathIndex.try_emplace(key, uint32_t(m_pathNodes.size()));
    if (inserted) m_pathNodes.push_back({parent, ra});
    return it->second;
}

void PCSX::CallStacks::rechain(CallStack* stack) {
    uint32_t path = 0;
    for (auto& call : stack->calls) {
        path = internPath(path, call.ra);
        call.path = path;
    }
}

void PCSX::CallStacks::rechainAll() {
    for (auto& stack : m_callstacks) rechain(&stack);
    refreshPath();
}

void PCSX::CallStacks::refreshPath() {
    if (!m_current) {
        m_currentPath = 0;
        m_pendingReturn = 0;
        return;
    }
    uint32_t path = lastPath(m_current);
    m_pendingReturn = m_current->pendingReturn;
    if (m_pendingReturn) path = internPath(path, m_pendingReturn);
    m_currentPath = path;
}

void PCSX::CallStacks::setPathTracking(bool enabled) {
    if (enabled == m_pathTracking) return;
    m_pathTracking = enabled;
    if (enabled) {
        rechainAll();
    } else {
        m_currentPath = 0;
        m_pendingReturn = 0;
        m_delaySlotPC = 0;
    }
}

void PCSX::CallStacks::clearPaths() {
    m_pathNodes.clear();
    m_pathIndex.clear();
    m_pathNodes.push_back({0, 0});
    if (m_pathTracking) rechainAll();
}
