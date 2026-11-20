local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local out = 0x100100 / 4
local code = [[
    # 0x80100000: t1 = 0x55, IR1 = 0x1234, MFC2 t1 <- IR1 with t1 read in the load delay slot
    ori   $t1, $zero, 0x55
    ori   $t0, $zero, 0x1234
    mtc2  $t0, $ir1
    nop
    nop
    nop
    mfc2  $t1, $ir1
    addu  $t2, $t1, $zero
    # t3 = 0x66, R11R12 = 0x1234, CFC2 t3 <- R11R12 with t3 read in the load delay slot
    ori   $t3, $zero, 0x66
    ctc2  $t0, $r11r12
    nop
    nop
    nop
    cfc2  $t3, $r11r12
    addu  $t4, $t3, $zero
    nop
    # store t1..t4 to 0x80100100 and spin
    lui   $t5, 0x8010
    sw    $t1, 0x100($t5)
    sw    $t2, 0x104($t5)
    sw    $t3, 0x108($t5)
    sw    $t4, 0x10c($t5)
    j     0x100054
    nop
]]
local frames = 0
Cop2DelayListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        PCSX.Assembler.New():parse(code):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
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
