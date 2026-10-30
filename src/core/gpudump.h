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

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/gpu.h"
#include "core/gpulogger.h"
#include "support/file.h"

namespace PCSX {

class GPU;

// Records the GP0 / GP1 traffic of the emulated GPU in the GPU dump format described at
// https://github.com/ps1dev/standards/blob/main/GPUDUMP.md
//
// A capture is armed with start(), and the file actually begins at the next vsync where no GP0 command
// is in flight. It opens with a reset, a full VRAM upload and the GP0 / GP1 state needed to reproduce
// the frame, then the trace begin marker, then the live traffic.
class GPUDumper {
  public:
    enum PacketType : uint8_t {
        GP0 = 0x00,
        GP1 = 0x01,
        VSync = 0x02,
        ThrowAway = 0x03,
        Readback = 0x04,
        TraceBegin = 0x05,
        GPUVersion = 0x06,
        GameID = 0x10,
        VideoFormat = 0x11,
        Comment = 0x12,
    };
    static constexpr char c_magic[16] = {'P', 'S', 'X', 'G', 'P', 'U', 'D', 'U', 'M', 'P', 'v', '1', 'r', '1', 0, 0};

    ~GPUDumper() { stop(); }
    void start(IO<File> file);
    void stop();
    bool armed() const { return m_state == State::Armed; }
    bool recording() const { return m_state == State::Recording; }
    uint64_t frames() const { return m_frames; }

    void gp0(uint32_t word) {
        if (m_state == State::Recording) push(GP0, &word, 1);
    }
    // Words as they sit in emulated memory, which is little endian.
    void gp0LE(const uint32_t* words, unsigned count);
    void gp1(uint32_t word) {
        if (m_state == State::Recording) push(GP1, &word, 1);
    }
    void read(unsigned count);
    void vsync(GPU* gpu, uint64_t cycle);

  private:
    enum class State { Idle, Armed, Recording };
    void begin(GPU* gpu);
    void push(uint8_t type, const uint32_t* words, unsigned count);
    void packet(uint8_t type, const uint32_t* words, unsigned count);
    void string(uint8_t type, std::string_view str);
    void flushPending();
    void flushFile();

    State m_state = State::Idle;
    IO<File> m_file;
    uint8_t m_pendingType = 0;
    std::vector<uint32_t> m_pending;
    uint32_t m_pendingReads = 0;
    std::vector<uint32_t> m_out;
    uint64_t m_frames = 0;
};

// Sequential reader for GPU dump files. It only splits the stream into packets; interpreting them is
// left to the caller.
class GPUDumpReader {
  public:
    struct Packet {
        uint8_t type;
        const uint32_t* words;
        uint32_t length;
    };
    // Takes ownership of a copy of the whole file. Returns false if the magic does not match.
    bool open(IO<File> file);
    bool open(std::vector<uint32_t>&& words);
    bool next(Packet& packet);
    void rewind() { m_ptr = 4; }
    bool atEnd() const { return m_ptr >= m_words.size(); }
    size_t position() const { return m_ptr; }
    void setPosition(size_t ptr) { m_ptr = ptr; }
    size_t size() const { return m_words.size(); }

  private:
    std::vector<uint32_t> m_words;
    size_t m_ptr = 4;
};

// Plays a GPU dump back into a private software GPU, detached from the emulated machine.
class GPUDumpPlayer {
  public:
    GPUDumpPlayer();
    ~GPUDumpPlayer();
    // Loads the dump and runs it up to the trace begin marker, which restores the captured state.
    bool load(IO<File> file);
    void unload();
    bool loaded() const { return m_gpu != nullptr; }
    // Runs the next frame, up to and including its vsync. Returns false at the end of the dump.
    bool step();
    void rewind();
    // Puts the player at the end of the given frame, as rewind() then that many step() calls would.
    // It restarts from the closest cached snapshot when that is faster. Returns false past the end.
    bool seek(uint64_t frame);
    uint64_t frame() const { return m_frame; }
    uint64_t frameCount() const { return m_frameCount; }
    GPU* gpu() { return m_gpu.get(); }
    GPULogger* logger() { return &m_logger; }
    const std::string& gameID() const { return m_gameID; }
    const std::string& comment() const { return m_comment; }

  private:
    void execute(const GPUDumpReader::Packet& packet);
    void cacheSnapshot();
    // One snapshot every m_snapshotInterval frames, taken as step() reaches them. A snapshot holds
    // 2MB of VRAM, so past c_maxSnapshots every other one is dropped and the interval doubles.
    static constexpr uint64_t c_snapshotInterval = 30;
    static constexpr size_t c_maxSnapshots = 128;
    uint64_t m_snapshotInterval = c_snapshotInterval;
    struct Snapshot {
        size_t position;
        GPU::Snapshot gpu;
    };
    std::map<uint64_t, Snapshot> m_snapshots;
    uint64_t m_frameCount = 0;
    std::unique_ptr<GPU> m_gpu;
    GPULogger m_logger{false};
    GPUDumpReader m_reader;
    size_t m_traceStart = 4;
    uint64_t m_frame = 0;
    std::string m_gameID, m_comment;
};

}  // namespace PCSX
