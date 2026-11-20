local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local base = 0x100000 / 4
local out = 0x100100 / 4
local stubs = {
    -- 0x80100000: GP0(1Fh) raises IRQ1, then GPUSTAT -> 0x80100100 forever
    0x3c081f80, 0x3c091f00, 0xad091810, 0x3c0b8010,
    0x8d0a1814, 0x00000000, 0xad6a0100, 0x08040004, 0x00000000,
    0, 0, 0, 0, 0, 0, 0,
    -- 0x80100040: GP1(02h) acks IRQ1, then the same loop
    0x3c081f80, 0x3c090200, 0xad091814, 0x3c0b8010,
    0x8d0a1814, 0x00000000, 0xad6a0100, 0x08040014, 0x00000000,
}
local sentinel = 0xdeadbeef
local frames = 0
local state
local function irq1() return bit.band(ram[out], 0x01000000) ~= 0 and ram[out] ~= sentinel end
IRQ1SaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        for i, w in ipairs(stubs) do ram[base + i - 1] = w end
        ram[out] = sentinel
        PCSX.getRegisters().pc = 0x80100000
    elseif frames == 122 then
        if not irq1() then print('IRQ1 never raised') PCSX.quit(2) return end
        state = PCSX.createSaveState()
        ram[out] = sentinel
        PCSX.getRegisters().pc = 0x80100040
    elseif frames == 124 then
        if ram[out] == sentinel or irq1() then print('GP1(02h) did not ack IRQ1') PCSX.quit(3) return end
        PCSX.loadSaveState(state)
    elseif frames == 125 then
        ram[out] = sentinel
    elseif frames == 127 then
        if irq1() then PCSX.quit(0) else print('IRQ1 lost across the save state') PCSX.quit(1) end
    end
end)
