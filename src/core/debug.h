/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "core/psxemulator.h"
#include "core/system.h"
#include "fmt/format.h"
#include "support/list.h"
#include "support/tree.h"

namespace PCSX {

class Debug {
  public:
    Debug();
    static uint32_t normalizeAddress(uint32_t address);
    static bool isInKernel(uint32_t address, bool biosIsKernel = true);
    static inline std::function<const char*()> s_breakpoint_type_names[] = {
        lC_("Breakpoint type", "Exec"), lC_("Breakpoint type", "Read"), lC_("Breakpoint type", "Write")};
    enum class BreakpointType { Exec, Read, Write };
    enum class BreakpointCondition { Always, Change, Greater, Less, Equal };

    void checkDMAread(unsigned c, uint32_t address, uint32_t len) {
        std::string cause = fmt::format("DMA channel {} read", c);
        checkBP(address, BreakpointType::Read, len, cause.c_str());
        logDMAAccess(address, len, false);
    }
    void checkDMAwrite(unsigned c, uint32_t address, uint32_t len) {
        std::string cause = fmt::format("DMA channel {} write", c);
        checkBP(address, BreakpointType::Write, len, cause.c_str());
        logDMAAccess(address, len, true);
    }

  private:
    void checkBP(uint32_t address, BreakpointType type, uint32_t width, const char* cause = "");
    void logDMAAccess(uint32_t address, uint32_t len, bool isWrite);

  public:
    // call this if PC is being set, like when the emulation is being reset, or when doing fastboot
    void updatedPC(uint32_t newPC);
    // call this as soon as possible after any instruction is run, with the oldPC, the newPC,
    // the opcode that was just executed, the next opcode to run, and a boolean to tell if we just
    // jumped around because of a previous "and link" opcode.
    void process(uint32_t oldPC, uint32_t newPC, uint32_t oldCode, uint32_t newCode, bool linked);
    std::string generateFlowIDC();
    std::string generateMarkIDC();

    class Breakpoint;
    typedef Intrusive::Tree<uint32_t, Breakpoint> BreakpointTreeType;
    typedef Intrusive::List<Breakpoint> BreakpointUserListType;
    struct InternalTemporaryList {};
    typedef Intrusive::List<Breakpoint, InternalTemporaryList> BreakpointTemporaryListType;

    typedef std::function<bool(Breakpoint*, uint32_t address, unsigned width, const char* cause)> BreakpointInvoker;

    class Breakpoint : public BreakpointTreeType::Node,
                       public BreakpointUserListType::Node,
                       public BreakpointTemporaryListType::Node {
      public:
        Breakpoint(BreakpointType type, const std::string& source, BreakpointInvoker invoker, uint32_t base,
                   std::string label = "")
            : m_type(type), m_source(source), m_invoker(invoker), m_base(base), m_label(label) {}
        std::string name() const;
        BreakpointType type() const { return m_type; }
        BreakpointCondition condition() const { return m_condition; }
        void setCondition(BreakpointCondition condition) { m_condition = condition; }
        uint32_t conditionData() const { return m_conditionData; }
        void setConditionData(uint32_t data) { m_conditionData = data; }
        unsigned width() const { return getHigh() - getLow() + 1; }
        uint32_t address() const { return getLow(); }
        bool enabled() const { return m_enabled; }
        void enable() const { m_enabled = true; }
        void disable() const { m_enabled = false; }
        const std::string& source() const { return m_source; }
        const std::string& label() const { return m_label; }
        void label(const std::string& label) const { m_label = label; }
        uint32_t base() const { return m_base; }

      private:
        bool trigger(uint32_t address, unsigned width, const char* cause) {
            if (m_enabled) return m_invoker(this, address, width, strlen(cause) > 0 ? cause : m_source.c_str());
            return true;
        }

        const BreakpointType m_type;
        BreakpointCondition m_condition = BreakpointCondition::Always;
        uint32_t m_conditionData = 0;
        const std::string m_source;
        const BreakpointInvoker m_invoker;
        mutable std::string m_label;
        uint32_t m_base;
        mutable bool m_enabled = true;

        friend class Debug;
    };

    void stepIn() {
        m_step = STEP_IN;
        startStepping();
    }
    void stepOver() {
        m_step = STEP_OVER;
        startStepping();
    }
    void stepOut();

    // Replay from a rewind snapshot, for reverse execution in the gdb server. Instruction
    // boundaries are named by m_regs.cycle as process() sees them. While a replay runs,
    // process() keeps what changes the guest, the COP0 hardware breakpoints, and records the
    // user breakpoints it would have triggered instead of running them, then pauses at the
    // first boundary at or past stopAt. startReplay() is called right after restoring the
    // snapshot, and checks the boundary the snapshot sits on, which process() saw before the
    // capture. It returns false when that boundary is already at or past stopAt, in which
    // case there is nothing to run.
    struct Replay {
        uint64_t stopAt = 0;
        // The boundary the replay paused at.
        std::optional<uint64_t> reached;
        // The last boundary before stopAt, and the last one of those with a breakpoint hit.
        uint64_t lastBoundary = 0;
        std::optional<uint64_t> lastHit;
        bool hitPending = false;
    };
    bool startReplay(uint64_t stopAt);
    std::optional<Replay> stopReplay() { return std::exchange(m_replay, std::nullopt); }
    bool replaying() const { return m_replay.has_value(); }

    bool m_mapping_e = false;
    bool m_mapping_r8 = false, m_mapping_r16 = false, m_mapping_r32 = false;
    bool m_mapping_w8 = false, m_mapping_w16 = false, m_mapping_w32 = false;
    bool m_breakmp_e = false;
    bool m_breakmp_r8 = false, m_breakmp_r16 = false, m_breakmp_r32 = false;
    bool m_breakmp_w8 = false, m_breakmp_w16 = false, m_breakmp_w32 = false;
    bool m_checkKernel = false;

    void clearMaps() {
        memset(m_mainMemoryMap, 0, sizeof(m_mainMemoryMap));
        memset(m_biosMemoryMap, 0, sizeof(m_biosMemoryMap));
        memset(m_scratchPadMap, 0, sizeof(m_scratchPadMap));
    }

  private:
    void startStepping();

  public:
    inline Breakpoint* addBreakpoint(
        uint32_t address, BreakpointType type, unsigned width, const std::string& source,
        BreakpointInvoker invoker = [](const Breakpoint* self, uint32_t address, unsigned width, const char* cause) {
            g_system->pause();
            return true;
        }) {
        uint32_t base = address & 0xe0000000;
        address &= ~0xe0000000;
        return &*m_breakpoints.insert(address, address + width - 1, new Breakpoint(type, source, invoker, base));
    }
    inline Breakpoint* addBreakpoint(
        uint32_t address, BreakpointType type, unsigned width, const std::string& source, std::string label,
        BreakpointInvoker invoker = [](const Breakpoint* self, uint32_t address, unsigned width, const char* cause) {
            g_system->pause();
            return true;
        }) {
        uint32_t base = address & 0xe0000000;
        address &= ~0xe0000000;
        return &*m_breakpoints.insert(address, address + width - 1, new Breakpoint(type, source, invoker, base, label));
    }
    const BreakpointTreeType& getTree() { return m_breakpoints; }
    const Breakpoint* lastBP() { return m_lastBP; }
    void removeBreakpoint(const Breakpoint* bp) {
        if (m_lastBP == bp) m_lastBP = nullptr;
        delete const_cast<Breakpoint*>(bp);
    }
    void removeAllBreakpoints() {
        m_breakpoints.clear();
        m_lastBP = nullptr;
    }

  private:
    bool triggerBP(Breakpoint* bp, uint32_t address, unsigned width, const char* reason = "");
    void replayBoundary(uint32_t pc, uint32_t code);
    void replayCheckAccess(uint32_t code);
    void noteReplayHit(uint32_t normalizedAddress, BreakpointType type, uint32_t width);
    std::optional<Replay> m_replay;
    BreakpointTreeType m_breakpoints;

    uint8_t m_mainMemoryMap[0x00800000] = {0};
    uint8_t m_biosMemoryMap[0x00080000] = {0};
    uint8_t m_scratchPadMap[0x00000400] = {0};

    void markMap(uint32_t address, int mask);
    bool isMapMarked(uint32_t address, int mask);

    enum {
        STEP_NONE,
        STEP_IN,
        STEP_OVER,
        STEP_OUT,
    } m_step;

    bool m_stepperHasBreakpoint = false;

    bool m_wasInISR = false;
    Breakpoint* m_lastBP = nullptr;
    std::optional<std::tuple<uint32_t, bool>> m_scheduledCop0;
    EventBus::Listener m_listener;
};

}  // namespace PCSX
