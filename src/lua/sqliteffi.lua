-- lualoader, R"EOF(--
--   Copyright (C) 2026 PCSX-Redux authors
--
--   This program is free software; you can redistribute it and/or modify
--   it under the terms of the GNU General Public License as published by
--   the Free Software Foundation; either version 2 of the License, or
--   (at your option) any later version.
--
--   This program is distributed in the hope that it will be useful,
--   but WITHOUT ANY WARRANTY; without even the implied warranty of
--   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--   GNU General Public License for more details.
--
--   You should have received a copy of the GNU General Public License
--   along with this program; if not, write to the
--   Free Software Foundation, Inc.,
--   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

ffi.cdef [[
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

int sqliteOpen(const char* path, int readOnly, sqlite3** db);
int sqliteBindText(sqlite3_stmt* stmt, int index, const char* text, int size);
int sqliteBindBlob(sqlite3_stmt* stmt, int index, const void* data, int size);
int sqliteExec(sqlite3* db, const char* sql);

const char* sqlite3_libversion(void);
int sqlite3_close_v2(sqlite3* db);
const char* sqlite3_errmsg(sqlite3* db);
const char* sqlite3_errstr(int code);
int sqlite3_extended_errcode(sqlite3* db);
int sqlite3_busy_timeout(sqlite3* db, int ms);
int sqlite3_get_autocommit(sqlite3* db);
int sqlite3_changes(sqlite3* db);
int64_t sqlite3_last_insert_rowid(sqlite3* db);
int sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** stmt, const char** tail);
int sqlite3_step(sqlite3_stmt* stmt);
int sqlite3_reset(sqlite3_stmt* stmt);
int sqlite3_clear_bindings(sqlite3_stmt* stmt);
int sqlite3_finalize(sqlite3_stmt* stmt);
int sqlite3_bind_parameter_count(sqlite3_stmt* stmt);
int sqlite3_bind_parameter_index(sqlite3_stmt* stmt, const char* name);
int sqlite3_bind_null(sqlite3_stmt* stmt, int index);
int sqlite3_bind_int64(sqlite3_stmt* stmt, int index, int64_t value);
int sqlite3_bind_double(sqlite3_stmt* stmt, int index, double value);
int sqlite3_column_count(sqlite3_stmt* stmt);
const char* sqlite3_column_name(sqlite3_stmt* stmt, int index);
int sqlite3_column_type(sqlite3_stmt* stmt, int index);
int64_t sqlite3_column_int64(sqlite3_stmt* stmt, int index);
double sqlite3_column_double(sqlite3_stmt* stmt, int index);
const unsigned char* sqlite3_column_text(sqlite3_stmt* stmt, int index);
const void* sqlite3_column_blob(sqlite3_stmt* stmt, int index);
int sqlite3_column_bytes(sqlite3_stmt* stmt, int index);
]]

local C = ffi.load 'SQLITE'

local SQLITE_OK = 0
local SQLITE_ROW = 100
local SQLITE_DONE = 101
local typeNames = { 'integer', 'float', 'text', 'blob', 'null' }

-- Integers come back as int64_t cdata. Hand back a plain number whenever it is
-- exact, so ordinary rowids and counters compare and index like numbers.
local maxExact = 2 ^ 53
local function fromInt64(v)
    if v >= -maxExact and v <= maxExact then return tonumber(v) end
    return v
end

local function fail(db, level)
    error(string.format('sqlite: %s (%d)', ffi.string(C.sqlite3_errmsg(db)), C.sqlite3_extended_errcode(db)), level + 1)
end

-- A statement whose database was closed is unusable: close_v2 keeps the
-- connection alive only so the statement can still be finalized.
local function checkOpen(db, level)
    if db._db == nil then error('SQLite database used after close', level + 1) end
    -- SQLite can end a transaction on its own (INSERT OR ROLLBACK, some I/O
    -- errors). Writing on as if it were still open would autocommit work the
    -- surrounding transaction() means to roll back.
    if db._txDepth > 0 and C.sqlite3_get_autocommit(db._db) ~= 0 then
        error('SQLite transaction was rolled back by SQLite; it cannot be used until transaction() unwinds', level + 1)
    end
end

local blobMeta = { __index = { _type = 'SQLiteBlob' } }
local function blob(data)
    if type(data) ~= 'string' then error('SQLite.blob: needs a string', 2) end
    return setmetatable({ data = data }, blobMeta)
end

local stmtMethods = {}
local stmtMeta = { __index = stmtMethods }
local dbMethods = {}
local dbMeta = { __index = dbMethods }

local function stmtHandle(stmt)
    local h = stmt._stmt
    if h == nil then error('SQLite statement used after finalize', 3) end
    if stmt._owner._db == nil then error('SQLite statement used after its database was closed', 3) end
    return h
end

local function dbHandle(db)
    checkOpen(db, 3)
    return db._db
end

local function checkColumn(h, i)
    if type(i) ~= 'number' or i % 1 ~= 0 or i < 1 or i > C.sqlite3_column_count(h) then
        error('SQLite column index out of range: ' .. tostring(i), 3)
    end
    return i - 1
end

local function bindOne(stmt, h, index, value)
    if type(index) == 'string' then
        local named = C.sqlite3_bind_parameter_index(h, index)
        if named == 0 then error('SQLite bind: no parameter named ' .. index, 3) end
        index = named
    elseif type(index) ~= 'number' or index % 1 ~= 0 then
        error('SQLite bind: parameter index must be an integer or a name', 3)
    elseif index < 1 or index > C.sqlite3_bind_parameter_count(h) then
        error('SQLite bind: parameter index out of range: ' .. tostring(index), 3)
    end
    local t = type(value)
    local rc
    if value == nil then
        rc = C.sqlite3_bind_null(h, index)
    elseif t == 'boolean' then
        rc = C.sqlite3_bind_int64(h, index, value and 1 or 0)
    elseif t == 'number' then
        if value % 1 == 0 and value >= -2 ^ 63 and value < 2 ^ 63 then
            rc = C.sqlite3_bind_int64(h, index, value)
        else
            rc = C.sqlite3_bind_double(h, index, value)
        end
    elseif t == 'string' then
        rc = C.sqliteBindText(h, index, value, #value)
    elseif t == 'table' and value._type == 'SQLiteBlob' then
        rc = C.sqliteBindBlob(h, index, value.data, #value.data)
    elseif t == 'cdata' and ffi.istype('int64_t', value) then
        rc = C.sqlite3_bind_int64(h, index, value)
    else
        error('SQLite bind: cannot bind a value of type ' .. t, 3)
    end
    if rc ~= SQLITE_OK then fail(stmt._owner._db, 3) end
end

function stmtMethods:bind(index, value)
    bindOne(self, stmtHandle(self), index, value)
    return self
end

function stmtMethods:bindAll(...)
    local h = stmtHandle(self)
    local n = select('#', ...)
    if n ~= C.sqlite3_bind_parameter_count(h) then
        error(string.format('SQLite bindAll: %d values for %d parameters', n, C.sqlite3_bind_parameter_count(h)), 2)
    end
    for i = 1, n do bindOne(self, h, i, (select(i, ...))) end
    return self
end

function stmtMethods:step()
    local h = stmtHandle(self)
    checkOpen(self._owner, 2)
    local rc = C.sqlite3_step(h)
    if rc == SQLITE_ROW then return true end
    if rc == SQLITE_DONE then return false end
    C.sqlite3_reset(h)
    fail(self._owner._db, 2)
end

function stmtMethods:reset()
    C.sqlite3_reset(stmtHandle(self))
    return self
end

function stmtMethods:clearBindings()
    C.sqlite3_clear_bindings(stmtHandle(self))
    return self
end

function stmtMethods:columnCount() return C.sqlite3_column_count(stmtHandle(self)) end

function stmtMethods:columnName(i)
    local h = stmtHandle(self)
    return ffi.string(C.sqlite3_column_name(h, checkColumn(h, i)))
end

function stmtMethods:columnType(i)
    local h = stmtHandle(self)
    return typeNames[C.sqlite3_column_type(h, checkColumn(h, i))]
end

local function column(h, i)
    local t = C.sqlite3_column_type(h, i)
    if t == 1 then return fromInt64(C.sqlite3_column_int64(h, i)) end
    if t == 2 then return C.sqlite3_column_double(h, i) end
    if t == 3 then return ffi.string(C.sqlite3_column_text(h, i), C.sqlite3_column_bytes(h, i)) end
    if t == 4 then
        local p = C.sqlite3_column_blob(h, i)
        local n = C.sqlite3_column_bytes(h, i)
        if n == 0 then return '' end
        return ffi.string(p, n)
    end
    return nil
end

function stmtMethods:column(i)
    local h = stmtHandle(self)
    return column(h, checkColumn(h, i))
end

function stmtMethods:row()
    local h = stmtHandle(self)
    local r = {}
    for i = 0, C.sqlite3_column_count(h) - 1 do r[ffi.string(C.sqlite3_column_name(h, i))] = column(h, i) end
    return r
end

function stmtMethods:values()
    local h = stmtHandle(self)
    local r = {}
    for i = 0, C.sqlite3_column_count(h) - 1 do r[i + 1] = column(h, i) end
    return r
end

function stmtMethods:finalize()
    local h = self._stmt
    -- No owner check here: finalizing is the one thing a statement may still
    -- do after its database was closed, and it is what frees the connection.
    if h == nil then return end
    self._stmt = nil
    ffi.gc(h, nil)
    C.sqlite3_finalize(h)
end

function dbMethods:prepare(sql)
    local h = dbHandle(self)
    local out = ffi.new('sqlite3_stmt*[1]')
    if C.sqlite3_prepare_v2(h, sql, #sql, out, nil) ~= SQLITE_OK then fail(h, 2) end
    if out[0] == nil then error('SQLite prepare: no statement in the SQL text', 2) end
    local stmt = ffi.gc(out[0], function(p) C.sqlite3_finalize(p) end)
    return setmetatable({ _type = 'SQLiteStatement', _stmt = stmt, _owner = self }, stmtMeta)
end

function dbMethods:exec(sql)
    local h = dbHandle(self)
    if C.sqliteExec(h, sql) ~= SQLITE_OK then fail(h, 2) end
end

function dbMethods:run(sql, ...)
    local stmt = self:prepare(sql)
    local ok, err = pcall(function(...)
        stmt:bindAll(...)
        while stmt:step() do end
    end, ...)
    stmt:finalize()
    if not ok then error(err, 2) end
    return C.sqlite3_changes(self._db)
end

-- A loop that leaves early keeps its statement, and on a file its read lock,
-- until the garbage collector finalizes it. Use first() for a single row, or
-- prepare() and finalize() for explicit control.
function dbMethods:rows(sql, ...)
    local stmt = self:prepare(sql)
    stmt:bindAll(...)
    return function()
        if stmt._stmt == nil then return nil end
        local ok, more = pcall(stmt.step, stmt)
        if not ok then
            stmt:finalize()
            error(more, 2)
        end
        if more then return stmt:row() end
        stmt:finalize()
    end
end

function dbMethods:first(sql, ...)
    local stmt = self:prepare(sql)
    local ok, row = pcall(function(...)
        stmt:bindAll(...)
        if stmt:step() then return stmt:row() end
    end, ...)
    stmt:finalize()
    if not ok then error(row, 2) end
    return row
end

function dbMethods:changes() return C.sqlite3_changes(dbHandle(self)) end

function dbMethods:lastInsertRowId() return fromInt64(C.sqlite3_last_insert_rowid(dbHandle(self))) end

function dbMethods:busyTimeout(ms)
    C.sqlite3_busy_timeout(dbHandle(self), ms)
    return self
end

-- Runs fn inside a transaction: committed when fn returns, rolled back when it
-- raises, and the error is raised again. Nests through savepoints.
local savepointSerial = 0
local unpackResults = table.unpack or unpack
local function pack(...) return { n = select('#', ...), ... } end
function dbMethods:transaction(fn, ...)
    local h = dbHandle(self)
    local begin, commit, rollback
    if C.sqlite3_get_autocommit(h) ~= 0 then
        begin, commit, rollback = 'BEGIN', 'COMMIT', 'ROLLBACK'
    else
        savepointSerial = savepointSerial + 1
        local name = 'pcsx_sp_' .. savepointSerial
        begin = 'SAVEPOINT ' .. name
        commit = 'RELEASE ' .. name
        rollback = 'ROLLBACK TO ' .. name .. '; RELEASE ' .. name
    end
    self:exec(begin)
    self._txDepth = self._txDepth + 1
    local results = pack(pcall(fn, ...))
    self._txDepth = self._txDepth - 1
    if self._db == nil then
        -- fn closed the database, which already rolled back anything open.
        if results[1] then error('SQLite transaction: the database was closed inside it', 2) end
        error(results[2], 0)
    end
    -- SQLite ended the transaction itself; there is nothing left to commit or roll back.
    local lost = C.sqlite3_get_autocommit(h) ~= 0
    if results[1] and lost then
        error('SQLite transaction was rolled back by SQLite before it could commit', 2)
    end
    if results[1] then
        if C.sqliteExec(h, commit) == SQLITE_OK then return unpackResults(results, 2, results.n) end
        -- A failed COMMIT (a deferred constraint, say) leaves the transaction open.
        local err = string.format('sqlite: %s (%d)', ffi.string(C.sqlite3_errmsg(h)), C.sqlite3_extended_errcode(h))
        C.sqliteExec(h, rollback)
        error(err, 2)
    end
    if not lost then C.sqliteExec(h, rollback) end
    error(results[2], 0)
end

function dbMethods:close()
    local h = self._db
    if h == nil then return end
    self._db = nil
    ffi.gc(h, nil)
    C.sqlite3_close_v2(h)
end

local function open(path, options)
    path = path or ':memory:'
    if type(path) ~= 'string' then error('SQLite.open: path must be a string', 2) end
    local readOnly = type(options) == 'table' and options.readOnly and 1 or 0
    local out = ffi.new('sqlite3*[1]')
    local rc = C.sqliteOpen(path, readOnly, out)
    if rc ~= SQLITE_OK then
        local msg = out[0] ~= nil and ffi.string(C.sqlite3_errmsg(out[0])) or ffi.string(C.sqlite3_errstr(rc))
        if out[0] ~= nil then C.sqlite3_close_v2(out[0]) end
        error(string.format('sqlite: %s (%d)', msg, rc), 2)
    end
    local db = ffi.gc(out[0], function(p) C.sqlite3_close_v2(p) end)
    return setmetatable({ _type = 'SQLiteDatabase', _db = db, _txDepth = 0 }, dbMeta)
end

PCSX.SQLite = {
    open = open,
    blob = blob,
    version = function() return ffi.string(C.sqlite3_libversion()) end,
}

-- )EOF"
