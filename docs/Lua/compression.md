# Compression formats

`PCSX.Compression` encodes and decodes LZSS-family formats that are described from Lua. There are no built-in formats: every codec comes from a description. The description is a grammar, because the variation between these formats is in the productions: how a stream terminates, how its window works, which byte fills a reference below the start of the output, and which token encodings are carved out as escapes.

`PCSX.Compression.schema(description)` validates the description and returns a codec object. It raises an error if the description is invalid.

```lua
local lz = PCSX.Compression.schema {
    name = 'example',
    control = { unit = 1, perGroup = 8, lsbFirst = true, oneMeans = 'literal' },
    window  = { base = 'relative', maxDist = 0x0fff, fillByte = 0, overlap = true },
    emit    = { ringSize = 4096 },
    productions = {
        -- fill: a length nibble of 0xf is the escape
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
```

## The description

The stream is a sequence of groups. Each group starts with a control unit, whose bits say, item by item, whether the next item is a literal byte (or unit) or a token.

- `name`: a string used in diagnostics.
- `control`:
    - `unit`: size of a control unit in bytes, 1 or 2. Only the first byte of a 2-byte unit is read. Default 1.
    - `perGroup`: how many items one unit describes, 1 to 8. Default 8.
    - `lsbFirst`: whether bit 0 is consumed first, otherwise bit 7. Default true.
    - `oneMeans`: `'literal'` (default) means a set bit is a literal, `'token'` means a set bit selects a production.
    - `reverseBits`: bit-reverse each control byte before use.
- `window`:
    - `base`: how a distance becomes a source position. `'relative'` (default) is the output position minus the distance. `'absolute'` is the distance minus `start`. `'windowEnd'` is the output position minus `maxDist`, minus 1, plus the distance, minus `start`, and requires `maxDist`.
    - `start`, `maxDist`, `distBias`: the window start, the window size, and a bias added to every decoded distance. Default 0.
    - `fillByte`: the byte a reference below the start of the output yields. Default 0.
    - `overlap`: whether a match may overlap its own output. Default true.
- `emit`: constraints for the encoder only, kept apart from what the decoder accepts:
    - `ringSize`: size of the runtime window ring, which caps the distances the encoder emits. 0 means no cap.
    - `allowNegative`: whether the encoder may reference before the start of the output.
    - `terminator`: whether the encoder appends the `stop` production.
- `productions`: up to 4. Each one is tried in order, and the first whose guards all pass wins, so a guarded production shadows part of the encoding space of an unguarded one after it. That is how these formats carve escapes out of a fixed token width. A production has:
    - `bytes`: how many bytes the token takes, 1 to 4, including the ones the guards read.
    - `guards`: up to 2 conditions, each `{ at = ..., eq = n }` or `{ at = ..., ne = n }`, tested against the raw value before any bias.
    - exactly one of `match = { len = field, dist = field }`, `fill = { run = field, value = field }`, or `stop = true`. A match copies `len` bytes from the source position. A fill writes `run` copies of `value`. A stop ends the stream, and its presence makes the stream run until that token, rather than until the output size.

A field is `{ at = term, bias = n }`, where `at` is either one term or a list of up to three, and the terms are or'ed together before the bias is added. A term is `{ byte, mask, shift }`: byte `byte` of the token, masked, then shifted left by `shift`, or right if it's negative. `shift` defaults to 0.

## The codec

- `lz:decode(input, outSize)` returns the plain string, a statistics table, and how many input bytes were consumed. `input` is a string or a `Slice`, and `outSize` is what the container says the payload decodes to; a schema with a `stop` production still takes it as a safety cap. It raises an error with the reason, such as running out of input or no production matching a token.
- `lz:encode(input[, lazy])` returns the compressed string. A lazy parse is used when `lazy` is true, a greedy one otherwise. The encoder only supports `'relative'` windows, and checks its candidate tokens against the decoder's own production selector, so it never emits an encoding a guard has stolen.
- `lz:probe(input, outSize)` returns `true` and a table `{ bytes, consumed, stats }` if the input plausibly decodes under the schema, or `false` and the reason. It doesn't raise errors, so it can be swept across a buffer looking for payloads.

The statistics table has `match`, `fill`, `literal` (token counts), `overlapping` (matches whose distance is shorter than their length), `negativeRefs` (references before the start of the output), and, when there was a token of the kind, `distMin`, `distMax`, `lenMin`, `lenMax`, `runMin`, `runMax`. They describe what the original encoder emitted, which is the best evidence of what the target's own decoder tolerates.
