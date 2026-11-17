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

#include "gtest/gtest.h"
#include "main/main.h"

// Boots the gpu-irq guest suite headless under OpenBIOS. ret == 0 means the
// GP0(1Fh)/GP1(02h) IRQ flag behaviour in GPUSTAT.24 matched the soft GPU.
TEST(GPUIRQ, RequestAcknowledge) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-loadexe", "src/mips/tests/gpu/irq/gpu-irq.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// IRQ1 survives a save state. Code poked into RAM raises it with GP0(1Fh) and keeps copying
// GPUSTAT to 0x80100100; a state is saved, GP1(02h) acks the flag, the state is loaded, and
// GPUSTAT.24 must be set again. Loading replays GP1 writes that clear the flag before
// GPU::deserialize puts it back.
static const char c_irq1SaveState[] = R"(
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local base = 0x100000 / 4
local out = 0x100100 / 4
local stubs = {
    -- 0x80100000: GP0(1Fh) raises IRQ1, then GPUSTAT -> 0x80100100 forever
    0x3c081f80, 0x3c091f00, 0xad091810, 0x3c0b8010,
    0x8d0a1814, 0x00000000, 0xad6a0100, 0x08040004, 0x00000000,
    0, 0, 0, 0, 0, 0, 0,
    -- 0x80100040: GP1(02h) acks IRQ1, then the same loop
    0x3c081f80, 0x3c090200, 0xad091814, 0x3c0b8010,
    0x8d0a1814, 0x00000000, 0xad6a0100, 0x08040014, 0x00000000,
}
local sentinel = 0xdeadbeef
local frames = 0
local state
local function irq1() return bit.band(ram[out], 0x01000000) ~= 0 and ram[out] ~= sentinel end
IRQ1SaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 120 then
        for i, w in ipairs(stubs) do ram[base + i - 1] = w end
        ram[out] = sentinel
        PCSX.getRegisters().pc = 0x80100000
    elseif frames == 122 then
        if not irq1() then print('IRQ1 never raised') PCSX.quit(2) return end
        state = PCSX.createSaveState()
        ram[out] = sentinel
        PCSX.getRegisters().pc = 0x80100040
    elseif frames == 124 then
        if ram[out] == sentinel or irq1() then print('GP1(02h) did not ack IRQ1') PCSX.quit(3) return end
        PCSX.loadSaveState(state)
    elseif frames == 125 then
        ram[out] = sentinel
    elseif frames == 127 then
        if irq1() then PCSX.quit(0) else print('IRQ1 lost across the save state') PCSX.quit(1) end
    end
end)
)";

TEST(GPUIRQ, SaveStateKeepsFlag) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-exec", c_irq1SaveState);
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
