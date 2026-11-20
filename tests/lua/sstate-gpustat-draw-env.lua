local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
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

local function gpustat(s) return field(field(s, 5), 1) end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local function plant(words)
    local i = code
    ram[i] = 0x3c081f80
    i = i + 1 -- lui $t0, 0x1f80
    for _, w in ipairs(words) do
        ram[i] = 0x3c090000 + bit.rshift(w, 16)
        i = i + 1 -- lui $t1, hi
        ram[i] = 0x35290000 + bit.band(w, 0xffff)
        i = i + 1 -- ori $t1, $t1, lo
        ram[i] = 0xad091810
        i = i + 1 -- sw $t1, 0x1810($t0)
    end
    ram[i] = 0x1000ffff
    i = i + 1 -- b .
    ram[i] = 0
    i = i + 1
    PCSX.invalidateCache()
    PCSX.pauseEmulator()
    local timer = luv.new_timer()
    timer:start(1, 0, function()
        timer:close()
        PCSX.getRegisters().pc = 0x80100000
        PCSX.resumeEmulator()
    end)
end

local env = { 0xe10006b5, 0xe2012345, 0xe3002010, 0xe4040190, 0xe5001010, 0xe6000003 }
local rect = { 0x60ff0000, 0x00000000, 0x00100010 }
local probes = { { 20, 5 }, { 20, 10 }, { 40, 10 }, { 31, 17 } }

local function pixels()
    local vram = tostring(PCSX.GPU.getVRAM())
    local out = {}
    for i, p in ipairs(probes) do
        local o = (p[2] * 1024 + p[1]) * 2 + 1
        out[i] = vram:byte(o) + vram:byte(o + 1) * 256
    end
    return out
end

local frames = 0
local first, control
GpustatSaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        plant(env)
    elseif frames == 32 then
        first = tostring(PCSX.createSaveState())
        local stat = gpustat(first)
        if bit.band(stat, 0x9fff) ~= 0x1eb5 then
            print(string.format('GP0 writes did not land: GPUSTAT %08x', stat))
            PCSX.quit(1)
            return
        end
        plant(rect)
    elseif frames == 34 then
        control = pixels()
        if control[2] ~= 0xfc00 then
            print(string.format('control rectangle did not draw as expected: %04x', control[2]))
            PCSX.quit(2)
            return
        end
        PCSX.loadSaveState(toSlice(first))
    elseif frames == 36 then
        local second = tostring(PCSX.createSaveState())
        local a, b = bit.band(gpustat(first), 0x9fff), bit.band(gpustat(second), 0x9fff)
        if a ~= b then
            print(string.format('GPUSTAT draw-mode bits changed across save/load: %04x -> %04x', a, b))
            PCSX.quit(3)
            return
        end
        plant(rect)
    elseif frames == 38 then
        local after = pixels()
        for i, p in ipairs(probes) do
            if after[i] ~= control[i] then
                print(string.format('drawing environment lost across save/load: (%d,%d) %04x, expected %04x', p[1],
                                    p[2], after[i], control[i]))
                PCSX.quit(4)
                return
            end
        end
        PCSX.quit(0)
    end
end)
