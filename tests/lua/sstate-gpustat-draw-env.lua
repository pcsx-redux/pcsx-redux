local pb = require 'pb'
local protoc = require 'protoc'
local schema = protoc.new()
assert(schema:load(PCSX.getSaveStateProtoSchema(), 'savestate.proto'))
pb.option('int64_as_string')

local function gpustat(s) return pb.decode('SaveState', s).gpu.status end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local function plant(words)
    local lines = { 'lui $t0, 0x1f80' }
    for _, w in ipairs(words) do
        lines[#lines + 1] = string.format('lui $t1, 0x%04x', bit.rshift(w, 16))
        lines[#lines + 1] = string.format('ori $t1, $t1, 0x%04x', bit.band(w, 0xffff))
        lines[#lines + 1] = 'sw $t1, 0x1810($t0)' -- GP0
    end
    lines[#lines + 1] = 'b -1'
    lines[#lines + 1] = 'nop'
    PCSX.Assembler.New():parse(table.concat(lines, '\n')):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
    PCSX.invalidateCache()
    PCSX.pauseEmulator()
    local timer = luv.new_timer()
    timer:start(1, 0, function()
        timer:close()
        PCSX.getRegisters().pc = 0x80100000
        PCSX.resumeEmulator()
    end)
end

local env = { 0xe10006b5, 0xe2012345, 0xe3002010, 0xe4040190, 0xe5001010, 0xe6000003 }
local rect = { 0x60ff0000, 0x00000000, 0x00100010 }
local probes = { { 20, 5 }, { 20, 10 }, { 40, 10 }, { 31, 17 } }

local function pixels()
    local vram = tostring(PCSX.GPU.getVRAM())
    local out = {}
    for i, p in ipairs(probes) do
        local o = (p[2] * 1024 + p[1]) * 2 + 1
        out[i] = vram:byte(o) + vram:byte(o + 1) * 256
    end
    return out
end

local frames = 0
local first, control
GpustatSaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        plant(env)
    elseif frames == 32 then
        first = tostring(PCSX.createSaveState())
        local stat = gpustat(first)
        if bit.band(stat, 0x9fff) ~= 0x1eb5 then
            print(string.format('GP0 writes did not land: GPUSTAT %08x', stat))
            PCSX.quit(1)
            return
        end
        plant(rect)
    elseif frames == 34 then
        control = pixels()
        if control[2] ~= 0xfc00 then
            print(string.format('control rectangle did not draw as expected: %04x', control[2]))
            PCSX.quit(2)
            return
        end
        PCSX.loadSaveState(toSlice(first))
    elseif frames == 36 then
        local second = tostring(PCSX.createSaveState())
        local a, b = bit.band(gpustat(first), 0x9fff), bit.band(gpustat(second), 0x9fff)
        if a ~= b then
            print(string.format('GPUSTAT draw-mode bits changed across save/load: %04x -> %04x', a, b))
            PCSX.quit(3)
            return
        end
        plant(rect)
    elseif frames == 38 then
        local after = pixels()
        for i, p in ipairs(probes) do
            if after[i] ~= control[i] then
                print(string.format('drawing environment lost across save/load: (%d,%d) %04x, expected %04x',
                                    p[1], p[2], after[i], control[i]))
                PCSX.quit(4)
                return
            end
        end
        PCSX.quit(0)
    end
end)
