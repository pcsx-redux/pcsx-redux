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

// A GPU dump records the accurate 480i path whatever the Deinterlace setting says. The guest loops
// on a 640x480 rectangle with DFE clear, colored by GPUSTAT.31, so accurate drawing leaves even and
// odd rows in two different colors and deinterlaced drawing leaves one. The dump's initial VRAM, and
// the player's VRAM after the whole dump, must both show two colors.
static const char c_script[] = R"(
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
PCSX.settings.emulator.Deinterlace = DEINTERLACE

local GP0, GP1 = 0x1810, 0x1814
local writes = {
    { GP1, 0x00000000 }, { GP1, 0x06c60260 }, { GP1, 0x07040010 }, { GP1, 0x05000000 },
    { GP1, 0x08000027 }, { GP1, 0x03000000 },
    { GP0, 0xe3000000 }, { GP0, 0xe4077e7f }, { GP0, 0xe5000000 },
    { GP0, 0xe1000400 }, { GP0, 0x02000000 }, { GP0, 0x00000000 }, { GP0, 0x01e00280 },
    { GP0, 0xe1000000 },
}
local loop = {
    0x3c0c01e0, 0x358c0280, 0x3c0d6000, -- $t4 = 0x01e00280, $t5 = 0x60000000
    0x8d091814, -- loop: lw $t1, GPUSTAT($t0)
    0x00000000, -- load delay
    0x00094fc2, -- srl $t1, $t1, 31
    0x000948c0, -- sll $t1, $t1, 3
    0x340a00ff, -- ori $t2, $zero, 0xff
    0x012a5004, -- sllv $t2, $t2, $t1
    0x014d5025, -- or $t2, $t2, $t5
    0xad0a1810, -- sw $t2, GP0($t0)
    0xad001810, -- sw $zero, GP0($t0)
    0xad0c1810, -- sw $t4, GP0($t0)
    0x1000fff5, -- b loop
    0x00000000,
}

local function plant()
    local i = 0x100000 / 4
    ram[i] = 0x3c081f80 i = i + 1 -- lui $t0, 0x1f80
    for _, w in ipairs(writes) do
        ram[i] = 0x3c090000 + bit.rshift(w[2], 16) i = i + 1 -- lui $t1, hi
        ram[i] = 0x35290000 + bit.band(w[2], 0xffff) i = i + 1 -- ori $t1, $t1, lo
        ram[i] = 0xad090000 + w[1] i = i + 1 -- sw $t1, w[1]($t0)
    end
    for _, w in ipairs(loop) do ram[i] = w i = i + 1 end
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
        if colors[p] == nil then colors[p] = c elseif colors[p] ~= c then mixed = true end
    end
    print(string.format('%s: even rows %04x, odd rows %04x%s', what, colors[0], colors[1], mixed and ', mixed' or ''))
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
)";

static int run(bool deinterlace) {
    std::string script = std::string("local DEINTERLACE = ") + (deinterlace ? "true" : "false") + "\n" + c_script;
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-exec", script.c_str());
    return invoker.invoke();
}

TEST(GPUDumpDeinterlace, AccurateRecordsFields) { EXPECT_EQ(run(false), 0); }
TEST(GPUDumpDeinterlace, DeinterlaceRecordsFields) { EXPECT_EQ(run(true), 0); }
