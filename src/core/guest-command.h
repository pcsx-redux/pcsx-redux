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

#include <filesystem>

namespace PCSX {

/*

Emulator side of the guest command port (0x1f8020a4). See
src/mips/common/hardware/pcsxcmd.h for the protocol and the reasoning.

The guest hands over a pointer to a command struct with a plain volatile store.
Everything the emulator says back is written into that struct: there is no
return channel, and there deliberately isn't one, because the absence of a
write is what tells a guest running on an older build that nothing heard it.

*/

namespace GuestCommand {

// Handles one command struct at the given guest address. Never throws and never
// pauses; a malformed command is reported by writing a status into the struct
// when the struct itself is readable, and ignored when it is not.
void dispatch(uint32_t structAddress);

// Where PCSX_CMD_REPORT_WRITE puts what the guest writes. The file is created on
// the first non-empty write, so a run whose guest writes nothing leaves none.
// An empty path means this run collects no report.
void setReportPath(const std::filesystem::path& path);
void closeReport();

}  // namespace GuestCommand

}  // namespace PCSX
