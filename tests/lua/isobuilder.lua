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
local ffi = require 'ffi'

TestIsoBuilder = {}

-- Generate a small DTMF tone as test file content.
local function generateTestContent(size)
    local buf = Support.File.buffer()
    local data = ffi.new('uint8_t[?]', size)
    for i = 0, size - 1 do
        -- Simple pattern: sine-ish bytes for verifiable content.
        data[i] = math.floor(128 + 127 * math.sin(2 * math.pi * i / 256))
    end
    buf:write(ffi.cast('const char*', data), size)
    buf:rSeek(0)
    return buf
end

function TestIsoBuilder:test_minimalIso()
    -- Create a minimal ISO with one file in the root directory.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    lu.assertNotNil(builder)
    lu.assertFalse(builder:failed())

    -- Write empty system area (no license).
    builder:writeLicense()

    -- Set PVD fields.
    builder:setSystemIdent('PLAYSTATION')
    builder:setVolumeIdent('TEST_DISC')

    -- Create root directory (1 sector).
    local root = builder:createRoot(1)
    lu.assertNotNil(root)
    lu.assertTrue(root:isDir())

    -- Create a small test file.
    local content = generateTestContent(4096)
    local file = builder:createFile(root, 'TEST.DAT', content)
    lu.assertNotNil(file)
    lu.assertFalse(file:isDir())
    lu.assertEquals(file:getName(), 'TEST.DAT')

    -- Close - computes layout and writes everything.
    builder:close()

    -- The output buffer should now contain a valid ISO image.
    lu.assertTrue(out:size() > 0)

    -- Verify: open the ISO and read it back.
    out:rSeek(0)
    local iso = PCSX.openIso(out)
    lu.assertNotNil(iso)
    lu.assertFalse(iso:failed())

    local reader = iso:createReader()
    lu.assertNotNil(reader)

    -- Open the file we created.
    local readBack = reader:open('TEST.DAT;1')
    lu.assertNotNil(readBack)
    lu.assertEquals(readBack:size(), 4096)

    -- Verify content matches.
    readBack:rSeek(0)
    local readData = readBack:read(4096)
    lu.assertNotNil(readData)

    content:rSeek(0)
    local origData = content:read(4096)
    lu.assertEquals(tostring(readData), tostring(origData))
end

function TestIsoBuilder:test_subdirectory()
    -- Create an ISO with a subdirectory containing a file.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)

    builder:writeLicense()
    builder:setVolumeIdent('SUBDIR_TEST')

    local root = builder:createRoot(1)
    local dataDir = builder:createDir(root, 'DATA', 1)
    lu.assertTrue(dataDir:isDir())
    lu.assertEquals(dataDir:getName(), 'DATA')

    local content = generateTestContent(2048)
    builder:createFile(dataDir, 'INFO.BIN', content)

    builder:close()

    -- Read it back.
    out:rSeek(0)
    local iso = PCSX.openIso(out)
    local reader = iso:createReader()

    -- Open file through subdirectory path.
    local readBack = reader:open('DATA/INFO.BIN;1')
    lu.assertNotNil(readBack)
    lu.assertFalse(readBack:failed())
    lu.assertEquals(readBack:size(), 2048)
end

function TestIsoBuilder:test_pvdFields()
    -- Verify PVD string fields round-trip correctly.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)

    builder:setSystemIdent('PLAYSTATION')
    builder:setVolumeIdent('MY_VOL')
    builder:setPublisherIdent('TEST_PUB')
    builder:setApplicationIdent('TEST_APP')

    lu.assertEquals(builder:getSystemIdent(), 'PLAYSTATION')
    lu.assertEquals(builder:getVolumeIdent(), 'MY_VOL')
    lu.assertEquals(builder:getPublisherIdent(), 'TEST_PUB')
    lu.assertEquals(builder:getApplicationIdent(), 'TEST_APP')
end

function TestIsoBuilder:test_multipleFiles()
    -- Create an ISO with multiple files.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)

    builder:writeLicense()
    builder:setVolumeIdent('MULTI_FILE')

    local root = builder:createRoot(1)

    local content1 = generateTestContent(512)
    local content2 = generateTestContent(1024)
    local content3 = generateTestContent(3000)

    builder:createFile(root, 'FILE1.DAT', content1)
    builder:createFile(root, 'FILE2.DAT', content2)
    builder:createFile(root, 'FILE3.DAT', content3)

    builder:close()

    -- Read back and verify sizes.
    out:rSeek(0)
    local iso = PCSX.openIso(out)
    local reader = iso:createReader()

    local f1 = reader:open('FILE1.DAT;1')
    lu.assertNotNil(f1)
    lu.assertFalse(f1:failed())
    lu.assertEquals(f1:size(), 512)

    local f2 = reader:open('FILE2.DAT;1')
    lu.assertNotNil(f2)
    lu.assertFalse(f2:failed())
    lu.assertEquals(f2:size(), 1024)

    local f3 = reader:open('FILE3.DAT;1')
    lu.assertNotNil(f3)
    lu.assertFalse(f3:failed())
    lu.assertEquals(f3:size(), 3000)
end

function TestIsoBuilder:test_anchorLBA()
    -- Anchor a file to a specific LBA. The layout pass must pad prior sectors with
    -- empty M2F1 zero sectors, and the file's actual LBA must match the anchor.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('ANCHOR_TEST')

    local root = builder:createRoot(1)

    -- First file is unanchored.
    local first = builder:createFile(root, 'FIRST.DAT', generateTestContent(2048))

    -- Second file is anchored well past where it would naturally land.
    local anchored = builder:createFile(root, 'ANCHOR.DAT', generateTestContent(2048))
    anchored:setAnchorLBA(150)
    lu.assertTrue(anchored:hasAnchorLBA())
    lu.assertEquals(anchored:getAnchorLBA(), 150)

    builder:close()

    -- After build, the anchored file's assigned LBA must be exactly 150.
    lu.assertEquals(anchored:getLBA(), 150)
    -- And the first file must come earlier.
    lu.assertTrue(first:getLBA() < 150)

    -- Read back via ISO9660 and verify the dir entry agrees.
    out:rSeek(0)
    local iso = PCSX.openIso(out)
    local reader = iso:createReader()
    local entries = reader:listDir('')
    local foundAnchor = false
    for _, e in ipairs(entries) do
        if e.name == 'ANCHOR.DAT;1' then
            foundAnchor = true
            lu.assertEquals(e.lba, 150)
            lu.assertEquals(e.size, 2048)
        end
    end
    lu.assertTrue(foundAnchor)

    -- Read back the anchored file's actual content to confirm we can open it.
    local f = reader:open('ANCHOR.DAT;1')
    lu.assertNotNil(f)
    lu.assertFalse(f:failed())
    lu.assertEquals(f:size(), 2048)
end

function TestIsoBuilder:test_declaredSize()
    -- Declare a size in the dir entry that's larger than the actual content.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('DECL_SIZE')

    local root = builder:createRoot(1)
    local shadow = builder:createFile(root, 'SHADOW.BIN', generateTestContent(2048))
    shadow:setDeclaredSize(50 * 2048)  -- claim 50 sectors regardless of content
    lu.assertTrue(shadow:hasDeclaredSize())
    lu.assertEquals(shadow:getDeclaredSize(), 50 * 2048)

    builder:close()

    out:rSeek(0)
    local iso = PCSX.openIso(out)
    local reader = iso:createReader()
    local entries = reader:listDir('')
    local found = false
    for _, e in ipairs(entries) do
        if e.name == 'SHADOW.BIN;1' then
            found = true
            -- Declared size must be reflected, not the 2048-byte actual content.
            lu.assertEquals(e.size, 50 * 2048)
        end
    end
    lu.assertTrue(found)
end

function TestIsoBuilder:test_vpStyleShadowFile()
    -- VP-style layout: a small leading file, then a shadow file anchored to LBA 150
    -- whose declared size covers a large extent beyond its actual content. This
    -- emulates the Valkyrie Profile VALKYRIE.BIN pattern.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('VP_STYLE')

    local root = builder:createRoot(1)

    -- Small executable-like file.
    builder:createFile(root, 'EXEC.BIN', generateTestContent(4096))

    -- Shadow file anchored at LBA 150 with declared size 200 sectors.
    local shadow = builder:createFile(root, 'BIG.BIN', generateTestContent(2048))
    shadow:setAnchorLBA(150)
    shadow:setDeclaredSize(200 * 2048)

    builder:close()

    -- Verify shadow file at exactly LBA 150 with the declared extent.
    lu.assertEquals(shadow:getLBA(), 150)

    out:rSeek(0)
    local iso = PCSX.openIso(out)
    local reader = iso:createReader()
    local entries = reader:listDir('')
    local foundShadow = false
    for _, e in ipairs(entries) do
        if e.name == 'BIG.BIN;1' then
            foundShadow = true
            lu.assertEquals(e.lba, 150)
            lu.assertEquals(e.size, 200 * 2048)
        end
    end
    lu.assertTrue(foundShadow)
end

function TestIsoBuilder:test_m2RawRoundTrip()
    -- Write a multi-sector M2_RAW payload with a deterministic pattern and verify
    -- the rebuilt disc returns identical bytes when read back in M2_RAW. Regression
    -- guard for the multi-threaded writeFiles path treating M2_RAW as M2_FORM1.
    local payload = Support.File.buffer()
    local data = ffi.new('uint8_t[?]', 4672)
    for i = 0, 4671 do data[i] = i % 251 end  -- deterministic, non-zero pattern
    payload:write(ffi.cast('const char*', data), 4672)
    payload:rSeek(0)

    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('M2RAW_RT')
    local root = builder:createRoot(1)
    local file = builder:createFile(root, 'PATTERN.BIN', payload)
    file:setSectorMode('M2_RAW')
    builder:close()

    out:rSeek(0)
    local iso = PCSX.openIso(out)
    local reader = iso:createReader()
    local lba
    for _, e in ipairs(reader:listDir('')) do
        if e.name == 'PATTERN.BIN;1' then lba = e.lba end
    end
    lu.assertNotNil(lba)

    local rb = iso:open(lba, 4672, 'M2_RAW')
    for i = 0, 4671 do
        lu.assertEquals(rb:readU8At(i), i % 251,
            string.format('M2_RAW round-trip mismatch at byte %d', i))
    end
end

-- Payload size per sector for each sector mode, where that payload sits in the raw frame, and the form bit
-- the builder has to put in the subheader for the modes where it synthesizes one.
local sectorLayouts = {
    { mode = 'RAW', size = 2352, offset = 0 },
    { mode = 'M2_RAW', size = 2336, offset = 16 },
    { mode = 'M2_FORM1', size = 2048, offset = 24, form = 0x00 },
    { mode = 'M2_FORM2', size = 2324, offset = 24, form = 0x20 },
}

local function patternByte(i) return (i * 7 + 3) % 251 end

local function patternData(size)
    local data = ffi.new('uint8_t[?]', size)
    for i = 0, size - 1 do data[i] = patternByte(i) end
    return data
end

-- Counts the bytes of a read back payload that differ from the pattern, reporting the first one.
local function assertPattern(file, base, size, what)
    local bad, first = 0, nil
    for i = 0, size - 1 do
        if file:readU8At(base + i) ~= patternByte(i) then
            bad = bad + 1
            first = first or i
        end
    end
    lu.assertEquals(bad, 0, string.format('%s: %d of %d payload bytes corrupted, first at byte %s', what, bad,
                                          size, tostring(first)))
end

local function assertForm(file, base, form, what)
    for _, offset in ipairs({ 18, 22 }) do
        lu.assertEquals(bit.band(file:readU8At(base + offset), 0x20), form,
            string.format('%s: wrong form bit in subheader byte %d', what, offset))
    end
end

function TestIsoBuilder:test_writeSectorRoundTrip()
    -- Every mode writeSectorAt takes must land the caller's payload in the frame untouched.
    for _, layout in ipairs(sectorLayouts) do
        local out = Support.File.buffer()
        local builder = PCSX.isoBuilder(out)
        for lba = 0, 1 do
            builder:writeSectorAt(patternData(layout.size), layout.size, lba, layout.mode)
        end
        out:rSeek(0)
        for lba = 0, 1 do
            local what = string.format('writeSectorAt %s sector %d', layout.mode, lba)
            assertPattern(out, lba * 2352 + layout.offset, layout.size, what)
            if layout.form then assertForm(out, lba * 2352, layout.form, what) end
        end
    end
end

function TestIsoBuilder:test_closeRoundTrip()
    -- Same for file content laid out by close(), through both the single threaded writer and the parallel one.
    for _, layout in ipairs(sectorLayouts) do
        for _, threads in ipairs({ 1, 4 }) do
            local what = string.format('close(%d) %s', threads, layout.mode)
            local size = 2 * layout.size
            local payload = Support.File.buffer()
            local data = ffi.new('uint8_t[?]', size)
            for i = 0, size - 1 do data[i] = patternByte(i % layout.size) end
            payload:write(ffi.cast('const char*', data), size)
            payload:rSeek(0)

            local out = Support.File.buffer()
            local builder = PCSX.isoBuilder(out)
            builder:writeLicense()
            builder:setVolumeIdent('ROUNDTRIP')
            local root = builder:createRoot(1)
            local file = builder:createFile(root, 'PATTERN.BIN', payload)
            file:setSectorMode(layout.mode)
            builder:close(threads)

            out:rSeek(0)
            local iso = PCSX.openIso(out)
            local lba = file:getLBA()
            local rb = iso:open(lba, size, layout.mode)
            for s = 0, 1 do
                assertPattern(rb, s * layout.size, layout.size, what .. ' sector ' .. s)
            end
            if layout.form then
                local raw = iso:open(lba, 2 * 2352, 'RAW')
                for s = 0, 1 do assertForm(raw, s * 2352, layout.form, what .. ' sector ' .. s) end
            end
        end
    end
end

function TestIsoBuilder:test_closeM1()
    -- M1 file content is laid out at 2048 bytes per sector, and both writers have to put it on the disc.
    for _, threads in ipairs({ 1, 4 }) do
        local what = string.format('close(%d) M1', threads)
        local size = 2 * 2048
        local payload = Support.File.buffer()
        local data = ffi.new('uint8_t[?]', size)
        for i = 0, size - 1 do data[i] = patternByte(i % 2048) end
        payload:write(ffi.cast('const char*', data), size)
        payload:rSeek(0)

        local out = Support.File.buffer()
        local builder = PCSX.isoBuilder(out)
        builder:writeLicense()
        builder:setVolumeIdent('ROUNDTRIP')
        local root = builder:createRoot(1)
        local file = builder:createFile(root, 'PATTERN.BIN', payload)
        file:setSectorMode('M1')
        builder:close(threads)

        out:rSeek(0)
        local iso = PCSX.openIso(out)
        local rb = iso:open(file:getLBA(), size, 'M2_FORM1')
        for s = 0, 1 do assertPattern(rb, s * 2048, 2048, what .. ' sector ' .. s) end
    end
end

function TestIsoBuilder:test_anchorErrorOnBackwardLBA()
    -- Anchoring to an LBA that's already passed must raise an error at close time.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('ANCHOR_ERR')

    local root = builder:createRoot(1)
    -- A reasonably large first file so the cursor advances past LBA 25.
    builder:createFile(root, 'PADDING.DAT', generateTestContent(64 * 2048))

    -- Anchor a second file to an LBA that's been passed by the time we lay it out.
    local bad = builder:createFile(root, 'BAD.DAT', generateTestContent(2048))
    bad:setAnchorLBA(25)

    local ok = pcall(function() builder:close() end)
    lu.assertFalse(ok, 'expected close() to throw on backward anchor')
end

function TestIsoBuilder:test_writeSectorRaw()
    -- Raw sector writes, without any filesystem on top.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)

    -- The write cursor starts at the beginning of the image.
    lu.assertEquals(builder:getCurrentLBA(), 0)

    local sector = string.rep('A', 2048)
    lu.assertEquals(builder:writeSector(sector, 'M2_FORM1'), 0)
    lu.assertEquals(builder:getCurrentLBA(), 1)
    lu.assertEquals(builder:writeSector(sector, 'M2_FORM1'), 1)
    lu.assertEquals(builder:getCurrentLBA(), 2)

    -- Two sectors written, so two raw frames on the output.
    lu.assertEquals(out:size(), 2 * 2352)

    -- The payload of the first sector lands after the 24-byte header.
    out:rSeek(0)
    for i = 0, 2047 do
        lu.assertEquals(out:readU8At(24 + i), string.byte('A'),
            string.format('M2_FORM1 payload mismatch at byte %d', i))
    end
end

function TestIsoBuilder:test_writeSectorAtSkipsAhead()
    -- writeSectorAt places a sector at an explicit LBA and drags the cursor along.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)

    lu.assertEquals(builder:writeSectorAt(string.rep('B', 2048), 10, 'M2_FORM1'), 10)
    lu.assertEquals(builder:getCurrentLBA(), 11)
    lu.assertEquals(out:size(), 11 * 2352)

    -- Writing behind the cursor is allowed, and must not rewind it.
    lu.assertEquals(builder:writeSectorAt(string.rep('C', 2048), 3, 'M2_FORM1'), 3)
    lu.assertEquals(builder:getCurrentLBA(), 11)

    out:rSeek(0)
    lu.assertEquals(out:readU8At(3 * 2352 + 24), string.byte('C'))
    lu.assertEquals(out:readU8At(10 * 2352 + 24), string.byte('B'))
end

function TestIsoBuilder:test_writeSectorModeSizes()
    -- Each writable mode consumes a different amount of data, and short buffers must be refused
    -- rather than read past the end.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)

    local sizes = { RAW = 2352, M2_RAW = 2336, M2_FORM1 = 2048, M2_FORM2 = 2324 }
    local lba = 0
    for mode, size in pairs(sizes) do
        lu.assertEquals(builder:writeSectorAt(string.rep('D', size), lba, mode), lba,
            'failed to write mode ' .. mode)
        -- One byte short of what the mode needs must throw, not truncate or overread.
        local ok = pcall(function() builder:writeSectorAt(string.rep('D', size - 1), lba, mode) end)
        lu.assertFalse(ok, 'expected a short buffer to be refused for mode ' .. mode)
        lba = lba + 1
    end

    -- Modes that can't carry a raw sector write must be refused too.
    for _, mode in ipairs({ 'M1', 'GUESS' }) do
        local ok = pcall(function() builder:writeSectorAt(string.rep('D', 2352), 0, mode) end)
        lu.assertFalse(ok, 'expected mode ' .. mode .. ' to be refused')
    end
end

function TestIsoBuilder:test_writeSectorFromPointer()
    -- The raw writers also take a cdata pointer, in which case a size is mandatory.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)

    local data = ffi.new('uint8_t[?]', 2048)
    for i = 0, 2047 do data[i] = i % 256 end

    lu.assertEquals(builder:writeSector(data, 2048, 'M2_FORM1'), 0)
    out:rSeek(0)
    lu.assertEquals(out:readU8At(24 + 100), 100)

    -- No size means we can't know how much is safe to read.
    local ok = pcall(function() builder:writeSector(data, 'M2_FORM1') end)
    lu.assertFalse(ok, 'expected a pointer without a size to be refused')
end

function TestIsoBuilder:test_writeSectorPatchesAfterClose()
    -- The lookup-table pattern: build the filesystem, let close() assign the LBAs, then go back and
    -- write a sector containing them.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('LBATABLE')

    local root = builder:createRoot(1)

    -- A placeholder sector that will hold the table.
    local placeholder = Support.File.buffer()
    placeholder:write(string.rep('\0', 2048))
    placeholder:rSeek(0)
    local tableNode = builder:createFile(root, 'LBATABLE.BIN', placeholder)

    local assets = {}
    for i = 1, 3 do
        assets[i] = builder:createFile(root, string.format('ASSET%d.DAT', i), generateTestContent(2048))
    end

    builder:close()

    -- Every asset has a real LBA now, and none of them is the placeholder's.
    local payload = ''
    for i = 1, 3 do
        local lba = assets[i]:getLBA()
        lu.assertTrue(lba > 0)
        payload = payload .. string.char(lba % 256, math.floor(lba / 256) % 256, 0, 0)
    end
    payload = payload .. string.rep('\0', 2048 - #payload)

    local written = builder:writeSectorAt(payload, tableNode:getLBA(), 'M2_FORM1')
    lu.assertEquals(written, tableNode:getLBA())

    -- Read the table back through the filesystem, which also proves the rewritten sector still has
    -- valid EDC/ECC and a correct header.
    out:rSeek(0)
    local reader = PCSX.openIso(out):createReader()
    local back = reader:open('LBATABLE.BIN;1')
    lu.assertNotNil(back)
    for i = 1, 3 do
        local lba = assets[i]:getLBA()
        lu.assertEquals(back:readU8At((i - 1) * 4), lba % 256)
        lu.assertEquals(back:readU8At((i - 1) * 4 + 1), math.floor(lba / 256) % 256)
    end
end

function TestIsoBuilder:test_customLicense()
    -- A license file made of whole 2336-byte sectors (8-byte subheader, 2048 bytes of data, 280 of
    -- EDC/ECC space) is written to the system area even without Sony's text in it. Sectors the file
    -- does not cover stay zero.
    local license = Support.File.buffer()
    for i = 0, 11 do
        license:write(string.rep('\0', 8) .. string.rep(string.char(0x40 + i), 2048) .. string.rep('\0', 280))
    end
    license:rSeek(0)
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense(license)
    builder:setVolumeIdent('LICENSE')
    builder:createRoot(1)
    builder:close()

    for i = 0, 15 do
        local expected = i < 12 and 0x40 + i or 0
        for _, offset in ipairs({ 0, 1000, 2047 }) do
            lu.assertEquals(out:readU8At(i * 2352 + 24 + offset), expected,
                string.format('license sector %d byte %d', i, offset))
        end
    end
end

function TestIsoBuilder:test_unrecognizedLicenseIsZero()
    -- A file that is neither format leaves the system area zero.
    local license = Support.File.buffer()
    license:write(string.rep('X', 1000))
    license:rSeek(0)
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense(license)
    builder:setVolumeIdent('LICENSE')
    builder:createRoot(1)
    builder:close()

    for i = 0, 15 do
        lu.assertEquals(out:readU8At(i * 2352 + 24), 0, string.format('license sector %d', i))
    end
end

function TestIsoBuilder:test_sortedRecords()
    -- Directory records come out sorted by name whatever the insertion order, while file contents
    -- keep the insertion order on disc.
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('SORTED')
    local root = builder:createRoot(1)
    local zeta = builder:createFile(root, 'ZETA.DAT', generateTestContent(2048))
    local dir = builder:createDir(root, 'MIDDLE', 1)
    local alpha = builder:createFile(root, 'ALPHA.DAT', generateTestContent(2048))
    builder:createFile(dir, 'INNER.DAT', generateTestContent(2048))
    builder:close()

    lu.assertTrue(zeta:getLBA() < alpha:getLBA())

    out:rSeek(0)
    local reader = PCSX.openIso(out):createReader()
    local names = {}
    for _, e in ipairs(reader:listDir('')) do
        table.insert(names, e.name)
    end
    lu.assertEquals(names, { 'ALPHA.DAT;1', 'MIDDLE', 'ZETA.DAT;1' })
    local f = reader:open('MIDDLE/INNER.DAT;1')
    lu.assertNotNil(f)
    lu.assertFalse(f:failed())
    lu.assertEquals(f:size(), 2048)
end

local function buildManyFiles(rootSectors, count)
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('DIR_BOUNDS')
    local root = builder:createRoot(rootSectors)
    for i = 1, count do
        builder:createFile(root, string.format('FILE%02d.DAT', i), generateTestContent(16))
    end
    return builder, out
end

function TestIsoBuilder:test_directoryTooSmallIsAnError()
    -- 60 XA records of 60 bytes each need 2 sectors; a 1-sector root must be refused
    -- instead of serialized past the end of its extent.
    local builder = buildManyFiles(1, 60)
    local ok, err = pcall(builder.close, builder)
    lu.assertFalse(ok)
    lu.assertStrContains(tostring(err), 'needs 2 sectors for its entries but was created with 1')
end

function TestIsoBuilder:test_directoryLargeEnoughLists()
    local builder, out = buildManyFiles(2, 60)
    builder:close()
    out:rSeek(0)
    local iso = PCSX.openIso(out)
    local entries = iso:createReader():listDir('')
    local files = 0
    for _, e in ipairs(entries) do
        if e.name:match('^FILE%d%d%.DAT;1$') then files = files + 1 end
    end
    lu.assertEquals(files, 60)
end

-- Reads the XA attributes of every record in a directory extent, keyed by record name. The '.' and '..'
-- records come out as '\0' and '\1'.
local function readXAAttribs(iso, lba)
    local sector = tostring(iso:open(lba, 2048, 'M2_FORM1'):read(2048))
    local attribs = {}
    local offset = 1
    while offset <= #sector do
        local len = sector:byte(offset)
        if len == 0 then break end
        local nameLen = sector:byte(offset + 32)
        local name = sector:sub(offset + 33, offset + 32 + nameLen)
        local xa = offset + 33 + nameLen
        if nameLen % 2 == 0 then xa = xa + 1 end
        if sector:sub(xa + 6, xa + 7) == 'XA' then
            attribs[name] = sector:byte(xa + 4) * 256 + sector:byte(xa + 5)
        else
            attribs[name] = false
        end
        offset = offset + len
    end
    return attribs
end

function TestIsoBuilder:test_xaDefaults()
    local out = Support.File.buffer()
    local builder = PCSX.isoBuilder(out)
    builder:writeLicense()
    builder:setVolumeIdent('XA')
    local root = builder:createRoot(1)
    root:setXA {}
    local dir = builder:createDir(root, 'DATA', 1)
    dir:setXA {}
    local form1 = builder:createFile(dir, 'FORM1.DAT', generateTestContent(2048))
    form1:setXA {}
    local form2 = builder:createFile(dir, 'FORM2.STR', generateTestContent(2324))
    form2:setSectorMode('M2_FORM2')
    form2:setXA {}
    local interleaved = builder:createFile(dir, 'MOVIE.STR', generateTestContent(2324))
    interleaved:setSectorMode('M2_FORM2')
    interleaved:setXA { interleaved = true }
    builder:close()

    lu.assertEquals(root:getXAAttribs(), 0x8d55)
    lu.assertEquals(dir:getXAAttribs(), 0x8d55)
    lu.assertEquals(form1:getXAAttribs(), 0x0d55)
    lu.assertEquals(form2:getXAAttribs(), 0x1555)
    lu.assertEquals(interleaved:getXAAttribs(), 0x3555)

    out:rSeek(0)
    local iso = PCSX.openIso(out)
    lu.assertEquals(readXAAttribs(iso, root:getLBA()), { ['\0'] = 0x8d55, ['\1'] = 0x8d55, DATA = 0x8d55 })
    lu.assertEquals(readXAAttribs(iso, dir:getLBA()), {
        ['\0'] = 0x8d55,
        ['\1'] = 0x8d55,
        ['FORM1.DAT;1'] = 0x0d55,
        ['FORM2.STR;1'] = 0x1555,
        ['MOVIE.STR;1'] = 0x3555,
    })
    local f = iso:createReader():open('DATA/FORM2.STR;1')
    lu.assertNotNil(f)
    lu.assertFalse(f:failed())
end

function TestIsoBuilder:test_xaFields()
    local builder = PCSX.isoBuilder(Support.File.buffer())
    local root = builder:createRoot(1)
    local function file(name) return builder:createFile(root, name, generateTestContent(16)) end

    -- Without setXA a node inherits the XA flag and carries an attribute word of 0.
    local z = file('Z')
    lu.assertTrue(z:hasXA())
    lu.assertEquals(z:getXAAttribs(), 0)

    -- setXA(true) is setXA{}, setXA(false) drops the record altogether.
    local a, b = file('A'), file('B')
    a:setXA(true)
    b:setXA {}
    lu.assertEquals(a:getXAAttribs(), b:getXAAttribs())
    lu.assertTrue(a:hasXA())
    a:setXA(false)
    lu.assertFalse(a:hasXA())

    -- Named fields only touch their own bits.
    local c = file('C')
    c:setXA { worldExecute = false, groupExecute = false, ownerExecute = false }
    lu.assertEquals(c:getXAAttribs(), 0x0911)
    c:setXA { directory = true, cdda = true }
    lu.assertEquals(c:getXAAttribs(), 0xc555)
    root:setXA { directory = false, ownerRead = false }
    lu.assertEquals(root:getXAAttribs(), 0x0d54)

    -- Derived form bits follow the sector mode, in either order.
    local d = file('D')
    d:setXA {}
    d:setSectorMode('M2_FORM2')
    lu.assertEquals(d:getXAAttribs(), 0x1555)
    d:setSectorMode('M2_FORM1')
    lu.assertEquals(d:getXAAttribs(), 0x0d55)
    lu.assertEquals(d:getSectorMode(), 'M2_FORM1')

    -- The form follows the sector mode through a wrapper obtained by navigation too.
    local e = file('E')
    e:setXA {}
    local found = root:firstChild()
    while found:getName() ~= 'E' do
        found = found:nextSibling()
    end
    found:setSectorMode('M2_FORM2')
    lu.assertEquals(e:getXAAttribs(), 0x1555)

    -- An explicit form, or raw attributes, stop the following.
    local g = file('G')
    g:setXA { form = 1 }
    g:setSectorMode('M2_FORM2')
    lu.assertEquals(g:getXAAttribs(), 0x0d55)
    local h = file('H')
    h:setXA {}
    h:setXAAttribs(0x0d55)
    h:setSectorMode('M2_FORM2')
    lu.assertEquals(h:getXAAttribs(), 0x0d55)

    -- M1 contents go on the disc as Form 1 sectors.
    local m1 = file('M1')
    m1:setSectorMode('M1')
    m1:setXA {}
    lu.assertEquals(m1:getXAAttribs(), 0x0d55)
    m1:setSectorMode('M2_FORM2')
    lu.assertEquals(m1:getXAAttribs(), 0x1555)
    m1:setSectorMode('M1')
    lu.assertEquals(m1:getXAAttribs(), 0x0d55)

    -- A raw sector mode cannot say which form the file is.
    local i = file('I')
    i:setSectorMode('M2_RAW')
    lu.assertErrorMsgContains('pass form = 1 or 2', function() i:setXA {} end)
    i:setXA { form = 2, interleaved = true }
    lu.assertEquals(i:getXAAttribs(), 0x3555)
    local j = file('J')
    j:setXA {}
    lu.assertErrorMsgContains('pass form = 1 or 2', function() j:setSectorMode('RAW') end)
    lu.assertEquals(j:getSectorMode(), 'M2_FORM1')

    lu.assertErrorMsgContains('unknown field', function() a:setXA { owner = true } end)
    lu.assertErrorMsgContains('form must be 1 or 2', function() a:setXA { form = 3 } end)
    lu.assertErrorMsgContains('must be a boolean', function() a:setXA { interleaved = 1 } end)
    lu.assertErrorMsgContains('boolean or a table', function() a:setXA() end)
end
