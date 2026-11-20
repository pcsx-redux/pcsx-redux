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

TestSaveStateBranchSlot = {}

local CODE = 0x80100000
local PARK = CODE + 0x100

-- Assembles guest code at addr. Branches take an absolute target as @addr; j takes the
-- address's low 28 bits.
local function asm(addr, code)
    PCSX.Assembler.New():parse(code):compileToMemory(PCSX.getMemPtr(), addr, 0x80000000)
end

local function jPark() return string.format('j 0x%x', bit.band(PARK, 0x0fffffff)) end

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

local function runUntilBreak(addrs)
    local co = coroutine.running()
    local bps = {}
    local hit
    for _, addr in ipairs(addrs) do
        -- Held until it fires: a collected handle removes its breakpoint.
        bps[#bps + 1] = PCSX.addBreakpoint(addr, 'Exec', 4, 'branch slot', function()
            if hit then return false end
            local regs = PCSX.getRegisters()
            hit = { pc = regs.pc, epc = regs.CP0.r[14], cause = regs.CP0.r[13] }
            PCSX.pauseEmulator()
            resumeFromMainLoop(co)
            return false
        end)
    end
    PCSX.resumeEmulator()
    coroutine.yield()
    for _, bp in ipairs(bps) do bp:remove() end
    return hit
end

local function runForAVsync()
    local co = coroutine.running()
    local vsync
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsync:remove()
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
end

local savedDebug, savedSR

function TestSaveStateBranchSlot:setUp()
    if PCSX.settings.emulator.Dynarec then lu.skip('breakpoints need the interpreter') end
    savedDebug = PCSX.settings.emulator.Debug.Debug
    PCSX.settings.emulator.Debug.Debug = true
    savedSR = PCSX.getRegisters().CP0.r[12]
end

function TestSaveStateBranchSlot:tearDown()
    if savedDebug == nil then return end
    local regs = PCSX.getRegisters()
    regs.CP0.r[12] = savedSR
    regs.pc = PARK
    PCSX.settings.emulator.Debug.Debug = savedDebug
    savedDebug = nil
end

-- Runs from start to the given slot, optionally takes a save state there, dirties the cpu on
-- unrelated code and loads it back, then runs into the syscall's exception vector.
local function syscallInSlot(start, slot, viaSaveState, redirect)
    asm(PARK, jPark() .. '\nnop')
    PCSX.invalidateCache()
    local regs = PCSX.getRegisters()
    -- No interrupt may be taken in place of the syscall.
    regs.CP0.r[12] = bit.band(savedSR, bit.bnot(1))
    regs.pc = start
    local paused = runUntilBreak({ slot })
    lu.assertEquals(paused.pc, slot)
    if viaSaveState then
        local state = PCSX.createSaveState()
        regs.pc = PARK
        runForAVsync()
        PCSX.loadSaveState(state)
        lu.assertEquals(regs.pc, slot)
    end
    if redirect then regs.pc = redirect end
    local vector = runUntilBreak({ 0x80000080, 0xbfc00180 })
    lu.assertNotNil(vector)
    lu.assertEquals(bit.band(bit.rshift(vector.cause, 2), 0x1f), 8) -- Syscall
    return vector
end

-- A syscall in the slot of a not-taken bne that sits in a taken branch's slot: BD set, EPC at
-- the bne.
local function nested(viaSaveState)
    local T = CODE + 0x40
    asm(CODE, string.format('b @0x%x\nbne $zero, $zero, 16 # never taken', T))
    asm(T, 'syscall\n' .. jPark() .. '\nnop')
    local v = syscallInSlot(CODE, T, viaSaveState)
    lu.assertEquals(bit.rshift(v.cause, 31), 1)
    lu.assertEquals(v.epc, CODE + 4)
end

-- A syscall in the slot of a plain taken branch: BD set, EPC at the branch.
local function plain(viaSaveState)
    local B = CODE + 0x80
    asm(B, string.format('b @0x%x\nsyscall', PARK))
    local v = syscallInSlot(B, B + 4, viaSaveState)
    lu.assertEquals(bit.rshift(v.cause, 31), 1)
    lu.assertEquals(v.epc, B)
end

function TestSaveStateBranchSlot:test_nested_live() nested(false) end
function TestSaveStateBranchSlot:test_nested_through_save_state() nested(true) end
function TestSaveStateBranchSlot:test_plain_live() plain(false) end
function TestSaveStateBranchSlot:test_plain_through_save_state() plain(true) end

-- A pc written while paused between a branch and its slot leaves the branch behind: a syscall
-- at the new pc is in nobody's slot.
function TestSaveStateBranchSlot:test_pc_write_drops_branch_slot()
    local T = CODE + 0x40
    local S = CODE + 0xc0
    asm(CODE, string.format('b @0x%x\nbne $zero, $zero, 16 # never taken', T))
    asm(T, 'syscall\n' .. jPark() .. '\nnop')
    asm(S, 'syscall\n' .. jPark() .. '\nnop')
    local v = syscallInSlot(CODE, T, false, S)
    lu.assertEquals(bit.rshift(v.cause, 31), 0)
    lu.assertEquals(v.epc, S)
end
