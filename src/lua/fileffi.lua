-- lualoader, R"EOF(--
--   Copyright (C) 2022 PCSX-Redux authors
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
local C = ffi.load 'SUPPORT_FILE'

local function validateBuffer(buffer)
    if buffer:maxsize() < buffer.size then
        error('Invalid or corrupted LuaBuffer: claims size of ' .. buffer.size .. ' but actual size is ' ..
                  buffer:maxsize())
    end
    return buffer
end

local function read(self, ptr, size)
    if type(ptr) == 'number' and size == nil then
        size = ptr
        local buf = Support.NewLuaBuffer(size)
        size = Support.extra.safeFFI('File::read(C.readFileBuffer)', C.readFileBuffer, self._wrapper, buf)
        buf.size = size
        return validateBuffer(buf)
    elseif type(ptr) == 'cdata' and size == nil and ffi.typeof(ptr) == Support.File._LuaBuffer then
        return Support.extra.safeFFI('File::read(C.readFileBuffer)', C.readFileBuffer, self._wrapper,
                                     validateBuffer(ptr))
    elseif type(ptr) == 'userdata' then
        return Support._internal.readFileUserData(self._wrapper, ptr, size)
    elseif type(ptr) == 'table' and ptr._type == 'Slice' then
        return Support.extra.safeFFI('File::read(C.readFileToExistingSlice)', C.readFileToExistingSlice, self._wrapper,
                                     ptr._wrapper, size or ptr.size)
    end
    return Support.extra.safeFFI('File::read(C.readFileRawPtr)', C.readFileRawPtr, self._wrapper, ptr, size)
end

local function readToSlice(self, size)
    return Support.File._createSliceWrapper(Support.extra.safeFFI('File::read(C.readFileToSlice)', C.readFileToSlice,
                                                                  self._wrapper, size))
end

local function readAt(self, ptr, size, pos)
    if type(ptr) == 'number' and type(size) == 'number' and pos == nil then
        pos = size
        size = ptr
        local buf = Support.NewLuaBuffer(size)
        size = Support.extra.safeFFI('File::readAt(C.readFileAtBuffer)', C.readFileAtBuffer, self._wrapper, buf, pos)
        buf.size = size
        return validateBuffer(buf)
    elseif type(ptr) == 'cdata' and type(size) == 'number' and pos == nil and ffi.typeof(ptr) == Support.File._LuaBuffer then
        return Support.extra.safeFFI('File::readAt(C.readFileAtBuffer)', C.readFileAtBuffer, self._wrapper,
                                     validateBuffer(ptr), size)
    elseif type(ptr) == 'userdata' then
        return Support._internal.readFileUserDataAt(self._wrapper, ptr, size, pos)
    elseif type(ptr) == 'table' and ptr._type == 'Slice' then
        return Support.extra.safeFFI('File::readAt(C.readFileAtToExistingSlice)', C.readFileAtToExistingSlice,
                                     self._wrapper, ptr._wrapper, size or ptr.size, pos)
    end
    return Support.extra.safeFFI('File::readAt(C.readFileAtRawPtr)', C.readFileAtRawPtr, self._wrapper, ptr, size, pos)
end

local function readAtToSlice(self, size, pos)
    return Support.File._createSliceWrapper(Support.extra.safeFFI('File::readAt(C.readFileAtToSlice)',
                                                                  C.readFileAtToSlice, self._wrapper, size, pos))
end

-- Returns the whole file as a new Slice; tostring() it to get a Lua string. An empty file gives an empty Slice.
-- A seekable file is read from offset 0 to its end, and its read cursor does not move; when size() cannot tell
-- (a zReader opened without a size), it is read in chunks until a read comes back empty. A stream that cannot
-- seek is read from its read cursor until a read comes back empty, which consumes it; on a live stream such as
-- a uvFifo, that is only what has arrived so far. A failed file raises an error, as read() and size() do.
local function readWhole(self)
    if self:failed() then error('File::readWhole: the file is a failed file') end
    local seekable = self:seekable()
    if seekable then
        local known, size = pcall(self.size, self)
        if known then return self:readAtToSlice(size, 0) end
    end
    local out = Support.File.buffer()
    local pos = 0
    while true do
        local chunk
        if seekable then
            chunk = self:readAtToSlice(65536, pos)
        else
            chunk = self:readToSlice(65536)
        end
        local got = chunk.size
        if got == 0 then break end
        pos = pos + got
        out:writeMoveSlice(chunk)
    end
    return out:readAtToSlice(out:size(), 0)
end

local function write(self, data, size)
    if type(data) == 'cdata' and size == nil and ffi.typeof(data) == Support.File._LuaBuffer then
        return Support.extra.safeFFI('File::write(C.writeFileBuffer)', C.writeFileBuffer, self._wrapper,
                                     validateBuffer(data))
    elseif type(data) == 'userdata' and size == nil then
        return Support._internal.writeFileUserData(self._wrapper, data)
    elseif type(size) == 'number' then
        return Support.extra.safeFFI('File::write(C.writeFileRawPtr)', C.writeFileRawPtr, self._wrapper, data, size)
    end
    if type(data) ~= 'string' then data = tostring(data) end
    return Support.extra.safeFFI('File::write(C.writeFileRawPtr)', C.writeFileRawPtr, self._wrapper, data,
                                 string.len(data))
end

local function writeAt(self, data, size, pos)
    if type(data) == 'cdata' and type(size) == 'number' and pos == nil and ffi.typeof(data) == Support.File._LuaBuffer then
        return Support.extra.safeFFI('File::writeAt(C.writeFileAtBuffer)', C.writeFileAtBuffer, self._wrapper,
                                     validateBuffer(data), size)
    elseif type(data) == 'userdata' and type(size) == 'number' and pos == nil then
        return Support._internal.writeFileUserDataAt(self._wrapper, data, size)
    elseif type(size) == 'number' and type(pos) == 'number' then
        return Support.extra.safeFFI('File::writeAt(C.writeFileAtRawPtr)', C.writeFileAtRawPtr, self._wrapper, data,
                                     size, pos)
    end
    if type(data) ~= 'string' then data = tostring(data) end
    return Support.extra.safeFFI('File::writeAt(C.writeFileAtRawPtr)', C.writeFileAtRawPtr, self._wrapper, data,
                                 string.len(data), size)
end

local function writeMoveSlice(self, slice) C.writeFileMoveSlice(self._wrapper, slice._wrapper) end

local function writeAtMoveSlice(self, slice, pos) C.writeFileAtMoveSlice(self._wrapper, slice._wrapper, pos) end

local function rSeek(self, pos, wheel)
    if wheel == nil then wheel = 'SEEK_SET' end
    return Support.extra.safeFFI('File::rSeek', C.rSeek, self._wrapper, pos, wheel)
end

local function wSeek(self, pos, wheel)
    if wheel == nil then wheel = 'SEEK_SET' end
    return Support.extra.safeFFI('File::wSeek', C.wSeek, self._wrapper, pos, wheel)
end

local function readNum(self, ctype, pos)
    local size = ffi.sizeof(ctype)
    local buf = ctype()
    if pos == nil then
        Support.extra.safeFFI('File::readX', C.readFileRawPtr, self._wrapper, ffi.cast('void*', buf), size)
    else
        Support.extra.safeFFI('File::readXAt', C.readFileAtRawPtr, self._wrapper, ffi.cast('void*', buf), size, pos)
    end
    if (ffi.abi('be')) and size ~= 1 then
        local n = ctype()
        local s = ffi.cast('uint8_t*', buf)
        local d = ffi.cast('uint8_t*', n)
        for i = 0, size - 1, 1 do d[i] = s[size - i - 1] end
        buf = n
    end
    return buf[0]
end

local function writeNum(self, num, ctype, pos)
    local size = ffi.sizeof(ctype)
    local buf = ctype()
    buf[0] = num
    if (ffi.abi('be')) and size ~= 1 then
        local n = ctype()
        local s = ffi.cast('uint8_t*', buf)
        local d = ffi.cast('uint8_t*', n)
        for i = 0, size - 1, 1 do d[i] = s[size - i - 1] end
        buf = n
    end
    if pos == nil then
        Support.extra.safeFFI('File::writeX', C.writeFileRawPtr, self._wrapper, ffi.cast('void*', buf), size)
    else
        Support.extra.safeFFI('File::writeXAt', C.writeFileAtRawPtr, self._wrapper, ffi.cast('void*', buf), size, pos)
    end
end

local function startCachingAndWait(self)
    local captures = {}
    captures.current = coroutine.running()
    if not captures.current then error(':startCachingAndWait() needs to be called from a coroutine') end
    captures.callback = function()
        PCSX.nextTick(function()
            captures.callback:free()
            coroutine.resume(captures.current)
        end)
    end
    captures.callback = ffi.cast('void (*)()', captures.callback)
    if C.startFileCachingWithCallback(self._wrapper, captures.callback) then
        coroutine.yield()
    else
        captures.callback:free()
    end
end

local uint8_t = ffi.typeof 'uint8_t[1]'
local uint16_t = ffi.typeof 'uint16_t[1]'
local uint32_t = ffi.typeof 'uint32_t[1]'
local uint64_t = ffi.typeof 'uint64_t[1]'
local int8_t = ffi.typeof 'int8_t[1]'
local int16_t = ffi.typeof 'int16_t[1]'
local int32_t = ffi.typeof 'int32_t[1]'
local int64_t = ffi.typeof 'int64_t[1]'

local function deleteFile(wrapper) Support.extra.safeFFI('File::~File', C.deleteFile, wrapper) end

local function createFileWrapper(wrapper)
    local file = {
        _wrapper = ffi.gc(wrapper, deleteFile),
        _type = 'File',
        close = function(self) Support.extra.safeFFI('File::close', C.closeFile, self._wrapper) end,
        read = read,
        readToSlice = readToSlice,
        readAt = readAt,
        readAtToSlice = readAtToSlice,
        readWhole = readWhole,
        gets = function(self) return Support._internal.readFileGets(self._wrapper) end,
        write = write,
        writeAt = writeAt,
        writeMoveSlice = writeMoveSlice,
        writeAtMoveSlice = writeAtMoveSlice,
        rSeek = rSeek,
        rTell = function(self) return Support.extra.safeFFI('File::rTell', C.rTell, self._wrapper) end,
        wSeek = wSeek,
        wTell = function(self) return Support.extra.safeFFI('File::wTell', C.wTell, self._wrapper) end,
        size = function(self) return tonumber(Support.extra.safeFFI('File::size', C.getFileSize, self._wrapper)) end,
        seekable = function(self) return Support.extra.safeFFI('File::seekable', C.isFileSeekable, self._wrapper) end,
        writable = function(self) return Support.extra.safeFFI('File::writable', C.isFileWritable, self._wrapper) end,
        eof = function(self) return Support.extra.safeFFI('File::eof', C.isFileEOF, self._wrapper) end,
        failed = function(self) return C.isFileFailed(self._wrapper) end,
        cacheable = function(self) return C.isFileCacheable(self._wrapper) end,
        caching = function(self) return C.isFileCaching(self._wrapper) end,
        cacheProgress = function(self) return C.fileCacheProgress(self._wrapper) end,
        startCaching = function(self) return C.startFileCaching(self._wrapper) end,
        startCachingAndWait = startCachingAndWait,
        dup = function(self) return createFileWrapper(C.dupFile(self._wrapper)) end,
        subFile = function(self, start, size)
            return createFileWrapper(C.subFile(self._wrapper, start or 0, size or -1))
        end,
        readU8 = function(self) return readNum(self, uint8_t) end,
        readU16 = function(self) return readNum(self, uint16_t) end,
        readU32 = function(self) return readNum(self, uint32_t) end,
        readU64 = function(self) return readNum(self, uint64_t) end,
        readI8 = function(self) return readNum(self, int8_t) end,
        readI16 = function(self) return readNum(self, int16_t) end,
        readI32 = function(self) return readNum(self, int32_t) end,
        readI64 = function(self) return readNum(self, int64_t) end,
        readU8At = function(self, pos) return readNum(self, uint8_t, pos) end,
        readU16At = function(self, pos) return readNum(self, uint16_t, pos) end,
        readU32At = function(self, pos) return readNum(self, uint32_t, pos) end,
        readU64At = function(self, pos) return readNum(self, uint64_t, pos) end,
        readI8At = function(self, pos) return readNum(self, int8_t, pos) end,
        readI16At = function(self, pos) return readNum(self, int16_t, pos) end,
        readI32At = function(self, pos) return readNum(self, int32_t, pos) end,
        readI64At = function(self, pos) return readNum(self, int64_t, pos) end,
        writeU8 = function(self, num) writeNum(self, num, uint8_t) end,
        writeU16 = function(self, num) writeNum(self, num, uint16_t) end,
        writeU32 = function(self, num) writeNum(self, num, uint32_t) end,
        writeU64 = function(self, num) writeNum(self, num, uint64_t) end,
        writeI8 = function(self, num) writeNum(self, num, int8_t) end,
        writeI16 = function(self, num) writeNum(self, num, int16_t) end,
        writeI32 = function(self, num) writeNum(self, num, int32_t) end,
        writeI64 = function(self, num) writeNum(self, num, int64_t) end,
        writeU8At = function(self, num, pos) writeNum(self, num, uint8_t, pos) end,
        writeU16At = function(self, num, pos) writeNum(self, num, uint16_t, pos) end,
        writeU32At = function(self, num, pos) writeNum(self, num, uint32_t, pos) end,
        writeU64At = function(self, num, pos) writeNum(self, num, uint64_t, pos) end,
        writeI8At = function(self, num, pos) writeNum(self, num, int8_t, pos) end,
        writeI16At = function(self, num, pos) writeNum(self, num, int16_t, pos) end,
        writeI32At = function(self, num, pos) writeNum(self, num, int32_t, pos) end,
        writeI64At = function(self, num, pos) writeNum(self, num, int64_t, pos) end,
    }
    return file
end

local function open(filename, t)
    if (t == nil) then t = 'READ' end
    if (t == 'DOWNLOAD_URL_AND_WAIT') then
        local captures = {}
        captures.current = coroutine.running()
        if not captures.current then
            error(':open() with DOWNLOAD_URL_AND_WAIT needs to be called from a coroutine')
        end
        captures.callback = function()
            PCSX.nextTick(function()
                captures.callback:free()
                coroutine.resume(captures.current)
            end)
        end
        captures.callback = ffi.cast('void (*)()', captures.callback)
        local ret = createFileWrapper(C.openFileWithCallback(filename, captures.callback))
        coroutine.yield()
        return ret
    else
        return createFileWrapper(C.openFile(filename, t))
    end
end

local function buffer(ptr, size, type)
    local f
    if ptr == nil and size == nil and type == nil then
        f = C.bufferFileEmpty()
    elseif type == nil or type == 'READWRITE' then
        f = C.bufferFile(ptr, size)
    elseif type == 'READ' then
        f = C.bufferFileReadOnly(ptr, size)
    elseif type == 'ACQUIRE' then
        f = C.bufferFileAcquire(ptr, size)
    end

    if f == nil then error('Invalid parameters to Support.File.buffer') end

    return createFileWrapper(f)
end

local function zReader(file, size, raw)
    if type(size) == 'string' then
        raw = size
        size = nil
    end
    raw = raw == 'RAW'
    if size == nil then size = -1 end
    return createFileWrapper(C.zReader(file._wrapper, size, raw))
end

local function uvFifo(address, port)
    if type(address) ~= 'string' then error('address must be a string') end
    if type(port) ~= 'number' then error('port must be a number') end
    local ret = createFileWrapper(C.uvFifo(address, port))
    ret.isConnecting = function(file) return C.uvFifoIsConnecting(file._wrapper) end
    return ret
end

local function mem4g()
    local ret = createFileWrapper(C.mem4g())
    ret.lowestAddress = function(file) return C.mem4gLowestAddress(file._wrapper) end
    ret.highestAddress = function(file) return C.mem4gHighestAddress(file._wrapper) end
    ret.actualSize = function(file) return C.mem4gActualSize(file._wrapper) end
    return ret
end

local function ffmpegAudioFile(file, options)
    if type(options) ~= 'table' then options = {} end
    local channels, endianness, sampleFormat, frequency = options.channels, options.endianness, options.sampleFormat,
                                                          options.frequency
    return createFileWrapper(Support.extra.safeFFI('Support.File.ffmpegAudioFile', C.ffmpegAudioFile, file._wrapper,
                                                   channels or 'Stereo', endianness or 'Little', sampleFormat or 'S16',
                                                   frequency or 44100))
end

if (type(Support) ~= 'table') then Support = {} end

Support.NewLuaBuffer = function(size)
    local buf = Support.File._LuaBuffer(size)
    buf.size = size
    return buf
end

Support.isLuaBuffer = function(obj) return ffi.istype('LuaBuffer', obj) end

Support.File = {
    open = open,
    buffer = buffer,
    zReader = zReader,
    uvFifo = uvFifo,
    mem4g = mem4g,
    failedFile = function() return createFileWrapper(C.failedFile()) end,
    ffmpegAudioFile = ffmpegAudioFile,
    _createFileWrapper = createFileWrapper,
}

-- )EOF"
