-- Called from the test's -exec: -dofile runs before any -exec, so a parameter set there would
-- not be visible to this file's top level.
function GPUDumpDeinterlace(DEINTERLACE)
    PCSX.settings.emulator.Deinterlace = DEINTERLACE

    local function plant()
        PCSX.Assembler.New():parse([[
            lui   $t0, 0x1f80
            # GP1(00h) reset
            lui   $t1, 0x0000
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1814($t0)
            # GP1(06h) horizontal display range
            lui   $t1, 0x06c6
            ori   $t1, $t1, 0x0260
            sw    $t1, 0x1814($t0)
            # GP1(07h) vertical display range
            lui   $t1, 0x0704
            ori   $t1, $t1, 0x0010
            sw    $t1, 0x1814($t0)
            # GP1(05h) display start 0,0
            lui   $t1, 0x0500
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1814($t0)
            # GP1(08h) 640x480 interlaced
            lui   $t1, 0x0800
            ori   $t1, $t1, 0x0027
            sw    $t1, 0x1814($t0)
            # GP1(03h) display on
            lui   $t1, 0x0300
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1814($t0)
            # GP0(E3h) drawing area top left 0,0
            lui   $t1, 0xe300
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1810($t0)
            # GP0(E4h) drawing area bottom right 639,479
            lui   $t1, 0xe407
            ori   $t1, $t1, 0x7e7f
            sw    $t1, 0x1810($t0)
            # GP0(E5h) drawing offset 0,0
            lui   $t1, 0xe500
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1810($t0)
            # GP0(E1h) DFE set
            lui   $t1, 0xe100
            ori   $t1, $t1, 0x0400
            sw    $t1, 0x1810($t0)
            # GP0(02h) fill black at 0,0, 640x480
            lui   $t1, 0x0200
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1810($t0)
            lui   $t1, 0x0000
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1810($t0)
            lui   $t1, 0x01e0
            ori   $t1, $t1, 0x0280
            sw    $t1, 0x1810($t0)
            # GP0(E1h) DFE clear
            lui   $t1, 0xe100
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1810($t0)
            # $t4 = 0x01e00280 (640x480), $t5 = 0x60000000 (GP0(60h) rectangle)
            lui   $t4, 0x01e0
            ori   $t4, $t4, 0x0280
            lui   $t5, 0x6000
            # loop: a 640x480 rectangle at 0,0, red (0xff) when GPUSTAT.31 is 0, green (0xff00) when 1
            lw    $t1, 0x1814($t0)
            nop
            srl   $t1, $t1, 31
            sll   $t1, $t1, 3
            ori   $t2, $zero, 0xff
            nop                       # 0x801000cc: sllv $t2, $t2, $t1, patched in below
            or    $t2, $t2, $t5
            sw    $t2, 0x1810($t0)
            sw    $zero, 0x1810($t0)
            sw    $t4, 0x1810($t0)
            b     -11
            nop
        ]]):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
        -- The assembler's sllv takes a shift amount where rs goes, so it cannot encode sllv with a
        -- register: write 0x012a5004 (sllv $t2, $t2, $t1) over the placeholder nop by hand.
        ffi.cast('uint32_t*', PCSX.getMemPtr())[0xcc / 4 + 0x100000 / 4] = 0x012a5004
        PCSX.invalidateCache()
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        timer:start(1, 0, function()
            timer:close()
            PCSX.getRegisters().pc = 0x80100000
            PCSX.resumeEmulator()
        end)
    end

    -- Column 0 of rows 0-479: true when all even rows share one color, all odd rows another.
    local function twoFields(slice, what)
        local vram = tostring(slice)
        local colors = {}
        local mixed = false
        for y = 0, 479 do
            local o = y * 2048 + 1
            local c = bit.band(vram:byte(o) + vram:byte(o + 1) * 256, 0x7fff)
            local p = y % 2
            if colors[p] == nil then
                colors[p] = c
            elseif colors[p] ~= c then
                mixed = true
            end
        end
        print(
            string.format('%s: even rows %04x, odd rows %04x%s', what, colors[0], colors[1], mixed and ', mixed' or ''))
        return not mixed and colors[0] ~= 0 and colors[1] ~= 0 and colors[0] ~= colors[1]
    end

    local path = os.tmpname()
    os.remove(path)
    path = path .. '.gpd'
    local frames = 0
    DumpListener = PCSX.Events.createEventListener('GPU::Vsync', function()
        frames = frames + 1
        if frames == 30 then
            plant()
        elseif frames == 40 then
            PCSX.GPU.startDump(path)
        elseif frames > 40 and PCSX.GPU.isDumping() and PCSX.GPU.getDumpFrames() >= 6 then
            PCSX.GPU.stopDump()
            local player = PCSX.GPU.DumpPlayer
            local ok = player.load(path)
            ok = ok and twoFields(player.getVRAM(), 'initial snapshot')
            -- Stop on the last vsync: the words after it start a frame the dump does not finish.
            ok = ok and player.seek(player.getFrameCount())
            ok = ok and twoFields(player.getVRAM(), 'after ' .. player.getFrame() .. ' frames')
            player.unload()
            os.remove(path)
            PCSX.quit(ok and 0 or 1)
        elseif frames > 200 then
            PCSX.quit(2)
        end
    end)
end
