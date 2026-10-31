/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/system.h"

namespace PCSX {

namespace SaveStates {
struct Access;
}

class SIO;

class MemoryCard {
  public:
    MemoryCard() : m_sio(nullptr) { memset(m_mcdData, 0, c_cardSize); }
    MemoryCard(SIO *parent) : m_sio(parent) { memset(m_mcdData, 0, c_cardSize); }

    // Hardware events
    void acknowledge();
    void deselect() {
        memset(&m_tempBuffer, 0, c_sectorSize);
        m_currentCommand = Commands::None;
        m_commandTicks = 0;
        m_dataOffset = 0;
        m_sector = 0;
        m_spdr = Responses::IdleHighZ;
    }

    // File system / data manipulation
    void commit(const PCSX::u8string path) {
        if (!m_savedToDisk) {
            saveMcd(path);
        }
    }
    void createMcd(PCSX::u8string mcd);
    bool dataChanged() { return !m_savedToDisk; }
    void disablePocketstation() { m_pocketstationEnabled = false; };
    void enablePocketstation() { m_pocketstationEnabled = true; };
    char *getMcdData() { return m_mcdData; }
    // Reads the card from mcd, creating a formatted file first if it does not exist and createIfMissing is
    // set. Returns false, logging why and leaving the card's data as it was, when the file cannot be read.
    [[nodiscard]] bool loadMcd(PCSX::u8string mcd, bool createIfMissing = true);
    void saveMcd(PCSX::u8string mcd, const char *data, uint32_t adr, size_t size);
    void saveMcd(PCSX::u8string mcd) { saveMcd(mcd, m_mcdData, 0, c_cardSize); }

  private:
    enum Commands : uint8_t {
        Access = 0x81,  // Memory Card Select
        Read = 0x52,    // Read Command
        GetID = 0x53,   // Get ID Command
        Write = 0x57,   // Write Command
        None = 0x00,    // No command, idle state
        Error = 0xFF,   // Bad command

        // PocketStation command extensions
        PS_ChangeFuncValue = 0x50,  // Change a FUNC 03h related value or so
        PS_GetVersion = 0x58,       // Get an ID or Version value or so
        PS_PrepFileExec = 0x59,     // Prepare File Execution with Dir_index, and Parameter
        PS_GetDirIndex = 0x5A,      // Get Dir_index, ComFlags, F_SN, Date, and Time
        PS_ExecXferPSToPSX = 0x5B,  // Execute Function and transfer data from Pocketstation to PSX
        PS_ExecXferPSXToPS = 0x5C,  // Execute Function and transfer data from PSX to Pocketstation
        PS_ExecCustom = 0x5D,       // Execute Custom Download Notification Function   ;via SWI 01h with r0=3
        PS_GetComFlagsHi = 0x5E,    // Get-and-Send ComFlags.bit1,3,2
        PS_GetComFlagsLo = 0x5F,    // Get-and-Send ComFlags.bit0
    };
    enum Flags : uint8_t {
        DirectoryUnread = 0x08,  // Initial power on value
        DirectoryRead = 0x00     // Cleared after good MC Write
                                 // (test write sector 3F = 0x1F80) offset
    };
    enum Responses : uint8_t {
        IdleHighZ = 0xFF,            // High default state
        Dummy = 0x00,                // Filler Data
        ID1 = 0x5A,                  // Memory Card ID1
        ID2 = 0x5D,                  // Memory Card ID2
        CommandAcknowledge1 = 0x5C,  // Command Acknowledge 1
        CommandAcknowledge2 = 0x5D,  // Command Acknowledge 2
        GoodReadWrite = 0x47,        // Good Read/Write
        BadChecksum = 0x4E,          // Bad Checksum during Write
        BadSector = 0xFF,            // Bad Memory Card Sector
    };

    friend class SIO;
    friend struct SaveStates::Access;

    static constexpr size_t c_sectorSize = 8 * 16;
    static constexpr size_t c_blockSize = 8192;
    static constexpr size_t c_cardSize = 1024 * c_sectorSize;

    // State machine / handlers
    uint8_t transceive(uint8_t value);
    uint8_t tickReadCommand(uint8_t value);
    uint8_t tickWriteCommand(uint8_t value);
    uint8_t tickPS_GetDirIndex(uint8_t value);   // 5Ah
    uint8_t tickPS_GetVersion(uint8_t value);    // 58h
    uint8_t tickPS_PrepFileExec(uint8_t value);  // 59h
    uint8_t tickPS_ExecCustom(uint8_t value);    // 5Dh

    char m_mcdData[c_cardSize];
    uint8_t m_tempBuffer[c_sectorSize];
    bool m_savedToDisk = false;

    uint8_t m_checksumIn = 0, m_checksumOut = 0;
    uint16_t m_commandTicks = 0;
    uint8_t m_currentCommand = Commands::None;
    uint16_t m_sector = 0;
    uint32_t m_dataOffset = 0;

    uint8_t m_directoryFlag = Flags::DirectoryUnread;

    uint8_t m_spdr = Responses::IdleHighZ;

    // PocketStation Specific
    bool m_pocketstationEnabled = false;
    uint16_t m_directoryIndex = 0;

    SIO *m_sio;
};

}  // namespace PCSX
