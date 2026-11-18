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

// SIO1 state across save/load and rewind, with the SIO1 server running in protobuf mode and
// tests/lua/sstate-sio1-edges.lua connected to it as the peer: registers, IRQ flag, the RX FIFO
// replaced and not merged, the interrupt delay, a state without the SIO1 message, an unknown version.
TEST(SaveStateSIO1Edges, RoundTrip) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-sio1-server", "-sio1-port", "6790", "-dofile",
                        "tests/lua/sstate-sio1-edges.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
