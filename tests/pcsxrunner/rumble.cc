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

// The config mode commands the guest test uses only reach the DualShock handler
// on an analog pad, and safe mode starts port 1 as a digital one, so set the
// type here. The type is cached at map() time, hence the third line.
static const char setup[] = R"(
PCSX.settings.pads[1].DeviceType = 'Analog'
PCSX.settings.pads[1].Connected = true
PCSX.SIO0.slots[1].pads[1].map()
)";

TEST(Rumble, Interpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-luacov", "-exec", setup, "-loadexe", "src/mips/tests/sio/rumble/rumble.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

TEST(Rumble, Dynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-luacov", "-exec", setup, "-loadexe", "src/mips/tests/sio/rumble/rumble.ps-exe");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
