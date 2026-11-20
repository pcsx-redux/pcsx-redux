-- Called from the test's -exec: -dofile runs before any -exec, so a parameter set there would
-- not be visible to this file's top level.
function GPUDFE480iDeinterlace(DEINTERLACE)
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
            # GP0(60h) white rectangle at 0,0, 640x480
            lui   $t1, 0x60ff
            ori   $t1, $t1, 0xffff
            sw    $t1, 0x1810($t0)
            lui   $t1, 0x0000
            ori   $t1, $t1, 0x0000
            sw    $t1, 0x1810($t0)
            lui   $t1, 0x01e0
            ori   $t1, $t1, 0x0280
            sw    $t1, 0x1810($t0)
            b     -1
            nop
        ]]):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
        PCSX.invalidateCache()
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        timer:start(1, 0, function()
            timer:close()
            PCSX.getRegisters().pc = 0x80100000
            PCSX.resumeEmulator()
        end)
    end

    local frames = 0
    DeinterlaceListener = PCSX.Events.createEventListener('GPU::Vsync', function()
        frames = frames + 1
        if frames == 30 then
            plant()
        elseif frames == 34 then
            local vram = tostring(PCSX.GPU.getVRAM())
            local rows = { [0] = 0, [1] = 0 }
            for y = 0, 479 do
                local o = y * 2048 + 1
                if bit.band(vram:byte(o) + vram:byte(o + 1) * 256, 0x7fff) ~= 0 then
                    rows[y % 2] = rows[y % 2] + 1
                end
            end
            print(string.format('even rows %d, odd rows %d', rows[0], rows[1]))
            if DEINTERLACE then
                PCSX.quit((rows[0] == 240 and rows[1] == 240) and 0 or 1)
            else
                PCSX.quit((rows[0] + rows[1] == 240 and rows[0] * rows[1] == 0) and 0 or 1)
            end
        end
    end)
end
