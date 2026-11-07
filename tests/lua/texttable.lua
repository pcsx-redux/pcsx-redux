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

TestTextTable = {}

-- A made-up encoding: shifted letters, one two-byte entry that overlaps a
-- one-byte one, a two-byte character, and the usual control codes.
local tableText = '0F= \n' .. [[
# comment, then a blank line

10=A
11=B
12=C
1011=AB!
30=a
31=b
4D=é
8140=…
F7=\n
F8=<PAUSE>
FB07=<WAIT %d>
FB08=<COLOUR %x>
/FE=<BLOCK %d>
/FF=<END>
]]

local function tbl()
    return assert(PCSX.TextTable.parse(tableText))
end

local function hex(s)
    return (s:gsub('%x%x', function(h) return string.char(tonumber(h, 16)) end))
end

function TestTextTable:testDecodeLongestMatch()
    lu.assertEquals(tbl():decode(hex('10111012')), 'AB!AC')
end

function TestTextTable:testDecodeMultiByteText()
    lu.assertEquals(tbl():decode(hex('4D0F8140')), 'é …')
end

function TestTextTable:testUnknownByteRoundTrips()
    local t = tbl()
    lu.assertEquals(t:decode(hex('10EE11')), 'A<$EE>B')
    lu.assertEquals(t:encode('A<$EE>B'), hex('10EE11'))
end

function TestTextTable:testParameters()
    local t = tbl()
    lu.assertEquals(t:decode(hex('FB077810FB082A')), '<WAIT 120>A<COLOUR 2A>')
    lu.assertEquals(t:encode('<WAIT 120>A<COLOUR 2a>'), hex('FB077810FB082A'))
end

function TestTextTable:testParameterPastEndIsRaw()
    lu.assertEquals(tbl():decode(hex('10FB07')), 'A<$FB><$07>')
end

function TestTextTable:testParameterOutOfRange()
    local e, err = tbl():encode('<WAIT 300>')
    lu.assertNil(e)
    lu.assertStrContains(err, 'offset 0')
end

function TestTextTable:testBreakTokensGetTheirOwnLines()
    local t = tbl()
    local bytes = hex('FE0010F711FFFE01FF')
    local text = t:decode(bytes)
    lu.assertEquals(text, '<BLOCK 0>\nA\nB\n<END>\n<BLOCK 1>\n<END>\n')
    lu.assertEquals(t:encode(text), bytes)
end

function TestTextTable:testRealNewlinesBesideBreakTokens()
    local t = tbl()
    for _, bytes in ipairs { 'FE00F7FF', 'FFF7FE00', '10F7FF', 'F7F7FF', 'FF', 'FFF7' } do
        local b = hex(bytes)
        lu.assertEquals(t:encode(t:decode(b)), b, bytes)
    end
end

function TestTextTable:testEveryShortSequenceRoundTrips()
    local t = tbl()
    local alphabet = { '0F', '10', '11', '30', 'F7', 'F8', 'FE00', 'FF', 'EE', '8140' }
    local count = 0
    local function walk(prefix, depth)
        if depth == 0 then
            local b = hex(prefix)
            lu.assertEquals(t:encode(t:decode(b)), b, prefix)
            count = count + 1
            return
        end
        for _, a in ipairs(alphabet) do walk(prefix .. a, depth - 1) end
    end
    walk('', 4)
    lu.assertEquals(count, 10000)
end

function TestTextTable:testFirstEntryWinsOnSharedText()
    local t = assert(PCSX.TextTable.parse('20=x\n21=x\n'))
    lu.assertEquals(t:decode(hex('2021')), 'xx')
    lu.assertEquals(t:encode('xx'), hex('2020'))
end

function TestTextTable:testEncodeReportsUnmappedText()
    local e, err = tbl():encode('AB?')
    lu.assertNil(e)
    lu.assertStrContains(err, '"?"')
    lu.assertStrContains(err, 'offset 2')
end

function TestTextTable:testParseErrors()
    local cases = {
        { '10=A\n10=B\n', 'line 2: 10 already defined on line 1' },
        { '1=A\n', 'line 1: expected HEX=text' },
        { 'zz=A\n', 'line 1: expected HEX=text' },
        { 'FB=%d\n', 'line 1: FB must start with text' },
        { 'FB=\n', 'line 1: FB must start with text' },
    }
    for _, c in ipairs(cases) do
        local t, err = PCSX.TextTable.parse(c[1])
        lu.assertNil(t, c[1])
        lu.assertStrContains(err, c[2])
    end
end

function TestTextTable:testWindowsLineEndings()
    local t = assert(PCSX.TextTable.parse('10=A\r\n11=B\r\n'))
    lu.assertEquals(t:decode(hex('1011')), 'AB')
end

function TestTextTable:testUtf8ByteOrderMark()
    local t = assert(PCSX.TextTable.parse('\239\187\19110=A\n'))
    lu.assertEquals(t:decode(hex('10')), 'A')
end
