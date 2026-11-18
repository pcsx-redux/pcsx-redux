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

TestMds = {}

local content = string.rep('0123456789ABCDEF', 256)

local function u16(v) return string.char(v % 256, math.floor(v / 256) % 256) end
local function u32(v) return u16(v % 65536) .. u16(math.floor(v / 65536)) end

local function pad(s, n) return s .. string.rep('\0', n - #s) end

-- Build a one-track ISO, then store it as an .mdf with 96 bytes of
-- interleaved subchannel after each 2352-byte frame, plus a minimal .mds.
local function writeImage(base)
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    local root = builder:createRoot(1)
    local data = Support.File.buffer()
    data:write(content)
    data:rSeek(0)
    builder:createFile(root, 'TEST.DAT', data)
    builder:close()

    local size = out:size()
    lu.assertEquals(size % 2352, 0)
    local sectors = size / 2352
    out:rSeek(0)
    local mdf = io.open(base .. '.mdf', 'wb')
    for _ = 1, sectors do mdf:write(tostring(out:read(2352)), string.rep('\0', 96)) end
    mdf:close()

    -- Header: signature, session block at 0x58, one track block at 0x70,
    -- its extra block (pregap, length) at 0xc0.
    local mds = pad('MEDIA DESCRIPTOR', 0x50) .. u32(0x58) .. u32(0)
    mds = mds .. pad(string.rep('\0', 14) .. u16(1) .. u32(0) .. u32(0x70), 0x18)
    local track =
        string.char(0xec, 0x08, 0x14, 0x00, 0x01) .. string.rep('\0', 4) .. string.char(0, 2, 0) .. u32(0xc0) ..
            u16(2448)
    mds = mds .. pad(pad(track, 0x28) .. u32(0) .. u32(0), 0x50)
    mds = mds .. u32(0) .. u32(sectors)
    local f = io.open(base .. '.mds', 'wb')
    f:write(mds)
    f:close()
end

local function checkRead(path)
    local iso = PCSX.openIso(path)
    lu.assertFalse(iso:failed())
    local file = iso:createReader():open('TEST.DAT;1')
    lu.assertFalse(file:failed())
    lu.assertEquals(file:size(), #content)
    lu.assertEquals(tostring(file:read(#content)), content)
end

function TestMds:setUp()
    self.base = os.tmpname()
    writeImage(self.base)
end

function TestMds:tearDown()
    os.remove(self.base .. '.mdf')
    os.remove(self.base .. '.mds')
    os.remove(self.base)
end

function TestMds:test_openMdf() checkRead(self.base .. '.mdf') end

function TestMds:test_openMds() checkRead(self.base .. '.mds') end
