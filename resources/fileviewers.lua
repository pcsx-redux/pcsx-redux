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

-- File viewers for the ISO browser: TIM images, raw pixel data with a
-- user-supplied geometry, and sounds. The ISO browser calls
-- PCSX.FileViewers.open(file) when the user asks to view a file, then calls
-- draw() on the returned object every frame inside its window, and close()
-- once the window is closed.
--
-- Images decode into an RGBA buffer uploaded as a GL texture, re-uploaded
-- only when a knob changes. Sub-byte pixels are taken low bits first, as the
-- GPU stores them.

PCSX.FileViewers = PCSX.FileViewers or {}

local MAX_DIM = 4096

-- 15-bit colour to 0xAABBGGRR. 0x0000 is transparent, as the GPU treats it
-- when drawing textures.
local function psxColour(c)
    if c == 0 then return 0 end
    local r = c & 0x1f
    local g = (c >> 5) & 0x1f
    local b = (c >> 10) & 0x1f
    r = (r << 3) | (r >> 2)
    g = (g << 3) | (g >> 2)
    b = (b << 3) | (b >> 2)
    return 0xff000000 | (b << 16) | (g << 8) | r
end

local function grey(v, max)
    local g = math.floor(v * 255 / max)
    return 0xff000000 | (g << 16) | (g << 8) | g
end

-- A texture holder: upload(pix, w, h) replaces the content, draw(zoom) shows it.
local function newTexture()
    local t = {}
    function t:upload(pix, w, h)
        if not self.id then
            local id = ffi.new('GLuint[1]')
            gl.glGenTextures(1, id)
            self.id = id[0]
        end
        gl.glBindTexture(gl.GL_TEXTURE_2D, self.id)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MIN_FILTER, gl.GL_NEAREST)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MAG_FILTER, gl.GL_NEAREST)
        gl.glPixelStorei(gl.GL_UNPACK_ALIGNMENT, 4)
        gl.glTexImage2D(gl.GL_TEXTURE_2D, 0, gl.GL_RGBA, w, h, 0, gl.GL_RGBA, gl.GL_UNSIGNED_BYTE, pix)
        self.w, self.h = w, h
    end
    function t:draw(zoom)
        if self.id and self.w then imgui.Image(tonumber(self.id), self.w * zoom, self.h * zoom) end
    end
    function t:delete()
        if not self.id then return end
        local id = ffi.new('GLuint[1]', self.id)
        gl.glDeleteTextures(1, id)
        self.id = nil
    end
    return t
end

-- Reads `count` pixels of `bpp` bits starting at `byteOffset` into `data`,
-- calling put(index, value).
local function readPixels(data, size, byteOffset, bpp, count, put)
    if bpp >= 8 then
        local step = bpp / 8
        for i = 0, count - 1 do
            local p = byteOffset + i * step
            if p + step > size then return end
            local v
            if bpp == 8 then
                v = data[p]
            elseif bpp == 16 then
                v = data[p] + data[p + 1] * 256
            else
                v = data[p] + data[p + 1] * 256 + data[p + 2] * 65536
            end
            put(i, v)
        end
    else
        local perByte = 8 / bpp
        local mask = (1 << bpp) - 1
        for i = 0, count - 1 do
            local p = byteOffset + math.floor(i / perByte)
            if p >= size then return end
            local shift = (i % perByte) * bpp
            put(i, (data[p] >> shift) & mask)
        end
    end
end

-- Reads the first `limit` bytes of the file, or all of it if it is shorter.
local function readHead(file, limit)
    local size = math.min(file:size(), limit)
    return file:readAt(size, 0), size
end

local timBpp = { [0] = 4, [1] = 8, [2] = 16, [3] = 24 }

-- Returns a description of the TIM in `file`, or nil if it is not structurally
-- one. A TIM whose pixel block claims more bytes than the file holds is
-- returned with truncated = true.
function PCSX.FileViewers.parseTim(file)
    local size = file:size()
    if size < 20 or file:readU32At(0) ~= 0x10 then return nil end
    local flags = file:readU32At(4)
    if (flags & ~0xf) ~= 0 then return nil end
    local tim = { bpp = timBpp[flags & 3], hasClut = (flags & 8) ~= 0, size = size }
    local off = 8
    if tim.hasClut then
        local len = file:readU32At(off)
        local w, h = file:readU16At(off + 8), file:readU16At(off + 10)
        if len ~= 12 + w * h * 2 or off + len > size then return nil end
        tim.clut = { x = file:readU16At(off + 4), y = file:readU16At(off + 6), w = w, h = h, offset = off + 12 }
        off = off + len
    end
    if off + 12 > size then return nil end
    local len = file:readU32At(off)
    local w, h = file:readU16At(off + 8), file:readU16At(off + 10)
    if w == 0 or h == 0 then return nil end
    local expected = 12 + w * h * 2
    if len ~= expected then
        -- Some games ship a pixel block length field larger than the pixels,
        -- with w * h ending exactly at the end of the file. Accept that case
        -- and nothing looser.
        if off + expected ~= size then return nil end
        tim.badLength = len
    end
    tim.pix = { x = file:readU16At(off + 4), y = file:readU16At(off + 6), w = w, h = h, offset = off + 12 }
    tim.truncated = off + expected > size
    tim.width = math.floor(w * 16 / tim.bpp)
    tim.height = h
    tim.tooLarge = tim.width > MAX_DIM or tim.height > MAX_DIM
    if tim.clut and tim.bpp <= 8 then
        tim.paletteSize = tim.bpp == 4 and 16 or 256
        tim.palettes = math.max(1, math.floor(tim.clut.w * tim.clut.h / tim.paletteSize))
    end
    return tim
end

local function timInfo(tim)
    local lines = {
        string.format('%d bpp, %dx%d pixels, VRAM %d,%d', tim.bpp, tim.width, tim.height, tim.pix.x, tim.pix.y),
    }
    if tim.clut then
        lines[#lines + 1] = string.format('CLUT block %dx%d at VRAM %d,%d', tim.clut.w, tim.clut.h, tim.clut.x, tim.clut.y)
    end
    if tim.badLength then
        lines[#lines + 1] = string.format('Pixel block length field says %d, the pixels take %d.',
            tim.badLength, tim.pix.w * tim.pix.h * 2 + 12)
    end
    if tim.truncated then
        lines[#lines + 1] = string.format('Pixel block needs %d bytes, the file has %d.',
            tim.pix.w * tim.pix.h * 2 + 12, tim.size - tim.pix.offset + 12)
    end
    if tim.tooLarge then
        lines[#lines + 1] = string.format('Larger than %dx%d, not displayed.', MAX_DIM, MAX_DIM)
    end
    return table.concat(lines, '\n')
end

local function decodeTim(file, tim, palette)
    local data, size = readHead(file, tim.pix.offset + tim.pix.w * tim.pix.h * 2)
    local w, h = tim.width, tim.height
    local pix = ffi.new('uint32_t[?]', w * h)
    local lut
    if tim.paletteSize then
        lut = {}
        local base = tim.clut.offset + palette * tim.paletteSize * 2
        for i = 0, tim.paletteSize - 1 do
            local p = base + i * 2
            lut[i] = p + 1 < size and psxColour(data[p] + data[p + 1] * 256) or 0
        end
    end
    readPixels(data, size, tim.pix.offset, tim.bpp, w * h, function(i, v)
        if lut then
            pix[i] = lut[v] or 0
        elseif tim.bpp == 16 then
            pix[i] = psxColour(v)
        elseif tim.bpp == 24 then
            pix[i] = 0xff000000 | v
        else
            pix[i] = grey(v, (1 << tim.bpp) - 1)
        end
    end)
    return pix, w, h
end

-- Viewer for a structurally valid TIM. openFile() returns the file to decode.
function PCSX.FileViewers.timViewer(openFile, tim)
    local v = { name = 'TIM', palette = 0, zoom = 2, texture = newTexture() }
    function v.draw()
        imgui.TextUnformatted(timInfo(tim))
        if tim.truncated or tim.tooLarge then return end
        local dirty = not v.uploaded
        if tim.palettes and tim.palettes > 1 then
            local changed, n = imgui.SliderInt('palette', v.palette, 0, tim.palettes - 1)
            if changed then v.palette, dirty = n, true end
        end
        local zc, z = imgui.SliderInt('zoom', v.zoom, 1, 8)
        if zc then v.zoom = z end
        if dirty then
            local ok, err = pcall(function() v.texture:upload(decodeTim(openFile(), tim, v.palette)) end)
            v.err = not ok and tostring(err) or nil
            v.uploaded = true
        end
        if v.err then imgui.TextUnformatted('Error: ' .. v.err) end
        v.texture:draw(v.zoom)
    end
    function v.close() v.texture:delete() end
    return v
end

local rawBpps = { 1, 2, 4, 8, 16, 24 }

-- Viewer for arbitrary data with a user-supplied geometry.
function PCSX.FileViewers.rawViewer(openFile)
    local v = { name = 'Raw image', bppIndex = 4, width = 256, height = 0, header = 0, zoom = 2, texture = newTexture() }
    function v.draw()
        local dirty = not v.uploaded
        for i, b in ipairs(rawBpps) do
            if i > 1 then imgui.SameLine() end
            if imgui.RadioButton(b .. ' bpp', v.bppIndex == i) then
                if v.bppIndex ~= i then v.bppIndex, dirty = i, true end
            end
        end
        imgui.PushItemWidth(200)
        local c, n
        c, n = imgui.InputInt('width', v.width, 1, 16)
        if c then v.width, dirty = math.max(1, math.min(MAX_DIM, n)), true end
        c, n = imgui.InputInt('height (0 = fit)', v.height, 1, 16)
        if c then v.height, dirty = math.max(0, math.min(MAX_DIM, n)), true end
        c, n = imgui.InputInt('header bytes', v.header, 1, 16)
        if c then v.header, dirty = math.max(0, n), true end
        c, n = imgui.SliderInt('zoom', v.zoom, 1, 8)
        if c then v.zoom = n end
        imgui.PopItemWidth()
        if dirty then
            local ok, err = pcall(function()
                local file = openFile()
                local bpp = rawBpps[v.bppIndex]
                local w = v.width
                local available = math.max(0, file:size() - v.header)
                local h = v.height
                if h == 0 then h = math.ceil(available * 8 / bpp / w) end
                h = math.max(1, math.min(MAX_DIM, h))
                local data, size = readHead(file, v.header + math.ceil(w * h * bpp / 8))
                local pix = ffi.new('uint32_t[?]', w * h)
                local max = bpp <= 8 and ((1 << bpp) - 1) or nil
                readPixels(data, size, v.header, bpp, w * h, function(i, val)
                    if bpp == 16 then
                        pix[i] = 0xff000000 | psxColour(val)
                    elseif bpp == 24 then
                        pix[i] = 0xff000000 | val
                    else
                        pix[i] = grey(val, max)
                    end
                end)
                v.texture:upload(pix, w, h)
                v.info = string.format('%dx%d, %d bytes after the header', w, h, available)
            end)
            v.err = not ok and tostring(err) or nil
            v.uploaded = true
        end
        if v.info then imgui.TextUnformatted(v.info) end
        if v.err then imgui.TextUnformatted('Error: ' .. v.err) end
        v.texture:draw(v.zoom)
    end
    function v.close() v.texture:delete() end
    return v
end

local soundFormats = { 'SPU ADPCM', 'PCM 16 bits', 'PCM 8 bits' }
local MAX_SOUND_BYTES = 16 * 1024 * 1024
local MAX_PLOT_POINTS = 200000

-- Decodes the sound described by the viewer's knobs into one int16_t array
-- per channel. Returns the arrays, the number of samples per channel, the
-- bytes to hand to PCSX.SPU.playAudio, and an info string.
local function decodeSound(file, v)
    local available = math.max(0, file:size() - v.offset)
    local length = v.length == 0 and available or math.min(v.length, available)
    local capped = length > MAX_SOUND_BYTES
    if capped then length = MAX_SOUND_BYTES end
    local suffix = capped and ', first 16 MB only' or ''
    if v.format == 1 then
        local blocks = math.floor(length / 16)
        local data = file:readAt(blocks * 16, v.offset)
        local endBlock
        for i = 0, blocks - 1 do
            if (data.data[i * 16 + 1] & 1) ~= 0 then
                endBlock = i
                break
            end
        end
        if v.stopAtEnd and endBlock then blocks = endBlock + 1 end
        local samples = ffi.new('int16_t[?]', blocks * 28 + 1)
        local decoder = PCSX.Adpcm.NewDecoder()
        for i = 0, blocks - 1 do decoder:decodeSPUBlock(data.data + i * 16, samples + i * 28) end
        local loops = endBlock and (data.data[endBlock * 16 + 1] & 2) ~= 0
        local info = string.format('%d blocks, end flag %s%s%s', blocks,
            endBlock and ('on block ' .. endBlock) or 'not found', loops and ' (repeat)' or '', suffix)
        -- Play exactly what is plotted, once: drop the end and repeat flags,
        -- then end on the last block.
        for i = 0, blocks - 1 do
            local p = data.data + i * 16 + 1
            p[0] = p[0] & ~3
        end
        if blocks > 0 then data.data[(blocks - 1) * 16 + 1] = data.data[(blocks - 1) * 16 + 1] | 1 end
        data.size = blocks * 16
        return { samples }, blocks * 28, data, info
    end
    local bytes = v.format == 2 and 2 or 1
    local frame = bytes * v.channels
    local count = math.floor(length / frame)
    local data = file:readAt(count * frame, v.offset)
    local src = data.data
    local chans = {}
    for c = 1, v.channels do chans[c] = ffi.new('int16_t[?]', count + 1) end
    for i = 0, count - 1 do
        for c = 1, v.channels do
            local p = i * frame + (c - 1) * bytes
            local s
            if bytes == 2 then
                s = src[p] + src[p + 1] * 256
                if s >= 32768 then s = s - 65536 end
            elseif v.signed then
                s = src[p]
                if s >= 128 then s = s - 256 end
                s = s * 256
            else
                s = (src[p] - 128) * 256
            end
            chans[c][i] = s
        end
    end
    return chans, count, data, string.format('%d bytes%s', count * frame, suffix)
end

-- Returns the samples to plot and the number of samples each point covers.
-- Long sounds are reduced to a min / max envelope.
local function plotPoints(samples, count)
    if count <= MAX_PLOT_POINTS then return samples, count, 1 end
    local bucket = math.ceil(count * 2 / MAX_PLOT_POINTS)
    local buckets = math.ceil(count / bucket)
    local out = ffi.new('int16_t[?]', buckets * 2)
    for b = 0, buckets - 1 do
        local lo, hi = 32767, -32768
        for i = b * bucket, math.min(count, (b + 1) * bucket) - 1 do
            local s = samples[i]
            if s < lo then lo = s end
            if s > hi then hi = s end
        end
        out[b * 2], out[b * 2 + 1] = lo, hi
    end
    return out, buckets * 2, bucket / 2
end

-- Viewer for audio data: SPU ADPCM, or raw PCM, from a user-supplied offset.
-- `opts` may preset format ('spu', 'pcm16' or 'pcm8'), offset, length and
-- rate.
function PCSX.FileViewers.soundViewer(openFile, opts)
    opts = opts or {}
    local formatIndex = { spu = 1, pcm16 = 2, pcm8 = 3 }
    local v = {
        name = 'Sound',
        format = formatIndex[opts.format] or 1,
        channels = 1,
        signed = false,
        offset = opts.offset or 0,
        length = opts.length or 0,
        rate = opts.rate or 22050,
        stopAtEnd = true,
        generation = 0,
    }
    local function stop()
        if v.sound then v.sound:stop() end
        v.sound = nil
    end
    function v.draw()
        local dirty = not v.decoded
        for i, name in ipairs(soundFormats) do
            if i > 1 then imgui.SameLine() end
            if imgui.RadioButton(name, v.format == i) and v.format ~= i then v.format, dirty = i, true end
        end
        if v.format == 1 then
            local c, b = imgui.Checkbox('stop at the end flag', v.stopAtEnd)
            if c then v.stopAtEnd, dirty = b, true end
        else
            if imgui.RadioButton('mono', v.channels == 1) and v.channels ~= 1 then v.channels, dirty = 1, true end
            imgui.SameLine()
            if imgui.RadioButton('stereo', v.channels == 2) and v.channels ~= 2 then v.channels, dirty = 2, true end
            if v.format == 3 then
                imgui.SameLine()
                local c, b = imgui.Checkbox('signed', v.signed)
                if c then v.signed, dirty = b, true end
            end
        end
        imgui.PushItemWidth(200)
        local c, n
        c, n = imgui.InputInt('data offset', v.offset, 1, 16)
        if c then v.offset, dirty = math.max(0, n), true end
        c, n = imgui.InputInt('length (0 = to the end)', v.length, 1, 16)
        if c then v.length, dirty = math.max(0, n), true end
        c, n = imgui.InputInt('sample rate', v.rate, 100, 1000)
        if c then
            stop()
            v.rate = math.max(1, math.min(v.format == 1 and 176400 or 384000, n))
            v.generation = v.generation + 1
        end
        imgui.PopItemWidth()
        if dirty then
            stop()
            local ok, chans, count, bytes, info = pcall(decodeSound, openFile(), v)
            if ok then
                v.chans, v.count, v.bytes, v.info, v.err = chans, count, bytes, info, nil
                v.plots = {}
                for i, samples in ipairs(chans) do v.plots[i] = { plotPoints(samples, count) } end
            else
                v.chans, v.err = nil, tostring(chans)
            end
            v.decoded = true
            v.generation = v.generation + 1
        end
        if v.err then
            imgui.TextUnformatted('Error: ' .. v.err)
            return
        end
        if imgui.Button('Play') and v.count > 0 then
            stop()
            local ok, err = pcall(function()
                local desc
                if v.format == 1 then
                    desc = { format = 'spu', rate = v.rate }
                else
                    desc = { format = 'pcm', bits = v.format == 2 and 16 or 8, channels = v.channels, rate = v.rate }
                    if v.format == 3 then desc.signed = v.signed end
                end
                v.sound = PCSX.SPU.playAudio(v.bytes, desc)
            end)
            v.playErr = not ok and tostring(err) or nil
        end
        imgui.SameLine()
        if imgui.Button('Stop') then stop() end
        if v.sound and v.sound:isPlaying() then
            imgui.SameLine()
            imgui.TextUnformatted('playing')
        end
        if v.playErr then imgui.TextUnformatted('Error: ' .. v.playErr) end
        imgui.TextUnformatted(string.format('%s, %d samples, %.3f seconds', v.info, v.count, v.count / v.rate))
        if v.count == 0 then return end
        if not implot then return end
        -- The generation is part of the id, so a change of data or rate
        -- refits the axes while leaving the user free to zoom otherwise.
        implot.safe.BeginPlot('##waveform' .. v.generation, -1, 250, function()
            implot.SetupAxes('seconds', '')
            implot.SetupAxesLimits(0, v.count / v.rate, -32768, 32767)
            for i, p in ipairs(v.plots) do
                implot.PlotLine(#v.plots == 1 and 'samples' or (i == 1 and 'left' or 'right'), p[1], p[2],
                    p[3] / v.rate, 0)
            end
        end)
    end
    function v.close() stop() end
    return v
end

local MAX_TEXT_BYTES = 4 * 1024 * 1024

local function readWholeFile(path)
    local f = Support.File.open(path)
    if f:failed() then
        f:close()
        return nil, 'cannot open ' .. path
    end
    local data = tostring(f:readAt(f:size(), 0))
    f:close()
    return data
end

-- Viewer that decodes the file's bytes to text through a PCSX.TextTable and
-- lets the text be edited and encoded back. opts, all optional:
--   table   a PCSX.TextTable to use instead of asking for a table file
--   offset, length   the byte range to decode, length 0 meaning to the end
--   onSave(bytes)    called with the encoded bytes; without it the viewer
--                    offers to write them to a file
function PCSX.FileViewers.textViewer(openFile, opts)
    opts = opts or {}
    local v = { name = 'Text', tbl = opts.table, tablePath = '', offset = opts.offset or 0,
                length = opts.length or 0, savePath = '', text = '' }
    local function encode()
        v.encoded, v.encodeErr = v.tbl:encode(v.text)
    end
    local function decode()
        local file = openFile()
        local available = math.max(0, file:size() - v.offset)
        local length = v.length == 0 and available or math.min(v.length, available)
        if length > MAX_TEXT_BYTES then
            v.decodeErr = string.format('%d bytes is more than the %d this viewer decodes', length, MAX_TEXT_BYTES)
            return
        end
        v.original = tostring(file:readAt(length, v.offset))
        v.text = v.tbl:decode(v.original)
        v.decodeErr = nil
        encode()
    end
    function v.draw()
        local c, n
        if not opts.table then
            c, n = imgui.extra.InputText('table file', v.tablePath)
            if c then v.tablePath = n end
            imgui.SameLine()
            if imgui.Button('Load table') then
                local src, err = readWholeFile(v.tablePath)
                local tbl
                if src then tbl, err = PCSX.TextTable.parse(src) end
                v.tableErr = err
                if tbl then
                    v.tbl = tbl
                    v.dirty = true
                end
            end
            if v.tableErr then imgui.TextUnformatted('Error: ' .. v.tableErr) end
        end
        if not v.tbl then
            imgui.TextUnformatted('Load a table file to decode this file as text.')
            return
        end
        imgui.PushItemWidth(200)
        c, n = imgui.InputInt('offset', v.offset, 1, 16)
        if c then v.offset, v.dirty = math.max(0, n), true end
        imgui.SameLine()
        c, n = imgui.InputInt('length (0 = to the end)', v.length, 1, 16)
        if c then v.length, v.dirty = math.max(0, n), true end
        imgui.PopItemWidth()
        if v.dirty or not v.original then
            local ok, err = pcall(decode)
            if not ok then v.decodeErr = tostring(err) end
            v.dirty = false
        end
        if v.decodeErr then
            imgui.TextUnformatted('Error: ' .. v.decodeErr)
            return
        end
        if v.encodeErr then
            imgui.TextUnformatted('Cannot encode: ' .. v.encodeErr)
        elseif v.encoded == v.original then
            imgui.TextUnformatted(string.format('%d bytes, unchanged', #v.encoded))
        else
            imgui.TextUnformatted(string.format('%d bytes, was %d', #v.encoded, #v.original))
        end
        if v.encoded and not v.encodeErr then
            if opts.onSave then
                imgui.SameLine()
                if imgui.Button('Apply') then opts.onSave(v.encoded) end
            else
                imgui.PushItemWidth(300)
                c, n = imgui.extra.InputText('save to', v.savePath)
                imgui.PopItemWidth()
                if c then v.savePath = n end
                imgui.SameLine()
                if imgui.Button('Save') and v.savePath ~= '' then
                    -- io rather than Support.File: its writes are synchronous,
                    -- so an error is known before the result is shown.
                    local f, err = io.open(v.savePath, 'wb')
                    local ok
                    if f then
                        ok, err = f:write(v.encoded)
                        local closed, closeErr = f:close()
                        if ok and not closed then ok, err = nil, closeErr end
                    end
                    if ok then
                        v.saveInfo = string.format('wrote %d bytes to %s', #v.encoded, v.savePath)
                    else
                        v.saveInfo = 'cannot write: ' .. tostring(err)
                    end
                end
                if v.saveInfo then imgui.TextUnformatted(v.saveInfo) end
            end
        end
        c, n = imgui.extra.InputTextMultiline('##text', v.text, -1, -1)
        if c then
            v.text = n
            encode()
        end
    end
    function v.close() end
    return v
end

-- Entry point for the ISO browser. `file` is the LuaFile pointer it hands
-- over. Returns an object with draw() and close().
function PCSX.FileViewers.open(file)
    file = Support.File._createFileWrapper(ffi.cast('LuaFile*', file))
    local function openFile() return file end
    local viewers = {}
    local tim = PCSX.FileViewers.parseTim(file)
    if tim then viewers[#viewers + 1] = PCSX.FileViewers.timViewer(openFile, tim) end
    viewers[#viewers + 1] = PCSX.FileViewers.rawViewer(openFile)
    if PCSX.TextTable then viewers[#viewers + 1] = PCSX.FileViewers.textViewer(openFile) end
    if PCSX.Adpcm and PCSX.SPU and PCSX.SPU.playAudio then
        viewers[#viewers + 1] = PCSX.FileViewers.soundViewer(openFile)
    end
    local ret = {}
    function ret.draw()
        imgui.safe.BeginTabBar('viewers', function()
            for _, v in ipairs(viewers) do
                imgui.safe.BeginTabItem(v.name, function()
                    imgui.safe.BeginChild('view', 0, 0, 0, imgui.constant.WindowFlags.HorizontalScrollbar, v.draw)
                end)
            end
        end)
    end
    function ret.close()
        for _, v in ipairs(viewers) do v.close() end
        file:close()
    end
    return ret
end
