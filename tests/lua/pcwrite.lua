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

TestPCWrite = {}

local CODE = 0x80100000
local ALT = CODE + 0x40
local TARGET = CODE + 0x80

local function ram32(addr) return ffi.cast('uint32_t*', PCSX.getMemPtr() + (addr - 0x80000000)) end

local function poke(addr, words)
    local p = ram32(addr)
    for i, w in ipairs(words) do p[i - 1] = w end
end

local function j(addr) return 0x08000000 + bit.rshift(bit.band(addr, 0x0fffffff), 2) end
local function jal(addr) return 0x0c000000 + bit.rshift(bit.band(addr, 0x0fffffff), 2) end

-- Same as in rewind.lua: hand control back once the cpu has unwound out of Execute().
local function resumeFromMainLoop(co)
    local timer = luv.new_timer()
    local ticks = 0
    timer:start(0, 1, function()
        ticks = ticks + 1
        if ticks < 2 then return end
        timer:stop()
        timer:close()
        coroutine.resume(co)
    end)
end

local savedDebug

function TestPCWrite:setUp()
    if PCSX.settings.emulator.Dynarec then lu.skip('breakpoints need the interpreter') end
    savedDebug = PCSX.settings.emulator.Debug.Debug
    PCSX.settings.emulator.Debug.Debug = true
end

function TestPCWrite:tearDown()
    if savedDebug == nil then return end
    PCSX.settings.emulator.Debug.Debug = savedDebug
    savedDebug = nil
end

-- Pausing in the delay slot of a jal leaves the branch pending. A pc written from there is
-- where execution has to go: the pending branch must not run the new pc as its delay slot
-- and then jump to its own target anyway.
function TestPCWrite:test_write_in_delay_slot_drops_pending_branch()
    poke(CODE, { jal(TARGET), 0x00000000 })
    poke(ALT, { 0x24081234, j(ALT + 4), 0x00000000 }) -- addiu t0, zero, 0x1234 ; park
    poke(TARGET, { 0x24095678, j(TARGET + 4), 0x00000000 }) -- addiu t1, zero, 0x5678 ; park
    PCSX.invalidateCache()
    local regs = PCSX.getRegisters()
    regs.GPR.n.t0 = 0
    regs.GPR.n.t1 = 0
    regs.pc = CODE

    local co = coroutine.running()
    -- Held until it fires: a collected handle removes its breakpoint.
    local bp = PCSX.addBreakpoint(CODE + 4, 'Exec', 4, 'delay slot', function()
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
        return false
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
    bp:remove()
    lu.assertEquals(regs.pc, CODE + 4)

    regs.pc = ALT
    local vsync
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsync:remove()
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
    end)
    PCSX.resumeEmulator()
    coroutine.yield()

    lu.assertEquals(regs.GPR.n.t0, 0x1234)
    lu.assertEquals(regs.GPR.n.t1, 0)
    lu.assertTrue(regs.pc == ALT + 4 or regs.pc == ALT + 8, string.format('pc = 0x%08x', regs.pc))
end

-- The backing field is read-only, so a script can't store past setPC by accident.
function TestPCWrite:test_raw_pc_field_is_read_only()
    local regs = PCSX.getRegisters()
    local before = regs.pc
    lu.assertFalse(pcall(function() regs.rawPC = 0x80100000 end))
    lu.assertEquals(regs.pc, before)
end

-- Paused right after a load, the loaded value is still in flight. A register edited from there
-- has to keep the edit instead of taking the load on the next step.
function TestPCWrite:test_gpr_write_drops_pending_load()
    local DATA = CODE + 0x100
    ram32(DATA)[0] = 0xaaaaaaaa
    poke(CODE, {
        0x3c010000 + bit.rshift(DATA, 16), -- lui at, hi(DATA)
        0x8c280000 + bit.band(DATA, 0xffff), -- lw t0, lo(DATA)(at)
        0x00000000, j(CODE + 12), 0x00000000,
    })
    PCSX.invalidateCache()
    local regs = PCSX.getRegisters()
    regs.GPR.n.t0 = 0
    regs.pc = CODE

    local co = coroutine.running()
    local bp = PCSX.addBreakpoint(CODE + 8, 'Exec', 4, 'load delay slot', function()
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
        return false
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
    bp:remove()
    lu.assertEquals(regs.pc, CODE + 8)

    PCSX.setGPR(8, 0x12345678)
    local vsync
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsync:remove()
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
    end)
    PCSX.resumeEmulator()
    coroutine.yield()

    lu.assertEquals(regs.GPR.n.t0, 0x12345678)
end
