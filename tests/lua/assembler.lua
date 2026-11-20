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

TestAssembler = {}

local function words(text) return PCSX.Assembler.New():parse(text):compileToUint32Table(0x80100000) end

function TestAssembler:test_shift_by_immediate()
    lu.assertEquals(words('sll $t0, $t1, 3'), { 0x000940c0 })
    lu.assertEquals(words('srl $t0, 3'), { 0x000840c2 })
end

-- The variable shifts take the shift amount in rs.
function TestAssembler:test_shift_by_register()
    lu.assertEquals(words('sllv $t2, $t2, $t1'), { 0x012a5004 })
    lu.assertEquals(words('srlv $v0, $a0, $a1'), { 0x00a41006 })
    lu.assertEquals(words('srav $s0, $s1, $s2'), { 0x02518007 })
    lu.assertEquals(words('sllv $t0, $t1'), { 0x01284004 })
end
