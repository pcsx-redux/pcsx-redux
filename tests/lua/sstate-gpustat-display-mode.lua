local pb = require 'pb'
local protoc = require 'protoc'
local schema = protoc.new()
assert(schema:load(PCSX.getSaveStateProtoSchema(), 'savestate.proto'))
pb.option('int64_as_number')

local function displayMode(s)
    local control = pb.decode('SaveState', s).gpu.control
    local b1, b2, b3, b4 = control:byte(8 * 4 + 1, 8 * 4 + 4)
    return b1 + b2 * 0x100 + b3 * 0x10000 + b4 * 0x1000000
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local mode = 0x08000027
local frames = 0
local first
DisplayModeSaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        local asm = string.format([[
            lui $t0, 0x1f80
            lui $t1, 0x%04x
            ori $t1, $t1, 0x%04x
            sw $t1, 0x1814($t0) # GP1
            b -1
            nop
        ]], bit.rshift(mode, 16), bit.band(mode, 0xffff))
        PCSX.Assembler.New():parse(asm):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
        PCSX.invalidateCache()
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        timer:start(1, 0, function()
            timer:close()
            PCSX.getRegisters().pc = 0x80100000
            PCSX.resumeEmulator()
        end)
    elseif frames == 32 then
        first = tostring(PCSX.createSaveState())
        if displayMode(first) ~= mode then
            print(string.format('GP1(08h) write did not land: %08x', displayMode(first)))
            PCSX.quit(1)
            return
        end
        PCSX.loadSaveState(toSlice(first))
    elseif frames == 34 then
        local after = displayMode(tostring(PCSX.createSaveState()))
        if after ~= mode then
            print(string.format('display mode lost across save/load: %08x, expected %08x', after, mode))
            PCSX.quit(2)
            return
        end
        PCSX.quit(0)
    end
end)
