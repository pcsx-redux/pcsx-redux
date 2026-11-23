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
#include <utility>
#include <vector>

#include "elfio/elfio.hpp"
#include "gtest/gtest.h"
#include "support/file.h"
#include "support/mem4g.h"
#include "supportpsx/binloader.h"

// The caller's span check sees every span of a binary before any of it is read or written,
// and a refusal from it leaves the destination untouched.

namespace {

using Spans = std::vector<std::pair<uint32_t, uint64_t>>;

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

// Records what it is asked, and refuses anything bigger than 2MB.
PCSX::BinaryLoader::Info recordingInfo(Spans& spans) {
    PCSX::BinaryLoader::Info info;
    info.spanCheck = [&spans](uint32_t address, uint64_t size) -> std::optional<std::string> {
        spans.emplace_back(address, size);
        if (size > 0x200000) return "too big";
        return {};
    };
    return info;
}

bool load(std::vector<uint8_t>& exe, PCSX::IO<PCSX::Mem4G> dest, PCSX::BinaryLoader::Info info) {
    PCSX::IO<PCSX::File> in(new PCSX::BufferFile(exe.data(), exe.size()));
    std::map<uint32_t, std::string> symbols;
    return PCSX::BinaryLoader::load(in, dest.asA<PCSX::File>(), info, symbols);
}

}  // namespace

TEST(BinaryLoader, PSEXESpanIsChecked) {
    Spans spans;
    auto exe = makePSEXE(0x80010000, 0x800, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_TRUE(load(exe, dest, recordingInfo(spans)));
    ASSERT_EQ(spans.size(), 1);
    EXPECT_EQ(spans[0].first, 0x80010000);
    EXPECT_EQ(spans[0].second, 0x800);
    EXPECT_EQ(dest->lowestAddress(), 0x80010000);
    EXPECT_EQ(dest->actualSize(), 0x800);
}

TEST(BinaryLoader, RefusedPSEXEWritesNothing) {
    Spans spans;
    auto exe = makePSEXE(0x80010000, 0x200004, 0x200004);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_THROW(load(exe, dest, recordingInfo(spans)), std::runtime_error);
    EXPECT_TRUE(dest->isEmpty());
}

TEST(BinaryLoader, PSEXEClaimingAGigabyteIsRefusedBeforeReading) {
    // The header lies about the size, so a read would come up short; the refusal has to
    // come first.
    Spans spans;
    auto exe = makePSEXE(0x80010000, 0x40000000, 0x800);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_THROW(load(exe, dest, recordingInfo(spans)), std::runtime_error);
    ASSERT_EQ(spans.size(), 1);
    EXPECT_EQ(spans[0].second, 0x40000000);
    EXPECT_TRUE(dest->isEmpty());
}

TEST(BinaryLoader, PSEXEIsntBoundedWithoutACheck) {
    auto exe = makePSEXE(0xc0000000, 0x1000, 0x1000);
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    EXPECT_TRUE(load(exe, dest, {}));
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
    std::vector<char> big(0x200004);
    section* text = writer.sections.add(".text");
    text->set_type(SHT_PROGBITS);
    text->set_flags(SHF_ALLOC | SHF_EXECINSTR);
    text->set_address(0x80010000);
    text->set_addr_align(4);
    text->set_data(code, sizeof(code));
    // The second section is the one refused, so the first must not have been written.
    section* data = writer.sections.add(".data");
    data->set_type(SHT_PROGBITS);
    data->set_flags(SHF_ALLOC | SHF_WRITE);
    data->set_address(0x80020000);
    data->set_addr_align(4);
    data->set_data(big.data(), big.size());

    auto path = (std::filesystem::temp_directory_path() / "binloader-ram.elf").string();
    ASSERT_TRUE(writer.save(path));
    Spans spans;
    PCSX::IO<PCSX::Mem4G> dest(new PCSX::Mem4G());
    {
        PCSX::IO<PCSX::File> in(new PCSX::PosixFile(path));
        std::map<uint32_t, std::string> symbols;
        auto info = recordingInfo(spans);
        EXPECT_THROW(PCSX::BinaryLoader::load(in, dest.asA<PCSX::File>(), info, symbols), std::runtime_error);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    EXPECT_EQ(spans.size(), 2);
    EXPECT_TRUE(dest->isEmpty());
}
