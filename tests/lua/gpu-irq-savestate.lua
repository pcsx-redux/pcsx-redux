local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local out = 0x100100 / 4
local stubs = [[
    # 0x80100000: GP0(1Fh) raises IRQ1, then GPUSTAT -> 0x80100100 forever
    lui   $t0, 0x1f80
    lui   $t1, 0x1f00
    sw    $t1, 0x1810($t0)
    lui   $t3, 0x8010
    lw    $t2, 0x1814($t0)
    nop
    sw    $t2, 0x100($t3)
    j     0x100010
    nop
]] .. string.rep('    nop\n', 7) .. [[
    # 0x80100040: GP1(02h) acks IRQ1, then the same loop
    lui   $t0, 0x1f80
    lui   $t1, 0x0200
    sw    $t1, 0x1814($t0)
    lui   $t3, 0x8010
    lw    $t2, 0x1814($t0)
    nop
    sw    $t2, 0x100($t3)
    j     0x100050
    nop
]]
local sentinel = 0xdeadbeef
local frames = 0
local state
local function irq1() return bit.band(ram[out], 0x01000000) ~= 0 and ram[out] ~= sentinel end
IRQ1SaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        PCSX.Assembler.New():parse(stubs):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
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
