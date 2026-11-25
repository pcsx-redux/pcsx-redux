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

#include "gtest/gtest.h"
#include "lua/luawrapper.h"

// Numbers survive a toJson / fromJson round trip: large integers and doubles keep their value.
TEST(LuaWrapperJson, NumbersRoundTrip) {
    PCSX::Lua L;
    L.openlibs();
    L.load("t = { big = 4294967296, neg = -3000000000, frac = 0.1, small = 7 }", "test");
    L.getfield("t", LUA_GLOBALSINDEX);
    auto j = L.toJson();
    L.pop();
    EXPECT_EQ(j["big"].get<double>(), 4294967296.0);
    EXPECT_EQ(j["neg"].get<double>(), -3000000000.0);
    L.newtable();
    L.fromJson(j);
    L.setfield("u", LUA_GLOBALSINDEX);
    L.load("ok = u.big == 4294967296 and u.neg == -3000000000 and u.frac == 0.1 and u.small == 7", "test");
    L.getfield("ok", LUA_GLOBALSINDEX);
    EXPECT_TRUE(L.toboolean());
    L.close();
}

// JSON integers above the int range come back intact.
TEST(LuaWrapperJson, FromJsonLargeIntegers) {
    PCSX::Lua L;
    L.openlibs();
    L.newtable();
    L.fromJson(json::parse(R"({"u": 3000000000, "i": -3000000000, "f": 0.1})"));
    L.getfield("u");
    EXPECT_EQ(L.tonumber(), 3000000000.0);
    L.pop();
    L.getfield("i");
    EXPECT_EQ(L.tonumber(), -3000000000.0);
    L.pop();
    L.getfield("f");
    EXPECT_EQ(L.tonumber(), 0.1);
    L.close();
}
