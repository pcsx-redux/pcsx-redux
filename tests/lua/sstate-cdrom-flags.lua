-- CDRom fields 50/51.
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

local function putvarint(v)
    local out = {}
    repeat
        local b = v % 128
        v = math.floor(v / 128)
        if v > 0 then b = b + 128 end
        out[#out + 1] = string.char(b)
    until v == 0
    return table.concat(out)
end

-- Returns { {number, wiretype, raw key, payload} } for one message.
local function fields(s)
    local ret, pos = {}, 1
    while pos <= #s do
        local start = pos
        local key
        key, pos = varint(s, pos)
        local num, wt = math.floor(key / 8), key % 8
        local payload
        if wt == 0 then
            local p = pos
            _, pos = varint(s, pos)
            payload = s:sub(p, pos - 1)
        elseif wt == 1 then
            payload = s:sub(pos, pos + 7)
            pos = pos + 8
        elseif wt == 2 then
            local len
            len, pos = varint(s, pos)
            payload = s:sub(pos, pos + len - 1)
            pos = pos + len
        elseif wt == 5 then
            payload = s:sub(pos, pos + 3)
            pos = pos + 4
        else
            error('wire type ' .. wt)
        end
        ret[#ret + 1] = { num, wt, s:sub(start, start + #putvarint(key) - 1), payload }
    end
    return ret
end

local function encode(list)
    local out = {}
    for _, f in ipairs(list) do
        out[#out + 1] = f[3]
        if f[2] == 2 then out[#out + 1] = putvarint(#f[4]) end
        out[#out + 1] = f[4]
    end
    return table.concat(out)
end

-- Rewrites the CDRom message: drops fields 50 and 51, then appends them set if asked.
local function withFlags(s, set)
    local top = fields(s)
    for _, f in ipairs(top) do
        if f[1] == 15 then
            local kept = {}
            for _, c in ipairs(fields(f[4])) do if c[1] ~= 50 and c[1] ~= 51 then kept[#kept + 1] = c end end
            if set then
                kept[#kept + 1] = { 50, 0, putvarint(50 * 8), '\1' }
                kept[#kept + 1] = { 51, 0, putvarint(51 * 8), '\1' }
            end
            f[4] = encode(kept)
        end
    end
    return encode(top)
end

-- The value of CDRom field n, 0 if absent.
local function flag(s, n)
    for _, f in ipairs(fields(s)) do
        if f[1] == 15 then for _, c in ipairs(fields(f[4])) do if c[1] == n then return (varint(c[4], 1)) end end end
    end
    return 0
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local frames = 0
local base
CDRomFlagsListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 60 then
        base = tostring(PCSX.createSaveState())
        PCSX.loadSaveState(toSlice(withFlags(base, true)))
    elseif frames == 61 then
        local s = tostring(PCSX.createSaveState())
        if flag(s, 50) ~= 1 then
            print('sectorReadSinceSeek lost across the save state')
            PCSX.quit(1)
            return
        end
        if flag(s, 51) ~= 1 then
            print('scexCountersReset lost across the save state')
            PCSX.quit(2)
            return
        end
        PCSX.loadSaveState(toSlice(withFlags(base, false)))
    elseif frames == 62 then
        local s = tostring(PCSX.createSaveState())
        if flag(s, 50) ~= 0 then
            print('sectorReadSinceSeek survived a state without it')
            PCSX.quit(3)
            return
        end
        if flag(s, 51) ~= 0 then
            print('scexCountersReset survived a state without it')
            PCSX.quit(4)
            return
        end
        PCSX.quit(0)
    end
end)
