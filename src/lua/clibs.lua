-- lualoader, R"EOF(--
--   Copyright (C) 2026 PCSX-Redux authors
--
-- _CLIBS adapter: makes ffi.load resolve out of the Lua registry instead of
-- dlopen, which is what Pixel's 2020 lj_clib.c CLIB_VIRTUAL patch does on
-- LuaJIT. luaffifb's ffi.load is a plain dlopen wrapper with no registry hook,
-- so all ten of Redux's ffi.load sites fail there - the first one reached is
-- zlibffi.lua:167, "could not load library z".
--
-- NO-OP ON LUAJIT. It installs only when _CLIBS is present AND ffi.load cannot
-- already see it, so the LuaJIT build keeps using the C path.
--
-- THREE THINGS THIS ENCODES, all measured 2026-09-02 in the standalone spike:
--
--  1. ffi.cast(<function-pointer type>, ptr) DEREFERENCES ptr in luaffifb
--     54ca8c5, and on wasm it yields a silent NULL rather than visible garbage.
--     It has to hop through an integer cdata first. That is the one line the
--     whole adapter exists for.
--  2. The address in _CLIBS is LIGHTUSERDATA. ffi.cast("uintptr_t", ...) takes
--     it; a plain Lua number does not convert and raises.
--  3. dlsym cannot substitute even though everything is statically linked here:
--     pcsxlua.cc has zero `extern "C"`, so the registered functions carry
--     mangled C++ names while _CLIBS keys them by the unmangled source name.
--
-- AND ONE THING THE SPIKE DID NOT HAVE TO SOLVE. There, M.load was handed the
-- cdef text explicitly. Redux's call sites are bare `local C = ffi.load 'z'`
-- with the declarations in an ffi.cdef call earlier in the same chunk, so the
-- adapter has to remember them: ffi.cdef is wrapped to accumulate its source,
-- and ffi.load builds the proxy from everything cdef'd so far. That is why this
-- must load BEFORE any of the ffi files, not merely before first use.
local ffi = require 'ffi'

local M = {}

-- Turn `RET name(ARGS);` into `typedef RET (*__clib_name)(ARGS);`.
-- Deliberately NOT a C parser: it finds the declarator name and wraps it, so
-- luaffifb's own cdef parser does all the type work. Anything it cannot
-- confidently recognise is reported, never silently skipped - a symbol quietly
-- missing from the proxy would surface later as a nil call in unrelated code.
local function declToTypedef(decl)
    -- name is the identifier immediately before the argument list's open paren
    local ret, name, args = decl:match('^%s*(.-)([%a_][%w_]*)%s*(%b())%s*$')
    if not (ret and name and args) then return nil end
    if ret:find('typedef') or ret == '' then return nil end
    return string.format('typedef %s(*__clib_%s)%s;', ret, name, args), name
end

-- Pull top-level function declarations out of a cdef block. Skips anything
-- inside braces, so struct/union/enum bodies cannot contribute false hits.
function M.declarations(cdefSrc)
    local out, order = {}, {}
    local depth = 0
    local buf = {}
    for i = 1, #cdefSrc do
        local ch = cdefSrc:sub(i, i)
        if ch == '{' then depth = depth + 1 end
        if ch == '}' then depth = depth - 1 end
        if ch == ';' and depth == 0 then
            local decl = table.concat(buf)
            buf = {}
            local td, name = declToTypedef(decl)
            if td then
                out[name] = td;
                order[#order + 1] = name
            end
        elseif ch ~= ';' then
            buf[#buf + 1] = ch
        else
            buf = {}
        end
    end
    return out, order
end

-- Build the proxy table for one _CLIBS library.
-- Returns proxy, report. `report` names every symbol that had an address but no
-- declaration and vice versa, because a half-bound library is the failure that
-- would otherwise show up hours later as a nil call.
function M.load(libname, cdefSrc)
    local typedefs, order = M.declarations(cdefSrc)

    local reg = debug.getregistry()
    local clibs = reg._CLIBS
    if not clibs then error('_CLIBS is not in the Lua registry', 2) end
    local syms = clibs[libname]
    if not syms then error('_CLIBS has no library named ' .. tostring(libname), 2) end

    -- LAZY, like LuaJIT's ffi.load: resolve on first access, not up front.
    -- Not an optimisation for its own sake - Redux's GL binding declares 980
    -- functions across four cdef files while a shader touches four of them, so
    -- eager cdef+cast would pay for 976 it never calls, and would also force a
    -- decision about the ~all-declared-but-unregistered symbols at load time.
    local report = { declared = #order, withAddress = 0, addressNoDeclaration = {} }
    for name in pairs(syms) do
        if typedefs[name] == nil then
            report.addressNoDeclaration[#report.addressNoDeclaration + 1] = name
        else
            report.withAddress = report.withAddress + 1
        end
    end

    local bound = {}
    local proxy = setmetatable({}, {
        __index = function(t, k)
            local hit = bound[k]
            if hit ~= nil then return hit end
            local td, addr = typedefs[k], syms[k]
            -- Loud on failure, and it says WHICH half is missing. Redux's Lua
            -- reaches these as `C.foo`, so a plain nil surfaces later as
            -- "attempt to call a nil value" a long way from the real cause.
            if td == nil then
                -- Not a function: on LuaJIT a clib namespace also exposes the
                -- ENUM CONSTANTS from the cdef block, and Redux uses that -
                -- zlibffi.lua reads C.Z_NO_FLUSH straight off the handle. Those
                -- live in the parser's constant table, not in _CLIBS, and reach
                -- us through the default namespace. No dlsym is involved, so
                -- this is unaffected by ffi.C's symbol lookup dying under -O.
                local okc, cv = pcall(function() return ffi.C[k] end)
                if okc and cv ~= nil then
                    rawset(t, k, cv)
                    return cv
                end
                error(string.format('_CLIBS[%q].%s: no declaration in the cdef block, and not a cdef\'d constant',
                                    libname, tostring(k)), 2)
            end
            if addr == nil then
                error(string.format('_CLIBS[%q].%s: declared but never registered from C', libname, tostring(k)), 2)
            end
            ffi.cdef(td)
            -- THE DOUBLE HOP. ffi.cast(fnptr, addr) alone dereferences addr.
            local fn = ffi.cast('__clib_' .. k, ffi.cast('uintptr_t', addr))
            bound[k] = fn
            rawset(t, k, fn)
            return fn
        end,
    })
    return proxy, report
end

-- ---- installation ------------------------------------------------------

function M.install()
    local reg = debug.getregistry()
    if not reg._CLIBS then return false, 'no _CLIBS in the registry' end
    -- If ffi.load already resolves a known virtual library, this is the LuaJIT
    -- build and the C path is doing the job. Probed, not assumed from `jit`.
    local ok = pcall(function() return ffi.load('PCSX') end)
    if ok then return false, 'ffi.load already resolves _CLIBS (LuaJIT)' end

    local cdefSoFar = {}
    local realCdef, realLoad = ffi.cdef, ffi.load

    ffi.cdef = function(src, ...)
        if type(src) == 'string' then cdefSoFar[#cdefSoFar + 1] = src end
        return realCdef(src, ...)
    end

    local cache = {}
    ffi.load = function(name, ...)
        if reg._CLIBS[name] ~= nil then
            if cache[name] then return cache[name] end
            local proxy = M.load(name, table.concat(cdefSoFar, '\n'))
            cache[name] = proxy
            return proxy
        end
        return realLoad(name, ...)
    end
    return true
end

-- Self-installing, and returns nothing: Redux's L.load leaves a chunk's return
-- values on the Lua stack, and the emulator expects it empty once setLua is done.
-- Both refusals from M.install are the expected outcome on LuaJIT, where
-- lj_clib.c resolves _CLIBS itself, so stay quiet there. Only a runtime without
-- that C path depends on the adapter, and only there is a refusal worth printing.
-- The wasm Lua registers an empty `jit` table too, so key on jit.version, which
-- only LuaJIT fills in.
local ok, why = M.install()
local luajit = type(rawget(_G, 'jit')) == 'table' and type(jit.version) == 'string'
if not ok and not luajit then
    print('_CLIBS adapter not installed: ' .. tostring(why))
end
-- )EOF"
