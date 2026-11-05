/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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
#include <string.h>

#include "args/args.hxx"
#include "fmt/format.h"
#include "support/file.h"
#include "support/tool-args.h"
#include "supportpsx/iso9660-builder.h"
#include "supportpsx/iso9660-lowlevel.h"

// Number of blank sectors appended past the end of the volume when padding is on.
// 150 sectors is two seconds of disc time: enough slack that a real drive's read-ahead
// doesn't run off the end of the data while the BIOS is still reading the last sector.
static constexpr unsigned c_trailingPaddingSectors = 150;

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> licenseFlag(parser, "license", "", {"license"});
    args::Flag nopadFlag(parser, "nopad", "", {"nopad"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);

    fmt::print(R"(
exe2iso by Nicolas "Pixel" Noble
https://github.com/pcsx-redux/pcsx-redux/tree/main/tools/exe2iso/
)");

    const auto output = PCSX::ToolArgs::get(outputFlag);
    const auto inputs = args::get(inputsList);
    const auto license = PCSX::ToolArgs::get(licenseFlag);
    const bool asksForHelp = args::get(helpFlag);
    // Padding is on by default; -nopad opts out of the trailing blank sectors.
    const bool pad = !args::get(nopadFlag);
    const bool hasOutput = output.has_value();
    const bool hasExactlyOneInput = inputs.size() == 1;

    if (asksForHelp || !hasExactlyOneInput || !hasOutput) {
        fmt::print(R"(
Usage: {} input.ps-exe [-license file] [-nopad] -o output.bin
  input.ps-exe      mandatory: specify the input ps-exe file.
  -o output.bin     mandatory: name of the output file.
  -license file     optional: use this license file.
  -nopad            optional: don't append {} trailing blank sectors.
  -h                displays this help information and exit.
)",
                   argv[0], c_trailingPaddingSectors);
        return -1;
    }

    PCSX::IO<PCSX::File> exeFile(new PCSX::PosixFile(inputs[0]));
    if (exeFile->failed()) {
        fmt::print("Error opening input file {}\n", inputs[0]);
        return -1;
    }

    PCSX::IO<PCSX::File> licenseFile(new PCSX::FailedFile);
    if (license.has_value()) {
        licenseFile.setFile(new PCSX::PosixFile(license.value()));
        if (licenseFile->failed()) {
            fmt::print("Error opening license file {}\n", license.value());
            return -1;
        }
    }

    PCSX::IO<PCSX::File> out(new PCSX::PosixFile(output.value(), PCSX::FileOps::TRUNCATE));
    if (out->failed()) {
        fmt::print("Error opening output file {}\n", output.value());
        return -1;
    }

    PCSX::ISO9660Builder builder(out);

    // PlayStation discs identify themselves through the PVD system identifier.
    builder.getPVD().get<PCSX::ISO9660LowLevel::PVD_SystemIdent>().set("PLAYSTATION", ' ');

    // Sectors 0-15 are the license/system area. With no license file this writes zeroed
    // sectors, which boot on most everything except a region-locked (e.g. Japanese)
    // console; pass -license to embed a real one.
    builder.writeLicense(licenseFile);

    // The whole disc: a single PSX.EXE in the root directory. The builder appends the
    // ";1" version suffix and lays the file out as Mode 2 Form 1 data with valid EDC/ECC.
    PCSX::ISO9660::DirTree* root = builder.createRoot();
    builder.createFile(root, "PSX.EXE", exeFile);

    // Compute the layout and emit the full image: volume descriptors, path tables, the
    // root directory, and the executable.
    builder.close();

    // Optional trailing padding, sitting past the end of the declared volume so it's
    // purely physical. See c_trailingPaddingSectors for the rationale.
    if (pad) {
        uint8_t blank[2048];
        memset(blank, 0, sizeof(blank));
        for (unsigned i = 0; i < c_trailingPaddingSectors; i++) {
            builder.writeSector(blank, PCSX::IEC60908b::SectorMode::M2_FORM1);
        }
    }

    fmt::print("Done.\n");
    return 0;
}
