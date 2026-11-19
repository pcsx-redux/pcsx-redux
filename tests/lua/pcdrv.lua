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

TestPCdrvSaveState = {}

local CODE = 0x80100000
local NAME = 0x80140000
local BUF = 0x80150000
local FILENAME = 'pcdrv-savestate.bin'

-- One stub per PCdrv call: break <code> ; jr ra ; nop. The handler skips the break and leaves
-- its result in v1.
local CALLS = { init = 0x101, creat = 0x102, open = 0x103, close = 0x104, read = 0x105, write = 0x106, lseek = 0x107 }
local STUBS = {}
do
    local i = 0
    for name, code in pairs(CALLS) do
        STUBS[name] = CODE + i * 16
        i = i + 1
    end
end

local function plantStubs()
    for name, addr in pairs(STUBS) do
        PCSX.Assembler.New():parse(string.format([[
            break 0x%x
            jr    $ra
            nop
        ]], CALLS[name])):compileToMemory(PCSX.getMemPtr(), addr, 0x80000000)
    end
end

local function pcdrv(name, args, stage, fetch)
    local r = PCSX.callGuest { pc = STUBS[name], args = args, stage = stage, fetch = fetch }
    lu.assertEquals(r.status, 'returned', name)
    return r
end

local function creat() return pcdrv('creat', { NAME, 0 }, { { addr = NAME, data = FILENAME .. '\0' } }).v1 end
local function open(mode) return pcdrv('open', { NAME, 0, mode }, { { addr = NAME, data = FILENAME .. '\0' } }).v1 end
local function close(fd) return pcdrv('close', { fd }).v1 end
local function write(fd, data) return pcdrv('write', { 0, fd, #data, BUF }, { { addr = BUF, data = data } }).v1 end
local function read(fd, size)
    local r = pcdrv('read', { 0, fd, size, BUF }, nil, { { addr = BUF, size = size } })
    return r.out[1]:sub(1, r.v1)
end
local function seek(fd, offset) return pcdrv('lseek', { fd, 0, offset, 0 }).v1 end

local function hostFile()
    local f = assert(io.open(FILENAME, 'rb'))
    local data = f:read('*a')
    f:close()
    return data
end

local savedPCdrv

function TestPCdrvSaveState:setUp()
    if PCSX.settings.emulator.Dynarec then lu.skip('callGuest needs the interpreter') end
    savedPCdrv = PCSX.settings.emulator.Debug.PCdrv
    PCSX.settings.emulator.Debug.PCdrv = true
    os.remove(FILENAME)
    plantStubs()
    pcdrv('init', {})
end

function TestPCdrvSaveState:tearDown()
    if savedPCdrv == nil then return end
    pcdrv('init', {})
    PCSX.settings.emulator.Debug.PCdrv = savedPCdrv
    savedPCdrv = nil
    os.remove(FILENAME)
end

-- Leaves the read position at 3 and the write position at 4 in "AABBAAAA", so the two differ
-- from each other, from 0 and from the end of the file.
local function prepare()
    local fd = creat()
    lu.assertEquals(fd, 0)
    lu.assertEquals(write(fd, 'AAAAAAAA'), 8)
    lu.assertEquals(seek(fd, 2), 2)
    lu.assertEquals(write(fd, 'BB'), 2)
    lu.assertEquals(read(fd, 1), 'B')
    return fd
end

-- After a load the guest has to see the file where it left it: writes at the saved write
-- position rather than appended, reads from the saved read position, and nothing truncated.
local function checkResumed(fd)
    lu.assertEquals(write(fd, 'DD'), 2)
    lu.assertEquals(read(fd, 1), 'B')
    -- Closing flushes the host side.
    lu.assertEquals(close(fd), 0)
    lu.assertEquals(hostFile(), 'AABBDDAA')
end

function TestPCdrvSaveState:test_load_keeps_positions_of_a_created_file()
    local fd = prepare()
    local state = PCSX.createSaveState()
    lu.assertEquals(write(fd, 'CC'), 2)
    lu.assertEquals(read(fd, 1), 'B')
    lu.assertEquals(hostFile(), 'AABBCCAA')
    PCSX.loadSaveState(state)
    plantStubs()
    checkResumed(fd)
end

function TestPCdrvSaveState:test_restore_keeps_positions_of_a_created_file()
    local fd = prepare()
    PCSX.captureState()
    lu.assertEquals(write(fd, 'CC'), 2)
    lu.assertEquals(read(fd, 1), 'B')
    PCSX.restoreState()
    plantStubs()
    checkResumed(fd)
end

-- A state saved after a load has to name the file just as well as the first one did.
function TestPCdrvSaveState:test_second_generation_state_still_names_the_file()
    local fd = prepare()
    PCSX.loadSaveState(PCSX.createSaveState())
    local state = PCSX.createSaveState()
    lu.assertEquals(write(fd, 'CC'), 2)
    PCSX.loadSaveState(state)
    plantStubs()
    checkResumed(fd)
end

function TestPCdrvSaveState:test_load_keeps_the_read_position_of_a_read_only_file()
    local f = assert(io.open(FILENAME, 'wb'))
    f:write('0123456789')
    f:close()
    local fd = open(0)
    lu.assertEquals(fd, 0)
    lu.assertEquals(read(fd, 4), '0123')
    local state = PCSX.createSaveState()
    lu.assertEquals(read(fd, 4), '4567')
    PCSX.loadSaveState(state)
    plantStubs()
    lu.assertEquals(read(fd, 4), '4567')
    -- Still read-only.
    write(fd, 'X')
    lu.assertEquals(hostFile(), '0123456789')
end

-- A file deleted since the save comes back, with the guest still writing where it was.
function TestPCdrvSaveState:test_load_recreates_a_deleted_file()
    local fd = prepare()
    local state = PCSX.createSaveState()
    lu.assertEquals(close(fd), 0)
    os.remove(FILENAME)
    PCSX.loadSaveState(state)
    plantStubs()
    lu.assertEquals(write(fd, 'DD'), 2)
    lu.assertEquals(close(fd), 0)
    lu.assertEquals(hostFile(), '\0\0\0\0DD')
end
