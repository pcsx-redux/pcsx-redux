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

-- SIO1 TX interrupt, run here with nothing connected to SIO1. On a SCPH-1001 and a SCPH-5501,
-- setting CTRL bit 10 while the transmitter is idle raises SIO1 STAT bit 9 and I_STAT bit 8
-- before the next read, without any byte being written, and with TXEN clear too. An ack (CTRL
-- bit 4) while bit 10 stays set and the transmitter is idle leaves both set again.
TestSIO1TxIrq = {}

-- CTRL stores a0 to SIO1 CTRL, ISTAT stores a0 to I_STAT. Each then reads SIO1 STAT into t1
-- and I_STAT into t2, and parks.
local CTRL = 0x80100000
local PARK = CTRL + 20
local ISTAT = CTRL + 28

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

local function run(pc, val)
    local co = coroutine.running()
    local hit = false
    local bp = PCSX.addBreakpoint(PARK, 'Exec', 4, 'sio1 txirq park', function()
        if hit then return false end
        hit = true
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
        return false
    end)
    local regs = PCSX.getRegisters()
    regs.GPR.n.a0 = val
    regs.pc = pc
    PCSX.resumeEmulator()
    coroutine.yield()
    bp:remove()
    lu.assertTrue(hit)
    regs = PCSX.getRegisters()
    return bit.band(regs.GPR.n.t1, 0x200) ~= 0, bit.band(regs.GPR.n.t2, 0x100) ~= 0
end

local function ctrl(v) return run(CTRL, v) end
local function istatAck() return run(ISTAT, 0xfffffeff) end

local savedDebug, savedSR

function TestSIO1TxIrq:setUp()
    if PCSX.settings.emulator.Dynarec then lu.skip('breakpoints need the interpreter') end
    savedDebug = PCSX.settings.emulator.Debug.Debug
    PCSX.settings.emulator.Debug.Debug = true
    local regs = PCSX.getRegisters()
    savedSR = regs.CP0.r[12]
    regs.CP0.r[12] = bit.band(savedSR, bit.bnot(1))
    PCSX.Assembler.New():parse([[
        lui   $t0, 0x1f80
        sh    $a0, 0x105a($t0)
        lw    $t1, 0x1054($t0)
        lw    $t2, 0x1070($t0)
        nop
        b     -1
        nop
        lui   $t0, 0x1f80
        sw    $a0, 0x1070($t0)
        b     -8
        nop
    ]]):compileToMemory(PCSX.getMemPtr(), CTRL, 0x80000000)
    PCSX.invalidateCache()
    ctrl(0x0015)
    istatAck()
end

function TestSIO1TxIrq:tearDown()
    if savedDebug == nil then return end
    ctrl(0x0010)
    istatAck()
    local regs = PCSX.getRegisters()
    regs.CP0.r[12] = savedSR
    regs.pc = PARK
    PCSX.settings.emulator.Debug.Debug = savedDebug
    savedDebug = nil
end

function TestSIO1TxIrq:test_disabled_raises_nothing()
    local stat, istat = ctrl(0x0005)
    lu.assertFalse(stat)
    lu.assertFalse(istat)
end

function TestSIO1TxIrq:test_enable_while_idle()
    local stat, istat = ctrl(0x0405)
    lu.assertTrue(stat)
    lu.assertTrue(istat)
end

function TestSIO1TxIrq:test_enable_with_txen_clear()
    local stat, istat = ctrl(0x0404)
    lu.assertTrue(stat)
    lu.assertTrue(istat)
end

function TestSIO1TxIrq:test_ack_while_enabled_and_idle()
    ctrl(0x0405)
    istatAck()
    local stat, istat = ctrl(0x0415)
    lu.assertTrue(stat)
    lu.assertTrue(istat)
end

function TestSIO1TxIrq:test_ack_after_disable()
    ctrl(0x0405)
    istatAck()
    local stat, istat = ctrl(0x0015)
    lu.assertFalse(stat)
    lu.assertFalse(istat)
end
