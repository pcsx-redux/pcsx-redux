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

#pragma once

#include <stdint.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "support/file.h"

namespace PCSX {

namespace BinaryLoader {

enum class Region { UNKNOWN, NTSC, PAL };
struct Info {
    std::optional<Region> region;
    std::optional<uint32_t> pc;
    std::optional<uint32_t> sp;
    std::optional<uint32_t> gp;
    // Set by the caller to the amount of RAM the machine has. A binary that doesn't fit in RAM
    // or in the BIOS ROM is then refused with an exception before anything gets written.
    std::optional<uint32_t> ramSize;
};

bool load(IO<File> in, IO<File> dest, Info& info, std::map<uint32_t, std::string>& symbols);

}  // namespace BinaryLoader

}  // namespace PCSX
