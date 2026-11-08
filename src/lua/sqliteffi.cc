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

#include "lua/sqliteffi.h"

#include "lua/luawrapper.h"
#include "sqlite/sqlite3.h"

namespace {

// The few calls that need a C-side constant: SQLITE_TRANSIENT is a function
// pointer cast from -1, and a zero-length blob has to go through zeroblob or
// sqlite binds NULL instead. Everything else is called straight from Lua.
int sqliteOpen(const char* path, int readOnly, sqlite3** db) {
    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    return sqlite3_open_v2(path, db, flags | SQLITE_OPEN_URI, nullptr);
}

int sqliteBindText(sqlite3_stmt* stmt, int index, const char* text, int size) {
    return sqlite3_bind_text(stmt, index, text, size, SQLITE_TRANSIENT);
}

int sqliteBindBlob(sqlite3_stmt* stmt, int index, const void* data, int size) {
    if (size == 0) return sqlite3_bind_zeroblob(stmt, index, 0);
    return sqlite3_bind_blob(stmt, index, data, size, SQLITE_TRANSIENT);
}

int sqliteExec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

template <typename T, size_t S>
void registerSymbol(PCSX::Lua L, const char (&name)[S], const T ptr) {
    L.push<S>(name);
    L.push((void*)ptr);
    L.settable();
}

#define REGISTER(L, s) registerSymbol(L, #s, s)

void registerAllSymbols(PCSX::Lua L) {
    L.getfieldtable("_CLIBS", LUA_REGISTRYINDEX);
    L.push("SQLITE");
    L.newtable();
    REGISTER(L, sqliteOpen);
    REGISTER(L, sqliteBindText);
    REGISTER(L, sqliteBindBlob);
    REGISTER(L, sqliteExec);
    REGISTER(L, sqlite3_libversion);
    REGISTER(L, sqlite3_close_v2);
    REGISTER(L, sqlite3_errmsg);
    REGISTER(L, sqlite3_errstr);
    REGISTER(L, sqlite3_extended_errcode);
    REGISTER(L, sqlite3_busy_timeout);
    REGISTER(L, sqlite3_get_autocommit);
    REGISTER(L, sqlite3_changes);
    REGISTER(L, sqlite3_last_insert_rowid);
    REGISTER(L, sqlite3_prepare_v2);
    REGISTER(L, sqlite3_step);
    REGISTER(L, sqlite3_reset);
    REGISTER(L, sqlite3_clear_bindings);
    REGISTER(L, sqlite3_finalize);
    REGISTER(L, sqlite3_bind_parameter_count);
    REGISTER(L, sqlite3_bind_parameter_index);
    REGISTER(L, sqlite3_bind_null);
    REGISTER(L, sqlite3_bind_int64);
    REGISTER(L, sqlite3_bind_double);
    REGISTER(L, sqlite3_column_count);
    REGISTER(L, sqlite3_column_name);
    REGISTER(L, sqlite3_column_type);
    REGISTER(L, sqlite3_column_int64);
    REGISTER(L, sqlite3_column_double);
    REGISTER(L, sqlite3_column_text);
    REGISTER(L, sqlite3_column_blob);
    REGISTER(L, sqlite3_column_bytes);
    L.settable();
    L.pop();
}

}  // namespace

void PCSX::LuaFFI::open_sqlite(Lua L) {
    static int lualoader = 1;
    static const char* sqliteFFI = (
#include "lua/sqliteffi.lua"
    );
    registerAllSymbols(L);
    L.load(sqliteFFI, "src:lua/sqliteffi.lua");
}
