-- lualoader, R"EOF(--
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
ffi.cdef [[

typedef struct { char opaque[?]; } LuaMdecEncoded;

const char* mdecDecodeBS(const uint8_t* bs, uint32_t size, uint32_t width, uint32_t height, uint8_t* rgb);
LuaMdecEncoded* mdecEncodeBS(const uint8_t* rgb, uint32_t width, uint32_t height, uint32_t stride, uint32_t budget);
void mdecEncodedDestroy(LuaMdecEncoded* encoded);
const char* mdecEncodedError(const LuaMdecEncoded* encoded);
const uint8_t* mdecEncodedData(const LuaMdecEncoded* encoded);
uint32_t mdecEncodedSize(const LuaMdecEncoded* encoded);
int mdecEncodedQScale(const LuaMdecEncoded* encoded);

]]

local C = ffi.load 'CORE_MDEC'

-- A pointer to a LuaBuffer's or a string's bytes, and how many there are. The
-- caller keeps the original alive for the call.
local function input(data, name)
    if Support.isLuaBuffer(data) then
        return data.data, #data
    elseif type(data) == 'string' then
        return ffi.cast('const uint8_t*', data), #data
    end
    error(name .. ': input must be a LuaBuffer or a string')
end

local function dimension(v, name, what)
    if type(v) ~= 'number' or v < 1 or v > 0xffff or v ~= math.floor(v) then
        error(name .. ': ' .. what .. ' must be an integer in 1..65535')
    end
end

PCSX.MDEC = {
    -- Decodes one BS frame (versions 1 to 3, as STR movies carry them) with the
    -- standard tables, macroblocks column-major as retail STR has them. The frame
    -- decodes at its size rounded up to multiples of 16, which is what the
    -- picture holds: returns that many rows of width * 3 RGB bytes in a
    -- LuaBuffer, and the padded width and height. Returns nil and a message when
    -- the stream does not decode.
    decodeBS = function(bs, width, height)
        local ptr, size = input(bs, 'decodeBS')
        dimension(width, 'decodeBS', 'width')
        dimension(height, 'decodeBS', 'height')
        local w = bit.band(width + 15, bit.bnot(15))
        local h = bit.band(height + 15, bit.bnot(15))
        local rgb = Support.NewLuaBuffer(w * h * 3)
        local err = C.mdecDecodeBS(ptr, size, w, h, rgb.data)
        if err ~= nil then return nil, ffi.string(err) end
        return rgb, w, h
    end,
    -- Encodes width * height RGB bytes, rows packed, to one BS v2 frame at the
    -- lowest q_scale whose frame is at most maxBytes and that a player holding
    -- 28672 run-level halfwords decodes. maxBytes nil or 0 means no byte budget.
    -- Sides need not be multiples of 16: the last row and column repeat to pad.
    -- Returns the frame in a LuaBuffer and its q_scale, or nil and a message.
    encodeBS = function(rgb, width, height, maxBytes)
        local ptr, size = input(rgb, 'encodeBS')
        dimension(width, 'encodeBS', 'width')
        dimension(height, 'encodeBS', 'height')
        if maxBytes == nil then maxBytes = 0 end
        if type(maxBytes) ~= 'number' or maxBytes < 0 or maxBytes > 0xffffffff then
            error('encodeBS: maxBytes must be nil or a number in 0..4294967295')
        end
        if size < width * height * 3 then
            error('encodeBS: input too small, needs ' .. width * height * 3 .. ' bytes')
        end
        local encoded = C.mdecEncodeBS(ptr, width, height, width * 3, maxBytes)
        local err = C.mdecEncodedError(encoded)
        if err ~= nil then
            err = ffi.string(err)
            C.mdecEncodedDestroy(encoded)
            return nil, err
        end
        local bytes = C.mdecEncodedSize(encoded)
        local bs = Support.NewLuaBuffer(bytes)
        ffi.copy(bs.data, C.mdecEncodedData(encoded), bytes)
        local qScale = C.mdecEncodedQScale(encoded)
        C.mdecEncodedDestroy(encoded)
        return bs, qScale
    end,
}

-- )EOF"
