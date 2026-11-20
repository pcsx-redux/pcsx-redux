local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())

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

local function field(s, wanted)
    local pos = 1
    while pos <= #s do
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
            pos = pos + 8
        elseif wt == 5 then
            pos = pos + 4
        else
            error('wire type ' .. wt)
        end
        if num == wanted then return payload end
    end
end

local function displayMode(s)
    local control = field(field(s, 5), 2)
    local b1, b2, b3, b4 = control:byte(8 * 4 + 1, 8 * 4 + 4)
    return b1 + b2 * 0x100 + b3 * 0x10000 + b4 * 0x1000000
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local mode = 0x08000027
local frames = 0
local first
DisplayModeSaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        local i = 0x100000 / 4
        ram[i] = 0x3c081f80 -- lui $t0, 0x1f80
        ram[i + 1] = 0x3c090800 -- lui $t1, 0x0800
        ram[i + 2] = 0x35290027 -- ori $t1, $t1, 0x0027
        ram[i + 3] = 0xad091814 -- sw $t1, 0x1814($t0)
        ram[i + 4] = 0x1000ffff -- b .
        ram[i + 5] = 0
        PCSX.invalidateCache()
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        timer:start(1, 0, function()
            timer:close()
            PCSX.getRegisters().pc = 0x80100000
            PCSX.resumeEmulator()
        end)
    elseif frames == 32 then
        first = tostring(PCSX.createSaveState())
        if displayMode(first) ~= mode then
            print(string.format('GP1(08h) write did not land: %08x', displayMode(first)))
            PCSX.quit(1)
            return
        end
        PCSX.loadSaveState(toSlice(first))
    elseif frames == 34 then
        local after = displayMode(tostring(PCSX.createSaveState()))
        if after ~= mode then
            print(string.format('display mode lost across save/load: %08x, expected %08x', after, mode))
            PCSX.quit(2)
            return
        end
        PCSX.quit(0)
    end
end)
