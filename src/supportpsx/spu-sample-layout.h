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

#include <vector>

#include "support/file.h"
#include "supportpsx/adpcm.h"

namespace PCSX {

namespace SPUSampleLayout {

// How a tracker sample is laid out in SPU memory. All lengths and positions are in samples.
//
// One-shot samples drop the first `skip` samples, then end with a silent block that loops on itself, so
// the voice keeps running on silence once the sound is over.
//
// The SPU can only loop on whole 28-sample blocks, so looped samples are laid out as:
//   [pad zeros][sample skip .. loop start][loop body] x unroll
// - front zeros are padded so that the loop start lands exactly on a block boundary;
// - the loop body is unrolled the smallest number of times so that it ends on a block boundary,
//   which keeps the SPU loop period at exactly loopLength * unroll samples;
// - anything after the loop end is never heard, so it is dropped.
// If unrolling is not allowed, or would make the encoded sample reach `maxEncodedLength` bytes, the loop
// body is a single period rounded to the nearest whole block, and `exact` is false.
//
// With `rotateLoop`, there are no front zeros, so the sample starts right away. The SPU loop start moves
// `rotate` samples into the loop instead, onto the next block boundary, and the loop is written on past
// its end by the same `rotate` samples:
//   [sample skip .. loop start][first `rotate` loop samples][loop body, rotated by `rotate`] x unroll
// The data is the loop played cyclically from the loop start, so the sample start and any position up
// to one period past the loop start keep their own index, and the encoded length is the same as padded.
struct Layout {
    bool hasLoop = false;
    unsigned loopStart = 0;
    unsigned loopLength = 0;
    unsigned skip = 0;
    unsigned pad = 0;
    unsigned rotate = 0;
    unsigned unroll = 1;
    unsigned body = 0;
    bool exact = true;
    unsigned encodedLength = 0;  // bytes of SPU ADPCM
};

// The caller decides whether the sample loops, and clamps the loop to the sample beforehand.
// `skip` is the number of samples dropped from the front; for a looped sample it has to be at most
// `loopStart`.
Layout compute(unsigned length, bool hasLoop, unsigned loopStart, unsigned loopLength, unsigned skip,
               bool allowUnroll, unsigned maxEncodedLength, bool rotateLoop = false);

// Encodes `source` (the full sample, `length` samples, before skip) according to `layout`, and writes the
// SPU blocks to `out`. Returns the number of bytes written, which is layout.encodedLength.
unsigned encode(ADPCM::Encoder& encoder, const std::vector<int16_t>& source, const Layout& layout, IO<File> out);

}  // namespace SPUSampleLayout

}  // namespace PCSX
