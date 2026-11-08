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

TestAudioPlayback = {}

-- Polls until the sound stops, returning the wall-clock time waited in milliseconds. Measured
-- with hrtime because a 10ms sleep can take several times that on a loaded runner.
local function waitUntilStopped(sound, timeoutMs)
    local start = luv.hrtime()
    local waited = 0
    while sound:isPlaying() and waited < timeoutMs do
        luv.sleep(10)
        waited = math.floor((luv.hrtime() - start) / 1e6)
    end
    return waited
end

local function sinePCM16(rate, ms)
    local frames = math.floor(rate * ms / 1000)
    local buf = ffi.new('int16_t[?]', frames)
    for t = 0, frames - 1 do buf[t] = 8000 * math.sin(2 * math.pi * 440 * t / rate) end
    return ffi.string(buf, frames * 2)
end

local function encodeSPU(blockCount, attributeFor)
    local samples = ffi.new('int16_t[?]', blockCount * 28)
    for t = 0, blockCount * 28 - 1 do samples[t] = 8000 * math.sin(2 * math.pi * 440 * t / 44100) end
    local out = ffi.new('uint8_t[?]', blockCount * 16)
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'Normal'
    for i = 0, blockCount - 1 do e:processSPUBlock(samples + i * 28, out + i * 16, attributeFor(i)) end
    return ffi.string(out, blockCount * 16)
end

local function encodeXAGroups(groups)
    local samples = ffi.new('int16_t[?]', groups * 224)
    for t = 0, groups * 224 - 1 do samples[t] = 8000 * math.sin(2 * math.pi * 440 * t / 37800) end
    local out = ffi.new('uint8_t[?]', groups * 128)
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'XA'
    for g = 0, groups - 1 do e:processXABlock(samples + g * 224, out + g * 128, 'XAFourBits', 1) end
    return ffi.string(out, groups * 128)
end

-- Builds sectors from 18 sound groups each. submodes is a list with one submode byte per sector;
-- channels and codings optionally give each sector's channel and coding info bytes.
local function buildSectors(raw, submodes, channels, codings)
    local groups = encodeXAGroups(18)
    local parts = {}
    for i, submode in ipairs(submodes) do
        local sector = {}
        if raw then
            table.insert(sector, '\0' .. string.rep('\255', 10) .. '\0')
            table.insert(sector, string.char(0, 2, 0, 2))
        end
        local subheader = string.char(1, channels and channels[i] or 0, submode, codings and codings[i] or 0)
        table.insert(sector, subheader .. subheader)
        table.insert(sector, groups)
        table.insert(sector, string.rep('\0', 0x14 + 4))
        local s = table.concat(sector)
        lu.assertEquals(#s, raw and 2352 or 2336)
        table.insert(parts, s)
    end
    return table.concat(parts)
end

function TestAudioPlayback:test_descriptorErrors()
    local pcm = sinePCM16(44100, 10)
    local play = PCSX.SPU.playAudio
    lu.assertErrorMsgContains('second argument must be a descriptor table', play, pcm)
    lu.assertErrorMsgContains("'format' must be one of", play, pcm, { format = 'wav' })
    lu.assertErrorMsgContains("'rate' is required", play, pcm, { format = 'pcm', bits = 16, channels = 1 })
    lu.assertErrorMsgContains("'bits' must be one of 8, 16", play, pcm,
        { format = 'pcm', bits = 12, channels = 1, rate = 44100 })
    lu.assertErrorMsgContains("'chanels' is not valid", play, pcm,
        { format = 'pcm', bits = 16, chanels = 1, rate = 44100 })
    lu.assertErrorMsgContains('16 bits pcm data must be signed', play, pcm,
        { format = 'pcm', bits = 16, channels = 1, rate = 44100, signed = false })
    lu.assertErrorMsgContains('frame size', play, 'abc', { format = 'pcm', bits = 16, channels = 1, rate = 44100 })
    lu.assertErrorMsgContains('empty', play, '', { format = 'pcm', bits = 16, channels = 1, rate = 44100 })
    lu.assertErrorMsgContains("exactly one of the descriptor fields 'rate' or 'pitch'", play, pcm, { format = 'spu' })
    lu.assertErrorMsgContains("exactly one of", play, pcm, { format = 'spu', rate = 44100, pitch = 0x1000 })
    lu.assertErrorMsgContains("'channels' is not valid for format 'spu'", play, pcm,
        { format = 'spu', rate = 44100, channels = 1 })
    lu.assertErrorMsgContains('multiple of 16 bytes', play, 'abc', { format = 'spu', pitch = 0x1000 })
    lu.assertErrorMsgContains("'rate' must be one of 37800, 18900", play, pcm,
        { format = 'xa', bits = 4, channels = 1, rate = 44100 })
    lu.assertErrorMsgContains('multiple of 128 bytes', play, 'abc', { format = 'xa', bits = 4, channels = 1, rate = 37800 })
    lu.assertErrorMsgContains('read from the subheaders', play, pcm, { format = 'xa', sectors = true, bits = 4 })
    lu.assertErrorMsgContains('multiple of 2352 bytes', play, 'abc', { format = 'xa', sectors = true })
    lu.assertErrorMsgContains('no audio sector', play, buildSectors(false, { 0x08 }), { format = 'xa', sectors = true })
    lu.assertErrorMsgContains('first argument must be', play, 42, { format = 'spu', pitch = 0x1000 })
end

function TestAudioPlayback:test_pcmString()
    local sound = PCSX.SPU.playAudio(sinePCM16(44100, 100), { format = 'pcm', bits = 16, channels = 1, rate = 44100 })
    lu.assertTrue(sound:isPlaying())
    local waited = waitUntilStopped(sound, 3000)
    print('pcm 100ms sound stopped after ' .. waited .. 'ms')
    lu.assertFalse(sound:isPlaying())
    lu.assertTrue(waited >= 50)
end

function TestAudioPlayback:test_pcmSourcesAndStop()
    local frames = 22050
    local buf = Support.NewLuaBuffer(frames * 2)
    for i = 0, frames * 2 - 1 do buf.data[i] = 128 + math.floor(100 * math.sin(i / 20)) end
    local sound = PCSX.SPU.playAudio(buf, { format = 'pcm', bits = 8, channels = 2, rate = 22050, gain = 0.5 })
    lu.assertTrue(sound:isPlaying())
    sound:setGain(0.25)
    sound:stop()
    lu.assertFalse(sound:isPlaying())
    -- Stopping twice is fine.
    sound:stop()

    local file = Support.File.buffer()
    file:write(sinePCM16(44100, 50))
    sound = PCSX.SPU.playAudio(file, { format = 'pcm', bits = 16, channels = 1, rate = 44100 })
    lu.assertTrue(sound:isPlaying())
    local pcm = sinePCM16(44100, 50)
    local slice = Support.File.createEmptySlice()
    slice.size = #pcm
    ffi.copy(slice.mutable, pcm, #pcm)
    lu.assertEquals(slice.size, #pcm)
    local sound2 = PCSX.SPU.playAudio(slice, { format = 'pcm', bits = 16, channels = 1, rate = 48000 })
    lu.assertTrue(sound2:isPlaying())
    sound, sound2 = nil, nil
    collectgarbage()
    collectgarbage()
end

function TestAudioPlayback:test_spuOneShot()
    local data = encodeSPU(160, function(i) return i == 159 and 'OneShotEnd' or 'OneShot' end)
    local sound = PCSX.SPU.playAudio(data, { format = 'spu', pitch = 0x1000 })
    lu.assertTrue(sound:isPlaying())
    local waited = waitUntilStopped(sound, 3000)
    print('spu one-shot ~100ms sound stopped after ' .. waited .. 'ms')
    lu.assertFalse(sound:isPlaying())
end

function TestAudioPlayback:test_spuLoop()
    local data = encodeSPU(16, function(i)
        if i == 0 then return 'LoopStart' end
        if i == 15 then return 'LoopEnd' end
        return 'LoopBody'
    end)
    -- 16 blocks at 44100Hz is about 10ms; still playing half a second later means it loops.
    local sound = PCSX.SPU.playAudio(data, { format = 'spu', rate = 44100 })
    local waited = waitUntilStopped(sound, 500)
    lu.assertTrue(waited >= 500)
    lu.assertTrue(sound:isPlaying())
    sound:stop()
    lu.assertFalse(sound:isPlaying())
end

function TestAudioPlayback:test_xaGroupsAndSectors()
    local sound = PCSX.SPU.playAudio(encodeXAGroups(20), { format = 'xa', bits = 4, channels = 1, rate = 37800 })
    lu.assertTrue(sound:isPlaying())
    local waited = waitUntilStopped(sound, 3000)
    print('xa groups ~120ms sound stopped after ' .. waited .. 'ms')
    lu.assertFalse(sound:isPlaying())

    -- A data sector is skipped, audio sectors are played.
    for _, raw in ipairs({ false, true }) do
        local sectors = buildSectors(raw, { 0x08, 0x64, 0x64 })
        sound = PCSX.SPU.playAudio(sectors, { format = 'xa', sectors = true })
        lu.assertTrue(sound:isPlaying())
        sound:stop()
        sound = PCSX.SPU.playAudio(sectors, { format = 'xa', sectors = true, xaFile = 1, xaChannel = 0 })
        lu.assertTrue(sound:isPlaying())
        sound:stop()
        lu.assertErrorMsgContains('no audio sector', PCSX.SPU.playAudio, sectors,
            { format = 'xa', sectors = true, xaChannel = 3 })

        -- Without a filter, only the first audio stream plays, so a stereo channel interleaved
        -- with it is skipped instead of being rejected as a mixed format.
        sectors = buildSectors(raw, { 0x64, 0x64, 0x64, 0x64 }, { 0, 1, 0, 1 }, { 0, 1, 0, 1 })
        sound = PCSX.SPU.playAudio(sectors, { format = 'xa', sectors = true })
        lu.assertTrue(sound:isPlaying())
        sound:stop()
        sound = PCSX.SPU.playAudio(sectors, { format = 'xa', sectors = true, xaChannel = 1 })
        lu.assertTrue(sound:isPlaying())
        sound:stop()
    end
end
