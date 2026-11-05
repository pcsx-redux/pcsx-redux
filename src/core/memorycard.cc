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

#include "core/memorycard.h"

#include <sys/stat.h>

#include "core/sio.h"
#include "pocketstation/pocketstation.h"
#include "support/sjis_conv.h"

// Constructors + destructor are out-of-line so std::unique_ptr<PocketStation::PocketStation> sees
// the complete type here (otherwise an inline ctor/dtor would need it at every include site).
PCSX::MemoryCard::MemoryCard() : m_sio(nullptr) { memset(m_mcdData, 0, c_cardSize); }
PCSX::MemoryCard::MemoryCard(SIO *parent) : m_sio(parent) { memset(m_mcdData, 0, c_cardSize); }
PCSX::MemoryCard::~MemoryCard() = default;

void PCSX::MemoryCard::enablePocketstation() {
    if (m_pocketstationEnabled && m_pocketstation) return;
    m_pocketstationEnabled = true;
    createPocketstation();
}

void PCSX::MemoryCard::reloadPocketstation() {
    if (m_pocketstationEnabled) createPocketstation();
}

bool PCSX::MemoryCard::syncPocketstationFlash() {
    if (!m_pocketstation || m_pocketstation->flashSize() != c_cardSize) return false;
    uint8_t *flash = m_pocketstation->flashDataMutable();
    uint8_t *shadow = m_pocketstationFlashShadow.data();
    uint8_t *card = reinterpret_cast<uint8_t *>(m_mcdData);
    if (memcmp(flash, shadow, c_cardSize) != 0) {
        memcpy(card, flash, c_cardSize);
        memcpy(shadow, flash, c_cardSize);
        m_savedToDisk = false;
        return true;
    }
    if (memcmp(card, shadow, c_cardSize) != 0) {
        memcpy(flash, card, c_cardSize);
        memcpy(shadow, card, c_cardSize);
    }
    return false;
}

void PCSX::MemoryCard::disablePocketstation() {
    syncPocketstationFlash();
    m_pocketstationEnabled = false;
    m_pocketstationDocked = false;
    m_pocketstationManualDock = false;
    m_pocketstation.reset();
}

// Dock the device as soon as its kernel can take the edge, rather than on the first card access.
//
// Docking is not instantaneous from the kernel's side: the edge raises IRQ-11 and the kernel has to
// run before the card link is live. Measured against the harness, the retail kernel needs between
// one and two frames' worth of ARM7 cycles (66628 < n <= 133256) because its GUI only enables COM
// from the per-frame sense call, and OpenPSK needs ~60 even though it acts straight from the
// handler. Docking on first access gives either of them ZERO cycles - the dock edge and the first
// exchange land in the same call - so that first transaction is answered with 0xFF and the PS1 BIOS
// sees a card that is not there. Doing it from the step drivers instead means the device is docked
// while the emulator is merely running, which is where those cycles are.
//
// Gated on the kernel having unmasked the dock interrupt: before that the edge would be latched
// against a device that is not listening yet. Nothing here runs without a PocketStation, so an
// ordinary memory card is untouched. The lazy dock in transceive() stays as the backstop for a
// device that somehow never arms the interrupt.
void PCSX::MemoryCard::tickPocketstationDock() {
    if (!m_pocketstation || m_pocketstationDocked || m_pocketstationManualDock) return;
    if (!(m_pocketstation->peekIrqMask() & (1u << 11))) return;  // kernel has not armed IRQ-11 yet
    m_pocketstation->setDocked(true);
    m_pocketstationDocked = true;
}

// Explicit dock / undock from the UI. Latches manual control: from here on tickPocketstationDock()
// and the first-access backstop in transceive() both stand down, so an undock is not undone by the
// next emulation step (the automatic policy's readiness condition stays true forever once the
// kernel has armed the interrupt, so without this latch the undock would last exactly one step and
// still report success). Undocking also ends any command in flight - the device just left the slot.
void PCSX::MemoryCard::setPocketstationDocked(bool docked) {
    if (!m_pocketstation) return;
    m_pocketstationManualDock = true;
    if (docked == m_pocketstationDocked) return;
    if (!docked && m_pocketstationSelected) {
        m_pocketstation->comSetSelected(false);
        m_pocketstationSelected = false;
        m_pocketstationLeadPending = false;
    }
    m_pocketstation->setDocked(docked);
    m_pocketstationDocked = docked;
}

void PCSX::MemoryCard::deselect() {
    memset(&m_tempBuffer, 0, c_sectorSize);
    m_currentCommand = Commands::None;
    m_commandTicks = 0;
    m_dataOffset = 0;
    m_sector = 0;
    m_spdr = Responses::IdleHighZ;
    // /SEL inactive ends any in-progress COM command so the next byte re-arms FIQ-6. The device
    // stays docked (deselect is the chip-select line, not undocking).
    if (m_pocketstation && m_pocketstationSelected) m_pocketstation->comSetSelected(false);
    m_pocketstationSelected = false;
    m_pocketstationLeadPending = false;
}

void PCSX::MemoryCard::selectPocketstation() {
    if (!m_pocketstation || !m_pocketstationDocked || m_pocketstationSelected) return;
    m_pocketstation->comSetSelected(true);
    m_pocketstationSelected = true;
    m_pocketstationLeadPending = true;
}

void PCSX::MemoryCard::runPocketstation(uint64_t armCycles) {
    if (!m_pocketstation) return;
    const uint64_t repay = std::min(armCycles, m_pocketstationPrepaid);
    m_pocketstationPrepaid -= repay;
    armCycles -= repay;
    if (armCycles) m_pocketstation->runCycles(armCycles);
}

void PCSX::MemoryCard::createPocketstation() {
    syncPocketstationFlash();
    m_pocketstation.reset();
    m_pocketstationDocked = false;
    m_pocketstationManualDock = false;
    m_pocketstationSelected = false;
    m_pocketstationLeadPending = false;
    m_pocketstationPrepaid = 0;

    // The ARM7 needs a 16 KiB PocketStation BIOS dump, configured via SettingPocketstationBios.
    // If it is unset or unreadable, leave the device absent rather than crashing.
    const PCSX::u8string kernelPath = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingPocketstationBios>().string();
    if (kernelPath.empty()) return;
    const char *fname = reinterpret_cast<const char *>(kernelPath.c_str());

    FILE *f = fopen(fname, "rb");
    if (f == nullptr) {
        PCSX::g_system->printf(_("PocketStation: kernel image %s could not be opened; device not started.\n"), fname);
        return;
    }
    uint8_t kernel[16 * 1024];
    const size_t got = fread(kernel, 1, sizeof(kernel), f);
    fclose(f);
    if (got != sizeof(kernel)) {
        PCSX::g_system->printf(
            _("PocketStation: kernel image %s is %zu bytes, expected 16384; device not started.\n"), fname, got);
        return;
    }

    auto dev = std::make_unique<PocketStation::PocketStation>();
    dev->setKernel(kernel, sizeof(kernel));
    // The device keeps its own copy of the card image; syncPocketstationFlash() reconciles the two.
    dev->setFlash(reinterpret_cast<const uint8_t *>(m_mcdData), c_cardSize);
    m_pocketstationFlashShadow.assign(reinterpret_cast<const uint8_t *>(m_mcdData),
                                      reinterpret_cast<const uint8_t *>(m_mcdData) + c_cardSize);
    dev->reset();
    m_pocketstation = std::move(dev);
}

void PCSX::MemoryCard::savePocketstationState(SaveStates::PocketStationSlot& slot) {
    if (!m_pocketstation) return;
    const auto blob = m_pocketstation->saveState();
    slot.get<SaveStates::PocketStationDevice>().value.assign(reinterpret_cast<const char*>(blob.data()), blob.size());
    slot.get<SaveStates::PocketStationDocked>().value = m_pocketstationDocked;
    slot.get<SaveStates::PocketStationManualDock>().value = m_pocketstationManualDock;
    slot.get<SaveStates::PocketStationSelected>().value = m_pocketstationSelected;
    slot.get<SaveStates::PocketStationLeadPending>().value = m_pocketstationLeadPending;
    slot.get<SaveStates::PocketStationPrepaid>().value = m_pocketstationPrepaid;
    slot.get<SaveStates::PocketStationArmAccum>().value = m_sio->m_psxArmAccum[this - m_sio->m_memoryCard];
}

void PCSX::MemoryCard::loadPocketstationState(SaveStates::PocketStationSlot& slot) {
    const auto& blob = slot.get<SaveStates::PocketStationDevice>().value;
    if (blob.empty()) return;  // the state has no device for this slot; leave ours running as it is.
    if (!m_pocketstation) {
        PCSX::g_system->printf(_("PocketStation: the savestate has a device in slot %i, but that slot has none.\n"),
                               int(this - m_sio->m_memoryCard) + 1);
        return;
    }
    syncPocketstationFlash();
    if (!m_pocketstation->loadState(reinterpret_cast<const uint8_t*>(blob.data()), blob.size())) {
        PCSX::g_system->printf(_("PocketStation: the device state in this savestate is not one this build reads.\n"));
        return;
    }
    // A savestate does not roll back the card file, as with an ordinary card: the next sync pushes
    // m_mcdData over the flash the state carried.
    m_pocketstationFlashShadow.assign(m_pocketstation->flashData(),
                                      m_pocketstation->flashData() + m_pocketstation->flashSize());
    m_pocketstationDocked = slot.get<SaveStates::PocketStationDocked>().value;
    m_pocketstationManualDock = slot.get<SaveStates::PocketStationManualDock>().value;
    m_pocketstationSelected = slot.get<SaveStates::PocketStationSelected>().value;
    m_pocketstationLeadPending = slot.get<SaveStates::PocketStationLeadPending>().value;
    m_pocketstationPrepaid = slot.get<SaveStates::PocketStationPrepaid>().value;
    m_sio->m_psxArmAccum[this - m_sio->m_memoryCard] = slot.get<SaveStates::PocketStationArmAccum>().value;
}

void PCSX::MemoryCard::acknowledge() { m_sio->acknowledge(); }

uint8_t PCSX::MemoryCard::transceive(uint8_t value) {
    // Real PocketStation docked: route the SPI byte exchange to the device's COM shift registers.
    // The kernel's COM/FIQ handler (advanced by the inter-burst cycle-delta catch-up, NOT here)
    // produces the reply on its own clock. This is non-blocking: we push the incoming byte and pop
    // whatever reply the kernel had loaded (one-transaction SPI pipeline delay). The tickPS_*/tick
    // stub handlers below are the fallback for a faked card (no real device).
    if (m_pocketstation) {
        // First exchange after the device exists = it's now docked into the PSX slot. The device
        // has been booting via the catch-up since creation, so its GUI has set up the interrupt
        // mask; the dock edge (IRQ-11) latches, and the GUI's next per-frame SWI 05h enables COM.
        // Backstop dock, for a kernel that somehow never arms the dock interrupt. Skipped once the
        // user has taken manual control: an undocked device is out of the slot and must stay silent.
        if (!m_pocketstationDocked && !m_pocketstationManualDock) {
            m_pocketstation->setDocked(true);
            m_pocketstationDocked = true;
        }
        if (!m_pocketstationDocked) return Responses::IdleHighZ;  // out of the slot: nothing answers
        // The kernel answers each byte from its FIQ handler and drives /ACK when it has loaded the
        // next reply. Bring the device up to the R3000A first, then run it ahead until it
        // acknowledges or the budget runs out; runPocketstation() pays those cycles back. No
        // acknowledge means the command is complete (or the device is not answering), exactly as
        // on hardware.
        g_emulator->m_sio->stepPocketstation();
        if (!m_pocketstationSelected) selectPocketstation();
        uint64_t ran = 0;
        if (m_pocketstationLeadPending) {
            // The kernel's FIQ starts at the select and clears any stale Ready before it polls,
            // so the first byte must land after it has started (~60us on a PS1).
            m_pocketstation->runCycles(c_pocketstationSelectLead);
            ran += c_pocketstationSelectLead;
            m_pocketstationLeadPending = false;
        }
        m_pocketstation->comBeginTransfer(value);
        while (ran < c_pocketstationAckBudget && !m_pocketstation->comAcked()) {
            m_pocketstation->runCycles(c_pocketstationAckStep);
            ran += c_pocketstationAckStep;
        }
        const bool acked = m_pocketstation->comAcked();
        const uint8_t reply = m_pocketstation->comTakeReply();
        m_pocketstation->comEndTransfer();
        m_pocketstationPrepaid += ran;
        if (acked) acknowledge();
        return reply;
    }

    uint8_t data_out = m_spdr;

    if (m_currentCommand == Commands::None || m_currentCommand == Commands::Access) {
        m_currentCommand = value;
    }

    switch (m_currentCommand) {
        // Access memory card device
        case Commands::Access:  // 81h
            // Incoming value is the device command
            m_spdr = m_directoryFlag;
            acknowledge();
            break;

        // Read a sector
        case Commands::Read:  // 52h
            m_spdr = tickReadCommand(value);
            break;

        // Write a sector
        case Commands::Write:  // 57h
            m_spdr = tickWriteCommand(value);
            break;

        //
        case Commands::PS_GetVersion:  // 58h
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_GetVersion(value);
            }
            break;

        //
        case Commands::PS_PrepFileExec:  // 59h
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_PrepFileExec(value);
            }
            break;

        //
        case Commands::PS_GetDirIndex:  // 5Ah
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_GetDirIndex(value);
            }
            break;

        //
        case Commands::PS_ExecCustom:  // 5Dh
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_ExecCustom(value);
            }
            break;

        case Commands::GetID:  // Un-implemented, need data capture
        case Commands::Error:  // Unexpected/Unsupported command
        default:
            m_spdr = Responses::IdleHighZ;
            break;
    }

    return data_out;
}

uint8_t PCSX::MemoryCard::tickReadCommand(uint8_t value) {
    uint8_t data_out = 0xFF;

    switch (m_commandTicks) {
        case 0:
            data_out = Responses::ID1;
            break;

        case 1:
            data_out = Responses::ID2;
            break;

        case 2:
            data_out = Responses::Dummy;
            break;

        case 3:  // MSB
            m_sector = (value << 8);
            data_out = value;
            break;

        case 4:  // LSB
            // Store lower 8 bits of sector
            m_sector |= value;
            m_dataOffset = m_sector * 128;
            data_out = Responses::CommandAcknowledge1;
            break;

        case 5:  // 00h
            data_out = Responses::CommandAcknowledge2;
            break;

        case 6:  // 00h
            // Confirm MSB
            data_out = m_sector >> 8;
            break;

        case 7:  // 00h
            // Confirm LSB
            data_out = (m_sector & 0xFF);
            m_checksumOut = (m_sector >> 8) ^ (m_sector & 0xff);
            break;

        // Cases 8 through 135 overloaded to default operator below
        default:
            if (m_commandTicks >= 8 && m_commandTicks <= 135) {  // Stay here for 128 bytes
                if (m_sector >= 1024) {
                    data_out = Responses::BadSector;
                } else {
                    data_out = m_mcdData[m_dataOffset++];
                }

                m_checksumOut ^= data_out;
            } else {
                // Send this till the spooky extra bytes go away
                return Responses::CommandAcknowledge1;
            }
            break;

        case 136:
            data_out = m_checksumOut;
            break;

        case 137:
            data_out = Responses::GoodReadWrite;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickWriteCommand(uint8_t value) {
    uint8_t data_out = 0xFF;

    switch (m_commandTicks) {
            // Data is sent and received simultaneously,
            // so the data we send isn't received by the system
            // until the next bytes are exchanged. In this way,
            // you have to basically respond one byte earlier than
            // actually occurs between Slave and Master.
            // Offset "Send" bytes noted from nocash's psx specs.

        case 0:  // 57h
            data_out = Responses::ID1;
            break;

        case 1:  // 00h
            data_out = Responses::ID2;
            break;

        case 2:  // 00h
            data_out = Responses::Dummy;
            break;

        case 3:  // MSB
            // Store upper 8 bits of sector
            m_sector = (value << 8);
            // Reply with (pre)
            data_out = value;
            break;

        case 4:  // LSB
            // Store lower 8 bits of sector
            m_sector |= value;
            // m_dataOffset = (m_sector * 128);
            m_dataOffset = 0;
            m_checksumOut = (m_sector >> 8) ^ (m_sector & 0xFF);
            data_out = value;
            break;

        // Cases 5 through 135 overloaded to default operator below
        default:
            if (m_commandTicks >= 5 && m_commandTicks <= 132) {
                // Store data in temp buffer until checksum verified
                // no idea if official cards did this, but why not.
                m_tempBuffer[m_dataOffset] = value;

                // Calculate checksum
                m_checksumOut ^= value;

                // Reply with (pre)
                data_out = value;
                m_dataOffset++;
            } else {
                // Send this till the spooky extra bytes go away
                return Responses::CommandAcknowledge1;
            }
            break;

        case 133:  // CHK
            m_checksumIn = value;

            if (m_sector >= 1024) {
                m_commandTicks = 0xFF;

                // To-do: Need to log actual behavior here
                return Responses::BadSector;
            } else if (m_checksumIn != m_checksumOut) {
                // To-do: Log official card behavior
                // Below behavior is from 3rd party hardware
                m_commandTicks = 0xFF;
                return Responses::BadChecksum;
            } else {
                data_out = Responses::CommandAcknowledge1;
            }
            break;

        case 134:  // 00h
            data_out = Responses::CommandAcknowledge2;
            break;

        case 135:  // 00h
            m_directoryFlag = Flags::DirectoryRead;
            data_out = Responses::GoodReadWrite;
            memcpy(&m_mcdData[m_sector * 128], &m_tempBuffer, c_sectorSize);
            m_savedToDisk = false;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_GetDirIndex(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;
    static constexpr uint8_t response_count = 19;
    static constexpr uint8_t responses[response_count] = {0x12, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x13, 0x11, 0x4F,
                                                          0x41, 0x20, 0x01, 0x99, 0x19, 0x27, 0x30, 0x09, 0x04};

    if (m_commandTicks < response_count) {
        data_out = responses[m_commandTicks];

        // Don't ack the last byte
        if (m_commandTicks <= (response_count - 1)) {
            acknowledge();
        }
    }

    m_commandTicks++;

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_ExecCustom(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;

    switch (m_commandTicks) {
        case 0:  // 5D
            data_out = 0x03;
            break;

        default:
            data_out = 0x00;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_PrepFileExec(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;

    switch (m_commandTicks) {
        case 0:  // 59
            data_out = 0x06;
            break;

        default:
            data_out = 0x00;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_GetVersion(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;
    static constexpr uint8_t response_count = 3;
    static constexpr uint8_t responses[response_count] = {0x02, 0x01, 0x01};

    if (m_commandTicks < response_count) {
        data_out = responses[m_commandTicks];

        // Don't ack the last byte
        if (m_commandTicks <= (response_count - 1)) {
            acknowledge();
        }
    }

    m_commandTicks++;

    return data_out;
}

// To-do: "All the code starting here is terrible and needs to be rewritten"
void PCSX::MemoryCard::loadMcd(PCSX::u8string mcd) {
    char *data = m_mcdData;
    if (std::filesystem::path(mcd).is_relative()) {
        mcd = (g_system->getPersistentDir() / mcd).u8string();
    }
    const char *fname = reinterpret_cast<const char *>(mcd.c_str());
    size_t bytesRead;

    m_directoryFlag = Flags::DirectoryUnread;

    FILE *f = fopen(fname, "rb");
    if (f == nullptr) {
        PCSX::g_system->printf(_("The memory card %s doesn't exist - creating it\n"), fname);
        createMcd(mcd);
        f = fopen(fname, "rb");
        if (f != nullptr) {
            struct stat buf;

            if (stat(fname, &buf) != -1) {
                // Check if the file is a VGS memory card, skip the header if it is
                if (buf.st_size == c_cardSize + 64) {
                    fseek(f, 64, SEEK_SET);
                }
                // Check if the file is a Dexdrive memory card, skip the header if it is
                else if (buf.st_size == c_cardSize + 3904) {
                    fseek(f, 3904, SEEK_SET);
                }
            }
            bytesRead = fread(data, 1, c_cardSize, f);
            fclose(f);
            if (bytesRead != c_cardSize) {
                throw std::runtime_error(_("Error reading memory card."));
            }
        } else {
            PCSX::g_system->message(_("Memory card %s failed to load!\n"), fname);
        }
    } else {
        struct stat buf;
        PCSX::g_system->printf(_("Loading memory card %s\n"), fname);
        if (stat(fname, &buf) != -1) {
            if (buf.st_size == c_cardSize + 64) {
                fseek(f, 64, SEEK_SET);
            } else if (buf.st_size == c_cardSize + 3904) {
                fseek(f, 3904, SEEK_SET);
            }
        }
        bytesRead = fread(data, 1, c_cardSize, f);
        fclose(f);
        if (bytesRead != c_cardSize) {
            throw std::runtime_error(_("Error reading memory card."));
        } else {
            m_savedToDisk = true;
        }
    }
}

void PCSX::MemoryCard::saveMcd(PCSX::u8string mcd, const char *data, uint32_t adr, size_t size) {
    if (std::filesystem::path(mcd).is_relative()) {
        mcd = (g_system->getPersistentDir() / mcd).u8string();
    }
    const char *fname = reinterpret_cast<const char *>(mcd.c_str());
    FILE *f = fopen(fname, "r+b");

    if (f != nullptr) {
        struct stat buf;

        if (stat(fname, &buf) != -1) {
            if (buf.st_size == c_cardSize + 64) {
                fseek(f, adr + 64, SEEK_SET);
            } else if (buf.st_size == c_cardSize + 3904) {
                fseek(f, adr + 3904, SEEK_SET);
            } else {
                fseek(f, adr, SEEK_SET);
            }
        } else {
            fseek(f, adr, SEEK_SET);
        }

        fwrite(data + adr, 1, size, f);
        fclose(f);
        m_savedToDisk = true;
        PCSX::g_system->printf(_("Saving memory card %s\n"), fname);
    } else {
        // try to create it again if we can't open it
        f = fopen(fname, "wb");
        if (f != NULL) {
            fwrite(data, 1, c_cardSize, f);
            fclose(f);
        }
    }
}

void PCSX::MemoryCard::createMcd(PCSX::u8string mcd) {
    if (std::filesystem::path(mcd).is_relative()) {
        mcd = (g_system->getPersistentDir() / mcd).u8string();
    }
    const char *fname = reinterpret_cast<const char *>(mcd.c_str());
    int s = c_cardSize;

    const auto f = fopen(fname, "wb");
    if (f == nullptr) {
        return;
    }

    fputc('M', f);
    s--;
    fputc('C', f);
    s--;
    while (s-- > (c_cardSize - 127)) fputc(0, f);
    fputc(0xe, f);
    s--;

    for (int i = 0; i < 15; i++) {  // 15 blocks
        fputc(0xa0, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        for (int j = 0; j < 117; j++) {
            fputc(0x00, f);
            s--;
        }
        fputc(0xa0, f);
        s--;
    }

    for (int i = 0; i < 20; i++) {
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        for (int j = 0; j < 118; j++) {
            fputc(0x00, f);
            s--;
        }
    }

    while ((s--) >= 0) fputc(0, f);

    fclose(f);
}
