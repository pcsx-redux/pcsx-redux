-- SIO1 must follow a save state: the registers at 1F801050h-1F80105Fh, the RX FIFO the guest
-- reads through 1F801050h, the interrupt flag, and the character timing SIO1 schedules its
-- interrupts with. The runner starts the SIO1 server (protobuf mode) with -sio1-server, and this
-- script connects to it as the peer.
--
-- Cases, each failing with its own exit code:
--  10-19 full save/load mid-transfer: registers, IRQ flag, FIFO replaced and not merged, timing
--  20-29 the same through the in-memory rewind ring (createRewindState / rewindState)
--  30-39 the SaveState carries field 18 with a version field 1 set to 1
--  40-49 a state without field 18 (an older save) loads with SIO1 at its reset values
--  50-59 a state whose field 18 has a version this build does not know loads the same way
--
-- Guest code is planted at 0x80100000 and spins when done; results land at 0x80110000.
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local bytes = ffi.cast('uint8_t*', PCSX.getMemPtr())
-- Has to match the -sio1-port the runner passes.
local PORT = 6790
local RESULTS = 0x110000
local MARKER = 0x120000 / 4

-- Guest code is built as assembler text and compiled with PCSX.Assembler on run().
local function assembler()
    local a = { lines = {}, slots = 0 }
    function a.asm(fmt, ...) a.lines[#a.lines + 1] = string.format(fmt, ...) end
    function a.sh(reg, v)
        a.asm('li $t1, 0x%x', v)
        a.asm('sh $t1, 0x%x($t0)', reg)
    end
    function a.sb(reg, v)
        a.asm('li $t1, 0x%x', v)
        a.asm('sb $t1, 0x%x($t0)', reg)
    end
    local function store(op, reg)
        a.asm('%s $t1, 0x%x($t0)', op, reg)
        a.asm('nop')
        a.asm('sw $t1, %d($t3)', a.slots * 4)
        a.slots = a.slots + 1
        return a.slots
    end
    function a.lhu(reg) return store('lhu', reg) end
    function a.lbu(reg) return store('lbu', reg) end
    -- Write one TX byte, ack the IRQ flags while it is in flight (with TX interrupts enabled and
    -- the transmitter idle, the interrupt is raised again as soon as it is acked), and count loop
    -- turns until I_STAT bit 8 rises.
    function a.timeTx(ctrlAck, data)
        a.asm('lw $t1, 0x1074($t0)') -- I_MASK
        a.asm('nop')
        a.asm('andi $t1, $t1, 0xfeff')
        a.asm('sw $t1, 0x1074($t0)')
        a.sb(0x1050, data)
        a.sh(0x105a, ctrlAck)
        a.asm('li $t1, 0xfffffeff')
        a.asm('sw $t1, 0x1070($t0)') -- ack I_STAT bit 8
        a.asm('li $t2, 0')
        a.asm('li $t4, 0x200000')
        a.asm('lw $t1, 0x1070($t0)') -- loop: I_STAT
        a.asm('nop')
        a.asm('andi $t1, $t1, 0x100')
        a.asm('bne $t1, $zero, 4') -- to done
        a.asm('nop')
        a.asm('addiu $t2, $t2, 1')
        a.asm('bne $t2, $t4, -7') -- to loop
        a.asm('nop')
        a.asm('sw $t2, %d($t3)', a.slots * 4) -- done
        a.slots = a.slots + 1
        return a.slots
    end
    a.asm('lui $t0, 0x1f80')
    a.asm('li $t3, 0x%x', 0x80000000 + RESULTS)
    return a
end

local function run(a)
    a.asm('b -1')
    a.asm('nop')
    PCSX.Assembler.New():parse(table.concat(a.lines, '\n')):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
    PCSX.invalidateCache()
    PCSX.pauseEmulator()
    local timer = luv.new_timer()
    timer:start(1, 0, function()
        timer:close()
        PCSX.getRegisters().pc = 0x80100000
        PCSX.resumeEmulator()
    end)
end

local function slot(n) return ram[RESULTS / 4 + n - 1] end

-- The peer socket, closed before quitting so the event loop can shut down cleanly.
local client, done
local function finish(code)
    if done then return end
    done = true
    if client then
        local c = client
        client = nil
        c:close(function() PCSX.quit(code) end)
    else
        PCSX.quit(code)
    end
end

local function fail(code, msg)
    print(string.format('sstate-sio1 FAIL %d: %s', code, msg))
    finish(code)
end

-- The state's own schema, so the state can be read and edited as a table.
local pb = require 'pb'
local protoc = require 'protoc'
local schema = protoc.new()
assert(schema:load(PCSX.getSaveStateProtoSchema(), 'savestate.proto'))
pb.option('int64_as_string')
local function decode(s) return pb.decode('SaveState', s) end
-- The same state as a build from before SIO1 was saved would have written it.
local function stripSIO1(s)
    local state = decode(s)
    state.sio1 = nil
    return pb.encode('SaveState', state)
end
local function setSIO1Version(s, version)
    local state = decode(s)
    if not state.sio1 then return s, false end
    state.sio1.version = version
    return pb.encode('SaveState', state), true
end
local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

-- the peer: framed SIOPayload messages, data transfer only
local function sendBytes(str)
    for i = 1, #str do
        local body = '\x0a\x03\x0a\x01' .. str:sub(i, i)
        client:write(string.char(#body) .. body)
    end
end

local REGS = { 0x1054, 0x1058, 0x105a, 0x105c, 0x105e }
local NAMES = { [0x1054] = 'STAT', [0x1058] = 'MODE', [0x105a] = 'CTRL', [0x105c] = '5Ch', [0x105e] = 'BAUD' }

-- Reads the five registers, then pops `pops` bytes from DATA with STAT after each.
local function readback(pops)
    local a = assembler()
    local r = {}
    for _, reg in ipairs(REGS) do r[reg] = a.lhu(reg) end
    r.pops = {}
    for i = 1, pops do r.pops[i] = { data = a.lbu(0x1050), stat = a.lhu(0x1054) } end
    run(a)
    return r
end
local function collect(r)
    local v = {}
    for _, reg in ipairs(REGS) do v[reg] = slot(r[reg]) end
    v.pops = {}
    for i, p in ipairs(r.pops) do v.pops[i] = { data = slot(p.data), stat = slot(p.stat) } end
    return v
end

local MODE1, MODE2 = 0x004a, 0x004b -- 7-bit characters; reload factor 16, then 64
local CTRL = 0x0427 -- TXEN DTR RXEN RTS TXIRQEN

local frames = 0
local seq = {}
local at = 0
local function step(n, f)
    at = at + n
    seq[at] = f
end

local pre, preTime, saved, state, layout
local function compareRegs(code, label, got)
    for _, reg in ipairs(REGS) do
        if got[reg] ~= pre[reg] then
            fail(code, string.format('%s: %s reads %04x, %04x at save time', label, NAMES[reg], got[reg], pre[reg]))
            return false
        end
    end
    return true
end
local function checkFifo(code, label, got)
    for i, want in ipairs({ 0x41, 0x42, 0x43 }) do
        if got.pops[i].data ~= want then
            fail(code,
                 string.format('%s: RX byte %d reads %02x, %02x was in the FIFO at save time (FIFO replaced? merged?)',
                               label, i, got.pops[i].data, want))
            return false
        end
    end
    if bit.band(got.pops[2].stat, 2) == 0 then
        fail(code, label .. ': RXRDY clear with a byte still in the FIFO')
        return false
    end
    if bit.band(got.pops[3].stat, 2) ~= 0 then
        fail(code, label .. ': RXRDY still set after the three saved bytes: the FIFO has more than it had at save time')
        return false
    end
    return true
end
local function checkReset(code, label, got)
    local want = { [0x1054] = 0x0185, [0x1058] = 0, [0x105a] = 0, [0x105c] = 0, [0x105e] = 0 }
    for _, reg in ipairs(REGS) do
        if got[reg] ~= want[reg] then
            fail(code, string.format('%s: %s reads %04x, reset value %04x', label, NAMES[reg], got[reg], want[reg]))
            return false
        end
    end
    if got.pops[1].data ~= 0 then
        fail(code, string.format('%s: DATA reads %02x, reset value 00', label, got.pops[1].data))
        return false
    end
    if ram[MARKER] ~= 0x11111111 then
        fail(code + 1, string.format('%s: the rest of the state did not load (marker %08x)', label, ram[MARKER]))
        return false
    end
    return true
end

-- connect early; the server is up once settings are loaded
step(20, function()
    client = luv.new_tcp()
    client:connect('127.0.0.1', PORT, function(err)
        if err then
            print('sstate-sio1: cannot connect to the SIO1 server: ' .. err)
            finish(2)
            return
        end
        client:read_start(function() end)
    end)
end)
step(10, function()
    local a = assembler()
    a.sh(0x105a, 0x0040) -- reset
    a.sh(0x1058, MODE1)
    a.sh(0x105e, 0x1234) -- timing computed here, with reload factor 16
    a.sh(0x1058, MODE2) -- factor 64 from now on, timing not recomputed
    a.sh(0x105c, 0x6996)
    a.sh(0x105a, CTRL)
    preTime = a.timeTx(CTRL + 0x10, 0x5a)
    run(a)
end)
step(4, function()
    preTime = slot(preTime)
    if preTime == 0 or preTime >= 0x200000 then
        fail(3, string.format('baseline TX interrupt never came (count %d)', preTime))
        return
    end
    sendBytes('ABC')
end)
step(4, function()
    ram[MARKER] = 0x11111111
    layout = readback(0)
end)
step(2, function()
    pre = collect(layout)
    if bit.band(pre[0x1054], 0x202) ~= 0x202 then
        fail(4, string.format('setup: STAT %04x, expected RXRDY and IRQ set before the save', pre[0x1054]))
        return
    end
    state = tostring(PCSX.createSaveState())
    PCSX.createRewindState()
end)
-- disturb everything: drain the FIFO, ack, new mode/baud/5Ch, one new RX byte
step(2, function()
    ram[MARKER] = 0x22222222
    local a = assembler()
    for i = 1, 3 do a.lbu(0x1050) end
    a.sh(0x105a, 0x0037)
    a.sh(0x1058, 0x004f)
    a.sh(0x105e, 0x0100)
    a.sh(0x105c, 0x0000)
    run(a)
end)
step(2, function() sendBytes('D') end)
step(4, function() PCSX.loadSaveState(toSlice(state)) end)
step(4, function() layout = readback(3) end)
step(2, function()
    local got = collect(layout)
    if not compareRegs(10, 'load', got) then return end
    if not checkFifo(11, 'load', got) then return end
    local a = assembler()
    layout = a.timeTx(CTRL + 0x10, 0x5a)
    run(a)
end)
step(4, function()
    local t = slot(layout)
    if math.abs(t - preTime) > preTime / 8 then
        fail(12, string.format('load: TX interrupt came after %d loop turns, %d at save time', t, preTime))
        return
    end
    -- disturb again, then rewind to the ring state captured with the save
    local a = assembler()
    a.sh(0x105a, 0x0037)
    a.sh(0x1058, 0x004f)
    a.sh(0x105e, 0x0100)
    a.sh(0x105c, 0x0000)
    run(a)
end)
step(2, function() sendBytes('E') end)
step(4, function()
    if not PCSX.rewindState() then
        fail(20, 'rewindState() refused')
        return
    end
end)
step(4, function() layout = readback(3) end)
step(2, function()
    local got = collect(layout)
    if not compareRegs(21, 'rewind', got) then return end
    if not checkFifo(22, 'rewind', got) then return end
    local a = assembler()
    layout = a.timeTx(CTRL + 0x10, 0x5a)
    run(a)
end)
step(4, function()
    local t = slot(layout)
    if math.abs(t - preTime) > preTime / 8 then
        fail(23, string.format('rewind: TX interrupt came after %d loop turns, %d at save time', t, preTime))
        return
    end
    local msg = decode(state).sio1
    if not msg then
        fail(30, 'the save state has no field 18')
        return
    end
    local v = msg.version
    if v ~= 1 then
        fail(31, 'field 18 version field 1 is ' .. tostring(v) .. ', expected 1')
        return
    end
    sendBytes('F')
end)
step(4, function()
    ram[MARKER] = 0x22222222 -- so the marker check can see the rest of the state load
    PCSX.loadSaveState(toSlice(stripSIO1(state)))
end)
step(4, function() layout = readback(1) end)
step(2, function()
    if not checkReset(40, 'older state', collect(layout)) then return end
    -- back to a disturbed SIO1 with a byte in the FIFO before the unknown-version load
    PCSX.loadSaveState(toSlice(state))
end)
step(4, function() sendBytes('G') end)
step(4, function()
    local s, found = setSIO1Version(state, 2)
    if not found then
        fail(50, 'the save state has no field 18')
        return
    end
    ram[MARKER] = 0x22222222
    PCSX.loadSaveState(toSlice(s))
end)
step(4, function() layout = readback(1) end)
step(2, function()
    if not checkReset(52, 'unknown version', collect(layout)) then return end
    print('sstate-sio1 PASS')
    finish(0)
end)

SIO1SaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    if done then return end
    frames = frames + 1
    local f = seq[frames]
    if f then f() end
end)
