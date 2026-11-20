# Text tables

`PCSX.TextTable` converts between the bytes a game stores its strings in and readable text, using a table file in the layout romhacking tools have long used: one `HEX=text` entry per line. It is meant for dumping a game's script to text, editing it, and inserting it back.

`PCSX.TextTable.parse(source)` takes the table as a string and returns a table object, or `nil` and an error message naming the line.

```lua
local tbl = assert(PCSX.TextTable.parse(tableText))
local text = tbl:decode(bytes)
local bytes2 = assert(tbl:encode(text))
```

## The table file

```
# comment
41=A
8140=…
F7=\n
FB07=<WAIT %d>
/FF=<END>
```

- `HEX=text`: one or more bytes on the left, any UTF-8 text on the right. `\n` is a newline, `\\` a backslash.
- `%d` or `%x` in the text stands for one more byte after the entry's own bytes, written in decimal or as two hex digits. `%%` is a literal percent sign. The text has to start with literal text, not a parameter.
- A leading `/` marks a break token, such as the code that ends a message: `decode()` puts it on a line of its own, and `encode()` drops those newlines again.
- Blank lines and lines starting with `#` are ignored. Each byte sequence may only be defined once.

## Decoding and encoding

`tbl:decode(bytes)` takes a Lua string of bytes and returns the text. At each position it takes the entry with the longest matching bytes; parameter bytes do not count toward that, so `FB07=<WAIT %d>` wins over `FB=<OP %x>` on `FB 07`. A byte no entry covers becomes `<$XX>`, and so does an entry whose parameter bytes would run past the end of the input.

`tbl:encode(text)` returns the bytes as a Lua string, or `nil` and an error naming the offset of the first text it cannot encode. It also takes the longest match, `<$XX>` turns back into that byte, and when two entries share a text the first one in the table wins. Text produced by `decode()` encodes back to the same bytes as long as the table is unambiguous: no two entries share a text, and no entry's text can also be spelled by a run of other entries (`10=A`, `11=B` and `12=AB` would turn `10 11` into `12`). Unknown bytes never break this, so a table can be built up a few entries at a time while the script still round-trips.

## The Text viewer

The file viewers the ISO browser opens have a Text tab: it asks for a table file, decodes the file (or a byte range of it) into an editable text box, and shows whether the edited text encodes, and to how many bytes. The encoded bytes can be written to a file.

`PCSX.FileViewers.textViewer(openFile, opts)` builds the same viewer from Lua, for a script that knows where a game keeps its text. `openFile()` returns the file to decode. `opts` can carry `table` (a parsed table, so the viewer does not ask for one), `offset` and `length` (0 meaning to the end), and `onSave(bytes)`, which replaces the write-to-file field with an Apply button that hands the encoded bytes to the script, for example to put them back into the game's own archive.
