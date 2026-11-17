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

local function u32(v) return bit.tobit(v) % 4294967296 end

local function itype(op, rs, rt, imm) return u32(bit.bor(bit.lshift(op, 26), bit.lshift(rs, 21), bit.lshift(rt, 16), bit.band(imm, 0xffff))) end
local T0, T1, T2, T3, T4 = 8, 9, 10, 11, 12

local function assembler()
    local a = { pc = 0x100000 / 4, slots = 0 }
    function a.emit(w) ram[a.pc] = w a.pc = a.pc + 1 end
    function a.li(r, v)
        a.emit(itype(0x0f, 0, r, bit.rshift(v, 16)))
        a.emit(itype(0x0d, r, r, v))
    end
    function a.sh(reg, v)
        a.li(T1, v)
        a.emit(itype(0x29, T0, T1, reg))
    end
    function a.sb(reg, v)
        a.li(T1, v)
        a.emit(itype(0x28, T0, T1, reg))
    end
    local function store(op, reg)
        a.emit(itype(op, T0, T1, reg))
        a.emit(0)
        a.emit(itype(0x2b, T3, T1, a.slots * 4))
        a.slots = a.slots + 1
        return a.slots
    end
    function a.lhu(reg) return store(0x25, reg) end
    function a.lbu(reg) return store(0x24, reg) end
    -- Ack the IRQ flags, write one TX byte and count loop turns until I_STAT bit 8 rises.
    function a.timeTx(ctrlAck, data)
        a.emit(itype(0x23, T0, T1, 0x1074)) -- lw t1, I_MASK
        a.emit(0)
        a.emit(itype(0x0c, T1, T1, 0xfeff)) -- andi t1, ~0x100
        a.emit(itype(0x2b, T0, T1, 0x1074))
        a.sh(0x105a, ctrlAck)
        a.li(T1, 0xfffffeff)
        a.emit(itype(0x2b, T0, T1, 0x1070)) -- ack I_STAT bit 8
        a.li(T2, 0)
        a.li(T4, 0x200000)
        a.sb(0x1050, data)
        local loop = a.pc
        a.emit(itype(0x23, T0, T1, 0x1070)) -- lw t1, I_STAT
        a.emit(0)
        a.emit(itype(0x0c, T1, T1, 0x100))
        a.emit(itype(0x05, T1, 0, 4)) -- bne t1, zero, done
        a.emit(0)
        a.emit(itype(0x09, T2, T2, 1)) -- addiu t2, 1
        a.emit(itype(0x05, T2, T4, loop - (a.pc + 1))) -- bne t2, t4, loop
        a.emit(0)
        a.emit(itype(0x2b, T3, T2, a.slots * 4)) -- done: sw t2
        a.slots = a.slots + 1
        return a.slots
    end
    a.li(T0, 0x1f800000)
    a.li(T3, 0x80000000 + RESULTS)
    return a
end

local function run(a)
    a.emit(itype(0x04, 0, 0, -1)) -- b .
    a.emit(0)
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

local function fail(code, msg)
    print(string.format('sstate-sio1 FAIL %d: %s', code, msg))
    PCSX.quit(code)
end

-- protobuf helpers, as tests/lua/sstate-pad.lua
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
local SIO1FIELD = 18
local function stripSIO1(s)
    local out = {}
    for _, r in ipairs(records(s)) do
        if r.num ~= SIO1FIELD then out[#out + 1] = r.raw end
    end
    return table.concat(out)
end
local function setSIO1Version(s, version)
    local out = {}
    local found = false
    for _, r in ipairs(records(s)) do
        if r.num == SIO1FIELD then
            found = true
            local inner = { encodeVarint(8) .. encodeVarint(version) }
            for _, ir in ipairs(records(r.payload)) do
                if ir.num ~= 1 then inner[#inner + 1] = ir.raw end
            end
            local body = table.concat(inner)
            out[#out + 1] = encodeVarint(r.key) .. encodeVarint(#body) .. body
        else
            out[#out + 1] = r.raw
        end
    end
    return table.concat(out), found
end
local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

-- the peer: framed SIOPayload messages, data transfer only
local client
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
local function step(n, f) at = at + n seq[at] = f end

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
            fail(code, string.format('%s: RX byte %d reads %02x, %02x was in the FIFO at save time (FIFO replaced? merged?)',
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
            PCSX.quit(2)
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
    local msg = field(state, SIO1FIELD)
    if not msg then
        fail(30, 'the save state has no field 18')
        return
    end
    local v = field(msg, 1)
    if v ~= 1 then
        fail(31, 'field 18 version field 1 is ' .. tostring(v) .. ', expected 1')
        return
    end
    sendBytes('F')
end)
step(4, function() PCSX.loadSaveState(toSlice(stripSIO1(state))) end)
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
    PCSX.loadSaveState(toSlice(s))
end)
step(4, function() layout = readback(1) end)
step(2, function()
    if not checkReset(52, 'unknown version', collect(layout)) then return end
    print('sstate-sio1 PASS')
    PCSX.quit(0)
end)

SIO1SaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    local f = seq[frames]
    if f then f() end
end)
