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
local lu = require 'luaunit'

TestSPULogger = {}

local function contents(path)
    local f = io.open(path, 'rb')
    local data = f:read('*a')
    f:close()
    return data
end

-- A capture that cannot export must leave an existing file alone.
function TestSPULogger:test_failed_export_keeps_the_file()
    local path = os.tmpname()
    local f = io.open(path, 'wb')
    f:write('keep me')
    f:close()
    PCSX.SPU.Logger.clear()
    local ok, err = PCSX.SPU.Logger.exportSPD(path)
    lu.assertFalse(ok)
    lu.assertStrContains(err, 'no register writes')
    lu.assertEquals(contents(path), 'keep me')
    os.remove(path)
end
