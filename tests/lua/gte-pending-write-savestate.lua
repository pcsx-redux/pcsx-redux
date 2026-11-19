local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local out = 0x100100 / 4
local code = [[
    # 0x80100000: toggle t0, jump with MTC2 t0, IR1 in the delay slot
    xori  $t0, $t0, 1
    j     0x100010
    mtc2  $t0, $ir1
    nop
    # 0x80100010: MFC2 t1, IR1 three instructions after the write, loop if it matches
    nop
    nop
    mfc2  $t1, $ir1
    nop
    bne   $t1, $t0, @0x80100034
    nop
    j     0x100000
    nop
    nop
    # 0x80100034: mismatch, store t0 to 0x80100100 forever
    lui   $t2, 0x8010
    sw    $t0, 0x100($t2)
    j     0x100034
    nop
]]
local sentinel = 0xdeadbeef
local frames, state, saveFrame, tries = 0, nil, nil, 0
GtePendingListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    local r = PCSX.getRegisters()
    if frames == 120 then
        PCSX.Assembler.New():parse(code):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
        ram[out] = sentinel
        r.GPR.n.t0 = 0x10
        r.pc = 0x80100000
    elseif frames > 121 and not state then
        tries = tries + 1
        if ram[out] ~= sentinel then PCSX.quit(4) return end
        if r.CP2D.r[9] ~= r.GPR.n.t0 then
            state = PCSX.createSaveState()
            saveFrame = frames
        elseif tries > 300 then PCSX.quit(5) return end
    elseif state and frames == saveFrame + 1 then
        PCSX.loadSaveState(state)
    elseif state and frames == saveFrame + 4 then
        if ram[out] == sentinel then PCSX.quit(0) else PCSX.quit(1) end
    end
end)
