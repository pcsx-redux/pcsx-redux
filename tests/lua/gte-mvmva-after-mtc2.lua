local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local out = 0x110000 / 4
local asm = {[[
    mfc0  $t1, $sr        # interrupts off, COP2 on
    nop
    li    $t2, -2
    and   $t1, $t1, $t2
    lui   $t2, 0x4000
    or    $t1, $t1, $t2
    mtc0  $t1, $sr
    nop
    nop
    lui   $s0, 0x8011
    li    $t0, 0x1000     # RT = identity
    ctc2  $t0, $r11r12
    ctc2  $zero, $r13r21
    ctc2  $t0, $r22r23
    ctc2  $zero, $r31r32
    ctc2  $t0, $r33
    mtc2  $zero, $vxy0
    nop
    nop
    nop
    nop
]]}
for n = 0, 2 do
    table.insert(asm, [[
    li    $t1, 0x100
    mtc2  $t1, $vz0
    nop
    nop
    nop
    nop
    li    $t1, 0x200
    mtc2  $t1, $vz0
]] .. string.rep('    nop\n', n) .. [[
    mvmva 1, 0, 0, 3, 0   # sf = 1, RT * V0, no translation
    mfc2  $t2, $ir3
    nop
    sw    $t2, ]] .. 4 * n .. '($s0)\n')
end
table.insert(asm, [[
    li    $t1, 0x600d
    sw    $t1, 12($s0)
    b     -1
    nop
]])
local frames = 0
GteMvmvaListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        PCSX.Assembler.New():parse(table.concat(asm)):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
        for i = 0, 3 do ram[out + i] = 0 end
        PCSX.getRegisters().pc = 0x80100000
    elseif frames > 120 and ram[out + 3] == 0x600d then
        if ram[out + 1] == 0x200 and ram[out + 2] == 0x200 then PCSX.quit(0) else PCSX.quit(1) end
    elseif frames > 400 then
        PCSX.quit(2)
    end
end)
