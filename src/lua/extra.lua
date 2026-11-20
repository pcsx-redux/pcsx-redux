-- lualoader, R"EOF(--
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
ffi.cdef [[
uint64_t djbHash(const char* str, size_t len);
]]

local C = ffi.load 'SUPPORT_EXTRA'

-- Relative names resolve next to the caller's own chunk, which the C side reads off the stack. A file
-- run by dofile that forwards with a tail call ("return Support.extra.dofile(...)") has no frame left
-- by then, and the frame the C side sees is the pcall in runAs. That shape is exact: only runAs calls
-- pcall directly, so when it shows up two levels above a wrapper, the running file is the base.
-- Functions returned by loadfile are the plain chunks, so they get the caller's frame only.
local mainThread = {}
local running = setmetatable({}, { __mode = 'k' })
local pcall = pcall
local getinfo = debug.getinfo

local function runningStack()
    local co = coroutine.running() or mainThread
    local stack = running[co]
    if not stack then
        stack = {}
        running[co] = stack
    end
    return stack
end

local runAs

-- Called from a wrapper: level 3 is the wrapper's caller, level 4 the one above it.
local function forwardedFrom()
    local caller = getinfo(3, 'f')
    if not caller or caller.func ~= pcall then return end
    local above = getinfo(4, 'f')
    if not above or above.func ~= runAs then return end
    local stack = runningStack()
    return stack[#stack]
end

local function pack(...) return { n = select('#', ...), ... } end

runAs = function(func, name)
    local stack = runningStack()
    stack[#stack + 1] = name
    local ret = pack(pcall(func))
    stack[#stack] = nil
    if not ret[1] then error(ret[2], 0) end
    return unpack(ret, 2, ret.n)
end

Support.extra = {

    loadfile = function(name)
        local file, actualName = Support._internal.loadfile(name, forwardedFrom())
        return loadstring(file, '@' .. actualName)
    end,

    dofile = function(name)
        local file, actualName = Support._internal.loadfile(name, forwardedFrom())
        local func, msg = loadstring(file, '@' .. actualName)
        if func then return runAs(func, actualName) end
        error(msg)
    end,

    open = function(name)
        return Support.File._createFileWrapper(ffi.cast('LuaFile*', Support._internal.open(name, forwardedFrom())))
    end,

    safeFFI = function(name, func, ...)
        local status, ret = pcall(func, ...)
        if status then return ret end
        error('FFI call failed in ' .. name .. ': ' .. ret)
    end,

    djbHash = function(str) return C.djbHash(str, #str) end,
}
-- )EOF"
