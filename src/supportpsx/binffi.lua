-- lualoader, R"EOF(--
-- MIT License
--
-- Copyright (c) 2023 PCSX-Redux authors
--
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
--
-- The above copyright notice and this permission notice shall be included in all
-- copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
-- SOFTWARE.
local C = ffi.load 'SUPPORTPSX_BINARY'

ffi.cdef [[

enum Region { UNKNOWN, NTSC, PAL };

struct BinaryLoaderInfo {
    enum Region region;
    uint32_t pc;
    uint32_t sp;
    uint32_t gp;
};

struct PS1PackerOptions {
    uint32_t tload;
    bool shell;
    bool nokernel;
    bool resetstack;
    bool nopad;
    bool booty;
    bool raw;
    bool rom;
    bool cpe;
};

bool binaryLoaderLoad(LuaFile* src, LuaFile* dest, struct BinaryLoaderInfo* info);
void ps1PackerPack(LuaFile* src, LuaFile* dest, uint32_t addr, uint32_t pc, uint32_t gp, uint32_t sp,
          struct PS1PackerOptions options);
uint32_t uclWrapper(const uint8_t* in, uint32_t size, uint8_t* out);
uint32_t uclUnpackWrapper(const uint8_t* in, uint32_t inSize, uint8_t* out, uint32_t expectedOutSize);
uint32_t uclGetOverlapMargin(const uint8_t* src, size_t srcLen, size_t expectedDstLen);
uint32_t writeUclDecomp(LuaFile* dest);

]]

PCSX.Binary = {}

PCSX.Binary.load = function(src, dest)
    if type(src) ~= 'table' or src._type ~= 'File' then error('Expected a File object as first argument') end
    if type(dest) ~= 'table' or dest._type ~= 'File' then error('Expected a File object as second argument') end
    local info = ffi.new('struct BinaryLoaderInfo')
    if not C.binaryLoaderLoad(src._wrapper, dest._wrapper, info) then return nil end
    local ret = {}
    if info.region == 'NTSC' then
        ret.region = 'NTSC'
    elseif info.region == 'PAL' then
        ret.region = 'PAL'
    end
    if info.pc ~= 0 then ret.pc = info.pc end
    if info.sp ~= 0 then ret.sp = info.sp end
    if info.gp ~= 0 then ret.gp = info.gp end
    return ret
end

PCSX.Binary.pack = function(src, dest, addr, pc, gp, sp, options)
    if type(src) ~= 'table' or src._type ~= 'File' then error('Expected a File object as first argument') end
    if type(dest) ~= 'table' or dest._type ~= 'File' then error('Expected a File object as second argument') end
    if type(addr) ~= 'number' then error('Expected a number as third argument') end
    if type(pc) ~= 'number' then error('Expected a number as fourth argument') end
    if gp == nil then gp = 0 end
    if type(gp) ~= 'number' then error('Expected a number as fifth argument') end
    if sp == nil then sp = 0 end
    if type(sp) ~= 'number' then error('Expected a number as sixth argument') end
    if options == nil then options = {} end
    if type(options) ~= 'table' then error('Expected a table as seventh argument') end
    local opts = ffi.new('struct PS1PackerOptions')
    opts.tload = options.tload and options.tload or 0
    opts.booty = options.booty and true or false
    opts.nokernel = options.nokernel and true or false
    opts.resetstack = options.resetstack and true or false
    opts.shell = options.shell and true or false
    opts.nopad = options.nopad and true or false
    opts.raw = options.raw and true or false
    opts.rom = options.rom and true or false
    opts.cpe = options.cpe and true or false
    C.ps1PackerPack(src._wrapper, dest._wrapper, addr, pc, gp, sp, opts)
end

PCSX.Binary.createExe = function(src, dest, addr, pc, gp, sp)
    if type(src) ~= 'table' or src._type ~= 'File' then error('Expected a File object as first argument') end
    if type(dest) ~= 'table' or dest._type ~= 'File' then error('Expected a File object as second argument') end
    if type(addr) ~= 'number' then error('Expected a number as third argument') end
    if type(pc) ~= 'number' then error('Expected a number as fourth argument') end
    if gp == nil then gp = 0 end
    if type(gp) ~= 'number' then error('Expected a number as fifth argument') end
    if sp == nil then sp = 0 end
    if type(sp) ~= 'number' then error('Expected a number as sixth argument') end

    local size = src:size()
    size = bit.band(size + 0x7ff, bit.bnot(0x7ff))

    dest:writeU32(0x582d5350)
    dest:writeU32(0x45584520)
    dest:writeU32(0)
    dest:writeU32(0)
    dest:writeU32(pc)
    dest:writeU32(gp)
    dest:writeU32(addr)
    dest:writeU32(size)
    dest:writeU32(0)
    dest:writeU32(0)
    dest:writeU32(0)
    dest:writeU32(0)
    dest:writeU32(sp)
    while dest:size() < 0x800 do dest:writeU8(0) end
    dest:writeMoveSlice(src:readWhole())
    while bit.band(dest:size(), 0x7ff) ~= 0 do dest:writeU8(0) end
end

PCSX.Binary.createCpe = function(src, dest, addr, pc)
    if type(src) ~= 'table' or src._type ~= 'File' then error('Expected a File object as first argument') end
    if type(dest) ~= 'table' or dest._type ~= 'File' then error('Expected a File object as second argument') end
    if type(addr) ~= 'number' then error('Expected a number as third argument') end
    if type(pc) ~= 'number' then error('Expected a number as fourth argument') end

    local size = src:size()

    dest:writeU32(0x01455043)
    dest:writeU16(0x0008)
    dest:writeU8(3)
    dest:writeU16(0x0090)
    dest:writeU32(pc)
    dest:writeU8(1)
    dest:writeU32(addr)
    dest:writeU32(size)
    dest:writeMoveSlice(src:readWhole())
    dest:writeU8(0)
end

if type(PCSX.Misc) ~= 'table' then PCSX.Misc = {} end

PCSX.Misc.uclPack = function(src, dest)
    local srcPtr
    local srcSize
    if type(src) == 'table' and src._type == 'File' then
        src = src:readWhole()
        srcPtr = ffi.cast('uint8_t*', src.data)
        srcSize = src.size
    elseif type(src) == 'string' then
        srcPtr = ffi.cast('uint8_t*', src)
        srcSize = #src
    elseif Support.isLuaBuffer(src) then
        srcPtr = ffi.cast('uint8_t*', src.data)
        srcSize = src.size
    elseif type(src) == 'table' and src._type == 'Slice' then
        srcPtr = src.data
        srcSize = src.size
    else
        error('Expected a File object, string, LuaBuffer, or Slice as first argument')
    end

    local bufferSize = srcSize * 1.2 + 2064

    local retIsDest = false
    local destPtr
    local destSlice
    if not dest then
        dest = Support.File.createEmptySlice()
        dest:resize(bufferSize)
        destPtr = dest.mutable
        retIsDest = true
    elseif type(dest) == 'table' and dest._type == 'File' then
        destSlice = Support.File.createEmptySlice()
        destSlice:resize(bufferSize)
        destPtr = destSlice.mutable
    elseif Support.isLuaBuffer(dest) then
        destPtr = ffi.cast('uint8_t*', dest.data)
        dest:resize(bufferSize)
    elseif type(dest) == 'table' and dest._type == 'Slice' then
        destPtr = dest.mutable
        dest:resize(bufferSize)
    else
        error('Expected a File object, string, LuaBuffer, or Slice as second argument')
    end

    local outSize = C.uclWrapper(srcPtr, srcSize, destPtr)

    if outSize == 0 then error('Fatal error during data compression.') end

    if type(dest) == 'table' and dest._type == 'File' then
        destSlice:resize(outSize)
        dest:writeMoveSlice(destSlice)
    else
        dest:resize(outSize)
    end

    return retIsDest and dest or outSize
end

PCSX.Misc.writeUclDecomp = function(dest)
    if type(dest) ~= 'table' or dest._type ~= 'File' then error('Expected a File object as first argument') end
    return C.writeUclDecomp(dest._wrapper)
end

-- Decompress an NRV2E-compressed payload. `decompressedSize` is the known final size
-- and must be supplied; the caller almost always has it from a container header.
-- `src` accepts File / string / LuaBuffer / Slice (same shapes as uclPack); `dest` is
-- optional and accepts File / LuaBuffer / Slice. If omitted, a fresh Slice is created
-- and returned; otherwise the destination is filled and the byte count is returned.
PCSX.Misc.uclUnpack = function(src, decompressedSize, dest)
    if type(decompressedSize) ~= 'number' then error('Expected the decompressed size as the second argument') end

    local srcPtr
    local srcSize
    if type(src) == 'table' and src._type == 'File' then
        src = src:readWhole()
        srcPtr = ffi.cast('uint8_t*', src.data)
        srcSize = src.size
    elseif type(src) == 'string' then
        srcPtr = ffi.cast('uint8_t*', src)
        srcSize = #src
    elseif Support.isLuaBuffer(src) then
        srcPtr = ffi.cast('uint8_t*', src.data)
        srcSize = src.size
    elseif type(src) == 'table' and src._type == 'Slice' then
        srcPtr = src.data
        srcSize = src.size
    else
        error('Expected a File object, string, LuaBuffer, or Slice as first argument')
    end

    local retIsDest = false
    local destPtr
    local destSlice
    if not dest then
        dest = Support.File.createEmptySlice()
        dest:resize(decompressedSize)
        destPtr = dest.mutable
        retIsDest = true
    elseif type(dest) == 'table' and dest._type == 'File' then
        destSlice = Support.File.createEmptySlice()
        destSlice:resize(decompressedSize)
        destPtr = destSlice.mutable
    elseif Support.isLuaBuffer(dest) then
        dest:resize(decompressedSize)
        destPtr = ffi.cast('uint8_t*', dest.data)
    elseif type(dest) == 'table' and dest._type == 'Slice' then
        dest:resize(decompressedSize)
        destPtr = dest.mutable
    else
        error('Expected a File object, LuaBuffer, or Slice as third argument')
    end

    local outSize = C.uclUnpackWrapper(srcPtr, srcSize, destPtr, decompressedSize)

    if outSize == 0 then error('UCL decompression failed (bad data or wrong expected size).') end
    if outSize ~= decompressedSize then
        error(string.format('UCL decompression produced %d bytes but %d were expected.', outSize, decompressedSize))
    end

    if type(dest) == 'table' and dest._type == 'File' then dest:writeMoveSlice(destSlice) end

    return retIsDest and dest or outSize
end

-- Returns the number of bytes of overlap that UCL may read past the end of a compressed buffer.
-- This is used to determine how much extra space to allocate when decompressing a buffer in-place.
-- The `src` argument is the compressed buffer (File / string / LuaBuffer / Slice),
-- and `decompressedSize` is the known final size of the decompressed data.
-- The return value is the number of bytes of overlap.
PCSX.Misc.uclGetOverlapMargin = function(src, decompressedSize)
    if type(decompressedSize) ~= 'number' then error('Expected the decompressed size as the second argument') end

    local srcPtr
    local srcSize
    if type(src) == 'table' and src._type == 'File' then
        src = src:readWhole()
        srcPtr = ffi.cast('uint8_t*', src.data)
        srcSize = src.size
    elseif type(src) == 'string' then
        srcPtr = ffi.cast('uint8_t*', src)
        srcSize = #src
    elseif Support.isLuaBuffer(src) then
        srcPtr = ffi.cast('uint8_t*', src.data)
        srcSize = src.size
    elseif type(src) == 'table' and src._type == 'Slice' then
        srcPtr = src.data
        srcSize = src.size
    else
        error('Expected a File object, string, LuaBuffer, or Slice as first argument')
    end

    return C.uclGetOverlapMargin(srcPtr, srcSize, decompressedSize)
end

-- )EOF"
