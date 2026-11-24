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

// A table holding a userdata under __obj is an object, whatever the index form used to reach it.
TEST(LuaWrapperIndex, IsObjectWithRelativeIndex) {
    PCSX::Lua L;
    L.openlibs();
    static int anchor;
    L.newtable();
    L.push(static_cast<void*>(&anchor));
    L.setfield("__obj");
    EXPECT_TRUE(L.isobject(1));
    EXPECT_TRUE(L.isobject(-1));
    EXPECT_TRUE(L.isobject());
    EXPECT_EQ(L.gettop(), 1);
    L.close();
}

// call() with a relative table index calls the function found in that table.
TEST(LuaWrapperIndex, CallWithRelativeTableIndex) {
    PCSX::Lua L;
    L.openlibs();
    L.load("T = { f = function() called = true end }", "test");
    L.getfield("T", LUA_GLOBALSINDEX);
    EXPECT_NO_THROW(L.call("f", -1));
    L.getfield("called", LUA_GLOBALSINDEX);
    EXPECT_TRUE(L.toboolean());
    L.close();
}
