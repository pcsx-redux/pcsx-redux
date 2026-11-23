-- Code poked into RAM runs one case per block, with the case number in $s1 and the value read
-- stored to 0x80110000 + 4 * case. With the debugger on, every pause records $s1 and resumes.
-- Cases 1, 2, 4, 5 and 8 read a GTE register before a write to it has landed, and must pause.
-- Case 3 waits long enough, case 6 reads while a command runs (the stall lets the write land),
-- and case 7 writes the value the register already holds: none of those may pause.
-- Case 9, run after the end marker is written, is an unaligned SWC2 after a write: it faults
-- before reading the GTE, so it must not pause either, and is left to the kernel's handler.
-- After the program, a function run through PCSX.callGuest does a stale MFC2 (it reads the 0x200
-- left by case 9): a guest call must not pause the emulator from the inside, so it may not pause.
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
]],
    -- an unaligned swc2 faults with a write still pending, and never reads the register
    case(9, [[
    li    $t1, 0x100
    mtc2  $t1, $ir1
]] .. settle .. [[
    li    $t1, 0x200
    mtc2  $t1, $ir1
    swc2  $ir1, 37($s0)
    b     -1
    nop
]]),
})

local expectedValues = { 0x100, 0x100, 0x200, 0x100, 0x100, 0x200, 0x200, 0, 0xffffffff }
local guestCall = [[
    li    $t1, 0x300
    mtc2  $t1, $ir1
    mfc2  $v0, $ir1
    jr    $ra
    nop
]]
local pauses = {}
local frames = 0
local callPending = false
local callOnRun = false
local doneAt
local debug = PCSX.settings.emulator.Debug

local function finish()
    local expectedPauses = GteStaleReadsOff and {} or { 1, 2, 4, 5, 8 }
    print('pauses: ' .. table.concat(pauses, ','))
    for i, v in ipairs(expectedValues) do
        if ram[out + i] ~= v then
            print(string.format('case %d read %08x, expected %08x', i, ram[out + i], v))
            PCSX.quit(1)
            return
        end
    end
    local badVAddr = PCSX.getRegisters().CP0.r[8]
    if badVAddr ~= 0x80110025 then
        print(string.format('case 9 left BadVAddr at %08x, expected its unaligned 80110025', badVAddr))
        PCSX.quit(4)
        return
    end
    PCSX.Assembler.New():parse(guestCall):compileToMemory(PCSX.getMemPtr(), 0x80120000, 0x80000000)
    local before = #pauses
    local r = PCSX.callGuest { pc = 0x80120000 }
    if r.status ~= 'returned' or r.v0 ~= 0x200 then
        print(string.format('guest call: %s, v0 %08x, expected returned with 00000200', r.status, r.v0 or 0))
        PCSX.quit(5)
        return
    end
    if #pauses ~= before then
        print('the stale read in the guest call paused the emulator')
        PCSX.quit(6)
        return
    end
    if table.concat(pauses, ',') ~= table.concat(expectedPauses, ',') then
        print('expected pauses: ' .. table.concat(expectedPauses, ','))
        PCSX.quit(3)
        return
    end
    PCSX.quit(0)
end

GteStaleReadsPause = PCSX.Events.createEventListener('ExecutionFlow::Pause', function()
    if callPending then
        -- Make the guest call with the emulator running, where a pause would be seen.
        callPending = false
        callOnRun = true
    elseif frames >= 120 then
        pauses[#pauses + 1] = PCSX.getRegisters().GPR.n.s1
    end
    PCSX.resumeEmulator()
end)
GteStaleReadsRun = PCSX.Events.createEventListener('ExecutionFlow::Run', function()
    if not callOnRun then return end
    callOnRun = false
    finish()
end)
GteStaleReadsVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        debug.Debug = true
        debug.GTEStaleReads = not GteStaleReadsOff
        PCSX.Assembler.New():parse(asm):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
        for i = 0, 16 do ram[out + i] = 0xffffffff end
        PCSX.getRegisters().pc = 0x80100000
    elseif frames > 120 and ram[out + 16] == 0x600d and not doneAt then
        doneAt = frames
    elseif doneAt and frames > doneAt and not callPending then
        -- A frame after the end marker, case 9 has long run. The guest call runs from the
        -- resume that follows this pause, not from inside this vsync.
        callPending = true
        PCSX.pauseEmulator()
    elseif frames > 400 then
        PCSX.quit(2)
    end
end)
