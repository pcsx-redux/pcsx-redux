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

TestHtml = {}

local H = PCSX.Html

function TestHtml:test_document()
    local page = H.html { H.head { H.title 'T' }, H.body { H.p 'hi' } }
    lu.assertEquals(H.render(page),
        '<!DOCTYPE html>\n<html><head><title>T</title></head><body><p>hi</p></body></html>')
end

function TestHtml:test_fragment_and_empty()
    lu.assertEquals(H.render(H.div()), '<div></div>')
    lu.assertEquals(H.render(H.span ''), '<span></span>')
end

function TestHtml:test_text_is_escaped()
    lu.assertEquals(H.render(H.p 'a < b && c > "d"'), '<p>a &lt; b &amp;&amp; c &gt; "d"</p>')
    lu.assertEquals(H.render(H.p { '<script>alert(1)</script>' }),
        '<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>')
end

function TestHtml:test_attributes()
    local n = H.input { type = 'text', value = 'say "hi" & <go> \'x\'', disabled = true, hidden = false }
    lu.assertEquals(H.render(n),
        '<input disabled type="text" value="say &quot;hi&quot; &amp; &lt;go&gt; &#39;x&#39;">')
    lu.assertEquals(H.render(H.div { ['data-row'] = 3, id = 'r' }), '<div data-row="3" id="r"></div>')
end

function TestHtml:test_children_mix()
    local items = {}
    for i = 1, 3 do items[#items + 1] = H.li(i) end
    local n = H.ul { H.li 'first', items, false, H.li { 'x', 42 } }
    lu.assertEquals(H.render(n), '<ul><li>first</li><li>1</li><li>2</li><li>3</li><li>x42</li></ul>')
end

function TestHtml:test_raw_and_comment()
    lu.assertEquals(H.render(H.div { H.raw '<b>ok</b>', H.comment ' note ' }), '<div><b>ok</b><!-- note --></div>')
    lu.assertError(H.comment, 'a -- b')
end

function TestHtml:test_whitespace_preserved()
    lu.assertEquals(H.render(H.textarea 'line 1\n  line 2'), '<textarea>line 1\n  line 2</textarea>')
end

function TestHtml:test_voids()
    lu.assertEquals(H.render(H.br()), '<br>')
    lu.assertError(H.br, { 'child' })
end

function TestHtml:test_rejects_bad_names()
    lu.assertError(function() return H['bad tag'] end)
    lu.assertError(H.div, { ['on"click'] = 'x' })
    lu.assertError(H.div, function() end)
    lu.assertError(H.render, 'not a node')
end
