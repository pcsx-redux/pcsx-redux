/***************************************************************************
 *   Copyright (C) 2023 PCSX-Redux authors                                 *
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

#include "supportpsx/ps1-packer.h"

#include "args/args.hxx"
#include "fmt/format.h"
#include "support/file.h"
#include "support/mem4g.h"
#include "support/tool-args.h"
#include "supportpsx/binloader.h"

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> tloadFlag(parser, "tload", "", {"tload"});
    args::Flag shellFlag(parser, "shell", "", {"shell"});
    args::Flag nokernelFlag(parser, "nokernel", "", {"nokernel"});
    args::Flag resetstackFlag(parser, "resetstack", "", {"resetstack"});
    args::Flag rawFlag(parser, "raw", "", {"raw"});
    args::Flag bootyFlag(parser, "booty", "", {"booty"});
    args::Flag romFlag(parser, "rom", "", {"rom"});
    args::Flag cpeFlag(parser, "cpe", "", {"cpe"});
    args::Flag nopadFlag(parser, "nopad", "", {"nopad"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
    auto output = PCSX::ToolArgs::get(outputFlag);

    fmt::print(R"(
ps1-packer by Nicolas "Pixel" Noble
https://github.com/pcsx-redux/pcsx-redux/tree/main/tools/ps1-packer/
)");

    const auto inputs = args::get(inputsList);
    const bool asksForHelp = args::get(helpFlag);
    const bool hasOutput = output.has_value();
    const uint32_t tload = std::stoul(PCSX::ToolArgs::get(tloadFlag).value_or("0"), nullptr, 0);
    const bool oneInput = inputs.size() == 1;
    const bool shell = args::get(shellFlag);
    const bool nokernel = args::get(nokernelFlag);
    const bool resetstack = args::get(resetstackFlag);
    const bool raw = args::get(rawFlag);
    const bool booty = args::get(bootyFlag);
    const bool rom = args::get(romFlag);
    const bool cpe = args::get(cpeFlag);
    const bool nopad = args::get(nopadFlag);
    const unsigned outputTypeCount = (raw ? 1 : 0) + (booty ? 1 : 0) + (rom ? 1 : 0) + (cpe ? 1 : 0);
    if (asksForHelp || !oneInput || !hasOutput || (outputTypeCount > 1)) {
        fmt::print(R"(
Usage: {} input.ps-exe [-h] [-tload addr] [-shell] [-nokernel] [-resetstack] [-raw | -booty | -rom | -cpe] -o output.ps-exe
  input.ps-exe      mandatory: specify the input binary file.
  -o output.ps-exe  mandatory: name of the output file.
  -h                displays this help information and exit.
  -tload            force loading at this address instead of doing in-place.
  -shell            adds a kernel reset stub.
  -nokernel         avoids calling into the kernel.
  -resetstack       resets the stack pointer to the top of the memory.
  -nopad            disables padding of the output file to 2048 bytes. Only for PS-EXE output.

These options control the output format, and are mutually exclusive:
  -raw              outputs a raw file.
  -booty            outputs a counter-booty payload.
  -rom              outputs a bootable rom, which can be used in a cheat cart.
  -cpe              outputs a CPE file instead of a ps-exe one.
If none of these options is provided, a ps-exe file will be emitted by default.

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

    PCSX::PS1Packer::Options options;
    options.booty = booty;
    options.raw = raw;
    options.rom = rom;
    options.cpe = cpe;
    options.shell = shell;
    options.nokernel = nokernel;
    options.resetstack = resetstack;
    options.tload = tload;
    options.nopad = nopad;
    PCSX::IO<PCSX::File> out(new PCSX::PosixFile(output.value().c_str(), PCSX::FileOps::TRUNCATE));
    PCSX::PS1Packer::pack(new PCSX::SubFile(memory, memory->lowestAddress(), memory->actualSize()), out,
                          memory->lowestAddress(), info.pc.value_or(0), info.gp.value_or(0), info.sp.value_or(0),
                          options);

    fmt::print(R"(
Input file: {}
pc: 0x{:08x}  gp: 0x{:08x}  sp: 0x{:08x}
file size: {} -> {}

File {} created. All done.
)",
               input, info.pc.value_or(0), info.gp.value_or(0), info.sp.value_or(0), file->size(), out->size(),
               output.value());

    return 0;
}
