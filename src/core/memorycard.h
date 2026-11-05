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

#include <memory>
#include <vector>

#include "core/sstate.h"
#include "core/system.h"

namespace PCSX {

namespace SaveStates {
struct Access;
}

class SIO;

namespace PocketStation {
class PocketStation;
}

class MemoryCard {
  public:
    // Constructors and destructor are out-of-line (defined in memorycard.cc): the unique_ptr to
    // the forward-declared PocketStation device needs the complete type to instantiate its
    // destructor, which any inline ctor/dtor would otherwise force here.
    MemoryCard();
    MemoryCard(SIO *parent);
    ~MemoryCard();

    // Hardware events
    void acknowledge();
    // Out-of-line (memorycard.cc): when a real PocketStation is docked, deselect also ends the
    // in-progress COM command (re-arms FIQ-6 for the next one), which needs the complete device
    // type. The POD reset is unchanged.
    void deselect();

    // File system / data manipulation
    void commit(const PCSX::u8string path) {
        if (!m_savedToDisk) {
            saveMcd(path);
        }
    }
    void createMcd(PCSX::u8string mcd);
    bool dataChanged() { return !m_savedToDisk; }
    // Defined in memorycard.cc: enable creates+boots the device (kernel from the
    // SettingPocketstationBios path, flash from this slot's m_mcdData); disable tears it down.
    // Enabling a slot that already has a running device leaves it alone.
    void disablePocketstation();
    void enablePocketstation();
    // Rebuilds the device of an enabled slot, e.g. to boot a newly selected kernel image.
    void reloadPocketstation();
    // Two-way sync between the device's flash and m_mcdData, against the image both last agreed
    // on: device writes land in m_mcdData (marking the card unsaved), host-side edits of m_mcdData
    // land in the device. The device wins when both changed. Returns true when m_mcdData changed.
    bool syncPocketstationFlash();
    // Returns the docked PocketStation device, or nullptr if none (disabled, or no kernel image).
    PocketStation::PocketStation *getPocketstation() { return m_pocketstation.get(); }
    // Dock the device once its kernel has armed the dock interrupt, instead of waiting for the
    // first card access. Called from the cycle-delta and wall-clock drivers; no-op without a
    // device, once docked, or once the user has taken manual control. See memorycard.cc for why
    // first access is too late.
    void tickPocketstationDock();
    // Explicit dock / undock, both directions. Taking this hands control to the user: the automatic
    // dock and the first-access backstop both stand down for the life of the device, so an undock
    // is not silently reversed by the very next emulation step.
    void setPocketstationDocked(bool docked);
    bool isPocketstationDocked() const { return m_pocketstationDocked; }
    // Whether the user has overridden the automatic policy (for the UI to show, and for tests).
    bool pocketstationDockIsManual() const { return m_pocketstationManualDock; }
    // /SEL asserted on this slot's port. The line is shared with the pad, as on hardware, so the
    // device sees every selection of its port, not only card commands.
    void selectPocketstation();
    // Advances the device by armCycles, less the cycles transceive() already ran ahead of the
    // R3000A to collect an acknowledge.
    void runPocketstation(uint64_t armCycles);
    // Savestate fields for this slot's device. Save leaves the slot message empty without a device;
    // load restores only when the state carries a device and this slot has one to restore into.
    void savePocketstationState(SaveStates::PocketStationSlot& slot);
    void loadPocketstationState(SaveStates::PocketStationSlot& slot);
    char *getMcdData() { return m_mcdData; }
    void loadMcd(PCSX::u8string mcd);
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
    // PocketStation link timing, in ARM7 cycles at the docked 3.997696MHz clock: the PS1 selects
    // ~60us before the first byte completes; the kernel acknowledges within a few hundred cycles,
    // so the budget is a timeout, not a cost.
    static constexpr uint64_t c_pocketstationSelectLead = 250;
    static constexpr uint64_t c_pocketstationAckStep = 64;
    static constexpr uint64_t c_pocketstationAckBudget = 4096;

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
    bool m_pocketstationDocked = false;  // device has been docked (IRQ-11 fired) since creation.
    // Set once the user works the dock control by hand. While set, the automatic dock and the
    // first-access backstop both stand down, so a manual undock stays undocked.
    bool m_pocketstationManualDock = false;
    bool m_pocketstationSelected = false;     // /SEL held on this slot's port.
    bool m_pocketstationLeadPending = false;  // no byte yet since the select.
    uint64_t m_pocketstationPrepaid = 0;      // ARM7 cycles run ahead of the R3000A by transceive().
    uint16_t m_directoryIndex = 0;

    // Owned ARM7 device, present only while pocketstation mode is enabled AND a valid kernel
    // image is configured. Created/destroyed by enable/disablePocketstation().
    std::unique_ptr<PocketStation::PocketStation> m_pocketstation;
    // The flash image the device and m_mcdData last agreed on; see syncPocketstationFlash().
    std::vector<uint8_t> m_pocketstationFlashShadow;
    // (Re)create + boot the device from the kernel-path setting and this slot's card image.
    void createPocketstation();

    SIO *m_sio;
};

}  // namespace PCSX
