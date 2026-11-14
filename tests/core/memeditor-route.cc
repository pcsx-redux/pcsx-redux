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
#include "gui/memeditor-route.h"

using Target = PCSX::MemEditorRoute::Target;

static void expectRoute(uint32_t address, Target target, uint32_t offset) {
    const auto r = PCSX::MemEditorRoute::route(address);
    EXPECT_EQ(r.target, target) << std::hex << address;
    EXPECT_EQ(r.offset, offset) << std::hex << address;
}

TEST(MemEditorRoute, MainRam) {
    for (uint32_t base : {0x00000000u, 0x80000000u, 0xa0000000u}) {
        expectRoute(base | 0x1234, Target::MainRam, 0x1234);
        expectRoute(base | 0x7fffff, Target::MainRam, 0x7fffff);
    }
    expectRoute(0x00800000, Target::None, 0);
}

TEST(MemEditorRoute, ParallelPort) {
    for (uint32_t base : {0x1f000000u, 0x9f000000u, 0xbf000000u}) {
        expectRoute(base, Target::ParallelPort, 0);
        expectRoute(base | 0x7ffff, Target::ParallelPort, 0x7ffff);
        expectRoute(base | 0x80000, Target::None, 0);
    }
}

TEST(MemEditorRoute, Scratchpad) {
    for (uint32_t base : {0x1f800000u, 0x9f800000u}) {
        expectRoute(base, Target::Scratchpad, 0);
        expectRoute(base | 0x3ff, Target::Scratchpad, 0x3ff);
        expectRoute(base | 0x400, Target::None, 0);
    }
    expectRoute(0xbf800000, Target::None, 0);
}

TEST(MemEditorRoute, HardwareRegisters) {
    for (uint32_t base : {0x1f801000u, 0x9f801000u, 0xbf801000u}) {
        expectRoute(base, Target::HardwareRegisters, 0);
        expectRoute(base + 0x1fff, Target::HardwareRegisters, 0x1fff);
        expectRoute(base + 0x2000, Target::None, 0);
    }
    expectRoute(0x1f800fff, Target::None, 0);
    expectRoute(0x1f803000, Target::None, 0);
}

TEST(MemEditorRoute, Bios) {
    for (uint32_t base : {0x1fc00000u, 0x9fc00000u, 0xbfc00000u}) {
        expectRoute(base, Target::Bios, 0);
        expectRoute(base | 0x7ffff, Target::Bios, 0x7ffff);
        expectRoute(base | 0x80000, Target::None, 0);
    }
}

TEST(MemEditorRoute, Unmapped) {
    expectRoute(0xfffe0130, Target::None, 0);
    expectRoute(0xc0000000, Target::None, 0);
    expectRoute(0x1f900000, Target::None, 0);
}
