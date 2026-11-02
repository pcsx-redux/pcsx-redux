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

TestCompression = {}

-- A synthetic LZSS: one flag byte per 8 items, a set bit selects a match.
-- A match is two bytes, 12 bits of distance and 4 bits of length:
--
--   DDDDDDDD LLLLDDDD    dist = b0 | (b1 & 0x0f) << 8, len = (b1 >> 4) + lenBias
--
-- Everything below is hand-assembled against this description, so a schema
-- builder that ignored its argument would not decode any of it.
local function makeScheme(lsbFirst, lenBias)
    return PCSX.Compression.schema {
        name = 'synthetic',
        control = { unit = 1, perGroup = 8, lsbFirst = lsbFirst, oneMeans = 'token' },
        window = { base = 'relative', maxDist = 0x0fff, overlap = true },
        productions = {
            { bytes = 2,
              match = { len = { at = { 1, 0xf0, -4 }, bias = lenBias },
                        dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } } } } },
        },
    }
end

local lsb = makeScheme(true, 2)

-- Two literals then a match of distance 2, length 6 (raw length 4): the match
-- reads bytes it is itself writing.
local overlapStream = '\x04ab\x02\x40'

function TestCompression:test_literal_only()
    local plain, stats, consumed = lsb:decode('\x00ABCDE', 5)
    lu.assertEquals(plain, 'ABCDE')
    lu.assertEquals(consumed, 6)
    lu.assertEquals(stats.literal, 5)
    lu.assertEquals(stats.match, 0)
end

function TestCompression:test_overlapping_back_reference()
    local plain, stats, consumed = lsb:decode(overlapStream, 8)
    lu.assertEquals(plain, 'abababab')
    lu.assertEquals(consumed, #overlapStream)
    lu.assertEquals(stats.match, 1)
    lu.assertEquals(stats.overlapping, 1)
    lu.assertEquals(stats.distMin, 2)
    lu.assertEquals(stats.lenMax, 6)
end

-- The encoder must find the same overlapping match and pack it the same way.
function TestCompression:test_encoder_produces_the_hand_built_stream()
    lu.assertEquals(lsb:encode('abababab'), overlapStream)
end

-- Same tokens, flag bits consumed from the top instead: the match bit moves
-- from 0x04 to 0x20, and the LSB-first stream no longer means the same thing.
function TestCompression:test_flag_bit_order_matters()
    local msb = makeScheme(false, 2)
    lu.assertEquals(msb:encode('abababab'), '\x20ab\x02\x40')
    lu.assertEquals(msb:decode('\x20ab\x02\x40', 8), 'abababab')
    -- Under MSB-first, 0x04 marks the sixth item as the match, so every item
    -- the stream actually holds is read as a literal and it runs out at four.
    lu.assertErrorMsgContains('decode: ran out of input', msb.decode, msb, overlapStream, 8)
end

-- Same bytes, length bias 3 instead of 2: raw length 4 is now a 7-byte copy.
function TestCompression:test_length_bias_matters()
    local biased = makeScheme(true, 3)
    lu.assertEquals(biased:decode(overlapStream, 9), 'ababababa')
    lu.assertEquals(biased:encode('ababababa'), overlapStream)
    -- and the 2-bias scheme needs a different length nibble for that copy
    lu.assertEquals(lsb:encode('ababababa'), '\x04ab\x02\x50')
end

function TestCompression:test_roundtrip()
    local plain = string.rep('the quick brown fox ', 40)
    local packed = lsb:encode(plain)
    lu.assertTrue(#packed < #plain)
    lu.assertEquals(lsb:decode(packed, #plain), plain)
end

-- Input that runs out early is an error wherever the cut lands: before a flag
-- group, before a literal, before a match token or inside one.
function TestCompression:test_truncated_input()
    for _, case in ipairs { { '\x00ABCDE', 5, { 0, 4 } }, { overlapStream, 8, { 3, 4 } } } do
        local stream, size, cuts = case[1], case[2], case[3]
        for _, cut in ipairs(cuts) do
            local truncated = stream:sub(1, cut)
            lu.assertErrorMsgContains('decode: ran out of input', lsb.decode, lsb, truncated, size)
            local ok, reason = lsb:probe(truncated, size)
            lu.assertFalse(ok)
            lu.assertStrContains(reason, 'ran out of input')
        end
    end
end

function TestCompression:test_schema_validation()
    lu.assertErrorMsgContains('needs `productions`', PCSX.Compression.schema, { name = 'x' })
    lu.assertErrorMsgContains('needs exactly one of', PCSX.Compression.schema,
        { name = 'x', productions = { { bytes = 2 } } })
    local match = { len = { at = { 1, 0xf0, -4 }, bias = 2 }, dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } } } }
    -- a term reading past its own token
    lu.assertErrorMsgContains('schema: invalid', PCSX.Compression.schema,
        { productions = { { bytes = 2, match = { len = { at = { 4, 0xff } }, dist = match.dist } } } })
    lu.assertErrorMsgContains('schema: invalid', PCSX.Compression.schema,
        { control = { unit = 3 }, productions = { { bytes = 2, match = match } } })
    lu.assertErrorMsgContains('schema: invalid', PCSX.Compression.schema,
        { window = { base = 'windowEnd' }, productions = { { bytes = 2, match = match } } })
end

function TestCompression:test_reference_at_output_position()
    -- a match with distance 0 would copy the byte it is about to write
    lu.assertErrorMsgContains('points at or past', lsb.decode, lsb, '\x01\x00\x00', 4)
end

function TestCompression:test_encoder_rejects_non_relative_windows()
    local abs = PCSX.Compression.schema {
        control = { oneMeans = 'token' },
        window = { base = 'absolute' },
        productions = { { bytes = 2, match = { len = { at = { 1, 0xf0, -4 }, bias = 2 },
                                               dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } } } } } },
    }
    lu.assertErrorMsgContains('only supports relative', abs.encode, abs, 'abababab')
end

function TestCompression:test_encoder_honours_no_overlap()
    local strict = PCSX.Compression.schema {
        control = { oneMeans = 'token' },
        window = { maxDist = 0x0fff, overlap = false },
        productions = { { bytes = 2, match = { len = { at = { 1, 0xf0, -4 }, bias = 2 },
                                               dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } } } } } },
    }
    local plain = string.rep('ab', 40)
    local plainOut, stats = strict:decode(strict:encode(plain), #plain)
    lu.assertEquals(plainOut, plain)
    lu.assertEquals(stats.overlapping, 0)
    lu.assertTrue(stats.match > 0)
end

function TestCompression:test_encoder_bounds_from_biases()
    -- no length bias: a zero-length match must not be emitted
    local nobias = PCSX.Compression.schema {
        control = { oneMeans = 'token' },
        window = { maxDist = 0x0fff },
        productions = { { bytes = 2, match = { len = { at = { 1, 0xf0, -4 } },
                                               dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } } } } } },
    }
    local plain = 'abcabcabcxyz'
    lu.assertEquals(nobias:decode(nobias:encode(plain), #plain), plain)
    -- distance bias 3: a distance-1 or -2 match cannot be packed
    local far = PCSX.Compression.schema {
        control = { oneMeans = 'token' },
        window = { maxDist = 0x0fff },
        productions = { { bytes = 2, match = { len = { at = { 1, 0xf0, -4 }, bias = 2 },
                                               dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } }, bias = 3 } } } },
    }
    plain = 'aaaaaaaaababababab'
    lu.assertEquals(far:decode(far:encode(plain), #plain), plain)
end

function TestCompression:test_sixteen_bit_literals_roundtrip()
    local wide = PCSX.Compression.schema {
        control = { unit = 2, oneMeans = 'token' },
        window = { maxDist = 0x0fff },
        productions = { { bytes = 2, match = { len = { at = { 1, 0xf0, -4 }, bias = 2 },
                                               dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } } } } } },
    }
    for _, plain in ipairs { 'ABCDEF', 'ABCDE', 'xyzzy-plugh-xyzzy-plugh' } do
        lu.assertEquals(wide:decode(wide:encode(plain), #plain), plain)
    end
end
