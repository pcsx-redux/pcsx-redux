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


local lu = require 'luaunit'

TestSQLite = {}

function TestSQLite:test_open_in_memory()
    local db = PCSX.SQLite.open(':memory:')
    db:exec 'CREATE TABLE t (k INTEGER PRIMARY KEY, v TEXT); INSERT INTO t (v) VALUES (\'one\'), (\'two\')'
    local seen = {}
    for row in db:rows 'SELECT k, v FROM t ORDER BY k' do seen[#seen + 1] = row.k .. '=' .. row.v end
    lu.assertEquals(seen, { '1=one', '2=two' })
    lu.assertStrContains(PCSX.SQLite.version(), '3.')
    db:close()
end

function TestSQLite:test_open_on_disk()
    local path = os.tmpname()
    os.remove(path)
    local db = PCSX.SQLite.open(path)
    db:exec 'CREATE TABLE t (v TEXT)'
    lu.assertEquals(db:run('INSERT INTO t VALUES (?)', 'persisted'), 1)
    db:close()

    local reopened = PCSX.SQLite.open(path, { readOnly = true })
    local stmt = reopened:prepare 'SELECT v FROM t'
    lu.assertTrue(stmt:step())
    lu.assertEquals(stmt:column(1), 'persisted')
    lu.assertFalse(stmt:step())
    stmt:finalize()
    lu.assertErrorMsgContains('readonly', function() reopened:exec 'INSERT INTO t VALUES (\'nope\')' end)
    reopened:close()
    os.remove(path)
end

function TestSQLite:test_every_column_type_roundtrips()
    local db = PCSX.SQLite.open()
    db:exec 'CREATE TABLE t (i, f, s, b, n)'
    -- Every byte value, so a blob that went through a C string would come back short.
    local bytes = {}
    for i = 0, 255 do bytes[#bytes + 1] = string.char(i) end
    local blob = table.concat(bytes)
    local big = 0x7fffffffffffffffLL
    local insert = db:prepare 'INSERT INTO t VALUES (?, ?, ?, ?, ?)'
    insert:bindAll(-42, 3.25, 'texte accentu\xc3\xa9', PCSX.SQLite.blob(blob), nil)
    lu.assertFalse(insert:step())
    insert:reset():clearBindings()
    insert:bindAll(big, 1e300, '', PCSX.SQLite.blob(''), nil)
    lu.assertFalse(insert:step())
    insert:finalize()

    local stmt = db:prepare 'SELECT i, f, s, b, n FROM t ORDER BY rowid'
    lu.assertTrue(stmt:step())
    lu.assertEquals({ stmt:columnType(1), stmt:columnType(2), stmt:columnType(3), stmt:columnType(4),
                      stmt:columnType(5) }, { 'integer', 'float', 'text', 'blob', 'null' })
    lu.assertEquals(stmt:column(1), -42)
    lu.assertEquals(stmt:column(2), 3.25)
    lu.assertEquals(stmt:column(3), 'texte accentu\xc3\xa9')
    lu.assertEquals(stmt:column(4), blob)
    lu.assertNil(stmt:column(5))
    lu.assertTrue(stmt:step())
    lu.assertEquals(stmt:column(1), big)
    lu.assertEquals(stmt:column(2), 1e300)
    lu.assertEquals(stmt:column(3), '')
    lu.assertEquals(stmt:columnType(4), 'blob')
    lu.assertEquals(stmt:column(4), '')
    lu.assertFalse(stmt:step())
    stmt:finalize()
    db:close()
end

function TestSQLite:test_named_parameters()
    local db = PCSX.SQLite.open()
    db:exec 'CREATE TABLE t (a, b)'
    local stmt = db:prepare 'INSERT INTO t VALUES (:a, :b)'
    stmt:bind(':b', 'second'):bind(':a', 'first')
    lu.assertFalse(stmt:step())
    stmt:finalize()
    local row = db:rows 'SELECT a, b FROM t' ()
    lu.assertEquals(row, { a = 'first', b = 'second' })
    lu.assertEquals(db:lastInsertRowId(), 1)
    db:close()
end

function TestSQLite:test_constraint_error_is_a_lua_error()
    local db = PCSX.SQLite.open()
    db:exec 'CREATE TABLE t (k TEXT UNIQUE NOT NULL)'
    db:run('INSERT INTO t VALUES (?)', 'a')
    lu.assertErrorMsgContains('UNIQUE constraint failed: t.k', function() db:run('INSERT INTO t VALUES (?)', 'a') end)
    lu.assertErrorMsgContains('NOT NULL constraint failed: t.k', function() db:run('INSERT INTO t VALUES (?)', nil) end)
    lu.assertErrorMsgContains('syntax error', function() db:prepare 'SELEKT 1' end)
    lu.assertErrorMsgContains('2 values for 1 parameters', function() db:run('INSERT INTO t VALUES (?)', 'b', 'c') end)
    -- The failed statements left the database usable.
    lu.assertEquals(db:run('INSERT INTO t VALUES (?)', 'b'), 1)
    db:close()
end

function TestSQLite:test_transaction_rollback()
    local db = PCSX.SQLite.open()
    db:exec 'CREATE TABLE t (k TEXT UNIQUE)'
    local function count() return db:rows 'SELECT count(*) AS c FROM t' ().c end

    lu.assertEquals(db:transaction(function(a) db:run('INSERT INTO t VALUES (?)', a) return 'done', 2 end, 'x'), 'done')
    lu.assertEquals(count(), 1)

    lu.assertErrorMsgContains('UNIQUE constraint failed', function()
        db:transaction(function()
            db:run('INSERT INTO t VALUES (?)', 'y')
            db:run('INSERT INTO t VALUES (?)', 'x')
        end)
    end)
    lu.assertEquals(count(), 1)

    lu.assertErrorMsgContains('inner failure', function()
        db:transaction(function()
            db:run('INSERT INTO t VALUES (?)', 'z')
            error 'inner failure'
        end)
    end)
    lu.assertEquals(count(), 1)

    -- A nested transaction rolls back to its savepoint and leaves the outer one alive.
    db:transaction(function()
        db:run('INSERT INTO t VALUES (?)', 'outer')
        pcall(db.transaction, db, function()
            db:run('INSERT INTO t VALUES (?)', 'inner')
            error 'abandon inner'
        end)
    end)
    local keys = {}
    for row in db:rows 'SELECT k FROM t ORDER BY k' do keys[#keys + 1] = row.k end
    lu.assertEquals(keys, { 'outer', 'x' })
    db:close()
end

function TestSQLite:test_use_after_close_is_an_error()
    local db = PCSX.SQLite.open()
    local stmt = db:prepare 'SELECT 1'
    stmt:finalize()
    lu.assertErrorMsgContains('after finalize', function() stmt:step() end)
    db:close()
    lu.assertErrorMsgContains('after close', function() db:exec 'SELECT 1' end)
end
