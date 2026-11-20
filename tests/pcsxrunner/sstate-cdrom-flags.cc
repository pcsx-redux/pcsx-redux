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

// The CD-ROM's read-after-Pause resume flag (CDRom field 50) and the Test 04h/05h
// counter flag (field 51) must survive a save state. The script sets both in a saved
// state, loads it, saves again and checks both come back set. It then loads the same
// state with both fields removed, the shape of a state saved before they existed, and
// checks both come back cleared.
TEST(SaveStateCDRom, FlagsSurviveSaveState) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-dofile", "tests/lua/sstate-cdrom-flags.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
