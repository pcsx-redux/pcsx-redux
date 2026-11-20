-- Called from the test's -exec: -dofile runs before any -exec, so a parameter set there would
-- not be visible to this file's top level.
function GPUDFE480iDeinterlace(DEINTERLACE)
    local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
    PCSX.settings.emulator.Deinterlace = DEINTERLACE

    local function plant(writes)
        local i = 0x100000 / 4
        ram[i] = 0x3c081f80 i = i + 1 -- lui $t0, 0x1f80
        for _, w in ipairs(writes) do
            ram[i] = 0x3c090000 + bit.rshift(w[2], 16) i = i + 1 -- lui $t1, hi
            ram[i] = 0x35290000 + bit.band(w[2], 0xffff) i = i + 1 -- ori $t1, $t1, lo
            ram[i] = 0xad090000 + w[1] i = i + 1 -- sw $t1, w[1]($t0)
        end
        ram[i] = 0x1000ffff i = i + 1 -- b .
        ram[i] = 0 i = i + 1
        PCSX.invalidateCache()
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        timer:start(1, 0, function()
            timer:close()
            PCSX.getRegisters().pc = 0x80100000
            PCSX.resumeEmulator()
        end)
    end

    local GP0, GP1 = 0x1810, 0x1814
    local writes = {
        { GP1, 0x00000000 }, { GP1, 0x06c60260 }, { GP1, 0x07040010 }, { GP1, 0x05000000 },
        { GP1, 0x08000027 }, { GP1, 0x03000000 },
        { GP0, 0xe3000000 }, { GP0, 0xe4077e7f }, { GP0, 0xe5000000 },
        { GP0, 0xe1000400 }, { GP0, 0x02000000 }, { GP0, 0x00000000 }, { GP0, 0x01e00280 },
        { GP0, 0xe1000000 }, { GP0, 0x60ffffff }, { GP0, 0x00000000 }, { GP0, 0x01e00280 },
    }

    local frames = 0
    DeinterlaceListener = PCSX.Events.createEventListener('GPU::Vsync', function()
        frames = frames + 1
        if frames == 30 then
            plant(writes)
        elseif frames == 34 then
            local vram = tostring(PCSX.GPU.getVRAM())
            local rows = { [0] = 0, [1] = 0 }
            for y = 0, 479 do
                local o = y * 2048 + 1
                if bit.band(vram:byte(o) + vram:byte(o + 1) * 256, 0x7fff) ~= 0 then rows[y % 2] = rows[y % 2] + 1 end
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
