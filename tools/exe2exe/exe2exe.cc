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

#include <vector>

#include "args/args.hxx"
#include "fmt/format.h"
#include "support/file.h"
#include "support/mem4g.h"
#include "support/tool-args.h"
#include "supportpsx/binloader.h"

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
    auto output = PCSX::ToolArgs::get(outputFlag);

    fmt::print(R"(
exe2exe by Nicolas "Pixel" Noble
https://github.com/pcsx-redux/pcsx-redux/tree/main/tools/exe2exe/
)");

    const auto inputs = args::get(inputsList);
    const bool asksForHelp = args::get(helpFlag);
    const bool hasOutput = output.has_value();
    const bool oneInput = inputs.size() == 1;
    if (asksForHelp || !oneInput || !hasOutput) {
        fmt::print(R"(
Usage: {} input.ps-exe [-h] -o output.ps-exe
  input.ps-exe      mandatory: specify the input binary file.
  -o output.ps-exe  mandatory: name of the output file.
  -h                displays this help information and exit.

Valid input binary files can be in the following formats:
 - PS-EXE (needs the "PS-X EXE" signature)
 - ELF
 - CPE
 - PSF
 - MiniPSF
)",
                   argv[0]);
        return -1;
    }

    auto& input = inputs[0];
    PCSX::IO<PCSX::File> file(new PCSX::PosixFile(input));
    if (file->failed()) {
        fmt::print("Unable to open file: {}\n", input);
        return -1;
    }

    PCSX::BinaryLoader::Info info;
    PCSX::IO<PCSX::Mem4G> memory(new PCSX::Mem4G());
    std::map<uint32_t, std::string> symbols;
    bool success = PCSX::BinaryLoader::load(file, memory, info, symbols);
    if (!success) {
        fmt::print("Unable to load file: {}\n", input);
        return -1;
    }
    if (!info.pc.has_value()) {
        fmt::print("File {} is invalid.\n", input);
        return -1;
    }

    uint32_t tload = memory->lowestAddress();
    uint32_t pc = info.pc.value_or(0);
    uint32_t gp = info.gp.value_or(0);
    uint32_t sp = info.sp.value_or(0);

    if (tload & 3) {
        fmt::print("File {} is invalid: tload is not aligned to 4 bytes.\n", input);
        return -1;
    }

    if (pc & 3) {
        fmt::print("File {} is invalid: pc is not aligned to 4 bytes.\n", input);
        return -1;
    }

    if (gp & 3) {
        fmt::print("File {} is invalid: gp is not aligned to 4 bytes.\n", input);
        return -1;
    }

    if (sp & 3) {
        fmt::print("File {} is invalid: sp is not aligned to 4 bytes.\n", input);
        return -1;
    }

    uint32_t size = memory->actualSize();
    size = (size + 2047) & ~2047;

    PCSX::IO<PCSX::File> out = new PCSX::PosixFile(output.value().c_str(), PCSX::FileOps::TRUNCATE);

    out->write<uint64_t>(0x45584520582d5350);
    out->write<uint32_t>(0);
    out->write<uint32_t>(0);
    out->write<uint32_t>(pc);
    out->write<uint32_t>(gp);
    out->write<uint32_t>(tload);
    out->write<uint32_t>(size);
    out->write<uint32_t>(0);
    out->write<uint32_t>(0);
    out->write<uint32_t>(0);
    out->write<uint32_t>(0);
    out->write<uint32_t>(sp);
    for (unsigned i = 0; i < 499; i++) {
        out->write<uint32_t>(0);
    }

    auto data = memory.asA<PCSX::File>()->readAt(size, tload);
    out->write(std::move(data));

    fmt::print(R"(
Input file: {}
pc: 0x{:08x}  gp: 0x{:08x}  sp: 0x{:08x}

File {} created. All done.
)",
               input, pc, gp, sp, output.value());

    return 0;
}
