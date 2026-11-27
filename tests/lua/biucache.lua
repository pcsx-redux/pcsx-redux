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
-- BIU_CONFIG (0xfffe0130) is decoded by its bits: IS1 (bit 11) with TAG (bit 2) set, or with DS
-- (bit 7) clear, is a cache maintenance configuration, and while it is in place COP0 SR.IsC decides
-- whether a store reaches RAM.
local lu = require 'luaunit'
local ffi = require 'ffi'

TestBIUCache = {}

local CODE = 0x80100000
local UNCACHED = 0xa0100000
local PROBE = 0x80180000

local function ram8(addr) return ffi.cast('uint8_t*', PCSX.getMemPtr() + (addr - 0x80000000)) end

local function ram32(addr) return ffi.cast('uint32_t*', PCSX.getMemPtr() + (addr - 0x80000000)) end

local function assemble(addr, code) PCSX.Assembler.New():parse(code):compileToMemory(PCSX.getMemPtr(), addr, 0x80000000) end

local function writeLutMapped() return ffi.cast('void**', PCSX.getWriteLUT())[0x8000] ~= nil end

local function interpreter()
    assemble(CODE, '    jr    $ra\n    nop\n')
    local ok, err = pcall(PCSX.callGuest, { pc = CODE })
    if ok then return true end
    lu.assertStrContains(tostring(err), 'needs the interpreter')
    return false
end

-- Writes a0 to BIU_CONFIG, sets SR to a1, runs the store in a2's place, clears SR, then puts
-- BIU_CONFIG back to 1e988h. Runs uncached, as a flush routine does.
local function isolatedStore(store)
    assemble(CODE, [[
        mfc0  $t0, $sr
        mtc0  $zero, $sr
        lui   $t1, 0xfffe
        sw    $a0, 0x130($t1)
        mtc0  $a1, $sr
        nop
        ]] .. store .. [[

        mtc0  $zero, $sr
        nop
        lui   $t2, 0x0001
        ori   $t2, $t2, 0xe988
        sw    $t2, 0x130($t1)
        mtc0  $t0, $sr
        jr    $ra
        nop
    ]])
end

-- ps1-bare-metal's flushCache() ORs TAG and IS1 into the running 1e988h, leaving DS set.
-- The recompilers store to RAM through the write LUT without calling back, so it has to empty.
function TestBIUCache:test_tag_mode_with_ds_set_empties_the_write_lut()
    if not interpreter() then lu.skip('needs the interpreter') end
    assemble(CODE, [[
        lui   $t1, 0xfffe
        sw    $a0, 0x130($t1)
        jr    $ra
        nop
    ]])
    lu.assertEquals(PCSX.callGuest({ pc = UNCACHED, args = { 0x1e988 } }).status, 'returned')
    lu.assertTrue(writeLutMapped())
    lu.assertEquals(PCSX.callGuest({ pc = UNCACHED, args = { 0x1e98c } }).status, 'returned')
    local mapped = writeLutMapped()
    lu.assertEquals(PCSX.callGuest({ pc = UNCACHED, args = { 0x1e988 } }).status, 'returned')
    lu.assertFalse(mapped)
    lu.assertTrue(writeLutMapped())
end

-- With IsC set, sub-word stores and stores through KSEG1 don't reach RAM either.
function TestBIUCache:test_isolated_stores_skip_ram_with_ds_set()
    if not interpreter() then lu.skip('needs the interpreter') end
    for _, case in ipairs({
        { 'sb    $a2, 0($a3)', PROBE }, { 'sh    $a2, 0($a3)', PROBE }, { 'sw    $a2, 0($a3)', PROBE + 0x20000000 },
    }) do
        ram32(PROBE)[0] = 0x11223344
        isolatedStore(case[1])
        local r = PCSX.callGuest { pc = UNCACHED, args = { 0x1e98c, 0x10000, 0xaabbccdd, case[2] } }
        lu.assertEquals(r.status, 'returned')
        lu.assertEquals(ram32(PROBE)[0], 0x11223344, case[1])
    end
end

-- The same BIU values with IsC clear leave RAM reachable.
function TestBIUCache:test_stores_reach_ram_with_isc_clear()
    if not interpreter() then lu.skip('needs the interpreter') end
    for _, biu in ipairs({ 0, 0x800, 0x804, 0x1e90c, 0x1e98c }) do
        ram32(PROBE)[0] = 0x11223344
        isolatedStore('sw    $a2, 0($a3)')
        local r = PCSX.callGuest { pc = UNCACHED, args = { biu, 0, 0xaabbccdd, PROBE } }
        lu.assertEquals(r.status, 'returned')
        lu.assertEquals(ram32(PROBE)[0], 0xaabbccdd, string.format('%x', biu))
        ram8(PROBE)[0] = 0
        isolatedStore('sb    $a2, 0($a3)')
        r = PCSX.callGuest { pc = UNCACHED, args = { biu, 0, 0xaabbccdd, PROBE } }
        lu.assertEquals(ram8(PROBE)[0], 0xdd, string.format('%x', biu))
    end
end

-- The tag store itself goes to the i-cache, not to RAM.
function TestBIUCache:test_tag_store_with_ds_set_skips_ram()
    if not interpreter() then lu.skip('needs the interpreter') end
    ram32(PROBE)[0] = 0x11223344
    isolatedStore('sw    $a2, 0($a3)')
    local r = PCSX.callGuest { pc = UNCACHED, args = { 0x1e98c, 0x10000, 0, PROBE } }
    lu.assertEquals(r.status, 'returned')
    lu.assertEquals(ram32(PROBE)[0], 0x11223344)
end

-- A save state keeps a written 0 apart from cold start.
function TestBIUCache:test_save_state_keeps_explicit_zero()
    if not interpreter() then lu.skip('needs the interpreter') end
    assemble(CODE, [[
        lui   $t1, 0xfffe
        sw    $a0, 0x130($t1)
        jr    $ra
        nop
    ]])
    lu.assertEquals(PCSX.callGuest({ pc = UNCACHED, args = { 0 } }).status, 'returned')
    local state = PCSX.createSaveState()
    PCSX.loadSaveState(state)
    ram32(PROBE)[0] = 0x11223344
    assemble(CODE, [[
        sw    $a2, 0($a3)
        jr    $ra
        nop
    ]])
    lu.assertEquals(PCSX.callGuest({ pc = UNCACHED, args = { 0, 0, 0xaabbccdd, PROBE } }).status, 'returned')
    lu.assertEquals(ram32(PROBE)[0], 0xaabbccdd)
    assemble(CODE, [[
        lui   $t1, 0xfffe
        sw    $a0, 0x130($t1)
        jr    $ra
        nop
    ]])
    lu.assertEquals(PCSX.callGuest({ pc = UNCACHED, args = { 0x1e988 } }).status, 'returned')
end
