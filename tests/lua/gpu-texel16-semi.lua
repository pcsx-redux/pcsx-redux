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
-- drawTexel16(path) draws 16bpp texels onto a known background through the
-- GPU ports, writes the rows it touched to path, one line per row, and quits
-- with 0. The texture has one 8-texel band per value:
--   0x0000 transparent, never drawn
--   0x8000 black with bit 15 set, drawn, and blended when semi-transparent
--   0x7FFF white with bit 15 clear, drawn opaque even by a semi primitive
--   0xFFFF white with bit 15 set, blended by a semi primitive
--   0x001F, 0x801F red, opaque and blended, so the blend shows per channel
-- It is drawn as a semi-transparent raw rectangle, a semi-transparent raw
-- quad and an opaque raw rectangle, with ABR 0 (B/2 + F/2) over 0x4210.
-- The gtest runs this under each renderer and compares the files.
local ffi = require('ffi')
local bit = require('bit')

local CODE = 0x80100000
local GP0 = 0x1810
local GP1 = 0x1814

local TEXELS = { 0x0000, 0x8000, 0x7fff, 0xffff, 0x001f, 0x801f }
local BAND = 8
local TEX_W, TEX_H = #TEXELS * BAND, 8
local TEX_X = 640 -- texture page 10
local TPAGE = 10 + bit.lshift(0, 5) + bit.lshift(2, 7) -- 15bpp, ABR 0
local X = 16
local ROWS = { 16, 32, 48 } -- semi rectangle, semi quad, opaque rectangle

local function xy(x, y) return bit.bor(bit.lshift(y, 16), x) end

local function append(list, words) for _, w in ipairs(words) do list[#list + 1] = w end end

local function texelCommands()
    local words = {
        0xe1000000 + TPAGE + bit.lshift(1, 10), -- drawing to the display area allowed
        0xe2000000, 0xe3000000, 0xe4000000 + bit.lshift(511, 10) + 1023, 0xe5000000, 0xe6000000,
        0x02808080, xy(0, 0), xy(80, 64), -- fill with 0x4210
        0xa0000000, xy(TEX_X, 0), xy(TEX_W, TEX_H),
    }
    for _ = 1, TEX_H do
        for _, t in ipairs(TEXELS) do
            for _ = 1, BAND / 2 do words[#words + 1] = bit.bor(t, bit.lshift(t, 16)) end
        end
    end
    -- GP0(67h), semi-transparent raw rectangle.
    append(words, { 0x67808080, xy(X, ROWS[1]), 0, xy(TEX_W, TEX_H) })
    -- GP0(2Fh), semi-transparent raw quad, mapping the texture 1:1.
    local y = ROWS[2]
    append(words, {
        0x2f808080, xy(X, y), xy(0, 0), xy(X + TEX_W, y), bit.lshift(TPAGE, 16) + xy(TEX_W, 0), xy(X, y + TEX_H),
        xy(0, TEX_H), xy(X + TEX_W, y + TEX_H), xy(TEX_W, TEX_H),
    })
    -- GP0(65h), opaque raw rectangle.
    append(words, { 0x65808080, xy(X, ROWS[3]), 0, xy(TEX_W, TEX_H) })
    -- A fill away from the rows read ends the list, so the last batch is drawn by the
    -- renderer's own flush. Left to the flush getVRAM() does, called from startup Lua,
    -- the OpenGL renderer's last batch did not reach VRAM.
    append(words, { 0x02000000, xy(992, 496), xy(16, 1) })
    return words
end

local function fail(code, msg)
    print('gpu-texel16-semi: ' .. msg)
    PCSX.quit(code)
    error(msg)
end

local function call(code)
    PCSX.Assembler.New():parse(code):compileToMemory(PCSX.getMemPtr(), CODE, 0x80000000)
    local r = PCSX.callGuest { pc = CODE, cycles = 100000 }
    if r.status ~= 'returned' then fail(2, 'guest call ended with ' .. tostring(r.status)) end
end

-- Each word becomes a lui/ori/sw triple, so no branches have to be counted.
local function send(port, words)
    local lines = { '    lui   $t0, 0x1f80' }
    for _, w in ipairs(words) do
        lines[#lines + 1] = string.format('    lui   $t1, 0x%04x', bit.rshift(w, 16))
        lines[#lines + 1] = string.format('    ori   $t1, $t1, 0x%04x', bit.band(w, 0xffff))
        lines[#lines + 1] = string.format('    sw    $t1, 0x%04x($t0)', port)
    end
    lines[#lines + 1] = '    jr    $ra'
    lines[#lines + 1] = '    nop\n'
    call(table.concat(lines, '\n'))
end

function drawTexel16(path)
    -- A cold machine boots with the caches isolated; this BIU write is what makes stores land.
    call([[
    lui   $t0, 0x0001
    ori   $t0, $t0, 0xe988
    lui   $at, 0xfffe
    sw    $t0, 0x130($at)
    jr    $ra
    nop
]])
    send(GP1, { 0x00000000 })
    send(GP0, texelCommands())

    local vram = PCSX.GPU.getVRAM()
    local px = ffi.cast('const uint16_t*', vram.data)
    local out = {}
    for _, top in ipairs(ROWS) do
        for row = top - 1, top + TEX_H do
            local line = { string.format('y=%d', row) }
            for x = X - 4, X + TEX_W + 3 do line[#line + 1] = string.format('%04x', px[row * 1024 + x]) end
            out[#out + 1] = table.concat(line, ' ')
        end
    end
    local file = io.open(path, 'w')
    if not file then fail(3, 'unable to open ' .. tostring(path)) end
    file:write(table.concat(out, '\n') .. '\n')
    file:close()
    PCSX.quit(0)
end
