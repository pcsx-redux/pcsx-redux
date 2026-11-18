--   Copyright (C) 2024 PCSX-Redux authors
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

TestAdpcm = {}

local function swapEndian32(x)
    return bit.bor(bit.lshift(bit.band(x, 0x000000FF), 24), bit.lshift(bit.band(x, 0x0000FF00), 8),
                   bit.rshift(bit.band(x, 0x00FF0000), 8), bit.rshift(bit.band(x, 0xFF000000), 24))
end

local function generateToneSample(frequency, sampleRate, t) return math.sin(2 * math.pi * frequency * t / sampleRate) end

local function generateDTMF(frequency1, frequency2, sampleRate, t)
    return generateToneSample(frequency1, sampleRate, t) + generateToneSample(frequency2, sampleRate, t)
end

local DTMFFrequencies = {
    { 697, 1209 }, { 697, 1336 }, { 697, 1477 }, { 697, 1633 }, { 770, 1209 }, { 770, 1336 }, { 770, 1477 },
    { 770, 1633 }, { 852, 1209 }, { 852, 1336 }, { 852, 1477 }, { 852, 1633 }, { 941, 1209 }, { 941, 1336 },
    { 941, 1477 }, { 941, 1633 },
}

local function generateMonoWaveform(frequency1, frequency2, sampleRate, duration)
    local size = duration * sampleRate
    local samples = ffi.new('int16_t[?]', size)
    for t = 0, duration * sampleRate - 1 do samples[t] = 10000 * generateDTMF(frequency1, frequency2, sampleRate, t) end
    return samples, size
end

local function generateStereoWaveform(frequency1, frequency2, frequency3, frequency4, sampleRate, duration)
    local size = duration * sampleRate
    local samples = ffi.new('int16_t[?]', size * 2)
    for t = 0, duration * sampleRate - 1 do
        samples[2 * t + 0] = 10000 * generateDTMF(frequency1, frequency2, sampleRate, t)
        samples[2 * t + 1] = 10000 * generateDTMF(frequency3, frequency4, sampleRate, t)
    end
    return samples, size
end

local function generateDTMF1(sampleRate, duration)
    local samples, size = generateMonoWaveform(DTMFFrequencies[1][1], DTMFFrequencies[1][2], sampleRate, duration)
    lu.assertEquals(size % 28, 0)
    return samples, size
end

local function generateDTMFStereo(sampleRate, duration)
    local samples, size = generateStereoWaveform(DTMFFrequencies[1][1], DTMFFrequencies[1][2], DTMFFrequencies[6][1],
                                                 DTMFFrequencies[6][2], sampleRate, duration)
    lu.assertEquals(size % 28, 0)
    return samples, size
end

function TestAdpcm:test_simpleSPU()
    local sampleRate = 44100
    local duration = 1
    local samples, size = generateDTMF1(sampleRate, duration)
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'Normal'
    local blockCount = size / 28
    local ptr = ffi.cast('int16_t *', samples)
    local file = Support.File.buffer()
    file:write('VAGp')
    file:writeU32(0)
    file:writeU32(0)
    file:writeU32(swapEndian32((blockCount + 4) * 16))
    file:writeU32(swapEndian32(sampleRate))
    for i = 1, 11 do file:writeU32(0) end
    local out = Support.NewLuaBuffer(16)
    for i = 1, blockCount do
        e:processSPUBlock(ptr, out, i == blockCount and 'OneShotEnd' or 'OneShot')
        ptr = ptr + 28
        file:write(out)
    end
    e:finishSPU(out)
    file:write(out)
    file:close()
end

function TestAdpcm:test_simpleXA()
    local sampleRate = 37800
    local duration = 20
    local samples, size = generateDTMFStereo(sampleRate, duration)
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'XA'
    local blockCount = size / 112
    local ptr = ffi.cast('int16_t *', samples)
    local file = Support.File.buffer()
    local out = Support.NewLuaBuffer(128)
    for i = 1, blockCount do
        if (i % 18) == 1 then
            file:writeU8(0)
            file:writeU32(0xffffffff)
            file:writeU32(0xffffffff)
            file:writeU16(0xffff)
            file:writeU32(0)
            file:writeU8(2)
            file:writeU32(0x01640001)
            file:writeU32(0x01640001)
        end
        e:processXABlock(ptr, out, 'XAFourBits', 2)
        ptr = ptr + 112 * 2
        file:write(out)
        if (i % 18) == 0 then for j = 1, 6 do file:writeU32(0) end end
    end
    file:close()
end

-- Decoder tests

local function makeBlock(bytes)
    local block = ffi.new('uint8_t[16]')
    for i = 1, 16 do block[i - 1] = bytes[i] or 0 end
    return block
end

local function assertSamples(out, expected, what)
    for i = 1, #expected do lu.assertEquals(out[i - 1], expected[i], what .. ': sample ' .. (i - 1)) end
end

-- Expected values below were computed from the psx-spx formula:
--   s = (sign_extend(nibble) << 12 >> shift) + ((old * pos[filter] + older * neg[filter] + 32) >> 6)
--   pos = { 0, 60, 115, 98, 122 }, neg = { 0, 0, -52, -55, -60 }, then clamped to 16 bits.
-- Worked by hand for filter 1, shift 0, nibbles 7, -8, 0:
--   s0 = 7 * 4096 = 28672
--   s1 = -32768 + ((28672 * 60 + 32) >> 6) = -32768 + 26880 = -5888
--   s2 = 0 + ((-5888 * 60 + 32) >> 6) = floor(-353248 / 64) = -5520
local handBlocks = {
    {
        filter = 0,
        block = { 0x04, 0, 0x87, 0x21, 0xf3, 0x00, 0x5c },
        expected = {
            1792, -2048, 256, 512, 768, -256, 0, 0, -1024, 1280, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        },
    }, {
        filter = 1,
        block = { 0x10, 0, 0x87 },
        expected = {
            28672, -5888, -5520, -5175, -4852, -4549, -4265, -3998, -3748, -3514, -3294, -3088, -2895, -2714, -2544,
            -2385, -2236, -2096, -1965, -1842, -1727, -1619, -1518, -1423, -1334, -1251, -1173, -1100,
        },
    }, {
        filter = 2,
        block = { 0x22, 0, 0x87, 0x10 },
        expected = {
            7168, 4688, 2600, 1887, 1278, 763, 333, -22, -310, -539, -717, -850, -945, -1007, -1042, -1054, -1047,
            -1025, -991, -948, -898, -843, -785, -726, -667, -609, -552, -497,
        },
    }, {
        filter = 3,
        block = { 0x31, 0, 0x97, 0x00, 0x0f },
        expected = {
            14336, 7616, -658, -7553, -13048, -13489, -9442, -2866, 3726, 8168, 9305, 7229, 3073, -1507, -4948, -6282,
            -5367, -2820, 294, 2874, 4148, 3882, 2380, 308, -1574, -2675, -2743, -1901,
        },
    }, {
        -- Also exercises clamping on both ends.
        filter = 4,
        block = { 0x40, 0, 0x77, 0x88 },
        expected = {
            28672, 32767, 2814, -32768, -32768, -31744, -29792, -27031, -23598, -19642, -15319, -10787, -6201, -1708,
            2558, 6477, 9949, 12893, 15250, 16983, 18077, 18538, 18391, 17678, 16457, 14798, 12780, 10489,
        },
    },
}

function TestAdpcm:test_decodeSPUHandBuilt()
    local d = PCSX.Adpcm.NewDecoder()
    local out = ffi.new('int16_t[28]')
    for _, t in ipairs(handBlocks) do
        d:reset()
        local _, flags = d:decodeSPUBlock(makeBlock(t.block), out)
        lu.assertEquals(flags, 0)
        assertSamples(out, t.expected, 'filter ' .. t.filter)
    end
end

function TestAdpcm:test_decodeSPUHistoryAndFlags()
    local d = PCSX.Adpcm.NewDecoder()
    local out = ffi.new('int16_t[28]')
    d:reset()
    d:decodeSPUBlock(makeBlock(handBlocks[2].block), out)
    -- An all-zero filter 1 block continues decaying from the previous block's last sample, -1100.
    local _, flags = d:decodeSPUBlock(makeBlock({ 0x10, 0x03 }), out)
    lu.assertEquals(flags, 3)
    assertSamples(out, { -1031, -967, -907, -850, -797, -747 }, 'history carry')
    -- Reserved shift 13 acts as shift 9: 7 * 4096 >> 9 = 56.
    d:reset()
    d:decodeSPUBlock(makeBlock({ 0x0d, 0x06, 0x07 }), out)
    assertSamples(out, { 56, 0 }, 'shift 13')
    -- String input, and default output buffer.
    d:reset()
    local buf, f = d:decodeSPUBlock(string.char(0x10, 0x04, 0x87) .. string.rep('\0', 13))
    lu.assertEquals(#buf, 56)
    lu.assertEquals(f, 4)
    assertSamples(ffi.cast('int16_t*', buf.data), { 28672, -5888, -5520 }, 'string input')
end

local function measureError(reference, decoded, count, what)
    local maxErr = 0
    local signal = 0
    local noise = 0
    for i = 0, count - 1 do
        local r = reference[i]
        local e = math.abs(decoded[i] - r)
        if e > maxErr then maxErr = e end
        signal = signal + r * r
        noise = noise + e * e
    end
    local snr = noise == 0 and math.huge or 10 * math.log10(signal / noise)
    print(string.format('ADPCM round trip %s: max abs error %d, SNR %.2f dB', what, maxErr, snr))
    return maxErr, snr
end

-- Bounds measured with the current encoder and decoder, with some margin. See the printed values.
-- Measured: spu 277 / 52.89 dB, xa4 mono 279 / 48.74 dB, xa4 stereo 288 / 48.56 dB,
-- xa8 mono 19 / 67.14 dB, xa8 stereo 20 / 66.92 dB.
-- A decoder layout mistake would bring the SNR down to about 0 dB.
local roundTripBounds = {
    spu = { maxErr = 400, snr = 50 },
    xa4mono = { maxErr = 400, snr = 45 },
    xa4stereo = { maxErr = 400, snr = 45 },
    xa8mono = { maxErr = 100, snr = 60 },
    xa8stereo = { maxErr = 100, snr = 60 },
}

local function checkBounds(name, maxErr, snr)
    local b = roundTripBounds[name]
    lu.assertTrue(maxErr <= b.maxErr, name .. ': max abs error ' .. maxErr .. ' exceeds ' .. b.maxErr)
    lu.assertTrue(snr >= b.snr, name .. ': SNR ' .. snr .. ' below ' .. b.snr)
end

function TestAdpcm:test_processBlockReturnsFilterAndShift()
    local samples = ffi.new('int16_t[56]')
    for t = 0, 55 do samples[t] = (t % 2 == 0) and 1000 or -1000 end
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'FourBits'
    local out, filter, shift = e:processBlock(samples)
    lu.assertEquals(filter, 0)
    lu.assertEquals(shift, 4)
    e:reset 'FourBits'
    local _, filterL, shiftL, filterR, shiftR = e:processBlock(samples, 2)
    lu.assertEquals({ filterL, shiftL, filterR, shiftR }, { 0, 4, 0, 4 })
end

function TestAdpcm:test_processSPUBlockAttributeAsSecondArgument()
    local samples = ffi.new('int16_t[28]')
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'Normal'
    local out = e:processSPUBlock(samples, 'LoopEnd')
    lu.assertEquals(out.data[1], 0x03)
end

-- Full scale input, expected blocks taken from Sony's encvag.dll output for the same input.
function TestAdpcm:test_encodeFullScaleSPU()
    local cases = {
        { value = 32767, expected = '\x00\x00' .. string.rep('\x77', 14) },
        { value = -32768, expected = '\x01\x00' .. string.rep('\x88', 14) },
    }
    for _, c in ipairs(cases) do
        local samples = ffi.new('int16_t[56]')
        for t = 28, 55 do samples[t] = c.value end
        local out = ffi.new('uint8_t[32]')
        local e = PCSX.Adpcm.NewEncoder()
        e:reset 'FourBits'
        e:processSPUBlock(samples, out, 'OneShot')
        e:processSPUBlock(samples + 28, out + 16, 'OneShot')
        lu.assertEquals(ffi.string(out + 16, 16), c.expected)
    end
end

function TestAdpcm:test_roundTripSPU()
    local blockCount = 1575
    local size = 28 * blockCount
    local samples = ffi.new('int16_t[?]', size)
    for t = 0, size - 1 do samples[t] = 10000 * generateDTMF(697, 1209, 44100, t) end
    local encoded = ffi.new('uint8_t[?]', blockCount * 16)
    local decoded = ffi.new('int16_t[?]', size)
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'Normal'
    for i = 0, blockCount - 1 do e:processSPUBlock(samples + i * 28, encoded + i * 16, 'OneShot') end
    local d = PCSX.Adpcm.NewDecoder()
    d:reset()
    for i = 0, blockCount - 1 do d:decodeSPUBlock(encoded + i * 16, decoded + i * 28) end
    checkBounds('spu', measureError(samples, decoded, size, 'spu'))
end

local function roundTripXA(bits, channels)
    local name = 'xa' .. bits .. (channels == 1 and 'mono' or 'stereo')
    local perGroup = (bits == 4 and 224 or 112)
    local groups = 300
    local total = perGroup * groups
    local frames = total / channels
    local samples = ffi.new('int16_t[?]', total)
    for t = 0, frames - 1 do
        if channels == 1 then
            samples[t] = 10000 * generateDTMF(697, 1209, 37800, t)
        else
            samples[2 * t + 0] = 10000 * generateDTMF(697, 1209, 37800, t)
            samples[2 * t + 1] = 10000 * generateDTMF(770, 1336, 37800, t)
        end
    end
    local encoded = ffi.new('uint8_t[?]', groups * 128)
    local decoded = ffi.new('int16_t[?]', total)
    local e = PCSX.Adpcm.NewEncoder()
    e:reset 'XA'
    for g = 0, groups - 1 do
        e:processXABlock(samples + g * perGroup, encoded + g * 128, bits == 4 and 'XAFourBits' or 'XAEightBits',
                         channels)
    end
    local d = PCSX.Adpcm.NewDecoder()
    d:reset()
    for g = 0, groups - 1 do
        local _, written = d:decodeXASoundGroup(encoded + g * 128, decoded + g * perGroup, bits, channels)
        lu.assertEquals(written, perGroup)
    end
    checkBounds(name, measureError(samples, decoded, total, name))
end

function TestAdpcm:test_roundTripXA4Mono() roundTripXA(4, 1) end
function TestAdpcm:test_roundTripXA4Stereo() roundTripXA(4, 2) end
function TestAdpcm:test_roundTripXA8Mono() roundTripXA(8, 1) end
function TestAdpcm:test_roundTripXA8Stereo() roundTripXA(8, 2) end

-- Packs interleaved samples into XA sound groups using only filter 0, choosing the finest shift that
-- fits each unit. With filter 0 there is no prediction, so the exact decoded output is known: each
-- value decodes to code * 2^(12 - shift) for 4-bit, or code * 2^(8 - shift) for 8-bit.
local function packXAFilter0(samples, bits, channels, groups, expected)
    local perGroup = bits == 4 and 224 or 112
    local units = bits == 4 and 8 or 4
    local maxShift = bits == 4 and 12 or 8
    local minCode = bits == 4 and -8 or -128
    local maxCode = bits == 4 and 7 or 127
    local out = ffi.new('uint8_t[?]', groups * 128)
    for g = 0, groups - 1 do
        local group = out + g * 128
        for u = 0, units - 1 do
            local function index(i)
                if channels == 1 then return g * perGroup + u * 28 + i end
                return g * perGroup + math.floor(u / 2) * 56 + i * 2 + (u % 2)
            end
            local shift
            for sh = maxShift, 0, -1 do
                local step = 2 ^ (maxShift - sh)
                local fits = true
                for i = 0, 27 do
                    local c = math.floor(samples[index(i)] / step + 0.5)
                    if c < minCode or c > maxCode then
                        fits = false
                        break
                    end
                end
                if fits then
                    shift = sh
                    break
                end
            end
            lu.assertNotNil(shift)
            local step = 2 ^ (maxShift - shift)
            group[4 + u] = shift
            for i = 0, 27 do
                local c = math.floor(samples[index(i)] / step + 0.5)
                expected[index(i)] = c * step
                local word = group + 16 + i * 4
                if bits == 4 then
                    local b = math.floor(u / 2)
                    word[b] = bit.bor(word[b], bit.lshift(bit.band(c, 0x0f), (u % 2) * 4))
                else
                    word[u] = bit.band(c, 0xff)
                end
            end
        end
        -- Header copies, as per psx-spx.
        for i = 0, 3 do
            group[i] = group[4 + i]
            group[12 + i] = group[8 + i]
        end
    end
    return out
end

function TestAdpcm:test_decodeXALayout()
    for _, bits in ipairs({ 4, 8 }) do
        for _, channels in ipairs({ 1, 2 }) do
            local perGroup = bits == 4 and 224 or 112
            local groups = 4
            local total = perGroup * groups
            local samples = ffi.new('int16_t[?]', total)
            -- A different tone and amplitude per channel, so that swapped channels or units show up.
            for i = 0, total - 1 do
                local c = channels == 2 and (i % 2) or 0
                local t = math.floor(i / channels)
                samples[i] = (6000 + 5000 * c) * generateToneSample(c == 0 and 700 or 1900, 37800, t)
            end
            local expected = {}
            local packed = packXAFilter0(samples, bits, channels, groups, expected)
            local d = PCSX.Adpcm.NewDecoder()
            d:reset()
            local decoded = ffi.new('int16_t[?]', total)
            for g = 0, groups - 1 do
                local _, written = d:decodeXASoundGroup(packed + g * 128, decoded + g * perGroup, bits, channels)
                lu.assertEquals(written, perGroup)
            end
            local what = 'xa' .. bits .. ' ' .. channels .. 'ch'
            for i = 0, total - 1 do lu.assertEquals(decoded[i], expected[i], what .. ': sample ' .. i) end
            measureError(samples, decoded, total, 'filter 0 packing ' .. what)
        end
    end
end

function TestAdpcm:test_decoderArguments()
    local d = PCSX.Adpcm.NewDecoder()
    lu.assertErrorMsgContains('bitsPerSample must be 4 or 8', d.decodeXASoundGroup, d, string.rep('\0', 128), 5, 1)
    lu.assertErrorMsgContains('channels must be 1 or 2', d.decodeXASoundGroup, d, string.rep('\0', 128), 4, 3)
    lu.assertErrorMsgContains('input string too small', d.decodeXASoundGroup, d, string.rep('\0', 127), 4, 1)
    lu.assertErrorMsgContains('input string too small', d.decodeSPUBlock, d, string.rep('\0', 15))
end
