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

#include <span>
#include <string>
#include <vector>

namespace PCSX {

namespace SPD {

// Writes a capture-mode SPU dump (PSXSPUDUMP v1r1): sample data cut out of sound RAM, one
// packet restoring the register file as it stood when the capture began, a trace begin
// marker, then every captured register write grouped by tick with inline waits between the
// groups. The stream sits in a single pattern behind a non-looping order table, because the
// players in the wild only start a stream that is reachable from an order table.

// One SPU register write. reg is the byte offset from 1F801C00h, so 0x000-0x1ff.
struct RegWrite {
    uint64_t cycle;
    uint16_t reg;
    uint16_t value;
};

struct Options {
    // Tick rate in 16.16 Hz. Zero picks 1/16th of the NTSC hblank rate, which a player that
    // counts hblanks divides exactly.
    uint32_t tickRate = 0;
    // CPU cycles per second, to turn cycle stamps into ticks.
    uint32_t clock = 33868800;
    // Upload the whole of sound RAM past the capture buffers instead of only the span the
    // voices point into.
    bool fullRam = false;
    std::string comment = "Captured by PCSX-Redux";
};

struct Result {
    std::vector<uint8_t> data;
    std::string error;
    uint32_t sampleBase = 0;  // bytes into sound RAM
    uint32_t sampleSize = 0;  // bytes
    uint32_t writes = 0;      // register writes emitted after the trace begin marker
    uint64_t ticks = 0;       // stream duration
};

static constexpr uint32_t c_ntscHblanks = 15734;
static constexpr uint32_t c_palHblanks = 15625;
static constexpr uint32_t c_spuRamSize = 512 * 1024;

// True for registers whose replay would fight the player rather than reproduce the sound:
// the transfer address and FIFO (the samples arrive through the sample data packet), and the
// read-only status registers.
bool isDropped(uint16_t reg);
// SPUCNT with the transfer mode and IRQ enable bits cleared, for the same reason.
uint16_t filterValue(uint16_t reg, uint16_t value);

// baseline is the register file at the instant before writes[0], indexed by reg / 2.
// spuRam is all 512 KiB of sound RAM.
Result writeCapture(std::span<const uint16_t, 0x100> baseline, std::span<const RegWrite> writes,
                    const uint8_t *spuRam, const Options &options);

}  // namespace SPD

}  // namespace PCSX
