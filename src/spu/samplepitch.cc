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

#include "spu/samplepitch.h"

#include <algorithm>

#include "spu/adpcm.h"
#include "supportpsx/rootkey.h"

std::vector<int16_t> PCSX::SPU::decodeSample(const uint8_t* spuRam, const SPUSample::Extent& extent) {
    using SPUSample::c_blockSize;
    std::vector<int16_t> out;
    out.reserve(extent.blocks() * SPUSample::c_samplesPerBlock);
    // decodeBlock reads the block in place without writing it, but takes a mutable pointer.
    uint8_t* ram = const_cast<uint8_t*>(spuRam);
    AdpcmDecoder decoder;
    decoder.reset();
    for (uint32_t addr = extent.start; addr + c_blockSize <= extent.end; addr += c_blockSize) {
        decoder.decodeBlock(ram + addr);
        while (!decoder.bufferExhausted()) out.push_back(int16_t(std::clamp(decoder.takeSample(), -32768, 32767)));
    }
    return out;
}

std::optional<double> PCSX::SPU::sampleFundamental(const uint8_t* spuRam, const SPUSample::Extent& extent) {
    if (extent.truncated) return std::nullopt;
    const auto pcm = decodeSample(spuRam, extent);
    const bool loops = extent.repeats && extent.hasLoop;
    const uint32_t loopStart = (extent.loop - extent.start) / SPUSample::c_blockSize * SPUSample::c_samplesPerBlock;
    const auto window = RootKey::analysisWindow(pcm, loops, loopStart);
    return Fundamental::estimate(window, 44100);
}
