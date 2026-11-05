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

#pragma once

#include <stdint.h>

namespace PCSX {

namespace SPUSample {

constexpr uint32_t c_ramSize = 512 * 1024;
constexpr uint32_t c_blockSize = 16;
constexpr uint32_t c_samplesPerBlock = 28;

// ADPCM block header, byte 1.
constexpr uint8_t c_flagLoopEnd = 1;
constexpr uint8_t c_flagRepeat = 2;
constexpr uint8_t c_flagLoopStart = 4;

// Where a sample sits in sound RAM, from its start address to the block carrying the
// loop-end flag.
struct Extent {
    uint32_t start = 0;
    uint32_t end = 0;        // one past the block carrying the loop-end flag
    uint32_t loop = 0;       // last block before the end with the loop-start flag
    bool hasLoop = false;    // a loop-start flag was seen
    bool repeats = false;    // the end block has the repeat flag
    bool truncated = false;  // ran off the end of sound RAM with no end flag
    uint32_t blocks() const { return (end - start) / c_blockSize; }
};

// Walks sound RAM a block at a time from `start`. A pure function of sound RAM.
Extent walk(const uint8_t* spuRam, uint32_t start);

// Content address of a sample: the first 8 bytes of the MD5 of its ADPCM blocks, flag bytes
// included, from its start through its loop-end block. Loop targets are encoded as flags
// relative to the data, so the same sample uploaded at another address hashes the same.
uint64_t signature(const uint8_t* spuRam, const Extent& extent);

}  // namespace SPUSample

}  // namespace PCSX
