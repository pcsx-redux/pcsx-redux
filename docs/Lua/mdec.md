# MDEC frames

## Introduction & Rationale

`PCSX.MDEC` converts between the compressed video frames a PlayStation game feeds to its MDEC and plain 24-bit RGB. Games that play STR movies keep each frame as a BS frame: the MDEC run-level stream, VLC-coded, with an 8-byte header in front. The API has two functions. `decodeBS` turns one BS frame into a picture, and `encodeBS` turns a picture into one BS frame. Together they are enough to pull a frame out of a movie, edit it, and put it back.

A BS frame is only the video payload. It is not the sector layout an STR movie is stored in on a disc, and `PCSX.MDEC` knows nothing about sectors, chunk headers or XA audio. Reading a frame's chunks off a disc and writing them back is the script's job, and the worked example below shows one way to do it.

`decodeBS` accepts BS versions 1, 2 and 3. Versions 1 and 2 store the DC of each block as a raw 10-bit value, and version 3 delta-codes it. Any other version number is refused. `encodeBS` always writes version 2.

Decoding uses the standard quantization and scale tables, and macroblocks are taken column-major, which is how retail STR movies store them.

## Decoding

```lua
PCSX.MDEC.decodeBS(bs, width, height)
```

`bs` is one BS frame, as a `LuaBuffer` or a string, with the 8-byte header at the start. Bytes after the end of the frame's run-level are ignored, so a frame concatenated from whole chunks can be passed as it is. `width` and `height` are the picture size in pixels, integers from 1 to 65535. They are rounded up to multiples of 16, which is the size the frame is actually coded at.

On success it returns three values:

- `rgb`: a `LuaBuffer` of `paddedW * paddedH * 3` bytes, three bytes (R, G, B) per pixel, rows packed with no stride.
- `paddedW`, `paddedH`: the rounded-up sizes, which are also the row length and row count of `rgb`.

It returns `nil` and a message when the stream does not decode as a frame of that size. The messages include:

- `shorter than a BS header`
- `not a BS stream: word 0 is not the MDEC decode command`
- `BS version out of range: expected 1, 2 or 3`
- `the stream's block count does not match the frame size`
- `the stream ends before the frame does`
- `a run goes past coefficient 63`

A frame decoded with a size other than the one it was encoded at fails on its block count, not with a wrong picture.

An argument of the wrong kind raises an error instead: `bs` that is neither a `LuaBuffer` nor a string (`decodeBS: input must be a LuaBuffer or a string`), or a `width` or `height` that is not an integer in 1..65535 (`decodeBS: width must be an integer in 1..65535`).

## Encoding

```lua
PCSX.MDEC.encodeBS(rgb, width, height, maxBytes)
```

`rgb` is `width * height * 3` bytes of 24-bit RGB, rows packed, as a `LuaBuffer` or a string. Extra bytes at the end are ignored. The sides do not need to be multiples of 16: the last row and the last column are repeated to pad the picture to the next multiple of 16.

The encoder tries q_scale values from 1 upward, and keeps the lowest one for which the frame meets two limits:

- It is at most `maxBytes` bytes long, header included. `maxBytes` is an integer; `nil` or 0 means no byte budget.
- Its run-level stays within 28672 halfwords, which is the default size of the buffer a psyqo `StrPlayer` holds. The run-level is counted as the BS header counts it, padded to a multiple of 64 halfwords.

A lower q_scale means a higher quality and a larger frame.

On success it returns two values: the BS version 2 frame in a `LuaBuffer`, and the q_scale it used. If no q_scale up to 63 meets the limits, it returns `nil` and a message. The messages are:

- `does not fit <maxBytes> bytes even at q_scale 63`
- `decodes to <n> run-level halfwords even at q_scale 63, more than rlHalfwords (28672): lower the size, or raise rlHalfwords for a player with a larger buffer`
- `decodes to <n> run-level halfwords even at q_scale 63, more than a BS header counts (131070): lower the size`

The run-level limit cannot be changed from Lua, and a larger `maxBytes` does not help with either of the last two: only a smaller picture decodes to less run-level.

One frame BS cannot hold at all: when the budget forces q_scale 63 and the last block is flat with a DC of -512, that block's first word is 0xfe00, which a BS reader takes for padding. `encodeBS` then returns `nil` and `the BS stream holds <n> of the frame's <m> blocks: ...`; a larger `maxBytes` avoids it.

An argument of the wrong kind raises an error: `encodeBS: input must be a LuaBuffer or a string`, `encodeBS: width must be an integer in 1..65535` (and the same for `height`), `encodeBS: maxBytes must be nil or an integer in 0..4294967295`, or `encodeBS: input too small, needs <n> bytes`.

## Worked example

This reads one frame of an STR movie from a disc image, changes a few pixels and writes the frame back. The sector layout below comes from the STR format, not from `PCSX.MDEC`.

An STR video sector is a Mode 2 Form 1 sector, so reading the movie through an `Iso` object in `'M2_FORM1'` mode (see the [File API](file-api.md)) gives 2048 bytes per sector, and writing in that mode recomputes each sector's EDC and ECC. The 2048 bytes start with a 32-byte chunk header, and the first fields of it are:

| Offset | Size | Meaning |
|--------|------|---------|
| 0 | u32 | `0x80010160`, the STR video sector marker |
| 4 | u16 | chunk index within the frame |
| 6 | u16 | chunk count of the frame |
| 8 | u32 | frame number |
| 12 | u32 | frame size in bytes, rounded up to 4 |
| 16 | u16 | picture width |
| 18 | u16 | picture height |

2016 bytes of frame data follow the header. A frame is the concatenation of the frame data of its chunks, in chunk order. XA audio sectors may be interleaved with the video ones, and are skipped here by checking the marker, the frame number and the chunk index.

```lua
local SECTOR, PAYLOAD = 2048, 2016

-- lba is the first sector of a frame's chunk 0, and count a number of sectors
-- that reaches past the frame's last chunk, with room for interleaved audio.
local iso = PCSX.openIso('movie.bin')
local f = iso:open(lba, count * SECTOR, 'M2_FORM1')

local function header(sector)
    local at = sector * SECTOR
    return f:readU32At(at), f:readU16At(at + 4), f:readU16At(at + 6), f:readU32At(at + 8),
           f:readU16At(at + 16), f:readU16At(at + 18)
end

local magic, first, chunks, frameNo, width, height = header(0)
assert(magic == 0x80010160 and first == 0, 'not the first chunk of a frame')

-- Gather the frame's chunks, and remember which sector each one sits in.
local where, parts = {}, {}
for sector = 0, count - 1 do
    local m, index, _, n = header(sector)
    if m == 0x80010160 and n == frameNo and index < chunks and not where[index] then
        where[index] = sector
        parts[index + 1] = tostring(f:readAt(PAYLOAD, sector * SECTOR + 32))
    end
end
assert(#parts == chunks, 'the frame is not all in the sectors read')
local frame = table.concat(parts)

local rgb, w, h = PCSX.MDEC.decodeBS(frame, width, height)
assert(rgb, w)

-- Paint a 32x32 red square in the top left corner.
for y = 0, 31 do
    for x = 0, 31 do
        local i = (y * w + x) * 3
        rgb.data[i], rgb.data[i + 1], rgb.data[i + 2] = 255, 0, 0
    end
end

-- The new frame has to fit the sectors the old one used.
local bs, qScale = PCSX.MDEC.encodeBS(rgb, w, h, chunks * PAYLOAD)
assert(bs, qScale)
print(('re-encoded at q_scale %d, %d of %d bytes'):format(qScale, #bs, chunks * PAYLOAD))

-- Zero padding after a BS frame is ignored by the decoder. Each chunk header
-- also carries the frame's size, rounded up to 4 bytes.
local padded = tostring(bs) .. string.rep('\0', chunks * PAYLOAD - #bs)
for index = 0, chunks - 1 do
    local at = where[index] * SECTOR
    f:writeU32At((#bs + 3) & ~3, at + 12)
    f:writeAt(padded:sub(index * PAYLOAD + 1, (index + 1) * PAYLOAD), at + 32)
end
```

The write patches the image in memory, and `iso:savePPF()` saves it as a PPF file.
