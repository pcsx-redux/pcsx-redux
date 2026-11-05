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

local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local bytes = ffi.cast('uint8_t*', PCSX.getMemPtr())
local code = 0x100000 / 4

local function varint(s, pos)
    local v, mul = 0, 1
    while true do
        local b = s:byte(pos)
        pos = pos + 1
        v = v + bit.band(b, 0x7f) * mul
        if b < 0x80 then return v, pos end
        mul = mul * 128
    end
end

local function encodeVarint(v)
    local out = {}
    while v >= 0x80 do
        out[#out + 1] = string.char(bit.bor(v % 128, 0x80))
        v = math.floor(v / 128)
    end
    out[#out + 1] = string.char(v)
    return table.concat(out)
end

-- Splits a message into { num, wt, key, payload } records.
local function records(s)
    local out = {}
    local pos = 1
    while pos <= #s do
        local start = pos
        local key
        key, pos = varint(s, pos)
        local num, wt = math.floor(key / 8), key % 8
        local payload
        if wt == 0 then
            payload, pos = varint(s, pos)
        elseif wt == 2 then
            local len
            len, pos = varint(s, pos)
            payload = s:sub(pos, pos + len - 1)
            pos = pos + len
        elseif wt == 1 then
            payload = s:sub(pos, pos + 7)
            pos = pos + 8
        elseif wt == 5 then
            payload = s:sub(pos, pos + 3)
            pos = pos + 4
        else
            error('wire type ' .. wt)
        end
        out[#out + 1] = { num = num, wt = wt, raw = s:sub(start, pos - 1), payload = payload, key = key }
    end
    return out
end

local function field(s, wanted)
    if not s then return nil end
    for _, r in ipairs(records(s)) do
        if r.num == wanted then return r.payload end
    end
end

-- Field numbers of pad1 and pad2 inside the SIO message (sstate.h).
local PAD1, PAD2 = 81, 82

-- The same state as a build from before the pad messages existed would have written it.
local function stripPads(s)
    local out = {}
    for _, r in ipairs(records(s)) do
        if r.num == 7 then
            local inner = {}
            for _, ir in ipairs(records(r.payload)) do
                if ir.num ~= PAD1 and ir.num ~= PAD2 then inner[#inner + 1] = ir.raw end
            end
            local body = table.concat(inner)
            out[#out + 1] = encodeVarint(r.key) .. encodeVarint(#body) .. body
        else
            out[#out + 1] = r.raw
        end
    end
    return table.concat(out)
end

local function padField(s, num) return field(field(field(s, 7), PAD1), num) or 0 end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

-- Sends the given bytes to the selected port through SIO0 and stores the replies at base.
-- A -1 in the list drops and raises the select line, which starts a new transfer; reset
-- also empties the receive FIFO first.
local function plant(send, base, reset)
    local i = code
    local function emit(w) ram[i] = w i = i + 1 end
    local function reselect()
        emit(0xa500104a) -- sh $zero, 0x104a($t0)
        emit(0x34091003) -- ori $t1, $zero, 0x1003
        emit(0xa509104a) -- sh $t1, 0x104a($t0)
    end
    emit(0x3c081f80) -- lui $t0, 0x1f80
    emit(0x3c0b0000 + bit.rshift(base, 16)) -- lui $t3, hi
    emit(0x356b0000 + bit.band(base, 0xffff)) -- ori $t3, $t3, lo
    if reset then
        emit(0x34090040) -- ori $t1, $zero, 0x40
        emit(0xa509104a) -- sh $t1, 0x104a($t0)
    end
    local n = 0
    for _, b in ipairs(send) do
        if b < 0 then
            reselect()
        else
            emit(0x34090000 + b) -- ori $t1, $zero, b
            emit(0xa1091040) -- sb $t1, 0x1040($t0)
            emit(0x910a1040) -- lbu $t2, 0x1040($t0)
            emit(0xa16a0000 + n) -- sb $t2, n($t3)
            n = n + 1
        end
    end
    emit(0x1000ffff) -- b .
    emit(0)
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
