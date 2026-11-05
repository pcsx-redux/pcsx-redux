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

#include "core/gpudump.h"

#include <string.h>

#include <algorithm>

#include "core/cdrom.h"
#include "core/gpu.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"

void PCSX::GPUDumper::start(IO<File> file) {
    stop();
    if (!file || file->failed()) return;
    m_file = file;
    m_frames = 0;
    m_state = State::Armed;
}

void PCSX::GPUDumper::stop() {
    if (m_state == State::Recording) {
        flushPending();
        flushFile();
    }
    if (m_file) m_file->close();
    m_file.reset();
    m_state = State::Idle;
}

void PCSX::GPUDumper::gp0LE(const uint32_t* words, unsigned count) {
    if (m_state != State::Recording) return;
    if (m_pendingType != GP0) flushPending();
    m_pendingType = GP0;
    for (unsigned i = 0; i < count; i++) m_pending.push_back(SWAP_LE32(words[i]));
}

void PCSX::GPUDumper::read(unsigned count) {
    if (m_state != State::Recording) return;
    if (!m_pending.empty()) flushPending();
    m_pendingReads += count;
}

void PCSX::GPUDumper::vsync(GPU* gpu, uint64_t cycle) {
    switch (m_state) {
        case State::Idle:
            return;
        case State::Armed:
            // Wait until the GP0 parser is between commands, or the replay would start mid-packet.
            if (gpu->isIdle()) begin(gpu);
            return;
        case State::Recording: {
            flushPending();
            uint32_t stamp[2] = {uint32_t(cycle), uint32_t(cycle >> 32)};
            packet(VSync, stamp, 2);
            flushFile();
            m_frames++;
        } break;
    }
}

void PCSX::GPUDumper::begin(GPU* gpu) {
    m_out.clear();
    m_pending.clear();
    m_pendingReads = 0;
    m_pendingType = 0;

    uint32_t magic[4];
    memcpy(magic, c_magic, sizeof(magic));
    // flushFile() converts to little endian, so pre-swap the magic to keep its bytes in order.
    for (auto w : magic) m_out.push_back(SWAP_LE32(w));

    uint32_t version = 2;
    packet(GPUVersion, &version, 1);
    auto& id = g_emulator->m_cdrom->getCDRomID();
    if (!id.empty()) string(GameID, id);

    std::vector<uint32_t> gp0, gp1;
    gpu->getRestoreSequence(gp0, gp1);
    string(VideoFormat, (gp1[2] & 0x08) ? "PAL" : "NTSC");
    string(Comment, "PCSX-Redux");

    // Reset first, so the VRAM upload below is not affected by a mask bit setting.
    packet(GP1, gp1.data(), 1);

    Slice vram = gpu->getVRAM(GPU::Ownership::ACQUIRE);
    std::vector<uint32_t> upload;
    upload.reserve(3 + 1024 * 512 / 2);
    upload.push_back(0xa0000000);
    upload.push_back(0x00000000);
    upload.push_back(0x02000400);
    const uint16_t* pixels = vram.data<uint16_t>();
    for (unsigned i = 0; i < 1024 * 512; i += 2) upload.push_back(pixels[i] | (uint32_t(pixels[i + 1]) << 16));
    packet(GP0, upload.data(), upload.size());

    packet(GP0, gp0.data(), gp0.size());
    packet(GP1, gp1.data() + 1, gp1.size() - 1);
    packet(TraceBegin, nullptr, 0);
    flushFile();
    m_state = State::Recording;
}

void PCSX::GPUDumper::push(uint8_t type, const uint32_t* words, unsigned count) {
    if (m_pendingReads || (m_pendingType != type && !m_pending.empty())) flushPending();
    m_pendingType = type;
    m_pending.insert(m_pending.end(), words, words + count);
}

void PCSX::GPUDumper::flushPending() {
    if (m_pendingReads) {
        packet(ThrowAway, &m_pendingReads, 1);
        m_pendingReads = 0;
    }
    if (!m_pending.empty()) {
        packet(m_pendingType, m_pending.data(), m_pending.size());
        m_pending.clear();
    }
}

void PCSX::GPUDumper::packet(uint8_t type, const uint32_t* words, unsigned count) {
    // The length field is 24 bits wide; split anything longer, which only a pathological stream reaches.
    do {
        unsigned chunk = std::min(count, 0xffffffu);
        m_out.push_back((uint32_t(type) << 24) | chunk);
        if (chunk) m_out.insert(m_out.end(), words, words + chunk);
        words += chunk;
        count -= chunk;
    } while (count);
}

void PCSX::GPUDumper::string(uint8_t type, std::string_view str) {
    std::vector<uint32_t> words((str.size() + 4) / 4, 0);
    memcpy(words.data(), str.data(), str.size());
    packet(type, words.data(), words.size());
}

void PCSX::GPUDumper::flushFile() {
    if (m_out.empty()) return;
    for (auto& w : m_out) w = SWAP_LE32(w);
    m_file->write(m_out.data(), m_out.size() * 4);
    m_out.clear();
}

bool PCSX::GPUDumpReader::open(IO<File> file) {
    if (!file || file->failed()) return false;
    size_t size = file->size();
    std::vector<uint32_t> words(size / 4);
    file->rSeek(0, SEEK_SET);
    ssize_t got = file->read(words.data(), words.size() * 4);
    // Keep only what was actually read; next() then stops at a packet cut short.
    words.resize(got > 0 ? size_t(got) / 4 : 0);
    return open(std::move(words));
}

bool PCSX::GPUDumpReader::open(std::vector<uint32_t>&& words) {
    m_words = std::move(words);
    for (auto& w : m_words) w = SWAP_LE32(w);
    m_ptr = 4;
    if (m_words.size() < 4) return false;
    uint32_t magic[4];
    memcpy(magic, GPUDumper::c_magic, sizeof(magic));
    for (unsigned i = 0; i < 4; i++) {
        if (m_words[i] != SWAP_LE32(magic[i])) return false;
    }
    return true;
}

bool PCSX::GPUDumpReader::next(Packet& packet) {
    if (m_ptr >= m_words.size()) return false;
    uint32_t header = m_words[m_ptr++];
    packet.type = header >> 24;
    packet.length = header & 0xffffff;
    if (packet.length > m_words.size() - m_ptr) {
        m_ptr = m_words.size();
        return false;
    }
    packet.words = m_words.data() + m_ptr;
    m_ptr += packet.length;
    return true;
}

PCSX::GPUDumpPlayer::GPUDumpPlayer() {}

PCSX::GPUDumpPlayer::~GPUDumpPlayer() { unload(); }

void PCSX::GPUDumpPlayer::unload() {
    if (m_gpu) m_gpu->shutdown();
    m_logger.clearFrameLog();
    m_logger.setOwner(nullptr);
    m_gpu.reset();
    m_snapshots.clear();
    m_snapshotInterval = c_snapshotInterval;
    m_frame = 0;
    m_frameCount = 0;
    m_gameID.clear();
    m_comment.clear();
}

static std::string packetString(const PCSX::GPUDumpReader::Packet& packet) {
    std::string ret(reinterpret_cast<const char*>(packet.words), packet.length * 4);
    auto end = ret.find('\0');
    if (end != std::string::npos) ret.resize(end);
    return ret;
}

bool PCSX::GPUDumpPlayer::load(IO<File> file) {
    unload();
    if (!m_reader.open(file)) return false;
    m_gpu = GPU::getSoft();
    m_gpu->detach(&m_logger);
    m_logger.setOwner(m_gpu.get());
    // No UI: a detached GPU must not draw into the main window. Callers read its VRAM instead.
    m_gpu->init(nullptr);
    rewind();
    GPUDumpReader::Packet packet;
    while (m_reader.next(packet)) {
        if (packet.type == GPUDumper::VSync) m_frameCount++;
    }
    m_reader.setPosition(m_traceStart);
    return true;
}

void PCSX::GPUDumpPlayer::rewind() {
    if (!m_gpu) return;
    m_frame = 0;
    m_logger.clearFrameLog();
    m_logger.setFrame(0);
    m_reader.rewind();
    m_gpu->writeStatus(0x00000000);
    m_gpu->clearVRAM();
    GPUDumpReader::Packet packet;
    // Everything before the trace begin marker is state restoration. A dump without one starts live.
    size_t start = 4;
    while (m_reader.next(packet)) {
        if (packet.type == GPUDumper::TraceBegin) {
            start = m_reader.position();
            break;
        }
        if (packet.type == GPUDumper::VSync) {
            m_reader.rewind();
            break;
        }
        execute(packet);
    }
    m_traceStart = start;
}

bool PCSX::GPUDumpPlayer::step() {
    if (!m_gpu) return false;
    GPUDumpReader::Packet packet;
    while (m_reader.next(packet)) {
        if (packet.type == GPUDumper::VSync) {
            m_gpu->vblank(true);
            m_frame++;
            m_logger.setFrame(m_frame);
            cacheSnapshot();
            return true;
        }
        execute(packet);
    }
    return false;
}

void PCSX::GPUDumpPlayer::cacheSnapshot() {
    if (m_frame % m_snapshotInterval != 0) return;
    // A command cut by the vsync, or readback words still queued, would be lost: the snapshot only
    // holds what a save state holds.
    if (!m_gpu->isIdle() || m_gpu->hasPendingReadback()) return;
    if (m_snapshots.contains(m_frame)) return;
    if (m_snapshots.size() >= c_maxSnapshots) {
        m_snapshotInterval *= 2;
        std::erase_if(m_snapshots, [this](const auto& entry) { return entry.first % m_snapshotInterval != 0; });
        if (m_frame % m_snapshotInterval != 0) return;
    }
    auto& snapshot = m_snapshots[m_frame];
    snapshot.position = m_reader.position();
    m_gpu->snapshot(snapshot.gpu);
}

bool PCSX::GPUDumpPlayer::seek(uint64_t frame) {
    if (!m_gpu) return false;
    if (frame > m_frameCount) return false;
    if (frame == 0) {
        rewind();
        return true;
    }
    // The logger keeps the commands of the last frame played, so the restart point is at most the
    // frame before the target, and the target frame itself is always played.
    auto it = m_snapshots.upper_bound(frame - 1);
    uint64_t from = 0;
    if (it != m_snapshots.begin()) from = std::prev(it)->first;
    if (m_frame >= frame || m_frame < from) {
        if (from == 0) {
            rewind();
        } else {
            auto& snapshot = std::prev(it)->second;
            m_gpu->restore(snapshot.gpu);
            m_reader.setPosition(snapshot.position);
            m_frame = from;
            m_logger.clearFrameLog();
            m_logger.setFrame(from);
        }
    }
    while (m_frame < frame) {
        if (!step()) return false;
    }
    return true;
}

void PCSX::GPUDumpPlayer::execute(const GPUDumpReader::Packet& packet) {
    switch (packet.type) {
        case GPUDumper::GP0:
            for (uint32_t i = 0; i < packet.length; i++) m_gpu->writeData(packet.words[i]);
            break;
        case GPUDumper::GP1:
            for (uint32_t i = 0; i < packet.length; i++) m_gpu->writeStatus(packet.words[i]);
            break;
        case GPUDumper::ThrowAway:
        case GPUDumper::Readback:
            if (packet.length >= 1) {
                // Reads past the end of a VRAM transfer have no effect, so a count larger than 2MB of VRAM
                // can only come from a corrupt file.
                uint32_t count = std::min<uint32_t>(packet.words[0], 1024 * 1024 / 2);
                for (uint32_t i = 0; i < count; i++) m_gpu->readData();
            }
            break;
        case GPUDumper::GameID:
            m_gameID = packetString(packet);
            break;
        case GPUDumper::Comment:
            m_comment = packetString(packet);
            break;
        default:
            break;
    }
}
