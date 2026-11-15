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

TestCuePregap = {}

-- One bin, 50 sectors. Every audio sector is filled with its own index in the bin, so a read says
-- which bin sector it came from. Track 3 has a real pregap (INDEX 00, 5 sectors in the bin), tracks 4
-- and 5 have a PREGAP directive (5 sectors each, not in the bin).
local cue = [[
FILE "%s" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    INDEX 01 00:00:10
  TRACK 03 AUDIO
    INDEX 00 00:00:20
    INDEX 01 00:00:25
  TRACK 04 AUDIO
    PREGAP 00:00:05
    INDEX 01 00:00:35
  TRACK 05 AUDIO
    PREGAP 00:00:05
    INDEX 01 00:00:45
]]

local function marker(n) return string.rep(string.char(n, 0xa5, 0x5a, 0xc3), 588) end

-- Disc sectors, counted from track 1's INDEX 01, mapped to the bin sector they hold. The virtual
-- pregaps (35-39 and 50-54) hold no bin sector. Track 3's INDEX 00 (20-24) is in the bin but is
-- read as track 2, which plays silence past its own length, so it isn't checked here.
local layout = {}
for lba = 10, 19 do layout[lba] = lba end
for lba = 25, 34 do layout[lba] = lba end
for lba = 40, 49 do layout[lba] = lba - 5 end
for lba = 55, 59 do layout[lba] = lba - 10 end

function TestCuePregap:setUp()
    self.base = os.tmpname()
    local bin = io.open(self.base .. '.bin', 'wb')
    for n = 0, 49 do bin:write(n < 10 and string.rep('\0', 2352) or marker(n)) end
    bin:close()
    local f = io.open(self.base .. '.cue', 'wb')
    f:write(string.format(cue, self.base:match('[^/]*$') .. '.bin'))
    f:close()
end

function TestCuePregap:tearDown()
    os.remove(self.base .. '.bin')
    os.remove(self.base .. '.cue')
    os.remove(self.base)
end

local function readSector(iso, lba)
    -- Audio sectors are read by their absolute MSF position, which is 150 past the LBA.
    local file = iso:open(lba + 150, 2352, 'RAW')
    local data = tostring(file:read(2352))
    file:close()
    return data
end

function TestCuePregap:test_trackLayout()
    local iso = PCSX.openIso(self.base .. '.cue')
    lu.assertFalse(iso:failed())
    lu.assertEquals(iso:getTN(), 5)
    for lba = 10, 64 do
        if lba >= 20 and lba < 25 then goto continue end
        local data = readSector(iso, lba)
        local expected = layout[lba]
        if expected then
            lu.assertTrue(data == marker(expected), 'disc sector ' .. lba .. ' should hold bin sector ' .. expected)
        else
            -- A virtual pregap, or past the end of the bin: nothing from the bin may be read here.
            local at = data:find(string.char(0xa5, 0x5a, 0xc3), 1, true)
            lu.assertNil(at, at and ('disc sector ' .. lba .. ' read bin sector ' .. data:byte(at - 1)))
        end
        ::continue::
    end
end
