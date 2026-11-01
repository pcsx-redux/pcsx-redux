/*

MIT License

Copyright (c) 2020 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "supportpsx/binloader.h"

#include <map>
#include <string>

#include "elfio/elfio.hpp"
#include "fmt/format.h"
#include "support/file.h"
#include "support/stream-file.h"
#include "support/strings-helpers.h"
#include "support/zfile.h"

namespace PCSX {

namespace {

void eraseSymbolsInSpan(uint32_t low, uint32_t high, std::map<uint32_t, std::string>& symbols) {
    for (auto s = symbols.begin(); s != symbols.end();) {
        if (s->first >= low && s->first < high) {
            s = symbols.erase(s);
        } else {
            s++;
        }
    }
}

bool loadCPE(IO<File> file, IO<File> dest, BinaryLoader::Info& info, std::map<uint32_t, std::string>& symbols) {
    uint32_t magic = file->read<uint32_t>();
    if (magic != 0x1455043) return false;
    file->skip<uint16_t>();

    uint8_t opcode;

    while ((opcode = file->byte())) {
        uint16_t reg;
        uint32_t value;
        bool setRegister = false;

        switch (opcode) {
            case 1: {  // load
                uint32_t addr = file->read<uint32_t>();
                uint32_t size = file->read<uint32_t>();
                dest->writeAt(file->read(size), addr);
                eraseSymbolsInSpan(addr, addr + size, symbols);
            } break;
            case 2: {
                file->read<uint32_t>();
            } break;
            case 3: {
                reg = file->read<uint16_t>();
                value = file->read<uint32_t>();
                setRegister = true;
            } break;
            case 4: {
                reg = file->read<uint16_t>();
                value = file->read<uint16_t>();
                setRegister = true;
            } break;
            case 5: {
                reg = file->read<uint16_t>();
                value = file->read<uint8_t>();
                setRegister = true;
            } break;
            case 6: {
                reg = file->read<uint16_t>();
                value = file->read<uint16_t>();
                uint32_t remainder = file->read<uint8_t>();
                value |= remainder << 16;
                setRegister = true;
            } break;
            case 7: {
                file->read<uint32_t>();
            } break;
            case 8: {
                file->byte();
            } break;
        }

        if (setRegister) {
            switch (reg) {
                case 0x90: {
                    info.pc = value;
                } break;
            }
        }
    }

    return true;
}

// `overlay` means another PS-EXE has already been loaded underneath this one, which
// happens for a minipsf sitting on its _lib. The psf spec takes the initial pc/sp
// from the FIRST exe loaded; a minipsf's own header carries a placeholder pc (the
// lib's text base, not an entry point), so honouring it starts execution on data.
bool loadPSEXE(IO<File> file, IO<File> dest, BinaryLoader::Info& info, std::map<uint32_t, std::string>& symbols,
               bool overlay = false) {
    uint64_t magic = file->read<uint64_t>();
    if (magic != 0x45584520582d5350) return false;

    file->read<uint32_t>();
    file->read<uint32_t>();

    const auto pcInFile = file->read<uint32_t>();
    if (!overlay) info.pc = pcInFile;
    const auto gpInFile = file->read<uint32_t>();
    if (!overlay && gpInFile != 0) info.gp = gpInFile;
    uint32_t addr = file->read<uint32_t>();
    uint32_t size = file->read<uint32_t>();
    file->read<uint32_t>();
    file->read<uint32_t>();
    file->read<uint32_t>();
    file->read<uint32_t>();
    const auto spInFile = file->read<uint32_t>();
    if (!overlay && spInFile != 0) info.sp = spInFile;
    file->rSeek(0x71, SEEK_SET);
    uint8_t regionByte = file->byte();
    file->rSeek(2048, SEEK_SET);
    dest->writeAt(file->read(size), addr);
    eraseSymbolsInSpan(addr, addr + size, symbols);
    switch (regionByte) {
        case 'A':
        case 'J':
            info.region = BinaryLoader::Region::NTSC;
            break;
        case 'E':
            info.region = BinaryLoader::Region::PAL;
            break;
    }
    return true;
}

// `seenExe` tracks whether any PS-EXE has been loaded yet across the whole recursive
// load, so that a _lib's entry point survives the minipsf that sits on top of it.
bool loadPSF(IO<File> file, IO<File> dest, BinaryLoader::Info& info, std::map<uint32_t, std::string>& symbols,
             bool& seenExe, bool seenRefresh = false, unsigned depth = 0) {
    if (depth >= 10) return false;
    uint32_t magic = file->read<uint32_t>();
    if (magic != 0x1465350) return false;
    uint32_t R = file->read<uint32_t>();
    uint32_t N = file->read<uint32_t>();
    uint32_t C = file->read<uint32_t>();
    file->rSeek(R, SEEK_CUR);
    IO<File> zpsexe(new SubFile(file, file->rTell(), N));
    file->rSeek(N, SEEK_CUR);
    char tagtag[6];
    file->read(tagtag, 5);
    tagtag[5] = 0;
    std::string tagsStorage;

    std::map<std::string_view, std::string_view> pairs;

    if (strcmp(tagtag, "[TAG]") == 0) {
        size_t tagsSize = file->size() - file->rTell();
        tagsStorage = file->readString(tagsSize);
        std::string_view tags = tagsStorage;

        auto lines = StringsHelpers::split(tags, "\n\r");

        for (auto& line : lines) {
            auto e = line.find('=', 0);
            if (e == std::string::npos) continue;
            pairs[line.substr(0, e)] = line.substr(e + 1);
        }
    }

    if (!seenRefresh && pairs.find("refresh") != pairs.end()) {
        const auto& refresh = pairs["refresh"];
        if (refresh == "50") {
            info.region = BinaryLoader::Region::PAL;
        } else if (refresh == "60") {
            info.region = BinaryLoader::Region::NTSC;
        }
        seenRefresh = true;
    }

    if (pairs.find("_lib") != pairs.end()) {
        std::filesystem::path subFilePath(file->filename());
        IO<File> subFile(new PosixFile(subFilePath.parent_path() / pairs["_lib"]));
        if (!subFile->failed()) loadPSF(subFile, dest, info, symbols, seenExe, seenRefresh, depth + 1);
    }

    IO<File> psexe(new ZReader(zpsexe));
    loadPSEXE(psexe, dest, info, symbols, seenExe);
    seenExe = true;

    unsigned libNum = 2;

    while (true) {
        std::string libName = fmt::format("_lib{}", libNum++);
        if (pairs.find(libName) == pairs.end()) break;
        std::filesystem::path subFilePath(file->filename());
        IO<File> subFile(new PosixFile(subFilePath.parent_path() / pairs[libName]));
        if (!subFile->failed()) loadPSF(subFile, dest, info, symbols, seenExe, seenRefresh, depth + 1);
    }

    return true;
}

bool loadELF(IO<File> file, IO<File> dest, BinaryLoader::Info& info, std::map<uint32_t, std::string>& symbols) {
    using namespace ELFIO;
    elfio reader;
    FileIStream stream(file);

    if (!reader.load(stream)) return false;
    if (reader.get_class() != ELFCLASS32) return false;

    info.pc = static_cast<uint32_t>(reader.get_entry());

    Elf_Half sec_num = reader.sections.size();
    Elf_Half seg_num = reader.segments.size();
    for (unsigned i = 0; i < sec_num; i++) {
        section* psec = reader.sections[i];

        if (!(psec->get_flags() & SHF_ALLOC)) continue;
        if (psec->get_type() == SHT_NOBITS) continue;

        auto name = psec->get_name();
        if (StringsHelpers::endsWith(name, "_Header")) continue;

        auto size = psec->get_size();
        auto data = psec->get_data();
        auto vaddr = psec->get_address();
        auto loadAddr = vaddr;
        for (unsigned j = 0; j < seg_num; j++) {
            segment* pseg = reader.segments[j];
            if (pseg->get_type() != PT_LOAD) continue;
            auto segVaddr = pseg->get_virtual_address();
            if (vaddr >= segVaddr && vaddr + size <= segVaddr + pseg->get_memory_size()) {
                loadAddr = pseg->get_physical_address() + (vaddr - segVaddr);
                break;
            }
        }
        dest->writeAt(data, size, loadAddr);
        eraseSymbolsInSpan(vaddr, vaddr + size, symbols);
    }

    for (unsigned i = 0; i < sec_num; i++) {
        section* psec = reader.sections[i];
        auto name = psec->get_name();

        auto type = psec->get_type();
        if (type != SHT_SYMTAB) continue;
        const ELFIO::symbol_section_accessor symbolstab(reader, psec);
        for (unsigned s = 0; s < symbolstab.get_symbols_num(); s++) {
            std::string name;
            Elf64_Addr value;
            Elf_Xword size;
            unsigned char bind;
            unsigned char type;
            Elf_Half section_index;
            unsigned char other;
            symbolstab.get_symbol(s, name, value, size, bind, type, section_index, other);
            symbols[value] = name;
        }
    }

    return true;
}

}  // namespace

}  // namespace PCSX

bool PCSX::BinaryLoader::load(IO<File> in, IO<File> dest, Info& info, std::map<uint32_t, std::string>& symbols) {
    {
        IO<File> ny(new PosixFile(in->filename().parent_path() / "libps.exe"));
        if (!ny->failed()) loadPSEXE(ny, dest, info, symbols);
    }

    if (in->failed()) return false;
    if (loadCPE(in, dest, info, symbols)) return true;
    in->rSeek(0, SEEK_SET);
    if (loadPSEXE(in, dest, info, symbols)) return true;
    in->rSeek(0, SEEK_SET);
    bool seenExe = false;
    if (loadPSF(in, dest, info, symbols, seenExe)) return true;
    in->rSeek(0, SEEK_SET);
    if (loadELF(in, dest, info, symbols)) return true;
    return false;
}
