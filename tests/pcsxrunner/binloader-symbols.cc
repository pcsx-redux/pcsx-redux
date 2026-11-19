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

#include "elfio/elfio.hpp"
#include "gtest/gtest.h"
#include "support/file.h"
#include "supportpsx/binloader.h"

// The ELF loader turns the symbol table into the address -> name map every debugger
// view and the profiler name code with. A linker script puts zero-sized markers at the
// same address as real functions, and objcopy adds absolute _size symbols whose value
// is a length, so the import has to pick names, not just copy the table.

namespace {

std::map<uint32_t, std::string> loadSymbols() {
    using namespace ELFIO;
    elfio writer;
    writer.create(ELFCLASS32, ELFDATA2LSB);
    writer.set_type(ET_EXEC);
    writer.set_machine(EM_MIPS);
    writer.set_entry(0x1000);

    section* text = writer.sections.add(".text");
    text->set_type(SHT_PROGBITS);
    text->set_flags(SHF_ALLOC | SHF_EXECINSTR);
    text->set_address(0x1000);
    text->set_addr_align(4);
    char code[0x40] = {};
    text->set_data(code, sizeof(code));

    section* strtab = writer.sections.add(".strtab");
    strtab->set_type(SHT_STRTAB);
    string_section_accessor strings(strtab);
    section* symtab = writer.sections.add(".symtab");
    symtab->set_type(SHT_SYMTAB);
    symtab->set_addr_align(4);
    symtab->set_entry_size(writer.get_default_entry_size(SHT_SYMTAB));
    symtab->set_link(strtab->get_index());
    symbol_section_accessor symbols(writer, symtab);

    Elf_Half t = text->get_index();
    symbols.add_symbol(strings, "start.s", 0, 0, STB_LOCAL, STT_FILE, 0, SHN_ABS);
    symbols.add_symbol(strings, ".text", 0x1000, 0, STB_LOCAL, STT_SECTION, 0, t);
    // A function, then two markers at the same address, as a linker script leaves them.
    symbols.add_symbol(strings, "main", 0x1000, 0x20, STB_GLOBAL, STT_FUNC, 0, t);
    symbols.add_symbol(strings, "__build_id", 0x1000, 0, STB_GLOBAL, STT_NOTYPE, 0, t);
    symbols.add_symbol(strings, "__init_array_start", 0x1000, 0, STB_GLOBAL, STT_NOTYPE, 0, t);
    // The markers first this time, then the function.
    symbols.add_symbol(strings, "__text_end", 0x1020, 0, STB_GLOBAL, STT_NOTYPE, 0, t);
    symbols.add_symbol(strings, "helper", 0x1020, 0x10, STB_GLOBAL, STT_FUNC, 0, t);
    // A local asm label on its own, which has nothing better to lose to.
    symbols.add_symbol(strings, "loop", 0x1030, 0, STB_LOCAL, STT_NOTYPE, 0, t);
    // An objcopy length and an undefined reference.
    symbols.add_symbol(strings, "_binary_font_raw_size", 0x0800, 0, STB_GLOBAL, STT_NOTYPE, 0, SHN_ABS);
    symbols.add_symbol(strings, "extern_thing", 0, 0, STB_GLOBAL, STT_NOTYPE, 0, SHN_UNDEF);

    auto path = (std::filesystem::temp_directory_path() / "binloader-symbols.elf").string();
    EXPECT_TRUE(writer.save(path));
    std::map<uint32_t, std::string> ret;
    {
        // Closed before the remove: Windows will not delete an open file.
        PCSX::IO<PCSX::File> in(new PCSX::PosixFile(path));
        PCSX::IO<PCSX::File> dest(new PCSX::BufferFile(PCSX::FileOps::READWRITE));
        PCSX::BinaryLoader::Info info;
        EXPECT_TRUE(PCSX::BinaryLoader::load(in, dest, info, ret));
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return ret;
}

}  // namespace

TEST(BinaryLoader, ElfSymbolImportPrefersFunctions) {
    auto symbols = loadSymbols();
    EXPECT_EQ(symbols[0x1000], "main");
    EXPECT_EQ(symbols[0x1020], "helper");
    EXPECT_EQ(symbols[0x1030], "loop");
    EXPECT_EQ(symbols.count(0), 0) << symbols[0];
    EXPECT_EQ(symbols.count(0x0800), 0) << symbols[0x0800];
    EXPECT_EQ(symbols.size(), 3);
}
