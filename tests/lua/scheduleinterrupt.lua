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

TestScheduleInterrupt = {}

function TestScheduleInterrupt:testFiresAfterCycles()
    local co = coroutine.running()
    local delay = 100000
    local r = {}
    local listener
    listener = PCSX.Events.createEventListener('CPU::Interrupt', function()
        r.at = tonumber(PCSX.getCPUCycles())
        listener:remove()
        PCSX.pauseEmulator()
        -- The first event loop tick after the pause still runs on the emulation stack.
        local timer, ticks = luv.new_timer(), 0
        timer:start(0, 1, function()
            ticks = ticks + 1
            if ticks < 2 then return end
            timer:stop()
            timer:close()
            coroutine.resume(co)
        end)
    end)
    local start = tonumber(PCSX.getCPUCycles())
    PCSX.scheduleInterrupt(delay)
    PCSX.resumeEmulator()
    coroutine.yield()

    lu.assertNotNil(r.at)
    lu.assertTrue(r.at - start >= delay, 'fired early: ' .. (r.at - start))
    lu.assertTrue(r.at - start < delay + 1000, 'fired late: ' .. (r.at - start))
end

function TestScheduleInterrupt:testRejectsBadCycleCounts()
    lu.assertErrorMsgContains('numeric cycle count', PCSX.scheduleInterrupt, 'soon')
    lu.assertErrorMsgContains('unsigned 32-bit integer', PCSX.scheduleInterrupt, -1)
    lu.assertErrorMsgContains('unsigned 32-bit integer', PCSX.scheduleInterrupt, 1.5)
    lu.assertErrorMsgContains('unsigned 32-bit integer', PCSX.scheduleInterrupt, 0x100000000)
end
