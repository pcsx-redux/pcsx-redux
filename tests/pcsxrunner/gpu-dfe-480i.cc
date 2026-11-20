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

static int runDeinterlace(bool deinterlace) {
    std::string call = std::string("GPUDFE480iDeinterlace(") + (deinterlace ? "true" : "false") + ")";
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-dofile", "tests/lua/gpu-dfe-480i-deinterlace.lua", "-exec", call.c_str());
    return invoker.invoke();
}

TEST(GPUDFE480i, AccurateSkipsOneField) { EXPECT_EQ(runDeinterlace(false), 0); }
TEST(GPUDFE480i, DeinterlaceDrawsBothFields) { EXPECT_EQ(runDeinterlace(true), 0); }
