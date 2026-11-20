-- lualoader, R"EOF(--
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
ffi.cdef [[

typedef struct {
    int32_t format;
    uint32_t bits;
    int32_t isSigned;
    uint32_t channels;
    uint32_t rate;
    int32_t sectors;
    int32_t xaFile;
    int32_t xaChannel;
    float gain;
} LuaSoundDescriptor;

typedef struct { char opaque[?]; } LuaSound;

LuaSound* spuPlayAudio(const void* data, uint64_t size, const LuaSoundDescriptor* desc, char* errorBuffer,
                       uint32_t errorSize);
void spuDestroySound(LuaSound* sound);
void spuStopSound(LuaSound* sound);
bool spuSoundIsPlaying(LuaSound* sound);
void spuSetSoundGain(LuaSound* sound, float gain);

]]

local C = ffi.load 'SPU_PLAYBACK'

local formats = { pcm = 0, spu = 1, xa = 2 }
local allowedKeys = {
    pcm = { format = true, bits = true, signed = true, channels = true, rate = true, gain = true },
    spu = { format = true, rate = true, pitch = true, gain = true },
    xa = {
        format = true,
        bits = true,
        channels = true,
        rate = true,
        sectors = true,
        xaFile = true,
        xaChannel = true,
        gain = true,
    },
}

local function fail(msg) error('PCSX.SPU.playAudio: ' .. msg, 0) end

local function checkInteger(desc, key, required)
    local v = desc[key]
    if v == nil then
        if required then
            fail('descriptor field \'' .. key .. '\' is required for format \'' .. desc.format .. '\'')
        end
        return nil
    end
    if type(v) ~= 'number' or v ~= math.floor(v) then fail('descriptor field \'' .. key .. '\' must be an integer') end
    return v
end

local function checkOneOf(desc, key, values)
    local v = checkInteger(desc, key, true)
    for _, allowed in ipairs(values) do if v == allowed then return v end end
    fail('descriptor field \'' .. key .. '\' must be one of ' .. table.concat(values, ', ') .. ', got ' .. v)
end

local function checkDescriptor(desc)
    if type(desc) ~= 'table' then fail('the second argument must be a descriptor table') end
    local format = desc.format
    if type(format) ~= 'string' or formats[format] == nil then
        fail('descriptor field \'format\' must be one of \'pcm\', \'spu\', \'xa\'')
    end
    for k, _ in pairs(desc) do
        if not allowedKeys[format][k] then
            fail('descriptor field \'' .. tostring(k) .. '\' is not valid for format \'' .. format .. '\'')
        end
    end

    local cdesc = ffi.new('LuaSoundDescriptor')
    cdesc.format = formats[format]
    cdesc.xaFile = -1
    cdesc.xaChannel = -1
    cdesc.gain = 1.0
    if desc.gain ~= nil then
        if type(desc.gain) ~= 'number' or not (desc.gain >= 0) then
            fail('descriptor field \'gain\' must be a non-negative number')
        end
        cdesc.gain = desc.gain
    end

    if format == 'pcm' then
        cdesc.bits = checkOneOf(desc, 'bits', { 8, 16 })
        cdesc.channels = checkOneOf(desc, 'channels', { 1, 2 })
        local rate = checkInteger(desc, 'rate', true)
        if rate < 1 or rate > 384000 then fail('descriptor field \'rate\' must be between 1 and 384000') end
        cdesc.rate = rate
        local signed = desc.signed
        if signed == nil then signed = cdesc.bits == 16 end
        if type(signed) ~= 'boolean' then fail('descriptor field \'signed\' must be a boolean') end
        if cdesc.bits == 16 and not signed then fail('16 bits pcm data must be signed') end
        cdesc.isSigned = signed and 1 or 0
    elseif format == 'spu' then
        if (desc.rate == nil) == (desc.pitch == nil) then
            fail('format \'spu\' requires exactly one of the descriptor fields \'rate\' or \'pitch\'')
        end
        if desc.rate ~= nil then
            local rate = checkInteger(desc, 'rate', true)
            if rate < 1 or rate > 176400 then fail('descriptor field \'rate\' must be between 1 and 176400') end
            cdesc.rate = rate
        else
            local pitch = checkInteger(desc, 'pitch', true)
            if pitch < 1 or pitch > 0x4000 then
                fail('descriptor field \'pitch\' must be between 1 and 0x4000')
            end
            -- 0x1000 is 44100Hz.
            cdesc.rate = math.max(1, math.floor(pitch * 44100 / 4096 + 0.5))
        end
    else
        local sectors = desc.sectors
        if sectors == nil then sectors = false end
        if type(sectors) ~= 'boolean' then fail('descriptor field \'sectors\' must be a boolean') end
        cdesc.sectors = sectors and 1 or 0
        if sectors then
            for _, k in ipairs({ 'bits', 'channels', 'rate' }) do
                if desc[k] ~= nil then
                    fail('descriptor field \'' .. k ..
                             '\' is not valid with sectors = true; it is read from the subheaders')
                end
            end
            local file = checkInteger(desc, 'xaFile', false)
            if file ~= nil then
                if file < 0 or file > 255 then
                    fail('descriptor field \'xaFile\' must be between 0 and 255')
                end
                cdesc.xaFile = file
            end
            local channel = checkInteger(desc, 'xaChannel', false)
            if channel ~= nil then
                if channel < 0 or channel > 255 then
                    fail('descriptor field \'xaChannel\' must be between 0 and 255')
                end
                cdesc.xaChannel = channel
            end
        else
            if desc.xaFile ~= nil or desc.xaChannel ~= nil then
                fail('descriptor fields \'xaFile\' and \'xaChannel\' are only valid with sectors = true')
            end
            cdesc.bits = checkOneOf(desc, 'bits', { 4, 8 })
            cdesc.channels = checkOneOf(desc, 'channels', { 1, 2 })
            cdesc.rate = checkOneOf(desc, 'rate', { 37800, 18900 })
        end
    end
    return cdesc
end

-- Returns a pointer and a size for the source, plus the object owning the data, which the caller keeps
-- in a local until spuPlayAudio returns. That call never re-enters Lua, so no collection can happen
-- while the native side reads the data.
local function getSource(source)
    if type(source) == 'string' then return ffi.cast('const uint8_t*', source), #source, source end
    if Support.isLuaBuffer(source) then return source.data, #source, source end
    if type(source) == 'table' then
        if source._type == 'Slice' then return source.data, source.size, source end
        if source._type == 'File' then
            local slice = source:readWhole()
            return slice.data, slice.size, slice
        end
    end
    fail('the first argument must be a string, a LuaBuffer, a File, or a Slice')
end

local errorSize = 256

local function createSoundWrapper(wrapper)
    return {
        _wrapper = ffi.gc(wrapper, C.spuDestroySound),
        _type = 'Sound',
        stop = function(self) C.spuStopSound(self._wrapper) end,
        isPlaying = function(self) return C.spuSoundIsPlaying(self._wrapper) end,
        setGain = function(self, gain)
            if type(gain) ~= 'number' or not (gain >= 0) then
                error('Sound:setGain: gain must be a non-negative number')
            end
            C.spuSetSoundGain(self._wrapper, gain)
        end,
    }
end

PCSX.SPU = PCSX.SPU or {}

-- Plays an in-memory audio buffer through the emulator's audio output, independently of the emulated
-- SPU, and of the emulation being paused or running. The source is copied or decoded before this
-- returns. See the descriptor checks above for the accepted formats. The returned object owns the
-- sound: playback stops when it is garbage collected, so keep it for as long as the sound should play.
PCSX.SPU.playAudio = function(source, descriptor)
    local cdesc = checkDescriptor(descriptor)
    local ptr, size, keepAlive = getSource(source)
    local errorBuffer = ffi.new('char[?]', errorSize)
    local sound = C.spuPlayAudio(ptr, size, cdesc, errorBuffer, errorSize)
    if sound == nil then fail(ffi.string(errorBuffer)) end
    return createSoundWrapper(sound)
end

-- )EOF"
