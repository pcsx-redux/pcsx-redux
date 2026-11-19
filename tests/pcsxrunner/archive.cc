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
#include <initializer_list>
#include <string_view>
#include <utility>

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
Support.extra.dofile('overlay/test.lua')
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
if not ran.overlaytest then
	error('Overlay test did not run successfully')
end
if ran.wrongoverlaytest then
	error('Wrong overlay test ran')
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
        writeOneFile(out, "overlay/test.lua", "ran.overlaytest = true");
		out->write<uint32_t>(0x06054b50);
		out->close();
		out = new PCSX::PosixFile("test2.zip", PCSX::FileOps::TRUNCATE);
		writeOneFile(out, "dir1/test.lua", archive2test1);
		writeOneFile(out, "dir2/test.lua", archive2test2);
		writeOneFile(out, "autoexec.lua", archive2autoexec);
        writeOneFile(out, "overlay/test.lua", "ran.wrongoverlaytest = true");
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

class ArchiveFilesTest : public ::testing::Test {
  protected:
    void writeZip(const char* path, std::initializer_list<std::pair<std::string_view, std::string_view>> entries) {
        PCSX::IO<PCSX::File> out = new PCSX::PosixFile(path, PCSX::FileOps::TRUNCATE);
        for (auto& [filename, contents] : entries) {
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
        out->write<uint32_t>(0x06054b50);
        out->close();
    }
    int run() {
        MainInvoker invoker("-no-ui", "-testmode", "-interpreter", "-archive", "files.zip");
        return invoker.invoke();
    }
    void TearDown() override {
        closeArchivesForTests();
        std::filesystem::remove("files.zip");
        std::filesystem::remove("files-inner.zip");
        std::filesystem::remove_all("archive-test-moddir");
    }
    // Runs archive-test-moddir/a.lua and expects it to return 'from archive'.
    static constexpr std::string_view c_runA = R"(
local success, result = pcall(function()
    local r = Support.extra.dofile('archive-test-moddir/a.lua')
    return r
end)
if success and result == 'from archive' then
    PCSX.quit(0)
else
    print('archive-test-moddir/a.lua returned: ' .. tostring(result))
    PCSX.quit(1)
end
)";
};

// A relative load inside an archive has to resolve inside the archive,
// whatever happens to exist in the current directory.
TEST_F(ArchiveFilesTest, HostDirectoryOfTheSameName) {
    std::filesystem::create_directories("archive-test-moddir");
    writeZip("files.zip", {{"archive-test-moddir/a.lua", "local r = Support.extra.dofile('b.lua')\nreturn r"},
                           {"archive-test-moddir/b.lua", "return 'from archive'"},
                           {"autoexec.lua", c_runA}});
    EXPECT_EQ(run(), 0);
}

TEST_F(ArchiveFilesTest, HostFileOfTheSameName) {
    std::filesystem::create_directories("archive-test-moddir");
    {
        PCSX::IO<PCSX::File> out = new PCSX::PosixFile("archive-test-moddir/b.lua", PCSX::FileOps::TRUNCATE);
        out->writeString("return 'from disk'");
        out->close();
    }
    writeZip("files.zip", {{"archive-test-moddir/a.lua", "local r = Support.extra.dofile('b.lua')\nreturn r"},
                           {"archive-test-moddir/b.lua", "return 'from archive'"},
                           {"autoexec.lua", c_runA}});
    EXPECT_EQ(run(), 0);
}

// Forwarding to another file with a tail call still loads it next to the calling file.
TEST_F(ArchiveFilesTest, TailCall) {
    writeZip("files.zip", {{"archive-test-moddir/a.lua", "return Support.extra.dofile('b.lua')"},
                           {"archive-test-moddir/b.lua", "return 'from archive'"},
                           {"autoexec.lua", c_runA}});
    EXPECT_EQ(run(), 0);
}

// loadfile and open, with a path going up a directory first.
TEST_F(ArchiveFilesTest, LoadfileAndParentPath) {
    writeZip("files.zip", {{"archive-test-moddir/a.lua", R"(
local f = Support.extra.loadfile('../other/b.lua')
local data = Support.extra.open('../other/data.txt')
local r = f() .. ' ' .. tostring(data:read(data:size()))
if r ~= 'from archive and data' then error('got ' .. r) end
return 'from archive'
)"},
                           {"other/b.lua", "return 'from archive and'"},
                           {"other/data.txt", "data"},
                           {"autoexec.lua", c_runA}});
    EXPECT_EQ(run(), 0);
}

// An archive's autoexec runs once when it is mounted, and not again when its files are used.
TEST_F(ArchiveFilesTest, AutoexecRunsOnce) {
    writeZip("files-inner.zip", {{"autoexec.lua", "autoexecRuns = (autoexecRuns or 0) + 1"},
                                 {"lib/b.lua", "return 'from archive'"}});
    writeZip("files.zip", {{"archive-test-moddir/a.lua", R"(
Support.extra.addArchive('files-inner.zip')
local r = Support.extra.dofile('lib/b.lua')
Support.extra.dofile('lib/b.lua')
if autoexecRuns ~= 1 then error('autoexec ran ' .. tostring(autoexecRuns) .. ' times') end
return r
)"},
                           {"autoexec.lua", c_runA}});
    EXPECT_EQ(run(), 0);
}

// Error messages name the archived file: missing files, runtime errors, syntax errors.
TEST_F(ArchiveFilesTest, ErrorMessages) {
    writeZip("files.zip", {{"archive-test-moddir/a.lua", R"(
local function expect(msg, needle)
    if not tostring(msg):find(needle, 1, true) then error('"' .. tostring(msg) .. '" does not mention "' .. needle .. '"') end
end
local ok, msg = pcall(Support.extra.dofile, 'nothere.lua')
if ok then error('nothere.lua loaded') end
expect(msg, 'nothere.lua')
ok, msg = pcall(function() local r = Support.extra.dofile('boom.lua') return r end)
if ok then error('boom.lua did not fail') end
expect(msg, 'archive-test-moddir/boom.lua:2:')
local f, err = Support.extra.loadfile('broken.lua')
if f then error('broken.lua compiled') end
expect(err, 'archive-test-moddir/broken.lua:1:')
return 'from archive'
)"},
                           {"archive-test-moddir/boom.lua", "local x = 1\nerror('boom')\n"},
                           {"archive-test-moddir/broken.lua", "this is not lua\n"},
                           {"autoexec.lua", c_runA}});
    EXPECT_EQ(run(), 0);
}

// A file loaded from disk resolves "../" through the filesystem: from a symlinked directory, the parent
// is the link target's parent, not the directory holding the link.
TEST_F(ArchiveFilesTest, DiskFallbackThroughSymlink) {
    std::filesystem::create_directories("archive-test-moddir/real/inner");
    std::error_code ec;
    std::filesystem::create_directory_symlink("real/inner", "archive-test-moddir/link", ec);
    // Creating symlinks on Windows needs a privilege or developer mode, which CI may not have.
    if (ec) GTEST_SKIP() << "cannot create a directory symlink here: " << ec.message();
    auto writeDiskFile = [](const char* path, std::string_view contents) {
        PCSX::IO<PCSX::File> out = new PCSX::PosixFile(path, PCSX::FileOps::TRUNCATE);
        out->writeString(contents);
        out->close();
    };
    writeDiskFile("archive-test-moddir/real/inner/a.lua", "local r = Support.extra.dofile('../b.lua')\nreturn r");
    writeDiskFile("archive-test-moddir/real/b.lua", "return 'through the link'");
    writeDiskFile("archive-test-moddir/b.lua", "return 'next to the link'");
    writeZip("files.zip", {{"autoexec.lua", R"(
local success, result = pcall(function()
    local r = Support.extra.dofile('archive-test-moddir/link/a.lua')
    return r
end)
if success and result == 'through the link' then
    PCSX.quit(0)
else
    print('archive-test-moddir/link/a.lua returned: ' .. tostring(result))
    PCSX.quit(1)
end
)"}});
    EXPECT_EQ(run(), 0);
}
