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

#include "core/memorycard.h"

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/system.h"
#include "gtest/gtest.h"

namespace {

constexpr size_t c_cardSize = 128 * 1024;

class TestSystem final : public PCSX::System {
  public:
    void softReset() override {}
    void hardReset() override {}
    void biosPutc(int) override {}
    const PCSX::Arguments& getArgs() const override { throw std::logic_error("unused"); }
    void printf(std::string&& s) override { output += s; }
    void log(PCSX::LogClass, std::string&& s) override { output += s; }
    void message(std::string&& s) override { output += s; }
    void luaMessage(const std::string&, bool) override {}
    void update(bool) override {}
    void close() override {}
    void purgeAllEvents() override {}
    void testQuit(int) override {}
    std::string output;
};

class MemoryCardLoad : public ::testing::Test {
  protected:
    void SetUp() override {
        m_oldSystem = PCSX::g_system;
        PCSX::g_system = &m_system;
        m_dir = std::filesystem::temp_directory_path() /
                ("pcsx-redux-mcd-test-" + std::to_string(::getpid()) + "-" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir);
        // Something that is not a blank card, to see whether a failed load touched it.
        memset(m_card.getMcdData(), 0x5a, c_cardSize);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::permissions(m_dir / "card.mcd", std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(m_dir, ec);
        PCSX::g_system = m_oldSystem;
    }
    static std::vector<char> readFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    static void writeFile(const std::filesystem::path& path, size_t size, char fill) {
        std::ofstream out(path, std::ios::binary);
        std::string data(size, fill);
        out.write(data.data(), data.size());
    }
    bool cardUntouched() {
        const char* data = m_card.getMcdData();
        for (size_t i = 0; i < c_cardSize; i++) {
            if (data[i] != 0x5a) return false;
        }
        return true;
    }
    PCSX::u8string u8(const std::filesystem::path& path) { return path.u8string(); }

    TestSystem m_system;
    PCSX::System* m_oldSystem = nullptr;
    std::filesystem::path m_dir;
    PCSX::MemoryCard m_card;
};

}  // namespace

// A card file that does not exist yet is created formatted and loaded, as before. The fresh card now counts
// as saved, so the first access no longer writes the same blank card back.
TEST_F(MemoryCardLoad, MissingFileIsCreated) {
    const auto path = m_dir / "card.mcd";
    EXPECT_TRUE(m_card.loadMcd(u8(path)));
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(std::filesystem::file_size(path), c_cardSize);
    EXPECT_EQ(m_card.getMcdData()[0], 'M');
    EXPECT_EQ(m_card.getMcdData()[1], 'C');
    EXPECT_FALSE(m_card.dataChanged());
}

// Unchanged behaviour: a good card file is loaded as is.
TEST_F(MemoryCardLoad, ValidFileLoads) {
    const auto path = m_dir / "card.mcd";
    writeFile(path, c_cardSize, 0x11);
    EXPECT_TRUE(m_card.loadMcd(u8(path)));
    EXPECT_EQ(m_card.getMcdData()[0], 0x11);
    EXPECT_EQ(m_card.getMcdData()[c_cardSize - 1], 0x11);
    EXPECT_FALSE(m_card.dataChanged());
}

// A card file that exists but cannot be read must not be replaced by a blank card.
TEST_F(MemoryCardLoad, UnreadableFileIsNotOverwritten) {
    if (::geteuid() == 0) GTEST_SKIP() << "root can read a write-only file";
    const auto path = m_dir / "card.mcd";
    writeFile(path, c_cardSize, 0x33);
    std::filesystem::permissions(path, std::filesystem::perms::owner_write);
    EXPECT_FALSE(m_card.loadMcd(u8(path)));
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    const auto contents = readFile(path);
    ASSERT_EQ(contents.size(), c_cardSize);
    EXPECT_EQ(contents[0], 0x33);
    EXPECT_EQ(contents[c_cardSize - 1], 0x33);
    EXPECT_TRUE(cardUntouched());
    EXPECT_NE(m_system.output.find(path.string()), std::string::npos) << m_system.output;
}

// A truncated card file is a failed load, reported, not an exception out of the emulator.
TEST_F(MemoryCardLoad, ShortFileFailsWithoutThrowing) {
    const auto path = m_dir / "card.mcd";
    writeFile(path, 100, 0x44);
    bool loaded = true;
    EXPECT_NO_THROW(loaded = m_card.loadMcd(u8(path)));
    EXPECT_FALSE(loaded);
    EXPECT_TRUE(cardUntouched());
    EXPECT_EQ(std::filesystem::file_size(path), 100u);
    EXPECT_NE(m_system.output.find(path.string()), std::string::npos) << m_system.output;
}

// A file that can be neither opened nor created leaves the card's data alone.
TEST_F(MemoryCardLoad, UncreatableFileFails) {
    const auto path = m_dir / "missing-dir" / "card.mcd";
    EXPECT_FALSE(m_card.loadMcd(u8(path)));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_TRUE(cardUntouched());
    EXPECT_NE(m_system.output.find(path.string()), std::string::npos) << m_system.output;
}

// An import names a file that should already be there: a missing one is not created.
TEST_F(MemoryCardLoad, ImportDoesNotCreate) {
    const auto path = m_dir / "card.mcd";
    EXPECT_FALSE(m_card.loadMcd(u8(path), false));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_TRUE(cardUntouched());
}
