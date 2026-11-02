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

TestSaveStateSPUCnt = {}

-- WRITE stores a0 to SPUCNT and falls into READ, which reads SPUCNT back the way a guest
-- does (lhu from 0x1f801daa) into t1, then parks. The nop lets the load land in t1 before
-- the park breakpoint fires.
local WRITE = 0x80100000
local READ = WRITE + 8
local PARK = WRITE + 20

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

local function runFrom(pc)
    local co = coroutine.running()
    local hit = false
    local bp = PCSX.addBreakpoint(PARK, 'Exec', 4, 'spucnt park', function()
        if hit then return false end
        hit = true
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
        return false
    end)
    PCSX.getRegisters().pc = pc
    PCSX.resumeEmulator()
    coroutine.yield()
    bp:remove()
    lu.assertTrue(hit)
end

local function guestWrite(val)
    PCSX.getRegisters().GPR.n.a0 = val
    runFrom(WRITE)
    return PCSX.getRegisters().GPR.n.t1
end

local function guestRead()
    PCSX.getRegisters().GPR.n.t1 = 0xdeadbeef
    runFrom(READ)
    return PCSX.getRegisters().GPR.n.t1
end

local savedDebug, savedSR, savedCtrl

function TestSaveStateSPUCnt:setUp()
    if PCSX.settings.emulator.Dynarec then lu.skip('breakpoints need the interpreter') end
    savedDebug = PCSX.settings.emulator.Debug.Debug
    PCSX.settings.emulator.Debug.Debug = true
    local regs = PCSX.getRegisters()
    savedSR = regs.CP0.r[12]
    regs.CP0.r[12] = bit.band(savedSR, bit.bnot(1))
    PCSX.Assembler.New():parse([[
        lui   $t0, 0x1f80
        sh    $a0, 0x1daa($t0)
        lui   $t0, 0x1f80
        lhu   $t1, 0x1daa($t0)
        nop
        b     -1
        nop
    ]]):compileToMemory(PCSX.getMemPtr(), WRITE, 0x80000000)
    PCSX.invalidateCache()
    savedCtrl = guestRead()
end

function TestSaveStateSPUCnt:tearDown()
    if savedDebug == nil then return end
    guestWrite(savedCtrl)
    local regs = PCSX.getRegisters()
    regs.CP0.r[12] = savedSR
    regs.pc = PARK
    PCSX.settings.emulator.Debug.Debug = savedDebug
    savedDebug = nil
end

-- SPUCNT bit 14 is the mute bit (0 = muted). A state saved with the SPU enabled but muted
-- must load back muted: the guest reads the same SPUCNT it wrote before the save.
function TestSaveStateSPUCnt:test_muted_survives_save_state()
    local muted = 0x8001 -- SPU enabled, CD audio enabled, muted
    lu.assertEquals(guestWrite(muted), muted)
    local state = PCSX.createSaveState()
    lu.assertEquals(guestWrite(0xc000), 0xc000)
    PCSX.loadSaveState(state)
    lu.assertEquals(guestRead(), muted)
end

-- And an unmuted state stays unmuted.
function TestSaveStateSPUCnt:test_unmuted_survives_save_state()
    local unmuted = 0xc001
    lu.assertEquals(guestWrite(unmuted), unmuted)
    local state = PCSX.createSaveState()
    lu.assertEquals(guestWrite(0x8000), 0x8000)
    PCSX.loadSaveState(state)
    lu.assertEquals(guestRead(), unmuted)
end
