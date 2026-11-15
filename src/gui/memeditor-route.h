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

// Which memory editor a JumpToMemory address belongs to, and the byte offset inside that editor.
// KUSEG, KSEG0 and KSEG1 mirrors resolve to the same editor; the scratchpad is not mapped in KSEG1.
struct MemEditorRoute {
    enum class Target { None, MainRam, ParallelPort, Scratchpad, HardwareRegisters, Bios };
    Target target = Target::None;
    uint32_t offset = 0;

    static constexpr MemEditorRoute route(uint32_t address) {
        const uint32_t segment = address >> 29;
        const bool kseg1 = segment == 5;
        if (segment == 6 || segment == 7) return {};
        const uint32_t phys = segment >= 4 ? address & 0x1fffffff : address;
        if (phys < 0x00800000) return {Target::MainRam, phys};
        if (phys >= 0x1f000000 && phys < 0x1f080000) return {Target::ParallelPort, phys - 0x1f000000};
        if (phys >= 0x1f800000 && phys < 0x1f800400 && !kseg1) return {Target::Scratchpad, phys - 0x1f800000};
        if (phys >= 0x1f801000 && phys < 0x1f803000) return {Target::HardwareRegisters, phys - 0x1f801000};
        if (phys >= 0x1fc00000 && phys < 0x1fc80000) return {Target::Bios, phys - 0x1fc00000};
        return {};
    }
};

}  // namespace PCSX
