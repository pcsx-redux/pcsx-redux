# HTML builder

## Introduction

`PCSX.Html` builds HTML out of Lua tables, the way Baltisot's xmllib did. Any key of `PCSX.Html` is a tag constructor: `PCSX.Html.div`, `PCSX.Html.p`, `PCSX.Html.textarea`, and so on. Nothing is predeclared, so any tag name works. The constructors produce nodes, and `render` turns a node into a string. It is meant for pages served from the [web server](web-server.md), but nothing ties it to that.

```lua
local H = PCSX.Html
local rows = {}
for i = 1, 3 do rows[#rows + 1] = H.li(i) end
local page = H.html {
    H.head { H.title 'Strings' },
    H.body {
        H.h1 'Room 3613',
        H.p { class = 'note', 'Fits & wraps <ok>' },
        H.input { type = 'text', name = 'q', value = 'a "b"' },
        H.ul { rows },
    },
}
local text = H.render(page)
```

## Constructors

A constructor takes one argument:

- nothing, which makes an empty element: `H.div()` renders as `<div></div>`.
- a string, a number, or a node, which becomes the only child: `H.p 'hi'`, `H.li(3)`, `H.p(H.b 'x')`.
- a table, whose string keys are the attributes and whose array part holds the children.

Anything else is an error.

Tag names must start with a letter, `_` or `:`, followed by letters, digits, `_`, `:`, `.` or `-`. Indexing `PCSX.Html` with any other name is an error. Names are used as written: they are not lowercased.

Six keys are not tags: `raw`, `comment`, `escape`, `escapeAttr`, `isNode` and `render`, described below. `PCSX.Html` itself is read-only, and assigning to it is an error.

## Children

The array part of the table is walked in order with `ipairs`, so it stops at the first `nil`. Each entry is handled as follows:

- a string or a number is text. Numbers go through `tostring`. `&`, `<` and `>` are escaped; quotes are left alone in text.
- a node is inserted as is.
- a plain table that is not a node is spliced in place, recursively. This is how a list of nodes built in a loop goes into a parent: `H.ul { H.li 'first', items }`.
- `false` is skipped, which allows `cond and H.span 'x'` inline when `cond` is a boolean. A `nil` would end the list instead.
- anything else, including `true` and functions, is an error.

Escaping happens when the node is built, not when it is rendered.

## Attributes

String keys of the table are attributes. Their names follow the same rule as tag names, so a key such as `on"click` is an error. Non-string, non-number keys are an error too.

- `true` renders the attribute bare: `{ disabled = true }` gives ` disabled`.
- `false` leaves the attribute out.
- any other value goes through `tostring`, and `&`, `<`, `>`, `"` and `'` are escaped. The value is always written between double quotes.

Attributes are rendered sorted by name, so the output does not depend on table iteration order:

```lua
H.render(H.div { ['data-row'] = 3, id = 'r' })  -- <div data-row="3" id="r"></div>
```

## Void elements

`area`, `base`, `br`, `col`, `embed`, `hr`, `img`, `input`, `link`, `meta`, `source`, `track` and `wbr` are void elements. They render without a closing tag, and giving one of them children is an error when the node is built. The check is on the exact lowercase name.

```lua
H.render(H.br())  -- <br>
H.br { 'child' }  -- error
```

## Rendering

```lua
PCSX.Html.render(node)
```

Returns the node as a string. Passing anything that is not a node is an error. Rendering adds no whitespace and no indentation, so the contents of `<pre>` and `<textarea>` come out exactly as given. If the node is an `html` element, the output starts with `<!DOCTYPE html>` followed by a newline; any other node renders as a fragment.

## Helpers

```lua
PCSX.Html.raw(s)
```

Returns a node holding `tostring(s)` verbatim, with no escaping. Use it for markup you built yourself, or for the body of a `<script>` element, which text escaping would break.

```lua
PCSX.Html.comment(s)
```

Returns a node that renders as `<!--s-->`, with `s` passed through `tostring` and not escaped. A string containing `--` is an error.

```lua
PCSX.Html.escape(s)
PCSX.Html.escapeAttr(s)
```

The two escaping functions used internally, returning a string. `escape` replaces `&`, `<` and `>`. `escapeAttr` also replaces `"` with `&quot;` and `'` with `&#39;`.

```lua
PCSX.Html.isNode(v)
```

Returns `true` if `v` is a node made by a constructor, `raw` or `comment`, and `false` otherwise.
