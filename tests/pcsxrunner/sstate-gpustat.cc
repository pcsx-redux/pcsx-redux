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

// The drawing environment set by GP0(E1h)..GP0(E6h) must survive a save state round trip.
// The script plants a few instructions that write GP0(E1h) and GP0(E6h), saves, loads,
// and saves again. GPUSTAT bits 0-12 and 15 (texpage, dither, draw-to-display, mask)
// must read the same in both states. A flat rectangle drawn after the load must land
// where the same rectangle landed before it, which covers the drawing area, offset and
// mask state that GPUSTAT does not report.
//
// plant() writes each GP0 word through a stub at 0x80100000 that then spins; like
// tests/lua/pcwrite.lua, it pauses and sets the pc from the main loop, not mid-Execute(). The
// environment is texpage (5,1) 8-bit with dither and draw-to-display, a texture window,
// drawing area (16,8)-(400,256), offset (16,2), and mask set + check. The rectangle is
// flat blue 16x16 at (0,0): it lands at (16,2), clipped to rows 8 and below.
TEST(SaveStateGPU, DrawEnvironmentRoundTrips) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-dofile", "tests/lua/sstate-gpustat-draw-env.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// GP1(08h) is replayed after GP1(00h) on load, and GP1(00h) resets the display mode: a 480i mode set
// before the save has to be the one a second save records.
TEST(SaveStateGPU, DisplayModeRoundTrips) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-dofile", "tests/lua/sstate-gpustat-display-mode.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
