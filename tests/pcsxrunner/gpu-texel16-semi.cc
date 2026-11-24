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

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "main/main.h"

// 16bpp texels against semi-transparency, OpenGL renderer against the soft one.
// Only a 0x0000 texel is transparent, 0x8000 is drawn black, and only texels
// with bit 15 set are blended: a 0x7FFF texel drawn by a semi-transparent
// primitive lands opaque. tests/lua/gpu-texel16-semi.lua pushes the GP0
// commands through a guest call, draws a semi rectangle, a semi quad and an
// opaque rectangle over a known background, and dumps the rows it drew. The
// soft renderer's rows are the reference the OpenGL ones have to match.
// The OpenGL renderer needs a GL context, so this runs with the GUI and only
// on Linux, where CI runs the suite under xvfb. Without a display it is skipped.
#if defined(__linux__)
static bool haveDisplay() { return std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY"); }

static std::vector<std::string> drawTexels(bool opengl) {
    auto path = std::filesystem::path(testing::TempDir()) / (opengl ? "texel16-gl.txt" : "texel16-soft.txt");
    std::filesystem::remove(path);
    std::string out = "drawTexel16([==[" + path.generic_string() + "]==])";
    int ret;
    if (opengl) {
        MainInvoker invoker("-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter", "-openglgpu",
                            "-dofile", "tests/lua/gpu-texel16-semi.lua", "-exec", out.c_str());
        ret = invoker.invoke();
    } else {
        MainInvoker invoker("-no-ui", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter", "-dofile",
                            "tests/lua/gpu-texel16-semi.lua", "-exec", out.c_str());
        ret = invoker.invoke();
    }
    EXPECT_EQ(ret, 0);
    std::vector<std::string> rows;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) rows.push_back(line);
    return rows;
}

TEST(OpenGLTexel16, SemiMatchesSoft) {
    if (!haveDisplay()) GTEST_SKIP() << "no display";
    auto soft = drawTexels(false);
    // The reference itself, checked by hand on one row of each rectangle. Over 0x4210 with
    // B/2 + F/2, 0x8000 blends to 0xA108 and 0xFFFF to 0xDEF7, keeping bit 15, 0x7FFF lands
    // as is and 0x0000 leaves the background. Drawn opaque, every texel but 0x0000 lands as is.
    ASSERT_EQ(soft.size(), 30u);
    EXPECT_EQ(soft[4],
              "y=19 4210 4210 4210 4210 4210 4210 4210 4210 4210 4210 4210 4210 a108 a108 a108 a108 a108 a108 a108 "
              "a108 7fff 7fff 7fff 7fff 7fff 7fff 7fff 7fff def7 def7 def7 def7 def7 def7 def7 def7 001f 001f 001f "
              "001f 001f 001f 001f 001f a117 a117 a117 a117 a117 a117 a117 a117 4210 4210 4210 4210");
    EXPECT_EQ(soft[24],
              "y=51 4210 4210 4210 4210 4210 4210 4210 4210 4210 4210 4210 4210 8000 8000 8000 8000 8000 8000 8000 "
              "8000 7fff 7fff 7fff 7fff 7fff 7fff 7fff 7fff ffff ffff ffff ffff ffff ffff ffff ffff 001f 001f 001f "
              "001f 001f 001f 001f 001f 801f 801f 801f 801f 801f 801f 801f 801f 4210 4210 4210 4210");
    auto gl = drawTexels(true);
    ASSERT_EQ(gl.size(), soft.size());
    for (size_t i = 0; i < soft.size(); i++) EXPECT_EQ(gl[i], soft[i]);
}
#endif
