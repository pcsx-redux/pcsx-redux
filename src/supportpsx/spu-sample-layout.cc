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


#include "supportpsx/spu-sample-layout.h"

#include <algorithm>
#include <numeric>

PCSX::SPUSampleLayout::Layout PCSX::SPUSampleLayout::compute(unsigned length, bool hasLoop, unsigned loopStart,
                                                             unsigned loopLength, unsigned skip, bool allowUnroll,
                                                             unsigned maxEncodedLength, bool rotateLoop) {
    Layout l;
    l.hasLoop = hasLoop;
    l.skip = skip;
    if (length == 0) return l;
    if (!hasLoop) {
        l.encodedLength = (length - skip + 27) / 28 * 16 + 16;
        return l;
    }
    l.loopStart = loopStart;
    l.loopLength = loopLength;
    const unsigned align = (28 - (l.loopStart - l.skip) % 28) % 28;
    if (rotateLoop) {
        l.rotate = align;
    } else {
        l.pad = align;
    }
    const unsigned preLoop = align + l.loopStart - l.skip;
    l.unroll = 28 / std::gcd(l.loopLength, 28u);
    l.body = l.loopLength * l.unroll;
    if (!allowUnroll || ((preLoop + l.body) / 28 * 16 >= maxEncodedLength)) {
        l.unroll = 1;
        l.body = std::max(28u, (l.loopLength + 14) / 28 * 28);
        l.exact = l.body == l.loopLength;
    }
    l.encodedLength = (preLoop + l.body) / 28 * 16;
    return l;
}

unsigned PCSX::SPUSampleLayout::encode(ADPCM::Encoder& encoder, const std::vector<int16_t>& source,
                                       const Layout& layout, IO<File> out) {
    constexpr uint8_t silentLoopBlock[16] = {0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t spuBlock[16];
    unsigned encodedLength = 0;
    if (source.empty()) return 0;
    if (!layout.hasLoop) {
        int16_t input[28];
        unsigned pos = layout.skip;
        unsigned length = source.size() - layout.skip;
        while (length >= 28) {
            std::copy_n(source.begin() + pos, 28, input);
            pos += 28;
            length -= 28;
            encoder.processSPUBlock(input, spuBlock, ADPCM::Encoder::BlockAttribute::OneShot);
            spuBlock[1] = length == 0 ? 1 : 0;
            out->write(spuBlock, 16);
            encodedLength += 16;
        }
        if (length != 0) {
            std::copy_n(source.begin() + pos, length, input);
            std::fill(input + length, input + 28, 0);
            encoder.processSPUBlock(input, spuBlock, ADPCM::Encoder::BlockAttribute::OneShot);
            spuBlock[1] = 0;
            out->write(spuBlock, 16);
            encodedLength += 16;
        }
        out->write(silentLoopBlock, 16);
        return encodedLength + 16;
    }
    std::vector<int16_t> pcm(layout.pad, 0);
    pcm.insert(pcm.end(), source.begin() + layout.skip, source.begin() + layout.loopStart);
    const unsigned loopBlock = (pcm.size() + layout.rotate) / 28;
    for (unsigned j = 0; j < layout.rotate + layout.body; j++) {
        pcm.push_back(source[layout.loopStart + j % layout.loopLength]);
    }
    const unsigned blocks = pcm.size() / 28;
    for (unsigned b = 0; b < blocks; b++) {
        // The loop start block is forced to filter 0, so it decodes the same whether the SPU reaches it
        // from the previous block or jumps to it from the loop end.
        encoder.processSPUBlock(pcm.data() + b * 28, spuBlock, ADPCM::Encoder::BlockAttribute::OneShot,
                                b == loopBlock);
        uint8_t blockAttribute = 0;
        if (b >= loopBlock) blockAttribute |= 2;
        if (b == loopBlock) blockAttribute |= 4;
        if (b == (blocks - 1)) blockAttribute |= 1;
        spuBlock[1] = blockAttribute;
        out->write(spuBlock, 16);
        encodedLength += 16;
    }
    return encodedLength;
}
