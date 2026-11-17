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

#include <uv.h>

#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "support/uvfile.h"
#include "support/zfile.h"

using namespace PCSX;

TEST(UvFile, CloseAndWaitReportsSuccess) {
    UvThreadOp::UvThread uvThread;
    auto path = std::filesystem::temp_directory_path() / "pcsx-redux-uvfile-close-ok.bin";
    IO<UvFile> file(new UvFile(path, FileOps::TRUNCATE));
    ASSERT_FALSE(file->failed());
    std::string payload(200000, 'x');
    file->write(payload.data(), payload.size());
    EXPECT_EQ(file->closeAndWait(), 0);
    EXPECT_EQ(std::filesystem::file_size(path), payload.size());
    std::filesystem::remove(path);
}

#if defined(__linux__)
// Every write to /dev/full fails with ENOSPC, while the open succeeds.
TEST(UvFile, CloseAndWaitReportsQueuedWriteError) {
    UvThreadOp::UvThread uvThread;
    IO<UvFile> file(new UvFile("/dev/full", FileOps::TRUNCATE));
    ASSERT_FALSE(file->failed());
    {
        ZWriter writer(file, ZWriter::GZIP);
        std::string payload(200000, 'x');
        writer.write(payload.data(), payload.size());
        writer.close();
    }
    EXPECT_EQ(file->closeAndWait(), UV_ENOSPC);
}

// The wasm build's UvFile is a PosixFile, and closes through this.
TEST(PosixFile, CloseCheckedReportsWriteError) {
    IO<PosixFile> file(new PosixFile("/dev/full", FileOps::TRUNCATE));
    ASSERT_FALSE(file->failed());
    std::string payload(200000, 'x');
    file->write(payload.data(), payload.size());
    EXPECT_EQ(file->closeChecked(), -1);
}
#endif

TEST(PosixFile, CloseCheckedReportsSuccess) {
    auto path = std::filesystem::temp_directory_path() / "pcsx-redux-posixfile-close-ok.bin";
    IO<PosixFile> file(new PosixFile(path, FileOps::TRUNCATE));
    ASSERT_FALSE(file->failed());
    std::string payload(200000, 'x');
    file->write(payload.data(), payload.size());
    EXPECT_EQ(file->closeChecked(), 0);
    EXPECT_EQ(std::filesystem::file_size(path), payload.size());
    std::filesystem::remove(path);
}
