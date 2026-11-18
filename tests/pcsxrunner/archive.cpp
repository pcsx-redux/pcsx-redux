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

#include <filesystem>

#include "gtest/gtest.h"
#include "main/main.h"
#include "support/file.h"
#include "support/zip-internal.h"

namespace {

	std::string archive1test1 = R"(
local success, result = pcall(function()
	Support.extra.dofile('dummy.lua')
end
)
if success then
	error('Should have failed to open dummy.lua')
end
ran.archive1test1 = true
)";
	std::string archive1test2 = R"(
local notfile = Support.extra.open('dummy.nothere')
if not notfile:failed() then
	error('Should have failed to open dummy.nothere')
end
local success, result = pcall(function()
	Support.extra.addArchive('dummy.zip')
end
)
if success then
	error('Should have failed to open dummy.zip')
end
Support.extra.dofile('test1.lua')
ran.archive1test2 = true
)";
	std::string archive1test3 = R"(
local file = Support.extra.open('arc/somedata.txt')
if file:failed() then
	error('Should have opened arc/somedata.txt')
end
local str = tostring(file:read(file:size()))
if str ~= 'This is some data' then
	error('Should have read the correct data from arc/somedata.txt')
end
Support.extra.dofile('test2.lua')
ran.archive1test3 = true
)";
	std::string archive2test1 = R"(
Support.extra.addArchive('test1.zip')
Support.extra.dofile('arc/test3.lua')
ran.archive2test1 = true
)";
	std::string archive2test2 = R"(
Support.extra.dofile('dir1/test.lua')
ran.archive2test2 = true
)";
	std::string archive2autoexec = R"(
ran = {}
Support.extra.dofile('dir2/test.lua')
gotall = true
if not ran.archive1test1 then
	gotall = false
end
if not ran.archive1test2 then
	gotall = false
end
if not ran.archive1test3 then
	gotall = false
end
if not ran.archive2test1 then
	gotall = false
end
if not ran.archive2test2 then
	gotall = false
end
if not gotall then
	error('Not all tests ran successfully')
end
PCSX.quit(0)
)";
}

void closeArchivesForTests();

class ArchiveTest : public ::testing::Test {
private:
	void writeOneFile(PCSX::IO<PCSX::File> out, std::string_view filename, std::string_view contents) {
		PCSX::LocalFileHeader header;
		header.reset();
		header.get<PCSX::Signature>().value = 0x04034b50;
		header.get<PCSX::FilenameLength>().value = filename.size();
		header.get<PCSX::CompressedSize>().value = contents.size();
		header.get<PCSX::UncompressedSize>().value = contents.size();
		header.serialize(out);
		out->writeString(filename);
		out->writeString(contents);
	}
protected:
	void SetUp() override {
		PCSX::IO<PCSX::File> out = new PCSX::PosixFile("test1.zip", PCSX::FileOps::TRUNCATE);
        writeOneFile(out, "arc/somedata.txt", "This is some data");
		writeOneFile(out, "arc/test1.lua", archive1test1);
		writeOneFile(out, "arc/test2.lua", archive1test2);
		writeOneFile(out, "arc/test3.lua", archive1test3);
		out->write<uint32_t>(0x06054b50);
		out->close();
		out = new PCSX::PosixFile("test2.zip", PCSX::FileOps::TRUNCATE);
		writeOneFile(out, "dir1/test.lua", archive2test1);
		writeOneFile(out, "dir2/test.lua", archive2test2);
		writeOneFile(out, "autoexec.lua", archive2autoexec);
		out->write<uint32_t>(0x06054b50);
		out->close();
	}
	void TearDown() override {
		closeArchivesForTests();
		std::filesystem::remove("test1.zip");
		std::filesystem::remove("test2.zip");
	}
};

TEST_F(ArchiveTest, test) {
	MainInvoker invoker("-no-ui", "-testmode", "-interpreter", "-luacov", "-archive", "test2.zip");
	int ret = invoker.invoke();
	EXPECT_EQ(ret, 0);
}
