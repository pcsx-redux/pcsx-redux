/*

MIT License

Copyright (c) 2023 PCSX-Redux authors

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

#include <cstddef>
#include <cstdint>
#include <optional>

#include "support/file.h"

namespace PCSX::PS1Packer {

struct Options {
    uint32_t tload = 0;
    bool shell = false;
    bool nokernel = false;
    bool resetstack = false;
    bool nopad = false;
    bool booty = false;
    bool raw = false;
    bool rom = false;
    bool cpe = false;
    bool datel = false;
};

void pack(IO<File> src, IO<File> dest, uint32_t addr, uint32_t pc, uint32_t gp, uint32_t sp, const Options&);

}  // namespace PCSX::PS1Packer
