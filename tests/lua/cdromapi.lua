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

TestCDRomApi = {}

-- Runs the emulator for the given number of frames, then hands control back.
local function runFrames(count)
    local co = coroutine.running()
    local frames = 0
    local listener
    listener = PCSX.Events.createEventListener('GPU::Vsync', function()
        frames = frames + 1
        if frames < count then return end
        listener:remove()
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        timer:start(1, 0, function()
            timer:close()
            coroutine.resume(co)
        end)
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
end

local function buildIso()
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setSystemIdent('PLAYSTATION')
    builder:setVolumeIdent('LID_TEST')
    local root = builder:createRoot(1)
    local content = Support.File.buffer()
    content:write('lid test')
    content:rSeek(0)
    builder:createFile(root, 'LID.TXT', content)
    builder:close()
    out:rSeek(0)
    return out
end

function TestCDRomApi:tearDown() PCSX.CDRom.closeLid() end

function TestCDRomApi:test_openAndCloseLid()
    PCSX.CDRom.closeLid()
    lu.assertFalse(PCSX.CDRom.isLidOpen())
    PCSX.CDRom.openLid()
    lu.assertTrue(PCSX.CDRom.isLidOpen())
    PCSX.CDRom.closeLid()
    lu.assertFalse(PCSX.CDRom.isLidOpen())
end

function TestCDRomApi:test_scheduledCloseHappensLater()
    PCSX.CDRom.scheduleCloseLid()
    lu.assertTrue(PCSX.CDRom.isLidOpen())
    runFrames(10)
    lu.assertTrue(PCSX.CDRom.isLidOpen())
    runFrames(120)
    lu.assertFalse(PCSX.CDRom.isLidOpen())
end

function TestCDRomApi:test_setAndEjectIso()
    PCSX.CDRom.setIso(PCSX.openIso(buildIso()))
    local iso = PCSX.getCurrentIso()
    lu.assertFalse(iso:failed())
    local file = iso:createReader():open('LID.TXT;1')
    lu.assertEquals(file:size(), 8)
    lu.assertEquals(tostring(file:read(8)), 'lid test')

    PCSX.CDRom.ejectIso()
    lu.assertTrue(PCSX.getCurrentIso():failed())

    PCSX.CDRom.setIso(buildIso())
    lu.assertFalse(PCSX.getCurrentIso():failed())

    PCSX.CDRom.setIso('this file does not exist.cue')
    lu.assertTrue(PCSX.getCurrentIso():failed())
end
