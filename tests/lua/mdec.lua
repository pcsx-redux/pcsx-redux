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
local lu = require 'luaunit'
local ffi = require 'ffi'

TestMdec = {}

-- A smooth 24-bit RGB gradient, rows packed, as a Lua string.
local function gradient(w, h)
    local t = {}
    for y = 0, h - 1 do
        for x = 0, w - 1 do
            t[#t + 1] = string.char(math.floor(x * 255 / (w - 1)), math.floor(y * 255 / (h - 1)),
                                    math.floor((x + y) * 255 / (w + h - 2)))
        end
    end
    return table.concat(t)
end

-- Detail the encoder has to spend bytes on.
local function busy(w, h)
    local buf = Support.NewLuaBuffer(w * h * 3)
    local x = 12345
    for i = 0, w * h * 3 - 1 do
        x = (x * 1103515245 + 12345) % 2147483648
        local p = math.floor(i / 3)
        buf.data[i] = (p % w) % 16 < 8 and math.floor(x / 65536) % 256 or (p % 251)
    end
    return buf
end

-- The largest per-byte difference between `rgb` (w x h, rows packed) and the
-- top left of `decoded` (rows of dw pixels).
local function maxError(rgb, w, h, decoded, dw)
    local src = ffi.cast('const uint8_t*', rgb)
    local worst = 0
    for y = 0, h - 1 do
        for x = 0, w * 3 - 1 do
            local d = math.abs(src[y * w * 3 + x] - decoded.data[y * dw * 3 + x])
            if d > worst then worst = d end
        end
    end
    return worst
end

function TestMdec:test_roundTripWithoutBudget()
    local rgb = gradient(64, 48)
    local bs, qScale = PCSX.MDEC.encodeBS(rgb, 64, 48)
    lu.assertNotNil(bs, qScale)
    lu.assertTrue(Support.isLuaBuffer(bs))
    lu.assertEquals(qScale, 1)
    -- BS header: run-level words, the MDEC decode command, q_scale, version 2.
    lu.assertEquals(bs.data[2] + bs.data[3] * 256, 0x3800)
    lu.assertEquals(bs.data[4] + bs.data[5] * 256, 1)
    lu.assertEquals(bs.data[6] + bs.data[7] * 256, 2)
    local decoded, w, h = PCSX.MDEC.decodeBS(bs, 64, 48)
    lu.assertNotNil(decoded, w)
    lu.assertEquals({ w, h, #decoded }, { 64, 48, 64 * 48 * 3 })
    local e = maxError(rgb, 64, 48, decoded, w)
    lu.assertTrue(e <= 12, 'max error ' .. e)
end

function TestMdec:test_stringAndLuaBufferInputsAgree()
    local rgb = gradient(64, 48)
    local buf = Support.NewLuaBuffer(#rgb)
    ffi.copy(buf.data, rgb, #rgb)
    local a = PCSX.MDEC.encodeBS(rgb, 64, 48)
    local b = PCSX.MDEC.encodeBS(buf, 64, 48)
    lu.assertEquals(tostring(a), tostring(b))
    local da = PCSX.MDEC.decodeBS(a, 64, 48)
    local db = PCSX.MDEC.decodeBS(tostring(b), 64, 48)
    lu.assertEquals(tostring(da), tostring(db))
end

function TestMdec:test_unpaddedSidesDecodePadded()
    local rgb = gradient(50, 30)
    local bs = PCSX.MDEC.encodeBS(rgb, 50, 30)
    lu.assertNotNil(bs)
    local decoded, w, h = PCSX.MDEC.decodeBS(bs, 50, 30)
    lu.assertNotNil(decoded, w)
    lu.assertEquals({ w, h, #decoded }, { 64, 32, 64 * 32 * 3 })
    local e = maxError(rgb, 50, 30, decoded, w)
    lu.assertTrue(e <= 12, 'max error ' .. e)
end

function TestMdec:test_budgetRaisesTheQScale()
    local rgb = busy(64, 48)
    local loose, looseQ = PCSX.MDEC.encodeBS(rgb, 64, 48, 0)
    lu.assertNotNil(loose, looseQ)
    lu.assertEquals(looseQ, 1)
    local budget = math.floor(#loose / 3)
    local bs, qScale = PCSX.MDEC.encodeBS(rgb, 64, 48, budget)
    lu.assertNotNil(bs, qScale)
    lu.assertTrue(#bs <= budget, #bs .. ' > ' .. budget)
    lu.assertTrue(qScale > 1)
    lu.assertEquals(bs.data[4] + bs.data[5] * 256, qScale)
    -- More room never costs quality.
    local roomier, roomierQ = PCSX.MDEC.encodeBS(rgb, 64, 48, budget * 2)
    lu.assertNotNil(roomier, roomierQ)
    lu.assertTrue(roomierQ <= qScale)
    lu.assertTrue(#roomier <= budget * 2)
    lu.assertNotNil(PCSX.MDEC.decodeBS(bs, 64, 48))
end

function TestMdec:test_impossibleBudgetIsAnError()
    local bs, err = PCSX.MDEC.encodeBS(busy(64, 48), 64, 48, 16)
    lu.assertNil(bs)
    lu.assertStrContains(err, 'q_scale 63')
end

function TestMdec:test_garbageDoesNotDecode()
    local bs, err = PCSX.MDEC.decodeBS(string.rep('\xa5', 600), 64, 48)
    lu.assertNil(bs)
    lu.assertStrContains(err, 'not a BS stream')
    bs, err = PCSX.MDEC.decodeBS('', 64, 48)
    lu.assertNil(bs)
    lu.assertStrContains(err, 'shorter than a BS header')
    -- A valid frame read at the wrong size.
    local good = PCSX.MDEC.encodeBS(gradient(64, 48), 64, 48)
    bs, err = PCSX.MDEC.decodeBS(good, 64, 64)
    lu.assertNil(bs)
    lu.assertStrContains(err, 'block count')
    -- Truncated.
    bs, err = PCSX.MDEC.decodeBS(tostring(good):sub(1, 40), 64, 48)
    lu.assertNil(bs)
    lu.assertNotNil(err)
end

function TestMdec:test_badArgumentsRaise()
    lu.assertErrorMsgContains('input must be', PCSX.MDEC.decodeBS, 42, 64, 48)
    lu.assertErrorMsgContains('width must be', PCSX.MDEC.encodeBS, '', 0, 48)
    lu.assertErrorMsgContains('input too small', PCSX.MDEC.encodeBS, 'abc', 64, 48)
end
