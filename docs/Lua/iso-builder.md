# Building ISO images

## Introduction & Rationale

The `Iso` object described in the [File API](file-api.md) page works on an existing disc image. It is not read-only, since writing to it is what drives the [PPF](ppf.md) patch system, but what it can do is patch what is already there. The builder API is the other direction: it creates a fresh ISO9660 image from scratch, with a real filesystem in it, and writes it out to any `File` object.

The builder is filesystem-aware. You describe the directory tree you want, attach a `File` object as the content of each file, and the builder computes the whole layout at the end: the primary volume descriptor, both path tables, the directory extents, the LBA of every file, and the EDC/ECC of every sector. You never compute a sector address yourself unless you specifically want to.

Everything is deferred. `createRoot`, `createDir` and `createFile` only build an in-memory tree, and no byte is written until `close()` is called. This means the order in which you declare files does not have to match the order they end up on the disc, and it means a file's LBA is not known until after `close()`.

## Creating a builder

```lua
PCSX.isoBuilder(file) -- returns an ISOBuilder object
```

The `file` argument is a `File` object opened for writing, which will receive the image. It can be a normal file on disk, or an in-memory buffer created with `Support.File.buffer()` if you only want the image transiently.

```lua
:failed() -- returns true if the output file is unusable
```

## Writing the license sectors

```lua
:writeLicense(file) -- file is optional
```

The first 16 sectors of a PlayStation disc hold the license data. This is not a boot gate in the way it is often described: the license data *is* the boot logo, so what it mostly determines is what the console displays on the way in.

The `file` argument is a `File` object holding that data. It can be the official license file from the SDK, or simply a disc image dumped from another game, in which case the license is extracted from it. The two are told apart automatically, so you don't have to declare which one you are handing over.

Whether the license matters for booting depends on the machine:

- Japanese consoles refuse to boot a disc without a Japanese license.
- Very late European models refuse to boot a disc without a European license.
- Every other model boots regardless, though a missing or custom license changes the logo that comes up.
- Some emulators, unlike the hardware, refuse to boot without one at all.

A Japanese license is therefore the pragmatic choice, since it covers the widest range of consoles.

Calling `writeLicense()` with no argument, or with a file that failed to open, writes 16 sectors of zeroes instead. That is fine for development and for PCSX-Redux itself. No license data ships with PCSX-Redux, so you have to supply your own.

If you intend to write license sectors at all, do it before building the tree.

## The volume descriptor

The PVD fields that identify the volume are settable, and are padded with spaces to their fixed ISO9660 widths for you:

```lua
:setSystemIdent(str)          :getSystemIdent()
:setVolumeIdent(str)          :getVolumeIdent()
:setVolSetIdent(str)          :getVolSetIdent()
:setPublisherIdent(str)       :getPublisherIdent()
:setDataPreparerIdent(str)    :getDataPreparerIdent()
:setApplicationIdent(str)     :getApplicationIdent()
:setCopyrightFileIdent(str)   :getCopyrightFileIdent()
:setAbstractFileIdent(str)    :getAbstractFileIdent()
:setBibliographicFileIdent(str) :getBibliographicFileIdent()
```

The getters return the stored string with its padding trimmed off.

For a disc meant to look like a retail PlayStation release, the system identifier is `PLAYSTATION`. The remaining fields are cosmetic as far as the console is concerned.

Everything else in the PVD is computed during `close()` and cannot be set: the volume space size, the path table sizes and locations, the root directory entry, the type code, the standard identifier, and both version fields.

## Building the tree

```lua
:createRoot(sectorCount)               -- sectorCount defaults to 1
:createDir(parent, name, sectorCount)  -- sectorCount defaults to 1
:createFile(parent, name, contentFile)
```

All three return a `DirTree` node. `createRoot` must be called before anything else can be parented, and the node it returns is the root directory.

The `sectorCount` argument on the two directory functions is the number of sectors reserved for that directory's own extent. One sector holds roughly thirty entries depending on name lengths, so a directory with more children than that needs a larger count. It is not grown automatically.

`name` is the bare name. For files, the `;1` version suffix that ISO9660 requires is appended when the directory entry is serialized, so pass `SYSTEM.CNF`, not `SYSTEM.CNF;1`. Names are not validated or case-folded for you; ISO9660 level 1 wants uppercase 8.3, and it is on you to respect that if you care about the image being maximally portable.

`contentFile` is any `File` object. Its size at `close()` time is the size the file gets on the disc.

`createRoot` marks the root as having XA attributes, which is what a PlayStation disc looks like, and both `createDir` and `createFile` inherit the XA flag and the date from their parent. So setting a date on the root propagates to everything created under it afterwards, but not to anything created before.

## DirTree nodes

The node returned by the three creation functions carries the per-entry knobs.

Read-only properties, meaningful after `close()` for the ones that depend on layout:

```lua
:getName()   :getSize()   :getLBA()   :isDir()
```

Navigation, all returning a node or `nil`:

```lua
:parent()   :firstChild()   :nextSibling()
```

Flags:

```lua
:isHidden()     :setHidden(bool)
:shouldSkip()   :setSkip(bool)
:hasXA()        :setXA(fields)
```

`setHidden` sets the ISO9660 hidden bit on the directory entry. The file is still fully present and readable by anything that ignores the bit.

`setSkip` is different and much stronger: the node still participates in the layout, so it is allocated an LBA and its content is written to the disc, but no directory entry is emitted for it. The file exists on the disc and is completely invisible to the filesystem. This is how you reproduce discs that address most of their data by hardcoded LBA and only expose a handful of real entries.

Sector mode and XA:

```lua
:getSectorMode()       :setSectorMode(mode)   -- 'RAW', 'M1', 'M2_RAW', 'M2_FORM1', 'M2_FORM2'
:getXAAttribs()        :setXAAttribs(value)
:getXAFileNum()        :setXAFileNum(value)
```

Files default to `M2_FORM1`, which is what ordinary data on a PlayStation disc uses. `M2_FORM2` is for streaming data such as XA audio and STR video, where the sector carries 2324 bytes and no error correction. `M2_RAW` hands the builder full 2336-byte sectors including the subheader, for content that has to be reproduced byte for byte.

`setXA` takes a table of fields and works out the XA attribute word from the node, so the usual values never need spelling out:

```lua
root:setXA {}                        -- directory: 0x8d55
file:setXA {}                        -- M2_FORM1 file: 0x0d55
movie:setSectorMode('M2_FORM2')
movie:setXA { interleaved = true }   -- 0x3555, or 0x1555 without interleaved
```

Each field overrides only its own bits: `ownerRead`, `ownerExecute`, `groupRead`, `groupExecute`, `worldRead` and `worldExecute` (bits 0, 2, 4, 6, 8 and 10, all set unless you pass `false`), `form` (1 for bit 11, 2 for bit 12), `interleaved` (bit 13), `cdda` (bit 14) and `directory` (bit 15, which defaults to whether the node is a directory). Directories are form 1. A file's form comes from its sector mode and keeps following it, so `setSectorMode` can be called before or after `setXA`; an explicit `form`, or a later `setXAAttribs`, stops that. `RAW` and `M2_RAW` files carry their form in each sector's subheader, so `setXA` on one of them needs `form` spelled out. `cdda = true` leaves both form bits clear unless `form` is given, but the builder does not lay out audio tracks, so pointing such an entry at one is still up to you.

`setXA(true)` is the same as `setXA {}`, and `setXA(false)` drops the XA record from the entry. `setXAAttribs` writes the raw 16-bit value. A node nobody calls any of these on inherits the XA flag from its parent, with an attribute word of 0.

Dates:

```lua
:setDate(year, month, day, hour, minute, second, offset)
```

All seven arguments default to 0 if omitted. `year` is an offset from 1900 and `offset` is the timezone in 15-minute steps, matching the ISO9660 directory record format.

Layout overrides:

```lua
:hasAnchorLBA()      :getAnchorLBA()      :setAnchorLBA(lba)      :clearAnchorLBA()
:hasDeclaredSize()   :getDeclaredSize()   :setDeclaredSize(bytes) :clearDeclaredSize()
```

`setAnchorLBA` pins a node to a specific absolute LBA instead of letting the layout pass place it. Sectors between the previous end of the layout and the anchor are padded with empty M2_FORM1 data. `close()` throws if the layout cursor has already advanced past a requested anchor, since that cannot be satisfied.

`setDeclaredSize` overrides the `DataLength` field of the directory entry independently of how much content is actually attached. The pair is what lets you reproduce a disc whose single directory entry declares an extent spanning hundreds of megabytes of data that is itself addressed by LBA. Attach a small piece of content, declare the real extent, and place the rest of the data with `setSkip` nodes.

## Closing

```lua
:close(threadCount) -- threadCount defaults to 0
```

This is where the work happens: the layout is computed, then the PVD, the volume descriptor set terminator, both path tables, the directory extents, the anchor padding and finally the file contents are written in that order.

EDC and ECC are computed for every sector the builder assembles itself, and the file contents are processed in parallel. `threadCount` is the number of worker threads to use for that; 0 means one per hardware thread. There is no way to turn the computation off, since an image with wrong EDC is not a useful image. The only exception is file content in `RAW` or `M2_RAW` mode, which already carries its own EDC and ECC and is written as supplied.

The builder does not add trailing padding past the end of the volume, and on real hardware that is a problem. The Mechacon has trouble seeking near the end of a disc: it regularly overshoots, and with nothing past the last real sector, it gets stuck there. Zero-filled sectors after the data are a landing zone that guarantees it cannot.

How many of them you need is a function of the read pattern. If the read is linear from the start of an extent and never goes near the edge of the written area, read-ahead slack is all there is to cover, and the 150 sectors `exe2iso` appends, two seconds of disc time, are right for a disc holding a single executable. If arbitrary seeks near the last written sector are possible, size against the overshoot instead: the authoring tool appends 9000, since an archive can be seeked into anywhere, including its own edge. Neither number is wrong; they answer different questions.

PCSX-Redux reads a truncated tail perfectly happily, so an image missing its padding passes every test you can run without a burner. Append the sectors after `close()`: the layout always starts at sector 16 and leaves the cursor past the end of the volume, so a loop of blank `writeSector` calls lands exactly where it should.

## A complete example

```lua
local out = Support.File.open('mydisc.bin', 'TRUNCATE')
local builder = PCSX.isoBuilder(out)

builder:writeLicense(Support.File.open('license.dat'))
builder:setSystemIdent('PLAYSTATION')
builder:setVolumeIdent('MYDISC')

local root = builder:createRoot()

local cnf = Support.File.buffer()
cnf:write('BOOT=cdrom:\\PSX.EXE;1\r\nTCB=4\r\nEVENT=10\r\nSTACK=801FFFF0\r\n')
cnf:rSeek(0)
builder:createFile(root, 'SYSTEM.CNF', cnf)

local exe = builder:createFile(root, 'PSX.EXE', Support.File.open('mygame.ps-exe'))

local data = builder:createDir(root, 'DATA')
builder:createFile(data, 'LEVEL1.BIN', Support.File.open('level1.bin'))

local movie = builder:createFile(data, 'INTRO.STR', Support.File.open('intro.str'))
movie:setSectorMode('M2_FORM2')

builder:close()
print('PSX.EXE landed at LBA ' .. exe:getLBA())
out:close()
```

Note that `exe:getLBA()` is only meaningful after `close()`, since nothing is placed before then.

## Reading the result back

The builder writes an image that the reader side can open directly, which makes round-tripping easy to verify without leaving Lua:

```lua
local buffer = Support.File.buffer()
local builder = PCSX.isoBuilder(buffer)
-- ...build...
builder:close()

buffer:rSeek(0)
local reader = PCSX.openIso(buffer):createReader()
local f = reader:open('DATA/LEVEL1.BIN;1')
print(f:size())
```

Note the `rSeek(0)` calls. A `File` object has separate read and write pointers, so a buffer you have just written content into still has its read pointer where you left it. The builder rewinds every content file itself before reading it during `close()`, so rewinding a buffer before handing it to `createFile` is only a precaution, but rewind the output before reopening it as an ISO.

This is exactly what the test suite in `tests/lua/isobuilder.lua` does.

## Writing raw sectors

Everything above builds a filesystem. The builder can also write individual sectors directly, with no ISO9660 structure involved at all, which is what you want for reproducing a disc that does not have a conventional filesystem, or for placing data at a fixed location alongside one that does.

```lua
:writeSector(data[, mode])            -- writes at the current cursor, returns the LBA used
:writeSectorAt(data, lba[, mode])     -- writes at an explicit LBA, returns it
:getCurrentLBA()                      -- where the cursor currently sits
```

`data` is a Lua string. A cdata pointer works too, but then the size has to be passed explicitly, since a pointer does not carry one:

```lua
:writeSector(ptr, size[, mode])
:writeSectorAt(ptr, size, lba[, mode])
```

`mode` defaults to `M2_FORM1`. Each mode consumes a different amount of data, and passing less than it needs is an error rather than a truncated sector:

| mode | bytes consumed | notes |
| --- | --- | --- |
| `M2_FORM1` | 2048 | ordinary data; sync, header, subheader and EDC/ECC are generated |
| `M2_FORM2` | 2324 | streaming data; EDC is generated |
| `M2_RAW` | 2336 | subheader onwards, supplied by you; sync and header are generated |
| `RAW` | 2352 | the complete frame, written verbatim |

`M1` and `GUESS` cannot be written and will raise an error.

All LBAs here are relative to the start of the image, matching the rest of the ISO API, so sector 0 is the first sector of the file.

`writeSector` advances the cursor by one. `writeSectorAt` moves the cursor to just past the sector it wrote, but only ever forwards: writing behind the cursor leaves it where it was, so you can go back and fill something in without losing your place.

Note that raw sector writes and the filesystem API write to the same output file, and nothing arbitrates between them. `close()` always lays the filesystem out starting at sector 16, immediately after the system area, regardless of where the sector cursor happens to be, and will happily overwrite sectors you placed by hand. If you want to mix the two, use `setAnchorLBA` to push the filesystem past the region you are writing yourself.

The most useful thing this buys you is patching the image after the layout exists. A common shape on real discs is a lookup table that maps some game-side index to the LBA of the data it needs, which is a chicken-and-egg problem while you are building: the table has to contain LBAs that nothing knows until the layout has been computed. Raw sector writes let you close that loop by going back afterwards.

```lua
local builder = PCSX.isoBuilder(out)
builder:writeLicense(license)
local root = builder:createRoot()

-- Reserve a sector for the table itself, plus the files it will point at.
local placeholder = Support.File.buffer()
placeholder:write(string.rep('\0', 2048))
placeholder:rSeek(0)
local table_ = builder:createFile(root, 'LBATABLE.BIN', placeholder)

local assets = {}
for i, name in ipairs(assetNames) do
    assets[i] = builder:createFile(root, name, Support.File.open(name))
end

-- Now the layout exists, and every node knows where it landed.
builder:close()

-- Build the real table from the computed LBAs and drop it over the placeholder.
local bit = require 'bit'
local sector = ''
for i, node in ipairs(assets) do
    local lba = node:getLBA()
    sector = sector .. string.char(bit.band(lba, 0xff), bit.band(bit.rshift(lba, 8), 0xff),
                                   bit.band(bit.rshift(lba, 16), 0xff), bit.band(bit.rshift(lba, 24), 0xff))
end
sector = sector .. string.rep('\0', 2048 - #sector)
builder:writeSectorAt(sector, table_:getLBA(), 'M2_FORM1')
```

The EDC/ECC of the rewritten sector is recomputed as part of the write, so the patched image stays consistent.
