-- lualoader, R"EOF(--

--[[

Schema-driven LZSS-family codec, authored from Lua.

A format is described as a GRAMMAR, not as a mask/shift table, because the
variation between these formats is in the productions. Stream termination,
window semantics, the fill byte and guarded escapes all live IN the grammar; a
table has nowhere to put a stop condition, which is how the stop condition ends
up as a constant beside the table and the whole thing stops generalising.

    local lz = PCSX.Compression.schema {
        name = 'example',
        control = { unit = 1, perGroup = 8, lsbFirst = true, oneMeans = 'literal' },
        window  = { base = 'relative', maxDist = 0x0fff, fillByte = 0, overlap = true },
        emit    = { ringSize = 4096 },
        productions = {
            -- fill: length nibble 0xF is the escape
            { bytes = 2,
              guards = { { at = { 1, 0xf0, -4 }, eq = 0x0f } },
              fill = { run = { at = { 1, 0x0f }, bias = 3 }, value = { at = { 0, 0xff } } } },
            -- match: 12-bit distance, 4-bit length
            { bytes = 2,
              match = { len = { at = { 1, 0xf0, -4 }, bias = 3 },
                        dist = { at = { { 0, 0xff, 0 }, { 1, 0x0f, 8 } } } } },
        },
    }

    local plain = lz:decode(compressed, sizeOut)
    local packed = lz:encode(plain)

There are no built-in formats: every format comes from a description like the
one above.

Productions are tried in order and the FIRST whose guards all pass wins, so a
guarded production shadows part of an unguarded one's encoding space. That is
not a wart to work around: it is how these formats carve escapes out of a fixed
token width, and the encoder handles it by running its candidate token back
through this same selector rather than by re-deriving which encodings are
still free.

]]

local C = ffi.load 'SUPPORT_COMPRESSION'

-- Order must match the switch in compressionffi.cc.
local SIZEOF = { 'CompressionTerm', 'CompressionField', 'CompressionGuard', 'CompressionProduction', 'CompressionSchema', 'CompressionDecodeStats' }
for i, name in ipairs(SIZEOF) do
    local theirs = tonumber(C.compressionSizeof(i - 1))
    local ours = tonumber(ffi.sizeof(name))
    if theirs ~= ours then
        error(string.format(
            'compressionffi: %s is %d bytes in C and %d in the cdef - the struct changed on one side only',
            name, theirs, ours))
    end
end

local FIELD = { len = 'COMPRESSION_F_LEN', dist = 'COMPRESSION_F_DIST', run = 'COMPRESSION_F_RUN', value = 'COMPRESSION_F_VAL' }
local BASE = { relative = 'COMPRESSION_OB_RELATIVE', windowEnd = 'COMPRESSION_OB_WINDOW_END', absolute = 'COMPRESSION_OB_ABSOLUTE' }

-- A field is up to three (byte, mask, shift) terms OR'd together, plus a bias
-- added afterwards. `at` is either one term or a list of them; a positive shift
-- is a left shift.
local function setField(dst, spec, what)
    if spec == nil then return end
    local at = spec.at
    if type(at) ~= 'table' then error(what .. ': needs an `at` term') end
    local terms = (type(at[1]) == 'table') and at or { at }
    if #terms > 3 then error(what .. ': at most 3 terms per field') end
    for i, t in ipairs(terms) do
        if type(t[1]) ~= 'number' or type(t[2]) ~= 'number' then
            error(what .. ': each term is { byte, mask [, shift] }')
        end
        dst.terms[i - 1].byte = t[1]
        dst.terms[i - 1].mask = t[2]
        dst.terms[i - 1].shift = t[3] or 0
    end
    dst.nterms = #terms
    dst.bias = spec.bias or 0
end

local function setProduction(dst, p, index)
    local what = string.format('production %d', index)
    dst.nbytes = p.bytes or error(what .. ': needs `bytes`')

    local guards = p.guards or {}
    if #guards > 2 then error(what .. ': at most 2 guards') end
    for i, g in ipairs(guards) do
        local slot = dst.guards[i - 1]
        setField(slot.f, g, what .. ' guard ' .. i)
        -- Guards test the RAW value, before any bias is applied.
        if g.eq ~= nil then
            slot.op = 'COMPRESSION_G_EQ'
            slot.value = g.eq
        elseif g.ne ~= nil then
            slot.op = 'COMPRESSION_G_NE'
            slot.value = g.ne
        else
            error(what .. ' guard ' .. i .. ': needs `eq` or `ne`')
        end
    end
    dst.nguards = #guards

    local kinds = 0
    if p.match then kinds = kinds + 1 end
    if p.fill then kinds = kinds + 1 end
    if p.stop then kinds = kinds + 1 end
    if kinds ~= 1 then error(what .. ': needs exactly one of `match`, `fill` or `stop`') end

    if p.stop then
        dst.emit = 'COMPRESSION_E_END'
        return
    end
    local body = p.match or p.fill
    dst.emit = p.match and 'COMPRESSION_E_MATCH' or 'COMPRESSION_E_FILL'
    for key, id in pairs(FIELD) do
        if body[key] then setField(dst.fields[ffi.C[id]], body[key], what .. ' ' .. key) end
    end
end

local function build(desc)
    local s = ffi.new 'CompressionSchema'
    -- Pin the name string: the struct holds a pointer to it, and a Lua string
    -- that goes out of scope takes the C-visible name with it.
    local name = desc.name or 'anonymous'
    local pinned = ffi.new('char[?]', #name + 1)
    ffi.copy(pinned, name)
    s.name = pinned

    local ctl = desc.control or {}
    s.flag_unit_bytes = ctl.unit or 1
    s.flags_per_group = ctl.perGroup or 8
    s.flag_lsb_first = (ctl.lsbFirst ~= false) and 1 or 0
    s.flag_one_means_match = (ctl.oneMeans == 'token') and 1 or 0
    s.flag_reverse_bits = ctl.reverseBits and 1 or 0

    local w = desc.window or {}
    s.offset_base = BASE[w.base or 'relative'] or error('unknown window base ' .. tostring(w.base))
    s.window_start = w.start or 0
    s.max_dist = w.maxDist or 0
    s.dist_bias = w.distBias or 0
    s.fill_byte = w.fillByte or 0
    s.allow_overlap = (w.overlap ~= false) and 1 or 0

    local e = desc.emit or {}
    s.ring_size = e.ringSize or 0
    s.emit_allow_negative = e.allowNegative and 1 or 0
    s.emit_terminator = e.terminator and 1 or 0

    local prods = desc.productions or error('a schema needs `productions`')
    if #prods > 4 then error('at most 4 productions') end
    for i, p in ipairs(prods) do setProduction(s.prods[i - 1], p, i) end
    s.nprods = #prods

    if C.compressionValidate(s) ~= 0 then
        error('schema: invalid: token bytes must be 1..4, every term inside its token, control unit 1 or 2, '
              .. '1..8 flags per group, and windowEnd needs maxDist')
    end

    return s, pinned
end

local function tobytes(v)
    if type(v) == 'string' then return v end
    if type(v) == 'table' and v._type == 'Slice' then return tostring(v) end
    return tostring(v)
end

local function statsToTable(st)
    return {
        match = tonumber(st.tokens_match),
        fill = tonumber(st.tokens_fill),
        literal = tonumber(st.tokens_literal),
        overlapping = tonumber(st.overlap_tokens),
        negativeRefs = tonumber(st.negative_refs),
        distMin = st.tokens_match > 0 and tonumber(st.dist_min) or nil,
        distMax = st.tokens_match > 0 and tonumber(st.dist_max) or nil,
        lenMin = st.tokens_match > 0 and tonumber(st.len_min) or nil,
        lenMax = st.tokens_match > 0 and tonumber(st.len_max) or nil,
        runMin = st.tokens_fill > 0 and tonumber(st.run_min) or nil,
        runMax = st.tokens_fill > 0 and tonumber(st.run_max) or nil,
    }
end

local ERRORS = {
    [-3] = 'ran out of input', [-4] = 'output buffer full',
    [-6] = 'no production matched a token', [-7] = 'encoder output buffer full',
    [-8] = 'a match points at or past the output position', [-9] = 'invalid schema',
    [-10] = 'the encoder only supports relative windows',
}

local methods = {}

-- Returns plaintext, stats, bytesConsumed. `outSize` is what the container says
-- the payload decodes to; a schema that terminates on a token still takes it as
-- a safety cap, so a stream missing its terminator cannot run away.
function methods:decode(input, outSize)
    local src = tobytes(input)
    if type(outSize) ~= 'number' or outSize <= 0 then error('decode: needs a positive outSize') end
    local dst = ffi.new('uint8_t[?]', outSize + 64)
    local st = ffi.new 'CompressionDecodeStats'
    C.compressionStatsInit(st)
    local consumed = ffi.new 'size_t[1]'
    local n = C.compressionDecode(self._schema, ffi.cast('const uint8_t*', src), #src, dst, outSize + 64, outSize,
                                  consumed, nil, st)
    if n < 0 then error('decode: ' .. (ERRORS[n] or ('error ' .. n))) end
    return ffi.string(dst, n), statsToTable(st), tonumber(consumed[0])
end

-- Returns the compressed bytes. `lazy` selects a lazy parse instead of greedy.
function methods:encode(input, lazy)
    local src = tobytes(input)
    local cap = #src * 2 + 64
    local dst = ffi.new('uint8_t[?]', cap)
    local n = C.compressionEncode(self._schema, ffi.cast('const uint8_t*', src), #src, dst, cap, lazy and 1 or 0)
    if n < 0 then error('encode: ' .. (ERRORS[n] or ('error ' .. n))) end
    return ffi.string(dst, n)
end

-- Does `input` plausibly decode under this schema? Returns false plus a reason
-- rather than raising, so it can be swept across a buffer looking for payloads.
function methods:probe(input, outSize)
    local ok, plain, stats, consumed = pcall(methods.decode, self, input, outSize)
    if not ok then return false, plain end
    if #plain ~= outSize then return false, 'produced ' .. #plain .. ' of ' .. outSize end
    return true, { bytes = #plain, consumed = consumed, stats = stats }
end

local meta = { __index = methods }

local function wrap(schema, pinned)
    return setmetatable({ _type = 'CompressionSchema', _schema = schema, _pinned = pinned }, meta)
end

PCSX.Compression = {
    schema = function(desc)
        if type(desc) ~= 'table' then error('schema: needs a description table') end
        return wrap(build(desc))
    end,
}

-- )EOF"
