-- Code poked into RAM runs one case per block, with the case number in $s1 and the value read
-- stored to 0x80110000 + 4 * case. With the debugger on, every pause records $s1 and resumes.
-- Cases 1, 2, 4, 5 and 8 read a GTE register before a write to it has landed, and must pause.
-- Case 3 waits long enough, case 6 reads while a command runs (the stall lets the write land),
-- and case 7 writes the value the register already holds: none of those may pause.
-- With GteStaleReadsOff set (through -exec), the setting stays off and nothing may pause.
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local out = 0x110000 / 4
local settle = string.rep('    nop\n', 4)
local function case(n, body) return '    li    $s1, ' .. n .. '\n' .. body end
local asm = table.concat({
    [[
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
]],
    -- mfc2 right after mtc2, then after one and two nops
    case(1, [[
    li    $t1, 0x100
    mtc2  $t1, $ir1
]] .. settle .. [[
    li    $t1, 0x200
    mtc2  $t1, $ir1
    mfc2  $t2, $ir1
    nop
    sw    $t2, 4($s0)
]]),
    case(2, [[
    li    $t1, 0x100
    mtc2  $t1, $ir1
]] .. settle .. [[
    li    $t1, 0x200
    mtc2  $t1, $ir1
    nop
    mfc2  $t2, $ir1
    nop
    sw    $t2, 8($s0)
]]),
    case(3, [[
    li    $t1, 0x100
    mtc2  $t1, $ir1
]] .. settle .. [[
    li    $t1, 0x200
    mtc2  $t1, $ir1
    nop
    nop
    mfc2  $t2, $ir1
    nop
    sw    $t2, 12($s0)
]]),
    -- cfc2 right after ctc2
    case(4, [[
    li    $t1, 0x100
    ctc2  $t1, $ofx
]] .. settle .. [[
    li    $t1, 0x200
    ctc2  $t1, $ofx
    cfc2  $t2, $ofx
    nop
    sw    $t2, 16($s0)
]]),
    -- swc2 right after mtc2
    case(5, [[
    li    $t1, 0x100
    mtc2  $t1, $ir1
]] .. settle .. [[
    li    $t1, 0x200
    mtc2  $t1, $ir1
    swc2  $ir1, 20($s0)
]]),
    -- mfc2 waiting on a running RTPT: the stall gives the write time to land
    case(6, [[
    li    $t1, 0x100
    mtc2  $t1, $vxy0
]] .. settle .. [[
    li    $t1, 0x200
    rtpt
    mtc2  $t1, $vxy0
    mfc2  $t2, $vxy0
    nop
    sw    $t2, 24($s0)
]] .. settle),
    -- a write of the value already there reads the same either way
    case(7, [[
    li    $t1, 0x200
    mtc2  $t1, $ir1
]] .. settle .. [[
    mtc2  $t1, $ir1
    mfc2  $t2, $ir1
    nop
    sw    $t2, 28($s0)
]]),
    -- IRGB sets IR1 when it lands, so IR1 read right after it is stale too
    case(8, [[
    mtc2  $zero, $ir1
]] .. settle .. [[
    li    $t1, 0x1f
    mtc2  $t1, $irgb
    mfc2  $t2, $ir1
    nop
    sw    $t2, 32($s0)
]] .. settle),
    [[
    li    $t1, 0x600d
    sw    $t1, 0x40($s0)
    b     -1
    nop
]],
})

local expectedValues = { 0x100, 0x100, 0x200, 0x100, 0x100, 0x200, 0x200, 0 }
local pauses = {}
local frames = 0
local debug = PCSX.settings.emulator.Debug
GteStaleReadsPause = PCSX.Events.createEventListener('ExecutionFlow::Pause', function()
    if frames >= 120 then pauses[#pauses + 1] = PCSX.getRegisters().GPR.n.s1 end
    PCSX.resumeEmulator()
end)
GteStaleReadsVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        debug.Debug = true
        debug.GTEStaleReads = not GteStaleReadsOff
        PCSX.Assembler.New():parse(asm):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
        for i = 0, 16 do ram[out + i] = 0xffffffff end
        PCSX.getRegisters().pc = 0x80100000
    elseif frames > 120 and ram[out + 16] == 0x600d then
        local expectedPauses = GteStaleReadsOff and {} or { 1, 2, 4, 5, 8 }
        print('pauses: ' .. table.concat(pauses, ','))
        for i, v in ipairs(expectedValues) do
            if ram[out + i] ~= v then
                print(string.format('case %d read %08x, expected %08x', i, ram[out + i], v))
                PCSX.quit(1)
                return
            end
        end
        if table.concat(pauses, ',') ~= table.concat(expectedPauses, ',') then
            print('expected pauses: ' .. table.concat(expectedPauses, ','))
            PCSX.quit(3)
            return
        end
        PCSX.quit(0)
    elseif frames > 400 then
        PCSX.quit(2)
    end
end)
