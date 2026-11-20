local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local marker = 0x100100 / 4

local pb = require 'pb'
local protoc = require 'protoc'

-- The state's own schema, so the state can be read and edited as a table. A v4 state
-- differs from v5 only in values the current schema still carries (the version number
-- and a 1MB VRAM blob), so it is built by editing the decoded table.
local schema = protoc.new()
assert(schema:load(PCSX.getSaveStateProtoSchema(), 'savestate.proto'))
pb.option('int64_as_string')

local function toV4(s)
    local state = pb.decode('SaveState', s)
    state.save_state_info = { version_string = 'PCSX-Redux SaveState v4', version = 4 }
    local vram = state.gpu.vram
    if #vram ~= 0x200000 then error('v5 VRAM is ' .. #vram .. ' bytes') end
    state.gpu.vram = vram:sub(1, 0x100000)
    return pb.encode('SaveState', state)
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local function upperBankIsOpenBus()
    local vram = tostring(PCSX.GPU.getVRAM())
    if #vram ~= 0x200000 then return false end
    return vram:sub(0x100001) == string.rep('\255', 0x100000)
end

local frames = 0
local v5
V4SaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 60 then
        ram[marker] = 0x12345678
        v5 = tostring(PCSX.createSaveState())
        local v4 = toV4(v5)
        ram[marker] = 0xdeadbeef
        PCSX.loadSaveState(toSlice(v4))
    elseif frames == 62 then
        if ram[marker] ~= 0x12345678 then print('v4 save state did not load') PCSX.quit(1) return end
        if not upperBankIsOpenBus() then print('v4 load wrote the upper VRAM bank') PCSX.quit(2) return end
        ram[marker] = 0xdeadbeef
        PCSX.loadSaveState(toSlice(v5))
    elseif frames == 64 then
        if ram[marker] ~= 0x12345678 then print('v5 save state did not load') PCSX.quit(3) return end
        PCSX.quit(0)
    end
end)
