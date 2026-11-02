/*

MIT License

Copyright (c) 2026 PCSX-Redux authors

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

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "supportpsx/dct.h"

namespace PCSX {

namespace STR {

// A movie the way the PlayStation streams it: BS v2 frames cut into 2016-byte
// chunks behind the 32-byte STR chunk header, with 4-bit XA audio sectors
// interleaved, all read in one run at single or double speed.

struct Settings {
    unsigned fps = 15;      // 1..60
    unsigned speed = 2;     // 1 or 2: 75 or 150 sectors a second
    unsigned channels = 0;  // XA audio: 0 for none, 1 or 2
    unsigned rate = 37800;  // 37800 or 18900
    uint8_t fileNumber = 0;
    uint8_t channelNumber = 0;
    DCT::Transform transform = DCT::Transform::ExactMatrix;
    DCT::QuantTables quant;
};

// One frame in YCbCr 4:2:0, each side padded to a multiple of 16.
struct Planes {
    unsigned width = 0, height = 0;
    std::vector<uint8_t> y, cb, cr;
    // 24-bit RGB, `stride` bytes a row. The padding repeats the last row and column.
    void fromRGB(const uint8_t *rgb, unsigned w, unsigned h, size_t stride);
    DCT::Frame frame() const;
};

// Fills `planes` with the next frame: 1, 0 once there are no more, -1 on an
// error with `error` set.
using FrameSource = std::function<int(Planes &planes, std::string &error)>;

struct Result {
    bool failed = false;
    std::string error;
    unsigned frames = 0;
    unsigned sectors = 0;
    unsigned audioSectors = 0;
    unsigned width = 0, height = 0;
    unsigned mostSectorsAFrame = 0;  // a player that holds fewer drops the frames that take more
    double averageQScale = 0;
    int worstQScale = 0;
};

// What is wrong with `settings`, or an empty string when encode() takes them.
std::string check(const Settings &settings);

// Appends whole Mode 2 sectors of 2352 bytes to `out`, EDC and ECC included,
// addressed from 00:02:00 for a disc builder to restamp. Each frame gets the
// sectors its rate leaves it, at the lowest q_scale whose frame fits them. `pcm`
// is 16-bit, interleaved per `channels`, at `rate`.
Result encode(const Settings &settings, const FrameSource &frames, std::span<const int16_t> pcm,
              std::vector<uint8_t> &out);

}  // namespace STR

}  // namespace PCSX
