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

// The delay slot of a call runs on the caller's path, which CallStacks caches when the call
// is made. Clearing the paths renumbers every node, so the cached id has to be renumbered
// with them, or the delay slot lands on whatever path inherits the old id.

#include <stdint.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/callstacks.h"
#include "core/psxemulator.h"
#include "core/system.h"
#include "gtest/gtest.h"

namespace {

class TestSystem final : public PCSX::System {
  public:
    void softReset() override {}
    void hardReset() override {}
    void biosPutc(int) override {}
    const PCSX::Arguments& getArgs() const override { throw std::logic_error("unused"); }
    void printf(std::string&&) override {}
    void log(PCSX::LogClass, std::string&&) override {}
    void message(std::string&&) override {}
    void luaMessage(const std::string&, bool) override {}
    void update(bool) override {}
    void close() override {}
    void purgeAllEvents() override {}
    void testQuit(int) override {}
};

class CallStackPaths : public ::testing::Test {
  protected:
    void SetUp() override {
        m_oldSystem = PCSX::g_system;
        m_oldEmulator = PCSX::g_emulator;
        PCSX::g_system = &m_system;
        m_emulator = std::make_unique<PCSX::Emulator>();
        PCSX::g_emulator = m_emulator.get();
    }
    void TearDown() override {
        m_emulator.reset();
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }
    PCSX::CallStacks& callStacks() { return *m_emulator->m_callStacks; }
    // The return addresses of a path, outermost first.
    std::vector<uint32_t> chain(uint32_t path) {
        auto& nodes = callStacks().getPathNodes();
        std::vector<uint32_t> ret;
        while (path && path < nodes.size()) {
            ret.insert(ret.begin(), nodes[path].ra);
            path = nodes[path].parent;
        }
        return ret;
    }

  private:
    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

}  // namespace

TEST_F(CallStackPaths, ClearKeepsTheDelaySlotOnTheCaller) {
    constexpr uint32_t c_firstReturn = 0x80010108;
    constexpr uint32_t c_callerReturn = 0x80010208;
    constexpr uint32_t c_calleeReturn = 0x80020108;
    auto& cs = callStacks();
    cs.setPathTracking(true);
    cs.offsetSP(0x801ffff0, -32);
    // A first call that returns at once, so the caller's path below does not get id 1.
    cs.potentialRA(c_firstReturn, 0x801fffd0, 0x80030000);
    cs.pathAt(c_firstReturn - 4);
    cs.pathAt(c_firstReturn);
    // The caller spills its $ra, then calls; the clear lands between the jal and its slot.
    cs.storeRA(0x801fffe8, c_callerReturn);
    cs.potentialRA(c_calleeReturn, 0x801fffd0, 0x80040000);
    cs.clearPaths();
    uint32_t slot = cs.pathAt(c_calleeReturn - 4);
    EXPECT_EQ(chain(slot), std::vector<uint32_t>({c_callerReturn}));
    EXPECT_NE(slot, cs.currentPath());
    EXPECT_EQ(chain(cs.currentPath()), std::vector<uint32_t>({c_callerReturn, c_calleeReturn}));
}
