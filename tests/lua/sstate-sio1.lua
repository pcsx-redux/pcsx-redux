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

-- Has to match the -sio1-port the runner passes.
local PORT = 3470

local pb = require 'pb'
local protoc = require 'protoc'

-- The state's own schema, so the state can be read and edited as a table.
local schema = protoc.new()
assert(schema:load(PCSX.getSaveStateProtoSchema(), 'savestate.proto'))
pb.option('int64_as_number')

local function decode(s) return pb.decode('SaveState', s) end

local function sio1Field(s, name)
    local sio1 = decode(s).sio1
    return sio1 and sio1[name]
end

-- The same state as a build from before SIO1 was saved would have written it.
local function dropSio1(s)
    local state = decode(s)
    state.sio1 = nil
    return pb.encode('SaveState', state)
end

local function setVersion(s, version)
    local state = decode(s)
    state.sio1.version = version
    return pb.encode('SaveState', state)
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local DATA, STAT, MODE, CTRL, MISC, BAUD = 0x1050, 0x1054, 0x1058, 0x105a, 0x105c, 0x105e
local function w16(offset, value) return { 'w', offset, value } end
local function r16(offset) return { 'r16', offset } end
local function r8(offset) return { 'r8', offset } end

-- Runs the accesses as guest code against the SIO1 registers. Every read is stored as a halfword
-- at base, in order.
local function plant(ops, base)
    local asm = {
        string.format([[
        lui $t0, 0x1f80
        li $t3, 0x%x
        ]], base),
    }
    local n = 0
    for _, op in ipairs(ops) do
        if op[1] == 'w' then
            asm[#asm + 1] = string.format([[
                li $t1, 0x%x
                sh $t1, 0x%x($t0)
            ]], op[3], op[2])
        else
            asm[#asm + 1] = string.format([[
                %s $t2, 0x%x($t0)
                nop
                sh $t2, %d($t3)
            ]], op[1] == 'r8' and 'lbu' or 'lhu', op[2], n)
            n = n + 2
        end
    end
    asm[#asm + 1] = 'b -1\nnop\n'
    PCSX.Assembler.New():parse(table.concat(asm)):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
    PCSX.invalidateCache()
    PCSX.pauseEmulator()
    local timer = luv.new_timer()
    timer:start(1, 0, function()
        timer:close()
        PCSX.getRegisters().pc = 0x80100000
        PCSX.resumeEmulator()
    end)
end

local setup = { w16(MODE, 0x0001), w16(BAUD, 0x0100), w16(MISC, 0x1234), w16(CTRL, 0x0827) }
-- A different mode and baud rate, so a different interrupt delay, the IRQ flag acknowledged.
local disturb = { w16(MODE, 0x004e), w16(BAUD, 0x0040), w16(CTRL, 0x0837), w16(MISC, 0x5678) }
-- Everything the guest can read, then the receive buffer drained a byte at a time with STAT after each.
local readAll = {
    r16(STAT), r16(MODE), r16(CTRL), r16(MISC), r16(BAUD), r8(DATA), r16(STAT), r8(DATA), r16(STAT), r8(DATA),
    r16(STAT), r8(DATA), r16(STAT),
}

local function readings(base)
    local r = {}
    for i = 0, 12 do
        local a = base - 0x80000000 + i * 2
        r[#r + 1] = bytes[a] + bytes[a + 1] * 256
    end
    return {
        stat = r[1],
        mode = r[2],
        ctrl = r[3],
        misc = r[4],
        baud = r[5],
        data = { r[6], r[8], r[10], r[12] },
        after = { r[7], r[9], r[11], r[13] },
    }
end

-- One byte from the peer, framed the way the SIO1 protocol sends it: a size byte, then the message.
local function frame(char)
    local body = '\10\3\10\1' .. char
    return string.char(#body) .. body
end

local client, connected, done
local function finish(status)
    if done then return end
    done = true
    if client then
        client:close(function() PCSX.quit(status) end)
        client = nil
    else
        PCSX.quit(status)
    end
end

local failed = false
local function check(condition, message)
    if not condition then
        print(message)
        failed = true
        finish(1)
    end
    return condition
end

local function hex(v) return string.format('%04x', v or 0) end

-- The guest has to read after a load what it read at the save.
local function sameAsControl(control, got, what)
    for _, name in ipairs({ 'stat', 'mode', 'ctrl', 'misc', 'baud' }) do
        check(got[name] == control[name],
              what .. ': ' .. name .. ' reads ' .. hex(got[name]) .. ', expected ' .. hex(control[name]))
    end
    for i = 1, 4 do
        check(got.after[i] == control.after[i], what .. ': STAT after DATA read ' .. i .. ' is ' .. hex(got.after[i]) ..
                  ', expected ' .. hex(control.after[i]))
    end
    for i = 1, 3 do
        check(got.data[i] == control.data[i],
              what .. ': DATA read ' .. i .. ' is ' .. hex(got.data[i]) .. ', expected ' .. hex(control.data[i]))
    end
end

local function expectReset(r, what)
    check(r.stat == 0x0185, what .. ': STAT reads ' .. hex(r.stat) .. ', expected 0185')
    for _, name in ipairs({ 'mode', 'ctrl', 'misc', 'baud' }) do
        check(r[name] == 0, what .. ': ' .. name .. ' reads ' .. hex(r[name]) .. ', expected 0')
    end
    for i = 1, 4 do
        check(r.data[i] == 0, what .. ': DATA read ' .. i .. ' is ' .. hex(r.data[i]) .. ', expected 0')
        check(r.after[i] == 0x0185,
              what .. ': STAT after DATA read ' .. i .. ' is ' .. hex(r.after[i]) .. ', expected 0185')
    end
end

local steps = {}
local function step(f) steps[#steps + 1] = f end

local mid, control

step(function()
    client = luv.new_tcp()
    client:connect('127.0.0.1', PORT, function(err) connected = not err end)
end)

step(function()
    if not check(connected, 'could not connect to the SIO1 server') then return end
    plant(setup, 0x80110000)
end)

step(function() client:write(frame('A') .. frame('B') .. frame('C') .. frame('D')) end)

-- The guest reads A. B, C and D are left in the receive buffer, and the interrupt for them has fired.
step(function() plant({ r8(DATA) }, 0x80110100) end)

step(function()
    local first = bytes[0x110100]
    check(first == string.byte('A'), 'the guest did not read A first: ' .. hex(first))
    mid = tostring(PCSX.createSaveState())
    PCSX.createRewindState()
    check(sio1Field(mid, 'version') == 1, 'the state has no sio1 message of version 1')
    check(bit.band(sio1Field(mid, 'status') or 0, 0x200) ~= 0, 'the IRQ flag is not set in the state')
    check(sio1Field(mid, 'rx_fifo') == 'BCD', 'the receive buffer in the state is not BCD')
    -- Arrives after the save.
    client:write(frame('E'))
end)

step(function()
    check(PCSX.getRewindStateCount() == 1, 'the rewind ring does not hold the snapshot')
    plant(readAll, 0x80110300)
end)

-- What the guest reads from the state as it was saved, with E on top of what the state holds.
step(function()
    control = readings(0x80110300)
    check(bit.band(control.stat, 0x200) ~= 0, 'control: IRQ flag not set')
    check(bit.band(control.stat, 0x002) ~= 0, 'control: RXRDY not set')
    check(control.mode ~= 0 and control.ctrl ~= 0 and control.misc ~= 0 and control.baud ~= 0,
          'control: registers are not set up')
    local expected = { string.byte('B'), string.byte('C'), string.byte('D'), string.byte('E') }
    for i = 1, 4 do
        check(control.data[i] == expected[i], 'control: DATA read ' .. i .. ' is ' .. hex(control.data[i]))
    end
    plant(disturb, 0x80110400)
    -- Y and Z are to be gone after the load, and the socket stops in the middle of a message.
    client:write(frame('Y') .. frame('Z') .. frame('F'):sub(1, 3))
end)

step(function()
    local disturbed = tostring(PCSX.createSaveState())
    check(sio1Field(disturbed, 'rx_fifo') == 'YZ', 'the receive buffer was not YZ before the load')
    check(sio1Field(disturbed, 'cycle_count') ~= sio1Field(mid, 'cycle_count'),
          'the disturbance did not change the interrupt delay')
    PCSX.loadSaveState(toSlice(mid))
end)

-- The decoder was in the middle of the message for F, and has to carry on with the rest of it.
step(function() client:write(frame('F'):sub(4)) end)

step(function() plant(readAll, 0x80110500) end)

step(function()
    local got = readings(0x80110500)
    sameAsControl(control, got, 'save state')
    check(got.data[4] == string.byte('F'), 'save state: the fourth byte is ' .. hex(got.data[4]) .. ', expected F')
    local after = tostring(PCSX.createSaveState())
    check(sio1Field(after, 'cycle_count') == sio1Field(mid, 'cycle_count'),
          'save state: the interrupt delay did not come back')
    check(sio1Field(after, 'baud_rate') == sio1Field(mid, 'baud_rate'), 'save state: the baud rate did not come back')
end)

-- The same, with the snapshot taken together with the save state, through the rewind ring.
step(function()
    plant(disturb, 0x80110600)
    client:write(frame('Y') .. frame('Z') .. frame('G'):sub(1, 3))
end)

step(function() check(PCSX.rewindState(), 'there is no snapshot to rewind to') end)

step(function() client:write(frame('G'):sub(4)) end)

step(function() plant(readAll, 0x80110700) end)

step(function()
    local got = readings(0x80110700)
    sameAsControl(control, got, 'rewind')
    check(got.data[4] == string.byte('G'), 'rewind: the fourth byte is ' .. hex(got.data[4]) .. ', expected G')
    PCSX.loadSaveState(toSlice(mid))
end)

-- A state without the sio1 message, then one with a version that isn't 1, both loaded on top of live registers.
step(function()
    check((sio1Field(tostring(PCSX.createSaveState()), 'mode') or 0) ~= 0,
          'the registers are not live before the next load')
    PCSX.loadSaveState(toSlice(dropSio1(mid)))
end)

step(function() plant(readAll, 0x80110800) end)

step(function()
    expectReset(readings(0x80110800), 'state without sio1')
    PCSX.loadSaveState(toSlice(mid))
end)

step(function()
    check((sio1Field(tostring(PCSX.createSaveState()), 'mode') or 0) ~= 0,
          'the registers are not live before the next load')
    PCSX.loadSaveState(toSlice(setVersion(mid, 2)))
end)

step(function() plant(readAll, 0x80110900) end)

step(function()
    expectReset(readings(0x80110900), 'state with sio1 version 2')
    finish(failed and 1 or 0)
end)

local frames = 0
SIO1SaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if done or frames < 30 or frames % 2 ~= 0 then return end
    local f = table.remove(steps, 1)
    if f then f() end
end)
