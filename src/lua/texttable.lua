-- lualoader, R"EOF(--

--[[

Text tables: the byte <-> text mapping a game uses for its strings, in the
usual romhacking table-file layout, one entry per line.

    41=A            one byte, one string
    8140=           several bytes on the left, any UTF-8 on the right
    F7=\n           \n is a newline, \\ a backslash
    FB07=<WAIT %d>  each %d (decimal) or %x (two hex digits) consumes one
                    more byte after the entry's own bytes
    /FF=<END>       a leading / marks a break token: decode() puts it on a
                    line of its own, and encode() drops those newlines again
    # comment       blank lines and lines starting with # are ignored

    local tbl = assert(PCSX.TextTable.parse(tableText))
    local text = tbl:decode(bytes)
    local again = assert(tbl:encode(text))

Decoding takes the longest entry matching at each position; a byte no entry
covers becomes <$XX>, and encode() turns <$XX> back into that byte, so an
incomplete table still round-trips. Encoding also takes the longest match; when
two entries share a text, the first one in the table wins.

]]

local function unescape(s)
    return (s:gsub('\\(.)', function(c)
        if c == 'n' then return '\n' end
        return c
    end))
end

local function parseEntryText(text)
    local pieces = { '' }
    local params = {}
    local i = 1
    while i <= #text do
        local c = text:sub(i, i)
        local f = text:sub(i, i + 1)
        if f == '%d' or f == '%x' then
            params[#params + 1] = f:sub(2)
            pieces[#pieces + 1] = ''
            i = i + 2
        elseif f == '%%' then
            pieces[#pieces] = pieces[#pieces] .. '%'
            i = i + 2
        else
            pieces[#pieces] = pieces[#pieces] .. c
            i = i + 1
        end
    end
    return pieces, params
end

local methods = {}

local function formatParam(kind, v)
    if kind == 'd' then return tostring(v) end
    return string.format('%02X', v)
end

function methods:decode(bytes)
    local out = {}
    local i = 1
    local n = #bytes
    local prevBreak = false
    while i <= n do
        local entry
        for _, e in ipairs(self._byFirstByte[bytes:byte(i)] or {}) do
            if bytes:sub(i, i + #e.bytes - 1) == e.bytes and i + #e.bytes + #e.params - 1 <= n then
                entry = e
                break
            end
        end
        if not entry then
            out[#out + 1] = string.format('<$%02X>', bytes:byte(i))
            i = i + 1
            prevBreak = false
        else
            if entry.isBreak and #out > 0 and not prevBreak then out[#out + 1] = '\n' end
            local p = i + #entry.bytes
            out[#out + 1] = entry.pieces[1]
            for k, kind in ipairs(entry.params) do
                out[#out + 1] = formatParam(kind, bytes:byte(p))
                out[#out + 1] = entry.pieces[k + 1]
                p = p + 1
            end
            if entry.isBreak then out[#out + 1] = '\n' end
            prevBreak = entry.isBreak
            i = p
        end
    end
    return table.concat(out)
end

-- Returns the entry matching text at position p, the position after the
-- match, and the parameter bytes; nil when nothing matches.
local function matchAt(self, text, p)
    local raw = text:match('^<%$(%x%x)>', p)
    if raw then return nil, p + 5, string.char(tonumber(raw, 16)) end
    local best, bestEnd, bestParams
    for _, e in ipairs(self._byFirstChar[text:sub(p, p)] or {}) do
        local q = p
        local params = {}
        local ok = text:sub(q, q + #e.pieces[1] - 1) == e.pieces[1]
        q = q + #e.pieces[1]
        for k, kind in ipairs(e.params) do
            if not ok then break end
            local s = kind == 'd' and text:match('^%d%d?%d?', q) or text:match('^%x%x', q)
            local v = s and tonumber(s, kind == 'd' and 10 or 16)
            if not v or v > 255 then
                ok = false
            else
                params[k] = string.char(v)
                q = q + #s
                ok = text:sub(q, q + #e.pieces[k + 1] - 1) == e.pieces[k + 1]
                q = q + #e.pieces[k + 1]
            end
        end
        if ok and q > p and (not best or q > bestEnd) then
            best, bestEnd, bestParams = e, q, table.concat(params)
        end
    end
    return best, bestEnd, bestParams
end

function methods:encode(text)
    local out = {}
    local p = 1
    local n = #text
    local prevBreak = false
    while p <= n do
        local nl = text:sub(p, p) == '\n'
        if nl and prevBreak then
            -- the newline decode() puts after a break token
            p = p + 1
            prevBreak = false
        elseif nl and p < n and (matchAt(self, text, p + 1) or {}).isBreak then
            -- the newline decode() puts before a break token
            p = p + 1
        else
            local e, q, params = matchAt(self, text, p)
            if not q then
                return nil, string.format('no table entry for %q at offset %d', text:sub(p, p), p - 1)
            end
            if e then out[#out + 1] = e.bytes end
            out[#out + 1] = params
            prevBreak = e ~= nil and e.isBreak
            p = q
        end
    end
    return table.concat(out)
end

local meta = { __index = methods }

PCSX.TextTable = {
    parse = function(src)
        src = src:gsub('^\239\187\191', '')
        local self = { _byFirstByte = {}, _byFirstChar = {}, entries = {} }
        local seen = {}
        local lineNo = 0
        for line in (src .. '\n'):gmatch('([^\n]*)\n') do
            lineNo = lineNo + 1
            line = line:gsub('\r$', '')
            if line ~= '' and line:sub(1, 1) ~= '#' then
                local isBreak = line:sub(1, 1) == '/'
                if isBreak then line = line:sub(2) end
                local hex, text = line:match('^(%x+)=(.*)$')
                if not hex or #hex % 2 ~= 0 then
                    return nil, string.format('line %d: expected HEX=text', lineNo)
                end
                hex = hex:upper()
                if seen[hex] then
                    return nil, string.format('line %d: %s already defined on line %d', lineNo, hex, seen[hex])
                end
                seen[hex] = lineNo
                local pieces, params = parseEntryText(unescape(text))
                if pieces[1] == '' then
                    return nil, string.format('line %d: %s must start with text, not a parameter', lineNo, hex)
                end
                local bytes = hex:gsub('%x%x', function(h) return string.char(tonumber(h, 16)) end)
                local e = { bytes = bytes, pieces = pieces, params = params, isBreak = isBreak, line = lineNo }
                self.entries[#self.entries + 1] = e
                local b = self._byFirstByte[bytes:byte(1)] or {}
                self._byFirstByte[bytes:byte(1)] = b
                b[#b + 1] = e
                local c = self._byFirstChar[pieces[1]:sub(1, 1)] or {}
                self._byFirstChar[pieces[1]:sub(1, 1)] = c
                c[#c + 1] = e
            end
        end
        for _, list in pairs(self._byFirstByte) do
            table.sort(list, function(a, b)
                if #a.bytes ~= #b.bytes then return #a.bytes > #b.bytes end
                return a.line < b.line
            end)
        end
        return setmetatable(self, meta)
    end,
}

-- )EOF"
