/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include "gtest/gtest.h"
#include "main/main.h"

// misc.c's lid tests ask for the lid through these slots instead of a hand on the drive.
static const char c_lidSlots[] = R"(
PCSX.execSlots[0xd0] = function() PCSX.CDRom.openLid() end
PCSX.execSlots[0xd1] = function() PCSX.CDRom.closeLid() end
)";

TEST(cdrom, Interpreter) {
    MainInvoker invoker("-no-ui", "-run", "-stdout", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-iso", "test.cue", "-exec", c_lidSlots, "-loadexe",
                        "src/mips/tests/cdrom/cdrom.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(cdrom, Dynarec) {
    MainInvoker invoker("-no-ui", "-run", "-stdout", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-iso", "test.cue", "-exec", c_lidSlots, "-loadexe", "src/mips/tests/cdrom/cdrom.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
// Boots the test disc through OpenBIOS's shell and kernel loader, with no -loadexe. Passes once
// the disc's boot executable is in RAM and the CPU is running inside it, fails after 6000 frames.
static const char c_discBoot[] = R"(
local f = assert(io.open('src/mips/monitor/hosts/retail/monitor-retail.ps-exe', 'rb'))
local exe = f:read('*a')
f:close()
local function u32(o)
    local a, b, c, d = exe:byte(o + 1, o + 4)
    return a + b * 0x100 + c * 0x10000 + d * 0x1000000
end
local tAddr, tSize = u32(0x18), u32(0x1c)
local text = exe:sub(0x801, 0x800 + tSize)
local frames = 0
DiscBootListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    local pc = PCSX.getRegisters().pc
    if pc >= tAddr and pc < tAddr + tSize then
        local ram = ffi.string(PCSX.getMemPtr() + bit.band(tAddr, 0x1fffff), tSize)
        if ram == text then PCSX.quit(0) end
    end
    if frames >= 6000 then PCSX.quit(1) end
end)
)";

TEST(cdrom, OpenBIOSBootsDiscInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-iso", "test.cue", "-exec", c_discBoot);
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(cdrom, OpenBIOSBootsDiscDynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-iso",
                        "test.cue", "-exec", c_discBoot);
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
