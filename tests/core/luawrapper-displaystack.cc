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

#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lua/luawrapper.h"

namespace {

// Installs C++ printers for the duration of a test and records what reaches them.
struct Printers {
    std::vector<std::string> normal;
    std::vector<std::string> error;
    Printers(bool withNormal, bool withError) {
        if (withNormal) PCSX::Lua::normalPrinter = [this](std::string_view s) { normal.emplace_back(s); };
        if (withError) PCSX::Lua::errorPrinter = [this](std::string_view s) { error.emplace_back(s); };
    }
    ~Printers() {
        PCSX::Lua::normalPrinter = nullptr;
        PCSX::Lua::errorPrinter = nullptr;
    }
};

bool inDisplayStack(PCSX::Lua& L) {
    L.getfield("IN_DISPLAY_STACK", LUA_REGISTRYINDEX);
    bool r = L.toboolean();
    L.pop();
    return r;
}

}  // namespace

// A userdata entry is printed with its index, like every other entry.
TEST(LuaWrapperDisplayStack, UserdataEntryKeepsItsIndex) {
    PCSX::Lua L;
    L.openlibs();
    Printers p(true, true);
    L.newuser(16);
    L.push(lua_Number(42));
    L.displayStack(false);
    ASSERT_EQ(p.normal.size(), 2u);
    EXPECT_EQ(p.normal[0].rfind("1: (Userdata) ", 0), 0u) << p.normal[0];
    EXPECT_EQ(p.normal[1], "2: (Number) 42");
    EXPECT_EQ(L.gettop(), 2);
    L.close();
}

// Through the Lua printer, a userdata entry reaches printError and the in-display flag is cleared afterwards.
TEST(LuaWrapperDisplayStack, UserdataThroughLuaPrinter) {
    PCSX::Lua L;
    L.openlibs();
    L.load("printed = {} function printError(s) printed[#printed + 1] = s end", "test");
    L.newuser(16);
    EXPECT_NO_THROW(L.displayStack(true));
    EXPECT_FALSE(inDisplayStack(L));
    L.getfield("printed", LUA_GLOBALSINDEX);
    ASSERT_EQ(L.length(), 1u);
    L.rawgeti(1);
    EXPECT_EQ(L.tostring().rfind("1: (Userdata) ", 0), 0u) << L.tostring();
    L.close();
}

// A Lua printer that raises does not leave the in-display flag set.
TEST(LuaWrapperDisplayStack, RaisingPrinterClearsFlag) {
    PCSX::Lua L;
    L.openlibs();
    L.load("function printError(s) error('printer broke') end", "test");
    L.push(lua_Number(1));
    try {
        L.displayStack(true);
    } catch (...) {
    }
    EXPECT_FALSE(inDisplayStack(L));
    L.close();
}

// With only the error printer set, an error display goes to it.
TEST(LuaWrapperDisplayStack, OnlyErrorPrinterSet) {
    PCSX::Lua L;
    L.openlibs();
    Printers p(false, true);
    L.push(lua_Number(7));
    EXPECT_NO_THROW(L.displayStack(true));
    ASSERT_EQ(p.error.size(), 1u);
    EXPECT_EQ(p.error[0], "1: (Number) 7");
    EXPECT_EQ(L.gettop(), 1);
    L.close();
}

// The error dump of a failed pcall still shows what sat below the called function.
TEST(LuaWrapperDisplayStack, PcallErrorDumpKeepsBottomEntry) {
    PCSX::Lua L;
    L.openlibs();
    Printers p(true, true);
    L.push(lua_Number(42));
    L.load("return function() error('boom') end", "test");
    EXPECT_THROW(L.pcall(0), std::runtime_error);
    ASSERT_FALSE(p.error.empty());
    EXPECT_EQ(p.error[0], "1: (Number) 42");
    for (auto& line : p.error) EXPECT_EQ(line.find("(Table)"), std::string::npos) << line;
    L.close();
}
