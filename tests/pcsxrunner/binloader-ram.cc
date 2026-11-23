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

#include <stdint.h>

#include <filesystem>
#include <map>
#include <string>

#include <stdexcept>
#include <vector>

#include "elfio/elfio.hpp"
#include "gtest/gtest.h"
#include "support/file.h"
#include "support/mem4g.h"
#include "supportpsx/binloader.h"

// A loader told how much RAM there is refuses any binary that fits neither in RAM nor in
// the BIOS ROM, before writing a single byte. Without the bound, a header claiming too much
// data wraps around the RAM mirrors and lands over the kernel and the BIOS image.

namespace {

constexpr uint32_t c_ram = 2 * 1024 * 1024;

std::vector<uint8_t> makePSEXE(uint32_t addr, uint32_t size, uint32_t dataSize) {
    std::vector<uint8_t> exe(2048 + dataSize, 0x55);
    std::fill(exe.begin(), exe.begin() + 2048, 0);
    memcpy(exe.data(), "PS-X EXE", 8);
    auto put = [&exe](size_t offset, uint32_t value) { memcpy(exe.data() + offset, &value, 4); };
    put(0x10, addr);
    put(0x18, addr);
    put(0x1c, size);
    return exe;
}

bool load(std::vector<uint8_t>& exe, PCSX::IO<PCSX::Mem4G> dest, std::optional<uint32_t> ramSize) {
    PCSX::IO<PCSX::File> in(new PCSX::BufferFile(exe.data(), exe.size()));
    PCSX::BinaryLoader::Info info;
    info.ramSize = ramSize;
    std::map<uint32_t, std::string> symbols;
    return PCSX::BinaryLoader::load(in, dest.asA<PCSX::File>(), info, symbols);
}

}  // namespace

TEST(BinaryLoader, PSEXEInsideRAMLoads) {
    auto exe = makePSEXE(0x80010000, 0x800, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_TRUE(load(exe, dest, c_ram));
    EXPECT_EQ(dest->lowestAddress(), 0x80010000);
    EXPECT_EQ(dest->actualSize(), 0x800);
}

TEST(BinaryLoader, PSEXEEndingAtTheEndOfRAMLoads) {
    auto exe = makePSEXE(0x80000000 + c_ram - 0x800, 0x800, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_TRUE(load(exe, dest, c_ram));
    EXPECT_EQ(dest->actualSize(), 0x800);
}

TEST(BinaryLoader, PSEXEInARAMMirrorLoads) {
    auto exe = makePSEXE(0x80600000, 0x800, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_TRUE(load(exe, dest, c_ram));
    EXPECT_EQ(dest->lowestAddress(), 0x80600000);
    EXPECT_EQ(dest->actualSize(), 0x800);
}

TEST(BinaryLoader, PSEXELargerThanRAMIsRefused) {
    auto exe = makePSEXE(0x80000000, c_ram + 1, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_THROW(load(exe, dest, c_ram), std::runtime_error);
    EXPECT_TRUE(dest->isEmpty());
}

TEST(BinaryLoader, PSEXECrossingTheEndOfTheRAMWindowIsRefused) {
    auto exe = makePSEXE(0x80800000 - 0x800, 0x804, 0x804);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_THROW(load(exe, dest, c_ram), std::runtime_error);
    EXPECT_TRUE(dest->isEmpty());
}

TEST(BinaryLoader, PSEXEInTheBIOSLoads) {
    auto exe = makePSEXE(0xbfc00000, 0x800, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_TRUE(load(exe, dest, c_ram));
    EXPECT_EQ(dest->lowestAddress(), 0xbfc00000);
    EXPECT_EQ(dest->actualSize(), 0x800);
}

TEST(BinaryLoader, PSEXERunningPastTheBIOSIsRefused) {
    auto exe = makePSEXE(0xbfc80000 - 0x800, 0x804, 0x804);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_THROW(load(exe, dest, c_ram), std::runtime_error);
    EXPECT_TRUE(dest->isEmpty());
}

TEST(BinaryLoader, PSEXEClaimingAGigabyteIsRefused) {
    // The header lies about the size, so this also checks the data is never read in.
    auto exe = makePSEXE(0x80010000, 0x40000000, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_THROW(load(exe, dest, c_ram), std::runtime_error);
    EXPECT_TRUE(dest->isEmpty());
}

TEST(BinaryLoader, PSEXEOutsideOfTheRAMSegmentsIsRefused) {
    auto exe = makePSEXE(0xc0000000, 0x800, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_THROW(load(exe, dest, c_ram), std::runtime_error);
    EXPECT_TRUE(dest->isEmpty());
}

TEST(BinaryLoader, PSEXEIsntBoundedWithoutARAMSize) {
    auto exe = makePSEXE(0xc0000000, 0x1000, 0x1000);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_TRUE(load(exe, dest, std::nullopt));
    EXPECT_EQ(dest->actualSize(), 0x1000);
}

TEST(BinaryLoader, ELFIsCheckedBeforeAnySectionIsWritten) {
    using namespace ELFIO;
    elfio writer;
    writer.create(ELFCLASS32, ELFDATA2LSB);
    writer.set_type(ET_EXEC);
    writer.set_machine(EM_MIPS);
    writer.set_entry(0x80010000);

    char code[0x40] = {};
    section* text = writer.sections.add(".text");
    text->set_type(SHT_PROGBITS);
    text->set_flags(SHF_ALLOC | SHF_EXECINSTR);
    text->set_address(0x80010000);
    text->set_addr_align(4);
    text->set_data(code, sizeof(code));
    // The second section is the one that doesn't fit, so the first must not have been written.
    section* data = writer.sections.add(".data");
    data->set_type(SHT_PROGBITS);
    data->set_flags(SHF_ALLOC | SHF_WRITE);
    data->set_address(0x80800000 - 0x20);
    data->set_addr_align(4);
    data->set_data(code, sizeof(code));

    auto path = (std::filesystem::temp_directory_path() / "binloader-ram.elf").string();
    ASSERT_TRUE(writer.save(path));
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    {
        PCSX::IO<PCSX::File> in(new PCSX::PosixFile(path));
        PCSX::BinaryLoader::Info info;
        info.ramSize = c_ram;
        std::map<uint32_t, std::string> symbols;
        EXPECT_THROW(PCSX::BinaryLoader::load(in, dest.asA<PCSX::File>(), info, symbols), std::runtime_error);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    EXPECT_TRUE(dest->isEmpty());
}
