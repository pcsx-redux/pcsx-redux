/***************************************************************************
 *   Copyright (C) 2021 PCSX-Redux authors                                 *
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

#include "support/binpath.h"

#include <filesystem>
#include <string>

#include "gtest/gtest.h"

#ifdef __linux__

#include <stdio.h>

// Leaves the stack below the caller full of '/' bytes, so a path built from an
// unterminated buffer there picks up extra components.
static __attribute__((noinline)) void dirtyStack() {
    volatile char junk[BUFSIZ * 2];
    for (auto& c : junk) c = '/';
}

TEST(BinPath, ExecutablePathMatchesProcSelfExe) {
    auto expected = std::filesystem::read_symlink("/proc/self/exe");
    for (int i = 0; i < 16; i++) {
        dirtyStack();
        std::filesystem::path path = PCSX::BinPath::getExecutablePath();
        EXPECT_EQ(path, expected);
        EXPECT_EQ(path.parent_path(), expected.parent_path());
    }
}

#endif
