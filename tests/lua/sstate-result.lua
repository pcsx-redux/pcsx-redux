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

TestSaveStateResult = {}

function TestSaveStateResult:test_good_state_loads()
    lu.assertTrue(PCSX.loadSaveState(PCSX.createSaveState()))
end

function TestSaveStateResult:test_junk_is_refused()
    local junk = Support.File.buffer()
    junk:write(string.rep('\x5a', 600))
    lu.assertFalse(PCSX.loadSaveState(junk))
end

function TestSaveStateResult:test_good_state_loads_from_a_file()
    local file = Support.File.buffer()
    file:writeMoveSlice(PCSX.createSaveState())
    lu.assertTrue(PCSX.loadSaveState(file))
end

-- From a breakpoint the load is queued for the main loop, after the state is parsed.
function TestSaveStateResult:test_result_from_inside_execute()
    if PCSX.settings.emulator.Dynarec then lu.skip('breakpoints need the interpreter') end
    local ffi = require 'ffi'
    local PARK = 0x80100000
    local p = ffi.cast('uint32_t*', PCSX.getMemPtr() + (PARK - 0x80000000))
    p[0] = 0x08040000 -- j PARK
    p[1] = 0x00000000
    PCSX.invalidateCache()
    local state = PCSX.createSaveState()
    local junk = Support.File.buffer()
    junk:write(string.rep('\x5a', 600))
    local savedDebug = PCSX.settings.emulator.Debug.Debug
    PCSX.settings.emulator.Debug.Debug = true
    local co = coroutine.running()
    local good, bad
    local bp = PCSX.addBreakpoint(PARK, 'Exec', 4, 'sstate-result park', function()
        if good ~= nil then return false end
        good = PCSX.loadSaveState(state)
        bad = PCSX.loadSaveState(junk)
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        local ticks = 0
        timer:start(0, 1, function()
            ticks = ticks + 1
            if ticks < 2 then return end
            timer:stop()
            timer:close()
            coroutine.resume(co)
        end)
        return false
    end)
    PCSX.getRegisters().pc = PARK
    PCSX.resumeEmulator()
    coroutine.yield()
    bp:remove()
    PCSX.settings.emulator.Debug.Debug = savedDebug
    lu.assertTrue(good)
    lu.assertFalse(bad)
end
