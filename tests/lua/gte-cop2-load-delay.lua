local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local base = 0x100000 / 4
local out = 0x100100 / 4
local code = {
    -- 0x80100000: t1 = 0x55, IR1 = 0x1234, MFC2 t1 <- IR1 with t1 read in the load delay slot
    0x34090055, 0x34081234, 0x48884800, 0x00000000, 0x00000000, 0x00000000,
    0x48094800, 0x01205021,
    -- t3 = 0x66, R11R12 = 0x1234, CFC2 t3 <- R11R12 with t3 read in the load delay slot
    0x340b0066, 0x48c80000, 0x00000000, 0x00000000, 0x00000000,
    0x484b0000, 0x01606021, 0x00000000,
    -- store t1..t4 to 0x80100100 and spin
    0x3c0d8010, 0xada90100, 0xadaa0104, 0xadab0108, 0xadac010c, 0x08040015, 0x00000000,
}
local frames = 0
Cop2DelayListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        for i, w in ipairs(code) do ram[base + i - 1] = w end
        for i = 0, 3 do ram[out + i] = 0xdeadbeef end
        PCSX.getRegisters().pc = 0x80100000
    elseif frames == 123 then
        if ram[out] == 0x1234 and ram[out + 1] == 0x55 and ram[out + 2] == 0x1234 and ram[out + 3] == 0x66 then
            PCSX.quit(0)
        else
            PCSX.quit(1)
        end
    end
end)
