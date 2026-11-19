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

-- Files currently running through dofile / loadfile, per coroutine, each with the chunk that loaded it.
-- Relative names are resolved against the caller's own chunk; this stack is what lets the resolution
-- find the right file when the caller forwarded with a tail call and its frame is gone.
local mainThread = {}
local running = setmetatable({}, { __mode = 'k' })

local function runningStack()
    local co = coroutine.running() or mainThread
    local stack = running[co]
    if not stack then
        stack = {}
        running[co] = stack
    end
    return stack
end

local function current()
    local stack = runningStack()
    local top = stack[#stack]
    if top then return top.name, top.loader end
end

local function pack(...) return { n = select('#', ...), ... } end

local function runAs(func, name, loader, ...)
    local stack = runningStack()
    stack[#stack + 1] = { name = name, loader = loader }
    local ret = pack(pcall(func, ...))
    stack[#stack] = nil
    if not ret[1] then error(ret[2], 0) end
    return unpack(ret, 2, ret.n)
end

Support.extra = {

    loadfile = function(name)
        local file, actualName, loader = Support._internal.loadfile(name, current())
        local func, msg = loadstring(file, '@' .. actualName)
        if not func then return nil, msg end
        return function(...) return runAs(func, actualName, loader, ...) end
    end,

    dofile = function(name)
        local file, actualName, loader = Support._internal.loadfile(name, current())
        local func, msg = loadstring(file, '@' .. actualName)
        if func then return runAs(func, actualName, loader) end
        error(msg)
    end,

    open = function(name)
        return Support.File._createFileWrapper(ffi.cast('LuaFile*', Support._internal.open(name, current())))
    end,

    safeFFI = function(name, func, ...)
        local status, ret = pcall(func, ...)
        if status then return ret end
        error('FFI call failed in ' .. name .. ': ' .. ret)
    end,

    djbHash = function(str) return C.djbHash(str, #str) end,
}
-- )EOF"
