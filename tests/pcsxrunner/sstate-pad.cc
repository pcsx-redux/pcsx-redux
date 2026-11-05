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

// A pad's own protocol state has to follow a save state, not only the SIO side of it.
// tests/lua/sstate-pad.lua drives port 1 (an analog pad) through raw SIO0 writes from a planted stub:
// it enters config mode, then starts command 45h (get analog mode) and stops after three
// bytes, in the middle of the transfer. That is the state saved. The rest of the transfer
// is then sent once, as the control, and the pad is moved somewhere else entirely (config
// mode left). Loading the saved state and sending the rest of the transfer again must
// give the control's replies, which only holds if the pad itself, and not only SIO, came
// back to the middle of command 45h in config mode.
//
// The second half takes the same state, strips the pad messages from it the way a state
// written before they existed looks, and loads it: the pads must be left as they were.

TEST(SaveStatePad, TransferResumesAfterLoad) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-dofile", "tests/lua/sstate-pad.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
