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
local ffi = require 'ffi'

TestBinary = {}

-- A minimal PS-EXE: 2048 byte header with the region marker at 0x71, then one 2048 byte block of text.
local function psexe(regionByte)
    local data = ffi.new('uint8_t[4096]')
    ffi.copy(data, 'PS-X EXE', 8)
    local header = ffi.cast('uint32_t*', data)
    header[4] = 0x80010000 -- pc
    -- The loader writes the text at this offset in the destination file, so keep it small.
    header[6] = 0 -- text address
    header[7] = 2048 -- text size
    if regionByte then data[0x71] = string.byte(regionByte) end
    local f = Support.File.buffer()
    f:write(ffi.cast('const char*', data), 4096)
    f:rSeek(0)
    return f
end

local function load(regionByte)
    local info = PCSX.Binary.load(psexe(regionByte), Support.File.buffer())
    lu.assertNotNil(info)
    lu.assertEquals(info.pc, 0x80010000)
    return info
end

function TestBinary:test_regionNTSC()
    lu.assertEquals(load('A').region, 'NTSC')
    lu.assertEquals(load('J').region, 'NTSC')
end

function TestBinary:test_regionPAL() lu.assertEquals(load('E').region, 'PAL') end

function TestBinary:test_regionUnknown() lu.assertNil(load(nil).region) end
