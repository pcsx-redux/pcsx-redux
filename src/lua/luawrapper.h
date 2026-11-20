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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "fmt/printf.h"
#include "json.hpp"
#include "lua.hpp"
#include "support/ssize_t.h"

using json = nlohmann::json;

namespace PCSX {

class Lua {
  public:
    typedef int (*openlualib_t)(lua_State* L);

    static std::function<void(std::string_view)> normalPrinter;
    static std::function<void(std::string_view)> errorPrinter;

    Lua();
    Lua(lua_State* L) : L(L) {}
    Lua(Lua&& oL) noexcept : L(oL.L) { oL.L = nullptr; }
    Lua(const Lua& oL) : L(oL.L) {}

    Lua& operator=(Lua&& oL) noexcept;

    typedef int (*lua_CallWrapper)(lua_State*, lua_CFunction);

    int getabsolute(int index) {
        if ((index < 0) && (index > LUA_REGISTRYINDEX)) return gettop() + index + 1;
        return index;
    }

    int ref(int t = -2) { return luaL_ref(L, t); }
    void unref(int ref, int t = -1) { luaL_unref(L, t, ref); }

    void close();
    void openlibs();
    void open_base();
    void open_bit();
    void open_debug();
    void open_ffi();
    void open_jit();
    void open_math();
    void open_package();
    void open_string();
    void open_table();

    Lua thread(bool saveit = false);
    void weaken();

    int wrap_open(openlualib_t open) {
        int n = gettop();
        int r = open(L);
        while (n < gettop()) pop();
        return r;
    }
    void openlib(const char* libname, const struct luaL_Reg* l, int nup) { luaL_openlib(L, libname, l, nup); }

    void setCallWrap(lua_CallWrapper wrapper);
    void declareFunc(std::string_view funcName, lua_CFunction f, int tableIdx = LUA_GLOBALSINDEX, int upvalues = 0);
    void declareFunc(std::string_view funcName, std::function<int(Lua)>&& f, int tableIdx = LUA_GLOBALSINDEX);

    void call(std::string_view funcName, int tableIdx = LUA_GLOBALSINDEX, int nArgs = 0);
    void call(int nArgs = 0);
    int pcall(int nArgs = 0);

    void push() {
        checkstack();
        lua_pushnil(L);
    }
    void push(lua_Number n) {
        checkstack();
        lua_pushnumber(L, n);
    }
    void push(std::string_view s) {
        checkstack();
        lua_pushlstring(L, s.data(), s.size());
    }
    void push(bool b) {
        checkstack();
        lua_pushboolean(L, b);
    }
    template <size_t S>
    void push(const char (&str)[S]) {
        checkstack();
        lua_pushlstring(L, str, S - 1);
    }
    void push(const char* str, ssize_t size = -1) {
        if (size < 0) size = strlen(str);
        checkstack();
        lua_pushlstring(L, str, size);
    }
    void push(void* p) {
        checkstack();
        lua_pushlightuserdata(L, p);
    }
    void push(lua_CFunction f, int n = 0) {
        checkstack();
        lua_pushcclosure(L, f, n);
    }
    void push(std::function<int(Lua)>&& f);
    void pop(int idx = 1) { lua_pop(L, idx); }
    int checkstack(int extra = 1) { return lua_checkstack(L, extra); }

    int next(int t = -2) { return lua_next(L, t); }
    void copy(int i = -1) {
        checkstack();
        lua_pushvalue(L, i);
    }
    void remove(int i = 1) { lua_remove(L, i); }
    void insert(int i = 1) {
        checkstack();
        lua_insert(L, i);
    }
    void replace(int i = 1) { lua_replace(L, i); }
    void newtable() {
        checkstack();
        lua_newtable(L);
    }
    void* newuser(size_t s) {
        checkstack();
        return lua_newuserdata(L, s);
    }
    void setfield(std::string_view field, int i = -2, bool raw = false) {
        i = getabsolute(i);
        push(field);
        insert(-2);
        settable(i, raw);
    }
    void setfield(int idx, int i = -2, bool raw = false) {
        i = getabsolute(i);
        push(lua_Number(idx));
        insert(-2);
        settable(i, raw);
    }
    void settable(int tableIdx = -3, bool raw = false);
    void getfield(std::string_view field, int i = -1, bool raw = false) {
        i = getabsolute(i);
        push(field);
        gettable(i, raw);
    }
    void getfield(int idx, int i = -1, bool raw = false) {
        i = getabsolute(i);
        push(lua_Number(idx));
        gettable(i, raw);
    }
    void gettable(int tableIdx = -2, bool raw = false);
    void getfieldtable(std::string_view name, int tableIdx = -1, bool raw = false);
    void getfieldtable(int idx, int tableIdx = -1, bool raw = false);
    void rawseti(int idx, int tableIdx = -2) { lua_rawseti(L, tableIdx, idx); }
    void rawgeti(int idx, int tableIdx = -1) { lua_rawgeti(L, tableIdx, idx); }
    void setvar() { lua_settable(L, LUA_GLOBALSINDEX); }
    int gettop() { return lua_gettop(L); }
    void pushLuaContext(bool inTable = false);
    int error(std::string_view msg);
    template <typename... Args>
    int error(const char* format, Args&&... args) {
        return error(fmt::sprintf(format, std::forward<Args>(args)...));
    }

    int type(int i = -1) { return lua_type(L, i); }
    const char* typestring(int i = -1) { return lua_typename(L, lua_type(L, i)); }
    bool isnil(int i = -1) { return lua_isnil(L, i); }
    bool isboolean(int i = -1) { return lua_isboolean(L, i); }
    bool isnumber(int i = -1) { return lua_isnumber(L, i); }
    bool isstring(int i = -1) { return lua_isstring(L, i); }
    bool istable(int i = -1) { return lua_istable(L, i); }
    bool isfunction(int i = -1) { return lua_isfunction(L, i); }
    bool iscfunction(int i = -1) { return lua_iscfunction(L, i); }
    bool isuserdata(int i = -1) { return lua_isuserdata(L, i); }
    bool islightuserdata(int i = -1) { return lua_islightuserdata(L, i); }
    bool isproto(int i = -1) { return lua_type(L, i) == 9; }
    bool iscdata(int i = -1) { return lua_type(L, i) == 10; }
    bool isobject(int i = -1);
    size_t length(int i = -1) { return lua_objlen(L, i); }

    int upvalue(int i) { return lua_upvalueindex(i); }

    bool toboolean(int i = -1) { return lua_toboolean(L, i); }
    lua_Number tonumber(int i = -1) { return lua_tonumber(L, i); }
    lua_Number checknumber(int i = -1) { return luaL_checknumber(L, i); }
    std::string tostring(int i = -1);
    lua_CFunction tocfunction(int i = -1) { return lua_tocfunction(L, i); }
    template <typename T = void>
    T* touserdata(int i = -1) {
        return reinterpret_cast<T*>(lua_touserdata(L, i));
    }
    template <typename T = void>
    T* topointer(int i = -1) {
        return reinterpret_cast<T*>(const_cast<void*>(lua_topointer(L, i)));
    }

    void concat(int n = 2) { lua_concat(L, n); }

    void displayStack(bool error = false);

    json toJson(int t = -1);
    void fromJson(const json&, int t = -1);

    std::string escapeString(std::string_view);
    void load(std::string_view code, const char* name, bool docall = true);
    int yield(int nresults = 0) { return lua_yield(L, nresults); }
    bool yielded() { return lua_status(L) == LUA_YIELD; }
    int getmetatable(int i = -1) {
        checkstack();
        return lua_getmetatable(L, i);
    }
    int setmetatable(int i = -2) { return lua_setmetatable(L, i); }
    int sethook(lua_Hook func, int mask, int count) { return lua_sethook(L, func, mask, count); }

    lua_State* getState() { return L; }

    void getfenv(int index = -1) { lua_getfenv(L, index); }
    int setfenv(int index = -2) { return lua_setfenv(L, index); }

    bool newmetatable(const char* name) { return luaL_newmetatable(L, name) != 0; }

    std::optional<lua_Debug> getinfo(const char* what, int level = 1) {
        lua_Debug ar;
        int r = lua_getstack(L, level, &ar);
        if (!r) return std::nullopt;
        r = lua_getinfo(L, what, &ar);
        if (!r) return std::nullopt;
        return ar;
    }

    void interrupt();

  private:
    lua_State* L;
};

}  // namespace PCSX
