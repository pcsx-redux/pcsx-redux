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
local bytes = ffi.cast('uint8_t*', PCSX.getMemPtr())

-- The state's own schema, so the state can be read and edited as a table.
local pb = require 'pb'
local protoc = require 'protoc'
local schema = protoc.new()
assert(schema:load(PCSX.getSaveStateProtoSchema(), 'savestate.proto'))
pb.option('int64_as_number')

-- The same state as a build from before the pad messages existed would have written it.
local function stripPads(s)
    local state = pb.decode('SaveState', s)
    state.sio.pad1 = nil
    state.sio.pad2 = nil
    return pb.encode('SaveState', state)
end

-- PadTransfer fields 2 to 10 (sstate.h), by field number.
local PAD_FIELDS = {
    [2] = 'cmd',
    [3] = 'config_mode',
    [4] = 'analog_mode',
    [5] = 'buffer_len',
    [6] = 'current_byte',
    [7] = 'buffer',
    [8] = 'motor_mapping',
    [9] = 'config_commands_used',
    [10] = 'old_method_latch',
}

local function padField(s, num)
    local sio = pb.decode('SaveState', s).sio
    local pad = sio and sio.pad1
    local v = pad and pad[PAD_FIELDS[num]]
    if v == nil or v == false then return 0 end
    return v
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

-- Sends the given bytes to the selected port through SIO0 and stores the replies at base.
-- A -1 in the list drops and raises the select line, which starts a new transfer; reset
-- also empties the receive FIFO first.
local function guestCode(send, base, reset)
    local lines = {}
    local function asm(fmt, ...) lines[#lines + 1] = string.format(fmt, ...) end
    asm('lui $t0, 0x1f80')
    asm('lui $t3, 0x%x', bit.rshift(base, 16))
    asm('ori $t3, $t3, 0x%x', bit.band(base, 0xffff))
    if reset then
        asm('ori $t1, $zero, 0x40')
        asm('sh $t1, 0x104a($t0)')
    end
    local n = 0
    for _, b in ipairs(send) do
        if b < 0 then
            asm('sh $zero, 0x104a($t0)')
            asm('ori $t1, $zero, 0x1003')
            asm('sh $t1, 0x104a($t0)')
        else
            asm('ori $t1, $zero, 0x%x', b)
            asm('sb $t1, 0x1040($t0)')
            asm('lbu $t2, 0x1040($t0)')
            asm('sb $t2, %d($t3)', n)
            n = n + 1
        end
    end
    asm('b -1')
    asm('nop')
    return table.concat(lines, '\n')
end

local function plant(send, base, reset)
    PCSX.Assembler.New():parse(guestCode(send, base, reset)):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
    PCSX.invalidateCache()
    PCSX.pauseEmulator()
    local timer = luv.new_timer()
    timer:start(1, 0, function()
        timer:close()
        PCSX.getRegisters().pc = 0x80100000
        PCSX.resumeEmulator()
    end)
end

local function replies(base, n)
    local out = {}
    for i = 1, n do out[i] = string.format('%02x', bytes[base - 0x80000000 + i - 1]) end
    return table.concat(out, ' ')
end

local first = { -1, 0x01, 0x45, 0x00 }
local rest = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
local leaveConfig = { -1, 0x01, 0x43, 0x00, 0x00, 0x00 }
local enterConfig = { -1, 0x01, 0x43, 0x00, 0x01, 0x00 }
local function join(a, b)
    local out = {}
    for _, v in ipairs(a) do out[#out + 1] = v end
    for _, v in ipairs(b) do out[#out + 1] = v end
    return out
end

local frames = 0
local mid, control, afterRest, disturbed
PadSaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        PCSX.settings.pads[1].DeviceType = 'Analog'
        PCSX.settings.pads[1].Connected = true
        PCSX.SIO0.slots[1].pads[1].map()
        plant(join(enterConfig, first), 0x80110000, true)
    elseif frames == 32 then
        mid = tostring(PCSX.createSaveState())
        plant(rest, 0x80110100, false)
    elseif frames == 34 then
        control = replies(0x80110100, #rest)
        if control:find('^ff ff ff') or control == '00 00 00 00 00 00' then
            print('control transfer did not answer like a pad in config mode: ' .. control)
            PCSX.quit(1)
            return
        end
        plant(leaveConfig, 0x80110200, false)
    elseif frames == 36 then
        PCSX.loadSaveState(toSlice(mid))
    elseif frames == 38 then
        plant(rest, 0x80110300, false)
    elseif frames == 40 then
        local got = replies(0x80110300, #rest)
        if got ~= control then
            print('pad did not resume the transfer after load: [' .. got .. '] expected [' .. control .. ']')
            PCSX.quit(2)
            return
        end
        plant(leaveConfig, 0x80110400, false)
    elseif frames == 42 then
        disturbed = tostring(PCSX.createSaveState())
        PCSX.loadSaveState(toSlice(stripPads(mid)))
    elseif frames == 44 then
        local after = tostring(PCSX.createSaveState())
        if padField(disturbed, 5) == 0 then
            print('no pad1 message found in the saved state')
            PCSX.quit(4)
            return
        end
        for _, num in ipairs({ 2, 3, 4, 5, 6, 7, 8, 9, 10 }) do
            if padField(after, num) ~= padField(disturbed, num) then
                print('a state without pad messages changed the pad: field ' .. num)
                PCSX.quit(3)
                return
            end
        end
        PCSX.quit(0)
    end
end)
