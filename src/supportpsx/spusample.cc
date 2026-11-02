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

#include "supportpsx/spusample.h"

#include "support/md5.h"

PCSX::SPUSample::Extent PCSX::SPUSample::walk(const uint8_t* spuRam, uint32_t start) {
    Extent extent;
    start &= ~(c_blockSize - 1);
    extent.start = start;
    uint32_t addr = start;
    while (true) {
        if (addr >= c_ramSize) {
            extent.truncated = true;
            extent.end = c_ramSize;
            break;
        }
        const uint8_t flags = spuRam[addr + 1];
        if (flags & c_flagLoopStart) {
            extent.loop = addr;
            extent.hasLoop = true;
        }
        addr += c_blockSize;
        if (flags & c_flagLoopEnd) {
            extent.end = addr;
            extent.repeats = flags & c_flagRepeat;
            break;
        }
    }
    return extent;
}

uint64_t PCSX::SPUSample::signature(const uint8_t* spuRam, const Extent& extent) {
    MD5 md5;
    md5.update(spuRam + extent.start, extent.end - extent.start);
    uint8_t digest[16];
    md5.finish(digest);
    uint64_t ret = 0;
    for (unsigned i = 0; i < 8; i++) ret = (ret << 8) | digest[i];
    return ret;
}
