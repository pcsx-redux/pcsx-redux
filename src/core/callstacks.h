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

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "core/system.h"
#include "support/eventbus.h"
#include "support/list.h"
#include "support/tree.h"

namespace PCSX {

struct SaveStateWrapper;

class CallStacks {
  public:
    struct CallStack;

  private:
    typedef Intrusive::Tree<uint32_t, CallStack> TreeType;
    typedef Intrusive::List<CallStack> ListType;

  public:
    struct CallStack : public TreeType::Node, public ListType::Node {
        struct Call;
        typedef Intrusive::List<Call> ListType;
        struct Call : public ListType::Node {
            Call(uint32_t sp_, uint32_t fp_, uint32_t ra_, bool shadow_) : sp(sp_), fp(fp_), ra(ra_), shadow(shadow_) {}
            uint32_t sp, fp, ra;
            bool shadow;
            // Interned id of the call path ending with this call, valid while path tracking is on.
            uint32_t path = 0;
        };
        ~CallStack() { calls.destroyAll(); }
        ListType calls;
        uint32_t ra = 0, fp = 0;
        // Path tracking only: the return address of a callee that has not spilled $ra, or has
        // already popped its frame but not returned yet. Zero when there is none.
        uint32_t pendingReturn = 0;
    };

    // Call paths, interned for the profiler. Each path is a node in a prefix tree keyed on
    // (parent node, return address), and node 0 is the empty path. A node id names exactly one
    // path, so two paths can never land in the same bucket, which a bare hash cannot promise.
    // The current path is kept up to date at the push and pop points below, so reading it
    // costs nothing per instruction.
    struct PathNode {
        uint32_t parent;
        uint32_t ra;
    };
    void setPathTracking(bool enabled);
    bool pathTracking() const { return m_pathTracking; }
    // Drops every node and re-interns the live frames. Ids handed out earlier become invalid.
    void clearPaths();
    uint32_t currentPath() const { return m_currentPath; }
    const std::vector<PathNode>& getPathNodes() const { return m_pathNodes; }
    // Called for every instruction while tracking, before it executes, and returns the path
    // it runs on. The delay slot of a call still runs in the caller, and reaching the pending
    // return address means the callee that owned it has returned.
    uint32_t pathAt(uint32_t pc) {
        if (m_delaySlotPC) [[unlikely]] {
            bool inSlot = pc == m_delaySlotPC;
            m_delaySlotPC = 0;
            if (inSlot) return m_pathBeforeCall;
        }
        if (m_pendingReturn && (pc == m_pendingReturn)) [[unlikely]] {
            m_current->pendingReturn = 0;
            refreshPath();
        }
        return m_currentPath;
    }

    bool hasCurrent() { return m_current; }
    const CallStack& getCurrent() { return *m_current; }
    const TreeType& getCallstacks() { return m_callstacks; }

    void serialize(SaveStateWrapper*);
    void deserialize(const SaveStateWrapper*);

  private:
    TreeType m_callstacks;
    CallStack* m_current = nullptr;
    uint32_t m_currentSP = 0;

    EventBus::Listener m_listener;

    bool m_pathTracking = false;
    uint32_t m_currentPath = 0;
    // Mirror of m_current->pendingReturn, or 0 without a current stack.
    uint32_t m_pendingReturn = 0;
    // Set by a call for its delay slot, with the path from before the call.
    uint32_t m_delaySlotPC = 0;
    uint32_t m_pathBeforeCall = 0;
    std::vector<PathNode> m_pathNodes;
    std::unordered_map<uint64_t, uint32_t> m_pathIndex;
    uint32_t internPath(uint32_t parent, uint32_t ra);
    static uint32_t lastPath(const CallStack* stack) { return stack->calls.empty() ? 0 : (--stack->calls.end())->path; }
    void rechain(CallStack* stack);
    void rechainAll();
    void refreshPath();

  public:
    CallStacks() : m_listener(g_system->m_eventBus) {
        m_pathNodes.push_back({0, 0});
        m_listener.listen<Events::ExecutionFlow::Reset>([this](const auto& event) {
            m_callstacks.clear();
            m_current = nullptr;
            m_currentSP = 0;
            m_currentPath = 0;
            m_pendingReturn = 0;
            m_delaySlotPC = 0;
        });
    }
    ~CallStacks() { m_callstacks.destroyAll(); }
    void setSP(uint32_t oldSP, uint32_t newSP);
    void offsetSP(uint32_t oldSP, int32_t offset);
    void storeRA(uint32_t sp, uint32_t ra);
    void loadRA(uint32_t sp);
    // target is the address being called, 0 if unknown. Only the profiler uses it.
    void potentialRA(uint32_t ra, uint32_t sp, uint32_t target = 0);
};

}  // namespace PCSX
