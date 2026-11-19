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
// One exe per area of nugget's tests/cdrom.
#define CDROM_TEST(name, area)                                                                                      \
    TEST(cdrom, name##Interpreter) {                                                                                \
        MainInvoker invoker("-no-ui", "-run", "-stdout", "-bios", "src/mips/openbios/openbios.bin", "-testmode",    \
                            "-interpreter", "-iso", "test.cue", "-dofile", "tests/lua/cdrom-lid-slots.lua",         \
                            "-loadexe", "src/mips/tests/cdrom/" #area "/cdrom-" #area ".ps-exe");                   \
        int ret = invoker.invoke();                                                                                 \
        EXPECT_EQ(ret, 0);                                                                                          \
    }                                                                                                               \
    TEST(cdrom, name##Dynarec) {                                                                                    \
        MainInvoker invoker("-no-ui", "-run", "-stdout", "-bios", "src/mips/openbios/openbios.bin", "-testmode",    \
                            "-dynarec", "-iso", "test.cue", "-dofile", "tests/lua/cdrom-lid-slots.lua", "-loadexe", \
                            "src/mips/tests/cdrom/" #area "/cdrom-" #area ".ps-exe");                               \
        int ret = invoker.invoke();                                                                                 \
        EXPECT_EQ(ret, 0);                                                                                          \
    }

CDROM_TEST(Commands, commands)
CDROM_TEST(Reading, reading)
CDROM_TEST(Playback, playback)
CDROM_TEST(Race, race)
CDROM_TEST(Errors, errors)

// Boots the test disc through OpenBIOS's shell and kernel loader, with no -loadexe. Passes once
// the disc's boot executable is in RAM and the CPU is running inside it, fails after 6000 frames.
TEST(cdrom, OpenBIOSBootsDiscInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-iso", "test.cue", "-dofile", "tests/lua/cdrom-disc-boot.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(cdrom, OpenBIOSBootsDiscDynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-iso",
                        "test.cue", "-dofile", "tests/lua/cdrom-disc-boot.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
