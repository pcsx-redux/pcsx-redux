/***************************************************************************
 *   Copyright (C) 2018 PCSX-Redux authors                                 *
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

#include <magic_enum/magic_enum_all.hpp>
#include <utility>

#include "core/system.h"
#include "fmt/printf.h"

namespace PCSX {

enum class LogClass : unsigned {
    UNCATEGORIZED,  // anything printf that hasn't been converted yet
    MIPS,           // only the things coming from MIPS code
    UI,             // messages from the UI specifically
    SIO0,           // pad and memory card information
    SIO1,           // uart/sio1 information
    SIO1SERVER,     // uart/sio1 server information
    GTE,            // gte information
    CDROM,          // low level cdrom information
    CDROM_IO,       // high level cdrom information (iso file access)
    CPU,            // CPU-related information
    HARDWARE,       // hardware-level (0x1f801000) information
    DMA,            // dma-related information
    MEMORY,         // memory access related information
    IRQ,            // irq scheduling and triggering information
    KERNEL,         // kernel logs
    GDB,            // GDB logs
    SYSTEM,         // system logs from the emulator itself
    LUA,            // logs emitted by the Lua VM
    SPU,            // spu information
    GPU,            // gpu information
    WEBSERVER,      // web server information
};

template <LogClass logClass, bool enabled>
struct Logger {
    template <typename... Args>
    static void Log(const char *format, Args &&...args) {
        if (!enabled) return;
        std::string s = fmt::sprintf(format, std::forward<Args>(args)...);
        g_system->log(logClass, std::move(s));
    }
    static void Log(std::string &&s) {
        if (!enabled) return;
        g_system->log(logClass, std::move(s));
    }
    static constexpr bool c_enabled = enabled;
    static constexpr LogClass c_logClass = logClass;
};

}  // namespace PCSX
