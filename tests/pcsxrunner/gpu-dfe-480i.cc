/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include <string>

#include "gtest/gtest.h"
#include "main/main.h"

// In 640x480 interlaced with GP0(E1h).10 clear, the GPU skips the rows whose parity matches
// GPUSTAT.31 at draw time, fills included, as measured on v0 and v2 GPUs. ret == 0 means
// every check in the guest passed.
TEST(GPUDFE480i, Interpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-loadexe", "src/mips/tests/gpu/dfe-480i/gpu-dfe-480i.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(GPUDFE480i, Dynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-loadexe", "src/mips/tests/gpu/dfe-480i/gpu-dfe-480i.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// A 640x480 rectangle drawn in 480i with GP0(E1h).10 clear writes one field's rows. With the
// Deinterlace setting it writes all of them. The script sets the mode, clears VRAM with DFE set,
// then draws the rectangle with DFE clear, and counts the written rows of column 0 by parity.
static const char c_deinterlace[] = R"(
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
)";

static int runDeinterlace(bool deinterlace) {
    std::string script = std::string("local DEINTERLACE = ") + (deinterlace ? "true" : "false") + "\n" + c_deinterlace;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-exec", script.c_str());
    return invoker.invoke();
}

TEST(GPUDFE480i, AccurateSkipsOneField) { EXPECT_EQ(runDeinterlace(false), 0); }
TEST(GPUDFE480i, DeinterlaceDrawsBothFields) { EXPECT_EQ(runDeinterlace(true), 0); }
