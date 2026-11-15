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

-- Has to match the -gdb-port the runner passes.
local PORT = 3464

TestGdbServer = {}

local function checksum(payload)
    local sum = 0
    for i = 1, #payload do sum = sum + payload:byte(i) end
    return string.format('%02x', bit.band(sum, 0xff))
end

-- One client socket per test: raw RSP, acks left on. The stub echoes a '+' for every '+' it
-- receives, so any run of them can precede a reply.
local client
local buffer = ''
local waiter

-- A handle still open at quit keeps the loop from closing, which leaks it.
local function disconnect()
    if not client then return end
    local co = coroutine.running()
    client:read_stop()
    client:close(function() coroutine.resume(co) end)
    coroutine.yield()
    client = nil
    buffer = ''
end

function TestGdbServer:tearDown() disconnect() end

local function onData(err, data)
    if err or not data then
        if waiter then
            local co = waiter
            waiter = nil
            coroutine.resume(co)
        end
        return
    end
    buffer = buffer .. data
    if waiter then
        local co = waiter
        waiter = nil
        coroutine.resume(co)
    end
end

local function connect()
    if client then return end
    local co = coroutine.running()
    client = luv.new_tcp()
    -- An error raised inside a luv callback exits the process, so the result is checked once
    -- back in the test.
    client:connect('127.0.0.1', PORT, function(err)
        if not err then client:read_start(onData) end
        coroutine.resume(co, err)
    end)
    lu.assertNil(coroutine.yield())
end

-- Sends one packet and returns the payload of the reply.
local function request(payload)
    connect()
    client:write('$' .. payload .. '#' .. checksum(payload))
    while true do
        local reply = buffer:match('^%+*%$([^#]*)#%x%x')
        if reply then
            buffer = buffer:gsub('^%+*%$[^#]*#%x%x', '', 1)
            client:write('+')
            return reply
        end
        local co = coroutine.running()
        local timer = luv.new_timer()
        local timedOut = false
        timer:start(5000, 0, function()
            timedOut = true
            waiter = nil
            coroutine.resume(co)
        end)
        waiter = co
        coroutine.yield()
        timer:stop()
        timer:close()
        lu.assertFalse(timedOut, 'no reply to ' .. payload)
    end
end

local function le32(value)
    local s = ''
    for i = 0, 3 do s = s .. string.format('%02x', bit.band(bit.rshift(value, i * 8), 0xff)) end
    return s
end

-- gdb sends the register number in hex without padding: an inferior call writes a0 as P4=...
function TestGdbServer:test_single_digit_register_numbers()
    local regs = PCSX.getRegisters()
    lu.assertEquals(request('P4=' .. le32(0x12345678)), 'OK')
    lu.assertEquals(regs.GPR.n.a0, 0x12345678)
    lu.assertEquals(request('p4'), '78563412')
    lu.assertEquals(request('P1d=' .. le32(0x801ffe00)), 'OK')
    lu.assertEquals(regs.GPR.n.sp, 0x801ffe00)
    lu.assertEquals(request('p1d'), '00FE1F80')
end

-- A bad register packet is an error reply, and the connection stays usable after it.
function TestGdbServer:test_malformed_register_packet_keeps_connection()
    lu.assertEquals(request('P4=1234'), 'E00')
    lu.assertEquals(request('P48=' .. le32(0)), 'E00')
    lu.assertEquals(request('pzz'), 'E00')
    lu.assertEquals(request('P5=' .. le32(0xcafe)), 'OK')
    lu.assertEquals(PCSX.getRegisters().GPR.n.a1, 0xcafe)
end

-- With multiprocess negotiated, gdb detaches with D;<pid>.
function TestGdbServer:test_multiprocess_detach()
    lu.assertEquals(request('D;1'), 'OK')
    disconnect()
    lu.assertEquals(request('p4'):len(), 8)
end
