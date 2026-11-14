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

#include "core/hwevents.h"

#include <string.h>

#include <algorithm>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "fmt/format.h"

void PCSX::HWEvents::record(Type type, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3, uint16_t flags) {
    if (full()) return;
    size_t idx = m_count;
    size_t chunk = idx / kRecordsPerChunk;
    if (chunk >= m_chunks.size()) {
        m_chunks.emplace_back(std::make_unique<HWEvent[]>(kRecordsPerChunk));
    }
    m_count++;
    HWEvent& e = m_chunks[chunk][idx % kRecordsPerChunk];
    auto& regs = g_emulator->m_cpu->m_regs;
    e.cycle = regs.cycle;
    e.pc = regs.pc;
    e.type = type;
    e.flags = flags;
    e.payload[0] = p0;
    e.payload[1] = p1;
    e.payload[2] = p2;
    e.payload[3] = p3;
    if (s_recordObserver) [[unlikely]] {
        s_recordObserver(e);
    }
}

const char* PCSX::HWEvents::sourceName(unsigned source) {
    switch (source) {
        case unsigned(Source::IRQ):
            return "IRQ";
        case unsigned(Source::DMA):
            return "DMA";
        case unsigned(Source::CDROM):
            return "CDROM";
        case unsigned(Source::GPU):
            return "GPU";
        case unsigned(Source::SPU):
            return "SPU";
    }
    return nullptr;
}

namespace {

// The first `count` of the 12 bytes packed into payload[1..3], as hex pairs.
std::string packedBytes(const PCSX::HWEvent& e, unsigned count) {
    std::string out;
    if (count > 12) count = 12;
    for (unsigned i = 0; i < count; i++) {
        uint8_t b = e.payload[1 + i / 4] >> ((i % 4) * 8);
        if (i != 0) out += ' ';
        out += fmt::format("{:02x}", b);
    }
    return out;
}

}  // namespace

std::string PCSX::HWEvents::describe(const HWEvent& e) {
    const uint32_t* p = e.payload;
    switch (e.type) {
        case IRQAssert:
            return fmt::format("IRQ assert {:03x}, I_STAT {:03x}, I_MASK {:03x}", p[0], p[1], p[2]);
        case DMAStart:
            return fmt::format("DMA{} start, MADR {:08x}, BCR {:08x}, CHCR {:08x}", p[0], p[1], p[2], p[3]);
        case DMAComplete:
            return fmt::format("DMA{} complete, DICR {:08x}", p[0], p[1]);
        case CDCommand: {
            unsigned count = (p[0] >> 8) & 0xff;
            std::string out = fmt::format("CD command {:02x}", p[0] & 0xff);
            if (count) out += fmt::format(", params {}", packedBytes(e, count));
            if (count > 12) out += " ...";
            return out;
        }
        case CDResponse: {
            unsigned count = (p[0] >> 8) & 0xff;
            std::string out = fmt::format("CD response INT{}", p[0] & 0xff);
            if (count) out += fmt::format(", {}", packedBytes(e, count));
            if (count > 12) out += " ...";
            if (p[0] & (1u << 18)) {
                out += ", dropped";
            } else if (!(p[0] & (1u << 16))) {
                out += ", queued";
            }
            if (p[0] & (1u << 17)) out += ", IRQ";
            return out;
        }
        case CDSector: {
            unsigned m = p[0] / 75 / 60, s = (p[0] / 75) % 60, f = p[0] % 75;
            std::string out = fmt::format("CD sector {:02}:{:02}:{:02}, {} bytes to host, mode {}, Setmode {:02x}", m,
                                          s, f, p[1], p[2] & 0xff, (p[2] >> 8) & 0xff);
            if ((p[2] & 0xff) == 2) out += fmt::format(", subheader {:08x}", p[3]);
            return out;
        }
        case GPUCommand: {
            switch (p[1]) {
                case 0:
                    return fmt::format("GP0 {:08x}, port", p[0]);
                case 2:
                    return fmt::format("GP0 {:08x}, block DMA at {:08x}, {} words", p[0], p[2], p[3]);
                case 3:
                    return fmt::format("GP0 {:08x}, list DMA node {:08x}, {} words", p[0], p[2], p[3]);
            }
            return fmt::format("GP0 {:08x}, origin {}", p[0], p[1]);
        }
        case GPUControl:
            return fmt::format("GP1 {:08x}", p[0]);
        case SPURegWrite:
            return fmt::format("SPU write {:08x} = {:04x}, {}-bit", p[0], p[1], p[2]);
        case SPUKeyOn:
            return fmt::format("SPU key on, voices {:06x}", p[0]);
        case SPUKeyOff:
            return fmt::format("SPU key off, voices {:06x}", p[0]);
    }
    return fmt::format("type {:04x}, {:08x} {:08x} {:08x} {:08x}", e.type, p[0], p[1], p[2], p[3]);
}

void PCSX::HWEvents::clear() {
    m_chunks.clear();
    m_count = 0;
    m_generation++;
}

namespace {
constexpr char kMagic[8] = {'P', 'C', 'S', 'X', 'H', 'W', 'E', 'V'};
constexpr uint32_t kVersion = 1;
}  // namespace

bool PCSX::HWEvents::save(IO<File> file) const {
    auto put = [&file](const void* data, size_t size) { return file->write(data, size) == static_cast<ssize_t>(size); };
    uint32_t version = kVersion;
    uint32_t recordSize = sizeof(HWEvent);
    uint64_t count = m_count;
    if (!put(kMagic, sizeof(kMagic)) || !put(&version, sizeof(version)) || !put(&recordSize, sizeof(recordSize)) ||
        !put(&count, sizeof(count))) {
        return false;
    }
    size_t left = m_count;
    for (auto& chunk : m_chunks) {
        if (left == 0) break;
        size_t n = std::min(left, kRecordsPerChunk);
        if (!put(chunk.get(), n * sizeof(HWEvent))) return false;
        left -= n;
    }
    return true;
}

bool PCSX::HWEvents::load(IO<File> file) {
    clear();
    char magic[sizeof(kMagic)];
    if (file->read(magic, sizeof(magic)) != sizeof(magic)) return false;
    if (memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;
    if (file->read<uint32_t>() != kVersion) return false;
    if (file->read<uint32_t>() != sizeof(HWEvent)) return false;
    uint64_t count = file->read<uint64_t>();
    while (m_count < count) {
        size_t n = std::min<uint64_t>(count - m_count, kRecordsPerChunk);
        m_chunks.emplace_back(std::make_unique<HWEvent[]>(kRecordsPerChunk));
        ssize_t got = file->read(m_chunks.back().get(), n * sizeof(HWEvent));
        if (got != static_cast<ssize_t>(n * sizeof(HWEvent))) {
            clear();
            return false;
        }
        m_count += n;
    }
    return true;
}
