--   Copyright (C) 2024 PCSX-Redux authors
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

TestFile = {}

function TestFile:test_readAtToSlice()
    local buf = Support.File.buffer()
    buf:write('hello world')
    local slice = buf:readAtToSlice(5, 6)
    lu.assertEquals(slice.size, 5)
    lu.assertEquals(ffi.string(slice.data, slice.size), 'world')
end

function TestFile:test_buffer()
    local buf = Support.File.buffer()
    buf:write('hello')
    buf:write(' ')
    buf:write('world')
    lu.assertEquals(buf:size(), 11)
    local r = buf:read(buf:size())
    lu.assertEquals(tostring(r), 'hello world')
end

local pbBuffer = require 'pb.buffer'
local pbSlice = require 'pb.slice'

local function helloFile()
    local f = Support.File.buffer()
    f:write('hello world')
    f:rSeek(0)
    return f
end

function TestFile:test_readPBBuffer()
    local f = helloFile()
    local b = pbBuffer.new()
    lu.assertEquals(tonumber(f:read(b, 5)), 5)
    lu.assertEquals(b:result(), 'hello')
end

function TestFile:test_readAtPBBuffer()
    local f = helloFile()
    local b = pbBuffer.new()
    lu.assertEquals(tonumber(f:readAt(b, 5, 6)), 5)
    lu.assertEquals(b:result(), 'world')
end

function TestFile:test_writeAtPBSlice()
    local f = helloFile()
    lu.assertEquals(tonumber(f:writeAt(pbSlice.new('WORLD'), 6)), 5)
    lu.assertEquals(tostring(f:readAt(11, 0)), 'hello WORLD')
end

function TestFile:test_readAtLuaBuffer()
    local f = helloFile()
    local buf = Support.NewLuaBuffer(5)
    lu.assertEquals(tonumber(f:readAt(buf, 6)), 5)
    lu.assertEquals(tostring(buf), 'world')
end

function TestFile:test_writeLuaBuffer()
    local f = Support.File.buffer()
    local buf = Support.NewLuaBuffer(5)
    for i = 0, 4 do buf[i] = string.byte('hello', i + 1) end
    lu.assertEquals(tonumber(f:write(buf)), 5)
    lu.assertEquals(tonumber(f:writeAt(buf, 5)), 5)
    lu.assertEquals(tostring(f:readAt(10, 0)), 'hellohello')
end

function TestFile:test_readSliceWithoutSize()
    local f = helloFile()
    local slice = Support.File.createEmptySlice()
    slice:resize(5)
    lu.assertEquals(tonumber(f:read(slice)), 5)
    lu.assertEquals(tostring(slice), 'hello')
    slice = Support.File.createEmptySlice()
    slice:resize(5)
    lu.assertEquals(tonumber(f:readAt(slice, nil, 6)), 5)
    lu.assertEquals(tostring(slice), 'world')
end

function TestFile:test_downloadAndWait()
    -- Nothing listens on port 1, so the download fails fast; the coroutine still has to come back with a file.
    local f = Support.File.open('http://127.0.0.1:1/', 'DOWNLOAD_URL_AND_WAIT')
    lu.assertTrue(f:failed())
end

-- Reading past the end returns -1 from the file; every read form has to turn that into nothing read.
function TestFile:test_read_past_the_end_is_empty()
    local f = Support.File.buffer()
    lu.assertEquals(f:readAt(16, 0).size, 0)
    lu.assertEquals(f:read(16).size, 0)
    lu.assertEquals(f:readAtToSlice(16, 0).size, 0)
    lu.assertEquals(f:readToSlice(16).size, 0)
end

function TestFile:test_loading_an_empty_file_does_not_throw()
    local ok, err = pcall(PCSX.loadSaveState, Support.File.buffer())
    lu.assertTrue(ok, tostring(err))
end

function TestFile:test_readToSlice()
    local buf = Support.File.buffer()
    buf:write('hello world')
    local slice = buf:readToSlice(5)
    lu.assertEquals(slice.size, 5)
    lu.assertEquals(ffi.string(slice.data, slice.size), 'hello')
    slice = buf:readToSlice(16)
    lu.assertEquals(slice.size, 6)
    lu.assertEquals(ffi.string(slice.data, slice.size), ' world')
end

function TestFile:test_readWhole_seekable_ignores_the_cursor()
    local f = helloFile()
    f:rSeek(6)
    local slice = f:readWhole()
    lu.assertEquals(slice._type, 'Slice')
    lu.assertEquals(tostring(slice), 'hello world')
    lu.assertEquals(tonumber(f:rTell()), 6)
end

function TestFile:test_readWhole_empty_file()
    local slice = Support.File.buffer():readWhole()
    lu.assertEquals(slice._type, 'Slice')
    lu.assertEquals(slice.size, 0)
    lu.assertEquals(tostring(slice), '')
end

function TestFile:test_readWhole_failed_file_raises()
    local ok, err = pcall(function() return Support.File.failedFile():readWhole() end)
    lu.assertFalse(ok)
    lu.assertStrContains(tostring(err), 'readWhole')
end

-- A raw deflate stream holding one stored block of 'hello world'.
local function storedDeflate()
    local f = Support.File.buffer()
    f:write('\x01\x0b\x00\xf4\xffhello world')
    return f
end

function TestFile:test_readWhole_seekable_without_a_size()
    local z = Support.File.zReader(storedDeflate(), 'RAW')
    lu.assertTrue(z:seekable())
    lu.assertFalse(pcall(z.size, z))
    lu.assertEquals(tostring(z:readWhole()), 'hello world')
    lu.assertEquals(tonumber(z:rTell()), 0)
end

function TestFile:test_readWhole_seekable_with_a_size()
    local z = Support.File.zReader(storedDeflate(), 11, 'RAW')
    lu.assertEquals(tostring(z:readWhole()), 'hello world')
end

-- No File reachable from here is readable and non-seekable without a network peer, so this drives readWhole
-- through a stream that only answers the calls a stream answers.
function TestFile:test_readWhole_stream_reads_from_the_cursor_to_the_end()
    local text = string.rep('abc', 50000)
    local backing = Support.File.buffer()
    backing:write(text)
    local stream = {
        failed = function() return false end,
        seekable = function() return false end,
        readToSlice = function(_, size) return backing:readToSlice(size) end,
        readWhole = backing.readWhole,
    }
    lu.assertEquals(tostring(backing:readToSlice(3)), 'abc')
    local slice = stream:readWhole()
    lu.assertEquals(slice.size, #text - 3)
    lu.assertEquals(tostring(slice), text:sub(4))
    lu.assertEquals(stream:readWhole().size, 0)
end
