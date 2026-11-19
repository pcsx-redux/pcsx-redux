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

static int run(bool deinterlace) {
    std::string call = std::string("GPUDumpDeinterlace(") + (deinterlace ? "true" : "false") + ")";
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-dofile", "tests/lua/gpudump-deinterlace.lua", "-exec", call.c_str());
    return invoker.invoke();
}

TEST(GPUDumpDeinterlace, AccurateRecordsFields) { EXPECT_EQ(run(false), 0); }
TEST(GPUDumpDeinterlace, DeinterlaceRecordsFields) { EXPECT_EQ(run(true), 0); }
