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

#include <array>
#include <string>
#include <vector>

#include "core/spdwriter.h"

namespace PCSX {

// Records the CPU-side traffic that decides what lives where in sound RAM: uploads, the
// transfer address, per-voice start and loop pointers, and key on/off. Between an upload's
// extent and a voice's start address, this is enough to recover where each sample begins,
// which is what an ADPCM decoder needs in order to start with the right filter history.
//
// Sample *ends* are deliberately not logged. Once a start address is known, walking 16-byte
// blocks until the flag byte has bit 0 set is a pure function of sound RAM, so a viewer can
// derive it at draw time without any instrumentation.
//
// Everything recorded here is a write performed by the CPU, so every entry point runs on the
// emulation thread. The SPU's mixing thread never calls in. That is what lets the ring be a
// plain single-producer buffer with no locking.
class SPULogger {
  public:
    enum class Kind : uint8_t {
        DMAWrite,      // main RAM -> sound RAM, via DMA channel 4
        DMARead,       // sound RAM -> main RAM
        PIOWrite,      // halfword pushed through 1F801DA8h
        TransferAddr,  // 1F801DA6h
        VoiceStart,    // 1F801C06h + N*10h
        VoiceLoop,     // 1F801C0Eh + N*10h
        KeyOn,         // 1F801D88h
        KeyOff,        // 1F801D8Ch
        ReverbAddr,    // 1F801DA2h
        RegWrite,      // any register, address = offset from 1F801C00h
    };

    static constexpr uint8_t c_noVoice = 0xff;
    static constexpr size_t c_capacity = 256 * 1024;

    struct Event {
        uint64_t cycle = 0;
        uint32_t pc = 0;       // guest PC responsible for the write
        uint32_t address = 0;  // byte offset into sound RAM, where meaningful
        uint32_t length = 0;   // bytes, for transfers
        uint32_t value = 0;    // raw register value, or the key mask
        Kind kind = Kind::PIOWrite;
        uint8_t voice = c_noVoice;
    };

    void log(Kind kind, uint32_t address, uint32_t length, uint32_t value, uint8_t voice = c_noVoice) {
        if (m_enabled) logInternal(kind, address, length, value, voice);
    }

    bool enabled() const { return m_enabled; }
    // Turning the logger back on after a pause leaves a hole in the register history, which is
    // remembered so an export does not present the result as a continuous capture.
    void setEnabled(bool enabled);
    void clear();

    // Called by the SPU before it applies a register write, so the first write after a clear
    // can snapshot the register file it is about to change.
    void logRegWrite(uint32_t reg, uint16_t value, const uint16_t *registers) {
        if (!m_enabled) {
            // A register write missed while paused breaks the replay; anything else does not.
            if (m_baselineValid) m_gap = true;
            return;
        }
        if (!m_baselineValid) captureBaseline(registers);
        logInternal(Kind::RegWrite, reg, 0, value, c_noVoice);
    }

    // Writes the retained register writes as an SPU dump. Returns an empty string on success,
    // or why the log cannot be exported.
    std::string exportSPD(std::vector<uint8_t> &out, SPD::Options options);

    size_t size() const { return m_count; }
    // Oldest-first: index 0 is the oldest entry still retained.
    const Event &operator[](size_t index) const { return m_events[(m_head + index) % c_capacity]; }

    // Total events seen since the last clear(), including ones the ring has since dropped.
    // The difference against size() is how much history was lost.
    uint64_t seen() const { return m_seen; }

    // Bumped by every clear(), so a reader can tell a clear from the count having moved on.
    uint64_t clears() const { return m_clears; }

    static const char *kindName(Kind kind);
    static bool isVoiceScoped(Kind kind);

  private:
    void logInternal(Kind kind, uint32_t address, uint32_t length, uint32_t value, uint8_t voice);
    void captureBaseline(const uint16_t *registers);

    std::vector<Event> m_events;
    size_t m_head = 0;
    size_t m_count = 0;
    uint64_t m_seen = 0;
    uint64_t m_clears = 0;
    bool m_enabled = false;
    bool m_baselineValid = false;
    bool m_gap = false;
    std::array<uint16_t, 0x100> m_baseline{};
};

}  // namespace PCSX
