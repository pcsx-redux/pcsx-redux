-- lualoader, R"EOF(--

--[[

HTML builder: any key of PCSX.Html is a tag constructor, the way Baltisot's
xmllib worked. A constructor takes nothing, a string, or a table whose array
part holds the children and whose string keys are the attributes.

    local H = PCSX.Html
    local page = H.html {
        H.head { H.title 'Strings' },
        H.body {
            H.h1 'Room 3613',
            H.p { class = 'note', 'Fits & wraps <ok>' },
            H.input { type = 'text', name = 'q', value = 'a "b"' },
            H.ul { rows },  -- a plain table of nodes is spliced in place
        },
    }
    local text = H.render(page)

Text and attribute values are escaped; use H.raw(s) for markup you built
yourself (or a <script> body). An attribute set to true renders bare, false
leaves it out. false children are skipped, numbers are converted with
tostring. Void elements (br, img, input, meta, ...) have no closing tag.
Rendering adds no whitespace, so <pre> and <textarea> keep their contents.
Rendering an <html> node prepends <!DOCTYPE html>; any other node renders as
a fragment.

]]

local voids = {}
for name in ('area base br col embed hr img input link meta source track wbr'):gmatch('%S+') do
    voids[name] = true
end

local textEscapes = { ['&'] = '&amp;', ['<'] = '&lt;', ['>'] = '&gt;' }
local attrEscapes = { ['&'] = '&amp;', ['<'] = '&lt;', ['>'] = '&gt;', ['"'] = '&quot;', ["'"] = '&#39;' }

local function escape(s) return (tostring(s):gsub('[&<>]', textEscapes)) end
local function escapeAttr(s) return (tostring(s):gsub('[&<>"\']', attrEscapes)) end

local tagMeta = {}
local function isNode(v) return getmetatable(v) == tagMeta end

local function checkName(kind, name)
    if type(name) ~= 'string' or not name:match('^[%a_:][%w_:%.%-]*$') then
        error('PCSX.Html: invalid ' .. kind .. ' name ' .. tostring(name), 3)
    end
end

local function addChildren(nodes, list)
    for _, v in ipairs(list) do
        local t = type(v)
        if t == 'string' or t == 'number' then
            nodes[#nodes + 1] = escape(v)
        elseif isNode(v) then
            nodes[#nodes + 1] = v
        elseif t == 'table' then
            addChildren(nodes, v)
        elseif v ~= false then
            error('PCSX.Html: unsupported child of type ' .. t, 4)
        end
    end
end

local function makeTag(name, arg)
    local node = setmetatable({ tag = name, attrs = {}, nodes = {} }, tagMeta)
    if arg == nil then return node end
    local t = type(arg)
    if t == 'string' or t == 'number' or isNode(arg) then arg = { arg } end
    if type(arg) ~= 'table' then error('PCSX.Html: ' .. name .. ' takes a string or a table, not ' .. t, 3) end
    for k, v in pairs(arg) do
        if type(k) == 'string' then
            checkName('attribute', k)
            if v == true then
                node.attrs[k] = true
            elseif v ~= false then
                node.attrs[k] = escapeAttr(v)
            end
        elseif type(k) ~= 'number' then
            error('PCSX.Html: attribute keys must be strings', 3)
        end
    end
    addChildren(node.nodes, arg)
    if voids[name] and #node.nodes > 0 then error('PCSX.Html: <' .. name .. '> cannot have children', 3) end
    return node
end

local function renderNode(node, out)
    if node.raw then
        out[#out + 1] = node.raw
        return
    end
    if node.comment then
        out[#out + 1] = '<!--' .. node.comment .. '-->'
        return
    end
    out[#out + 1] = '<' .. node.tag
    local keys = {}
    for k in pairs(node.attrs) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do
        local v = node.attrs[k]
        out[#out + 1] = v == true and (' ' .. k) or (' ' .. k .. '="' .. v .. '"')
    end
    out[#out + 1] = '>'
    if voids[node.tag] then return end
    for _, child in ipairs(node.nodes) do
        if type(child) == 'string' then
            out[#out + 1] = child
        else
            renderNode(child, out)
        end
    end
    out[#out + 1] = '</' .. node.tag .. '>'
end

local builders = {
    raw = function(s) return setmetatable({ raw = tostring(s) }, tagMeta) end,
    comment = function(s)
        s = tostring(s)
        if s:find('--', 1, true) then error('PCSX.Html: a comment cannot contain --', 2) end
        return setmetatable({ comment = s }, tagMeta)
    end,
    escape = escape,
    escapeAttr = escapeAttr,
    isNode = isNode,
    render = function(node)
        if not isNode(node) then error('PCSX.Html.render takes a node', 2) end
        local out = {}
        if node.tag == 'html' then out[1] = '<!DOCTYPE html>\n' end
        renderNode(node, out)
        return table.concat(out)
    end,
}

PCSX.Html = setmetatable({}, {
    __index = function(_, name)
        if builders[name] then return builders[name] end
        checkName('tag', name)
        return function(arg) return makeTag(name, arg) end
    end,
    __newindex = function() error('PCSX.Html is read-only', 2) end,
})

-- )EOF"
