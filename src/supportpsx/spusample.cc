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

#include <stdio.h>

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

std::string PCSX::SPUSample::noteRecord(uint64_t signature, double note) {
    char buf[64];
    snprintf(buf, sizeof(buf), "spu-note %016llx %.4f", static_cast<unsigned long long>(signature), note);
    return buf;
}

namespace {

// note = 1*3DIGIT [ "." 1*DIGIT ], then 0 <= value < 128.
bool parseNote(std::string_view s, double& out) {
    size_t i = 0;
    double value = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i < 3) value = value * 10 + (s[i++] - '0');
    if (i == 0 || (i < s.size() && s[i] >= '0' && s[i] <= '9')) return false;
    if (i < s.size()) {
        if (s[i++] != '.' || i == s.size()) return false;
        double scale = 0.1;
        for (; i < s.size(); i++, scale /= 10) {
            if (s[i] < '0' || s[i] > '9') return false;
            value += (s[i] - '0') * scale;
        }
    }
    if (value >= 128.0) return false;
    out = value;
    return true;
}

}  // namespace

std::unordered_map<uint64_t, double> PCSX::SPUSample::noteRecords(std::string_view comment) {
    std::unordered_map<uint64_t, double> ret;
    constexpr std::string_view prefix = "spu-note ";
    while (!comment.empty()) {
        const size_t eol = comment.find('\n');
        std::string_view line = comment.substr(0, eol);
        comment = eol == std::string_view::npos ? std::string_view{} : comment.substr(eol + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() < prefix.size() + 18 || line.substr(0, prefix.size()) != prefix) continue;
        line.remove_prefix(prefix.size());
        uint64_t signature = 0;
        bool valid = line[16] == ' ';
        for (unsigned i = 0; valid && i < 16; i++) {
            const char c = line[i];
            unsigned v;
            if (c >= '0' && c <= '9') {
                v = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                v = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                v = c - 'A' + 10;
            } else {
                valid = false;
                break;
            }
            signature = (signature << 4) | v;
        }
        double note;
        if (valid && parseNote(line.substr(17), note)) ret[signature] = note;
    }
    return ret;
}
