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


-- Boots through the BIOS with the rewind ring capturing every frame, lets the shell be hijacked,
-- and reverse-continues (rewinds, on the dynarec) back onto the first instruction run after the hijack. A replay from a
-- snapshot taken before the hijack can't get there: the hijack only happens once per boot.
-- GDB_REVERSE_SHELL selects how the shell is hijacked: 'fastboot', 'handmade' (fastboot, with
-- automatic captures off and one snapshot taken by hand before the boot), 'exe' (side-loaded with
-- -loadexe, see tests/pcsxrunner/lua.cc), or 'shellhalt' (gdb writes the binary while paused at the shell).

local lu = require 'luaunit'

TestGdbReverseShell = {}

local PORT = 3463
local ENTRY = 0x80010000
-- li $t2, 0x1234 once, then addiu $t0, $t0, 1 in a loop. The side-loaded binary holds the same.
local CODE = { 0x240a1234, 0x25080001, 0x1000fffe, 0x00000000 }
local FRAMES_AFTER_SHELL = 3

local client, co, buffer, packets

local function checksum(s)
    local sum = 0
    for i = 1, #s do sum = sum + s:byte(i) end
    return string.format('%02x', sum % 256)
end

local function parse()
    while true do
        buffer = buffer:gsub('^%++', '')
        local data, sum, rest = buffer:match('^%$([^#]*)#(%x%x)(.*)$')
        if not data then return end
        buffer = rest
        client:write('+')
        packets[#packets + 1] = data
    end
end

local function recv()
    while #packets == 0 do coroutine.yield() end
    return table.remove(packets, 1)
end

local function send(cmd)
    client:write('$' .. cmd .. '#' .. checksum(cmd))
end

local function command(cmd)
    send(cmd)
    return recv()
end

-- Console output comes as O<hex> packets ahead of the reply.
local function monitor(cmd)
    local hex = cmd:gsub('.', function(c) return string.format('%02x', c:byte()) end)
    send('qRcmd,' .. hex)
    while true do
        local reply = recv()
        if not reply:match('^O%x+$') then return reply end
    end
end

local function le32(v)
    return string.format('%02x%02x%02x%02x', bit.band(v, 0xff), bit.band(bit.rshift(v, 8), 0xff),
                         bit.band(bit.rshift(v, 16), 0xff), bit.band(bit.rshift(v, 24), 0xff))
end

local function pc()
    return command('p25'):lower()
end

local function connect()
    co = coroutine.running()
    buffer, packets = '', {}
    client = luv.new_tcp()
    client:connect('127.0.0.1', PORT, function(err)
        lu.assertNil(err)
        client:read_start(function(rerr, chunk)
            if chunk then
                buffer = buffer .. chunk
                parse()
            end
            if coroutine.status(co) == 'suspended' then coroutine.resume(co) end
        end)
        coroutine.resume(co)
    end)
    coroutine.yield()
end

-- Same as in rewind.lua: the first tick after a pause from a vsync listener is still on the
-- emulation stack.
local function resumeFromMainLoop(c)
    local timer = luv.new_timer()
    local ticks = 0
    timer:start(0, 1, function()
        ticks = ticks + 1
        if ticks < 2 then return end
        timer:stop()
        timer:close()
        coroutine.resume(c)
    end)
end

-- Runs a few frames past the shell and pauses there.
local function runPastShell()
    local c = coroutine.running()
    local frames
    local shell, vsync
    shell = PCSX.Events.createEventListener('ExecutionFlow::ShellReached', function()
        shell:remove()
        frames = 0
    end)
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        if not frames then return end
        frames = frames + 1
        if frames < FRAMES_AFTER_SHELL then return end
        vsync:remove()
        PCSX.pauseEmulator()
        resumeFromMainLoop(c)
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
end

local saved

function TestGdbReverseShell:setUp()
    local emu = PCSX.settings.emulator
    saved = { emu.Debug.Debug, emu.RewindInterval, emu.RewindCount, emu.FastBoot }
    emu.Debug.Debug = true
    emu.RewindInterval = 1
    emu.RewindCount = 16
end

function TestGdbReverseShell:tearDown()
    if client then
        client:close()
        client = nil
    end
    local emu = PCSX.settings.emulator
    emu.Debug.Debug, emu.RewindInterval, emu.RewindCount, emu.FastBoot = saved[1], saved[2], saved[3], saved[4]
end

-- Boots past the shell with fastboot or a side-loaded binary, and returns the pc the hijack set.
local function bootPastHijack(mode)
    if mode == 'handmade' then
        -- No automatic captures, only a snapshot taken by hand before the boot: the ring is in use,
        -- so it still has to start again from the hijack.
        PCSX.settings.emulator.RewindInterval = 0
        PCSX.createRewindState()
        mode = 'fastboot'
    end
    if mode == 'fastboot' then
        PCSX.settings.emulator.FastBoot = true
        local ra
        local shell
        shell = PCSX.Events.createEventListener('ExecutionFlow::ShellReached', function()
            shell:remove()
            ra = PCSX.getRegisters().GPR.n.ra
        end)
        runPastShell()
        return ra
    end
    PCSX.settings.emulator.FastBoot = false
    runPastShell()
    return ENTRY
end

-- The machine boots once per process, so there is one test, and it depends on the core.
function TestGdbReverseShell:test_back_onto_the_hijack()
    local mode = GDB_REVERSE_SHELL
    if PCSX.settings.emulator.Dynarec then
        -- Reverse execution needs the interpreter, but a plain rewind doesn't: walking the ring
        -- all the way back ends on the hijack.
        if mode == 'shellhalt' then lu.skip('needs reverse execution') end
        local target = bootPastHijack(mode)
        lu.assertNotNil(target)
        lu.assertTrue(PCSX.getRewindStateCount() > (mode == 'handmade' and 0 or 1))
        while PCSX.getRewindStateCount() > 0 do lu.assertTrue(PCSX.rewindState()) end
        lu.assertEquals(PCSX.getRegisters().pc, target)
        return
    end
    local target
    if mode == 'fastboot' or mode == 'handmade' or mode == 'exe' then
        target = bootPastHijack(mode)
        connect()
    elseif mode == 'shellhalt' then
        PCSX.settings.emulator.FastBoot = false
        connect()
        lu.assertEquals(monitor('reset shellhalt'), 'OK')
        for i, w in ipairs(CODE) do
            lu.assertEquals(command(string.format('M%08x,4:', ENTRY + (i - 1) * 4) .. le32(w)), 'OK')
        end
        lu.assertEquals(command('P25=' .. le32(ENTRY)), 'OK')
        -- Leave the shell behind by a few frames, with a breakpoint that will not hit.
        lu.assertEquals(command('Z0,1f000100,4'), 'OK')
        local frames, vsync = 0
        vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
            frames = frames + 1
            if frames < FRAMES_AFTER_SHELL then return end
            vsync:remove()
            PCSX.pauseEmulator()
        end)
        lu.assertEquals(command('c'), 'T05')
        lu.assertEquals(command('z0,1f000100,4'), 'OK')
        target = ENTRY
    else
        lu.fail('unknown GDB_REVERSE_SHELL mode ' .. tostring(mode))
    end
    lu.assertNotNil(target)
    lu.assertNotEquals(pc(), le32(target), 'still sitting on the hijacked pc')
    local bp = string.format('%08x', target)
    lu.assertEquals(command('Z0,' .. bp .. ',4'), 'OK')
    lu.assertEquals(command('bc'), 'T05')
    lu.assertEquals(pc(), le32(target))
    -- And nothing before it: the boot up to the hijack can't be replayed.
    lu.assertEquals(command('bc'), 'T05replaylog:begin;')
    lu.assertEquals(pc(), le32(target))
    lu.assertEquals(command('z0,' .. bp .. ',4'), 'OK')
end
