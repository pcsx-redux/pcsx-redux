-- CDRom fields 50/51.
local pb = require 'pb'
local protoc = require 'protoc'

-- The state's own schema, so the state can be read and edited as a table.
local schema = protoc.new()
assert(schema:load(PCSX.getSaveStateProtoSchema(), 'savestate.proto'))
pb.option('int64_as_number')

local function decode(s) return pb.decode('SaveState', s) end

-- Rewrites the CDRom message: drops fields 50 and 51, then sets them if asked.
local function withFlags(s, set)
    local state = decode(s)
    state.cdrom.sectorReadSinceSeek = nil
    state.cdrom.scexCountersReset = nil
    if set then
        state.cdrom.sectorReadSinceSeek = true
        state.cdrom.scexCountersReset = true
    end
    return pb.encode('SaveState', state)
end

-- The value of a CDRom flag, 0 if absent.
local function flag(s, name)
    local cdrom = decode(s).cdrom
    return (cdrom and cdrom[name]) and 1 or 0
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local frames = 0
local base
CDRomFlagsListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 60 then
        base = tostring(PCSX.createSaveState())
        PCSX.loadSaveState(toSlice(withFlags(base, true)))
    elseif frames == 61 then
        local s = tostring(PCSX.createSaveState())
        if flag(s, 'sectorReadSinceSeek') ~= 1 then print('sectorReadSinceSeek lost across the save state') PCSX.quit(1) return end
        if flag(s, 'scexCountersReset') ~= 1 then print('scexCountersReset lost across the save state') PCSX.quit(2) return end
        PCSX.loadSaveState(toSlice(withFlags(base, false)))
    elseif frames == 62 then
        local s = tostring(PCSX.createSaveState())
        if flag(s, 'sectorReadSinceSeek') ~= 0 then print('sectorReadSinceSeek survived a state without it') PCSX.quit(3) return end
        if flag(s, 'scexCountersReset') ~= 0 then print('scexCountersReset survived a state without it') PCSX.quit(4) return end
        PCSX.quit(0)
    end
end)
