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

// A v4 save state is a v5 one with 1MB of VRAM. The script saves a state, rewrites it
// as v4 (version 4, VRAM cut to its first 1MB), and loads it. A RAM marker changed after
// saving must come back, and on a 1MB console the upper bank must still read the
// open-bus 0xFFFF a fresh console has there. The unmodified v5 state must load too.
TEST(SaveStateV4, LoadsIntoLowerBank) {
    // The open-bus check is a software-GPU property, so do not inherit a persisted renderer.
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-1mbvram", "-dofile", "tests/lua/sstate-v4.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
