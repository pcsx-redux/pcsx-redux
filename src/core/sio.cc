/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include "core/sio.h"

#include <sys/stat.h>

#include <algorithm>
#include <bitset>
#include <stdexcept>

#include "core/memorycard.h"
#include "core/pad.h"
#include "mips-common/util/sjis-fullwidth-ascii.hh"
#include "support/sjis_conv.h"
#include "support/strings-helpers.h"

// clk cycle byte
// 4us * 8bits = (PCSX::g_emulator->m_psxClockSpeed / 1000000) * 32; (linuzappz)
// TODO: add SioModePrescaler
#define SIO_CYCLES (m_regs.baud * 8)

void PCSX::SIO::acknowledge() {
    if (m_regs.control & ControlFlags::TX_ENABLE) {
        if (m_regs.control & ControlFlags::ACK_IRQEN) {
            scheduleInterrupt(SIO_CYCLES);
        }
    }
}

void PCSX::SIO::init() {
    reset();
    togglePocketstationMode();
    g_emulator->m_pads->init();
    g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);
}

bool PCSX::SIO::isReceiveIRQReady() {
    if (m_regs.control & ControlFlags::RX_IRQEN) {
        switch ((m_regs.control & 0x300) >> 8) {
            case 0:
                if (!(m_rxFIFO.size() >= 1)) {
                    return false;
                }
                break;

            case 1:
                if (!(m_rxFIFO.size() >= 2)) {
                    return false;
                }
                break;

            case 2:
                if (!(m_rxFIFO.size() >= 4)) {
                    return false;
                }
                break;

            case 3:
                if (!(m_rxFIFO.size() >= 8)) {
                    return false;
                }
                break;
        }
        return true;
    }

    return false;
}

bool PCSX::SIO::isTransmitReady() {
    const bool txEnabled = m_regs.control & ControlFlags::TX_ENABLE;
    const bool txFinished = m_regs.status & StatusFlags::TX_FINISHED;
    const bool txDataNotEmpty = !(m_regs.status & StatusFlags::TX_DATACLEAR);

    return (txEnabled && txFinished && txDataNotEmpty);
}

void PCSX::SIO::reset() {
    m_rxFIFO.clear();
    m_padState = Pads::PAD_STATE_IDLE;
    m_regs.status = StatusFlags::TX_DATACLEAR | StatusFlags::TX_FINISHED;
    m_regs.mode = 0;
    m_regs.control = 0;
    m_regs.baud = 0;
    m_bufferIndex = 0;
    deselectCards();
    m_currentDevice = DeviceType::None;
}

void PCSX::SIO::deselectCards() {
    for (auto& card : m_memoryCard) card.deselect();
    for (auto& card : m_tapMemoryCard) card.deselect();
}

// Whether the card in a multitap's slot B to D is inserted, and its file.
static std::pair<bool, PCSX::u8string> tapMcdSettings(unsigned index) {
    using namespace PCSX;
    auto& s = g_emulator->settings;
    switch (index) {
        case 0:
            return {s.get<Emulator::SettingMcd1BInserted>(), s.get<Emulator::SettingMcd1B>().string()};
        case 1:
            return {s.get<Emulator::SettingMcd1CInserted>(), s.get<Emulator::SettingMcd1C>().string()};
        case 2:
            return {s.get<Emulator::SettingMcd1DInserted>(), s.get<Emulator::SettingMcd1D>().string()};
        case 3:
            return {s.get<Emulator::SettingMcd2BInserted>(), s.get<Emulator::SettingMcd2B>().string()};
        case 4:
            return {s.get<Emulator::SettingMcd2CInserted>(), s.get<Emulator::SettingMcd2C>().string()};
        default:
            return {s.get<Emulator::SettingMcd2DInserted>(), s.get<Emulator::SettingMcd2D>().string()};
    }
}

void PCSX::SIO::writePad(uint8_t value) {
    switch (m_padState) {
        case Pads::PAD_STATE_IDLE:                          // start pad
            m_regs.status |= StatusFlags::RX_FIFONOTEMPTY;  // Transfer is Ready
            g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);

            switch (m_regs.control & ControlFlags::WHICH_PORT) {
                case SelectedPort::Port1:
                    if (!PCSX::g_emulator->m_pads->startPoll(Pads::Port::Port1, m_padAddress, m_buffer[0])) {
                        m_buffer[0] = 0xff;
                        return;
                    }
                    break;
                case SelectedPort::Port2:
                    if (!PCSX::g_emulator->m_pads->startPoll(Pads::Port::Port2, m_padAddress, m_buffer[0])) {
                        m_buffer[0] = 0xff;
                        return;
                    }
                    break;
            }

            m_maxBufferIndex = 2;
            m_bufferIndex = 0;
            m_padState = Pads::PAD_STATE_READ_COMMAND;
            break;

        case Pads::PAD_STATE_READ_COMMAND:
            m_padState = Pads::PAD_STATE_READ_DATA;
            m_bufferIndex = 1;
            switch (m_regs.control & ControlFlags::WHICH_PORT) {
                case SelectedPort::Port1:
                    m_buffer[m_bufferIndex] = PCSX::g_emulator->m_pads->poll(value, Pads::Port::Port1, m_padState);
                    break;
                case SelectedPort::Port2:
                    m_buffer[m_bufferIndex] = PCSX::g_emulator->m_pads->poll(value, Pads::Port::Port2, m_padState);
                    break;
            }

            if (!(m_buffer[m_bufferIndex] & 0x0f)) {
                m_maxBufferIndex = 2 + 32;
            } else {
                m_maxBufferIndex = 2 + (m_buffer[m_bufferIndex] & 0x0f) * 2;
            }
            break;

        case Pads::PAD_STATE_READ_DATA:
            m_bufferIndex++;
            switch (m_regs.control & ControlFlags::WHICH_PORT) {
                case SelectedPort::Port1:
                    m_buffer[m_bufferIndex] = PCSX::g_emulator->m_pads->poll(value, Pads::Port::Port1, m_padState);
                    break;
                case SelectedPort::Port2:
                    m_buffer[m_bufferIndex] = PCSX::g_emulator->m_pads->poll(value, Pads::Port::Port2, m_padState);
                    break;
            }

            if (m_bufferIndex == m_maxBufferIndex) {
                m_padState = Pads::PAD_STATE_IDLE;
                m_currentDevice = DeviceType::Ignore;
                return;
            }
            break;
    }

    if (m_padState == Pads::PAD_STATE_BAD_COMMAND) {
        return;
    }

    acknowledge();
}

void PCSX::SIO::transmitData() {
    m_regs.status &= ~StatusFlags::TX_FINISHED;
    g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);

    uint8_t m_rxBuffer = 0xff;

    const bool select = m_currentDevice == DeviceType::None;
    const unsigned portIndex = (m_regs.control & ControlFlags::WHICH_PORT) == SelectedPort::Port2 ? 1 : 0;
    if (select) {
        m_currentDevice = m_regs.data;
        m_padAddress = m_regs.data;
        const auto port = portIndex ? Pads::Port::Port2 : Pads::Port::Port1;
        // A multitap also answers 02h to 04h, its slots B to D.
        if (m_currentDevice >= 0x02 && m_currentDevice <= 0x04) {
            if (PCSX::g_emulator->m_pads->hasMultitap(port)) m_currentDevice = DeviceType::PAD;
        }
        // And 82h to 84h, the memory cards in those slots, when one is inserted. 81h stays card 1 or 2.
        if (m_currentDevice >= DeviceType::TapMemoryCardB && m_currentDevice <= DeviceType::TapMemoryCardD) {
            const unsigned index = portIndex * 3 + m_currentDevice - DeviceType::TapMemoryCardB;
            if (!PCSX::g_emulator->m_pads->hasMultitap(port) || !tapMcdSettings(index).first) {
                m_tapMemoryCard[index].m_directoryFlag = MemoryCard::Flags::DirectoryUnread;
                // Only the first byte after select is an address: ignore the rest of this one.
                m_currentDevice = DeviceType::Ignore;
            }
        }
    }

    switch (m_currentDevice) {
        case DeviceType::PAD:
            // Pad Process events
            writePad(m_regs.data);
            m_rxBuffer = m_buffer[m_bufferIndex];
            break;

        case DeviceType::MemoryCard:
            switch (m_regs.control & ControlFlags::WHICH_PORT) {
                case SelectedPort::Port1:
                    if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingMcd1Inserted>()) {
                        m_rxBuffer = m_memoryCard[0].transceive(m_regs.data);
                        if (m_memoryCard[0].dataChanged()) {
                            m_memoryCard[0].commit(
                                PCSX::g_emulator->settings.get<PCSX::Emulator::SettingMcd1>().string().c_str());
                        }
                    } else {
                        m_memoryCard[0].m_directoryFlag = MemoryCard::Flags::DirectoryUnread;
                        m_currentDevice = DeviceType::Ignore;
                        m_memoryCard[0].deselect();
                    }
                    break;

                case SelectedPort::Port2:
                    if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingMcd2Inserted>()) {
                        m_rxBuffer = m_memoryCard[1].transceive(m_regs.data);
                        if (m_memoryCard[1].dataChanged()) {
                            m_memoryCard[1].commit(
                                PCSX::g_emulator->settings.get<PCSX::Emulator::SettingMcd2>().string().c_str());
                        }
                    } else {
                        m_memoryCard[1].m_directoryFlag = MemoryCard::Flags::DirectoryUnread;
                        m_currentDevice = DeviceType::Ignore;
                        m_memoryCard[1].deselect();
                    }
                    break;
            }
            break;

        case DeviceType::TapMemoryCardB:
        case DeviceType::TapMemoryCardC:
        case DeviceType::TapMemoryCardD: {
            // Checked on every byte, not only on the select one: the port can switch under a transfer, and a
            // save state can resume one in a process that has not read this card's file yet.
            const unsigned index = portIndex * 3 + m_currentDevice - DeviceType::TapMemoryCardB;
            const auto port = portIndex ? Pads::Port::Port2 : Pads::Port::Port1;
            const auto [inserted, path] = tapMcdSettings(index);
            auto &card = m_tapMemoryCard[index];
            if (!PCSX::g_emulator->m_pads->hasMultitap(port) || !inserted) {
                card.m_directoryFlag = MemoryCard::Flags::DirectoryUnread;
                m_currentDevice = DeviceType::Ignore;
                card.deselect();
                break;
            }
            if (!m_tapMemoryCardLoaded[index]) {
                // loadMcd resets the directory flag; keep the one a save state may have restored.
                const uint8_t directoryFlag = card.m_directoryFlag;
                card.loadMcd(path);
                card.m_directoryFlag = directoryFlag;
                m_tapMemoryCardLoaded[index] = true;
            }
            // The card itself only knows 81h.
            m_rxBuffer = card.transceive(select ? DeviceType::MemoryCard : m_regs.data);
            if (card.dataChanged()) card.commit(path);
            break;
        }

        case DeviceType::Ignore:
            break;

        default:
            m_currentDevice = DeviceType::None;
            m_padState = Pads::PAD_STATE_IDLE;
            deselectCards();
            break;
    }

    m_rxFIFO.push(m_rxBuffer);
    updateFIFOStatus();
    m_regs.data = m_rxBuffer;
    g_emulator->m_mem->writeHardwareRegister<0x1040, uint8_t>(m_rxBuffer);

    if (isReceiveIRQReady() && !(m_regs.status & StatusFlags::IRQ)) {
        scheduleInterrupt(SIO_CYCLES);
    }
    m_regs.status |= StatusFlags::TX_DATACLEAR | StatusFlags::TX_FINISHED;
    g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);
}

void PCSX::SIO::write8(uint8_t value) {
    SIO0_LOG("sio write8 %x (PAR:%x PAD:%x)\n", value, m_bufferIndex, m_padState);

    m_regs.data = value;
    m_regs.status &= ~StatusFlags::TX_DATACLEAR;
    g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);

    if (isTransmitReady()) {
        transmitData();
    }
}

void PCSX::SIO::writeStatus16(uint16_t value) {}

void PCSX::SIO::writeMode16(uint16_t value) { m_regs.mode = value; }

void PCSX::SIO::writeCtrl16(uint16_t value) {
    const bool deselected = (m_regs.control & ControlFlags::SELECT_ENABLE) && (!(value & ControlFlags::SELECT_ENABLE));
    const bool selected = (!(m_regs.control & ControlFlags::SELECT_ENABLE)) && (value & ControlFlags::SELECT_ENABLE);
    const bool portChanged = (m_regs.control & ControlFlags::WHICH_PORT) && (!(value & ControlFlags::WHICH_PORT));
    const bool wasReady = isTransmitReady();

    m_regs.control = value;

    SIO0_LOG("sio ctrlwrite16 %x (PAR:%x PAD:%x)\n", value, m_bufferIndex, m_padState);

    if (selected && (m_regs.control & ControlFlags::TX_IRQEN) && !(m_regs.status & StatusFlags::IRQ)) {
        scheduleInterrupt(SIO_CYCLES);
    }

    if (deselected || portChanged) {
        // Select line de-activated, reset state machines
        m_currentDevice = DeviceType::None;
        m_padState = Pads::PAD_STATE_IDLE;
        deselectCards();
        m_bufferIndex = 0;
    }

    if (m_regs.control & ControlFlags::RESET_ERR) {
        m_regs.status &= ~(StatusFlags::RX_PARITYERR | StatusFlags::IRQ);
        m_regs.control &= ~ControlFlags::RESET_ERR;

        if (isReceiveIRQReady()) {
            m_regs.status |= StatusFlags::IRQ;
            g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);
        }
    }

    if (m_regs.control & ControlFlags::RESET) {
        m_rxFIFO.clear();
        m_padState = Pads::PAD_STATE_IDLE;
        deselectCards();
        m_bufferIndex = 0;
        m_regs.status = StatusFlags::TX_DATACLEAR | StatusFlags::TX_FINISHED;
        g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);
        g_emulator->m_cpu->unschedule(Schedule::SIO);
        m_regs.mode = 0;
        m_regs.baud = 0;
        m_regs.control = 0;
        m_currentDevice = DeviceType::None;
    }

    updateFIFOStatus();

    if (m_regs.control & ControlFlags::TX_IRQEN) {
        m_regs.status |= StatusFlags::IRQ;
        g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);
    }

    if (wasReady == false && isTransmitReady()) {
        transmitData();
    }
}

void PCSX::SIO::writeBaud16(uint16_t value) { m_regs.baud = value; }

uint8_t PCSX::SIO::read8() {
    uint8_t ret = 0xFF;

    if ((m_regs.status & StatusFlags::RX_FIFONOTEMPTY) && !m_rxFIFO.isEmpty()) {
        ret = m_rxFIFO.pull();
        updateFIFOStatus();
    }

    SIO0_LOG("sio read8 ;ret = %x (I:%x ST:%x BUF:(%x %x %x))\n", ret, m_bufferIndex, m_regs.status,
             m_buffer[m_bufferIndex > 0 ? m_bufferIndex - 1 : 0], m_buffer[m_bufferIndex],
             m_buffer[m_bufferIndex < c_padBufferSize - 1 ? m_bufferIndex + 1 : c_padBufferSize - 1]);

    g_emulator->m_mem->writeHardwareRegister<0x1040, uint8_t>(ret);

    return ret;
}

uint16_t PCSX::SIO::readStatus16() {
    uint16_t hard = m_regs.status;

    return hard;
}

void PCSX::SIO::scheduledCallback() {
    SIO0_LOG("Sio Interrupt (CP0.Status = %x)\n", PCSX::g_emulator->m_cpu->m_regs.CP0.n.Status);
    m_regs.status |= StatusFlags::IRQ;
    g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);
    g_emulator->m_mem->setIRQ(0x80);

#if 0
    // Rhapsody: fixes input problems
    // Twisted Metal 2: breaks intro
    m_statusReg |= StatusFlags::TX_DATACLEAR;
    m_statusReg |= StatusFlags::RX_FIFONOTEMPTY;
#endif
}

void PCSX::SIO::getMcdBlockInfo(int mcd, int block, McdBlock& info) {
    if (block < 1 || block > 15) {
        throw std::runtime_error(_("Wrong block number"));
    }

    uint16_t clut[16];

    info.reset();
    info.number = block;
    info.mcd = mcd;

    char* data = getMcdData(mcd);
    uint8_t* ptr = reinterpret_cast<uint8_t*>(data) + block * c_blockSize + 2;
    auto& ta = info.titleAscii;
    auto& ts = info.titleSjis;
    info.iconCount = std::max(1, *ptr & 0x3);

    ptr += 2;
    int x = 0;

    for (int i = 0; i < 48; i++) {
        uint8_t b = *ptr++;
        ts += b;
        uint16_t c = b;
        if (b & 0x80) {
            c <<= 8;
            b = *ptr++;
            ts += b;
            c |= b;
        }

        bool found = false;
        for (int s = 0; s < sizeof(::Sjis::c_fullwidthAsciiToSjis) / sizeof(::Sjis::c_fullwidthAsciiToSjis[0]); s++) {
            if (c == ::Sjis::c_fullwidthAsciiToSjis[s]) {
                c = s + 0x20;
                found = true;
                break;
            }
        }

        ta += found ? c : '?';
    }

    info.titleUtf8 = Sjis::toUtf8(ts);

    // Read CLUT
    ptr = reinterpret_cast<uint8_t*>(data) + block * c_blockSize + 0x60;
    std::memcpy(clut, ptr, 16 * sizeof(uint16_t));

    // Icons can have 1 to 3 frames of animation
    for (uint32_t i = 0; i < info.iconCount; i++) {
        uint16_t* icon = &info.icon[i * 16 * 16];
        ptr = reinterpret_cast<uint8_t*>(data) + block * c_blockSize + 128 + 128 * i;  // icon data

        // Fetch each pixel, store it in the icon array in ABBBBBGGGGGRRRRR with the alpha bit set to 1
        for (x = 0; x < 16 * 16; x++) {
            const uint8_t entry = (uint8_t)*ptr;
            icon[x++] = clut[entry & 0xf] | (1 << 15);
            icon[x] = clut[entry >> 4] | (1 << 15);
            ptr++;
        }
    }

    // Parse directory frame info
    const auto directoryFrame = (uint8_t*)data + block * c_sectorSize;
    uint32_t allocState = 0;
    allocState |= directoryFrame[0];
    allocState |= directoryFrame[1] << 8;
    allocState |= directoryFrame[2] << 16;
    allocState |= directoryFrame[3] << 24;
    info.allocState = allocState;

    char tmp[17];
    memset(tmp, 0, sizeof(tmp));
    std::strncpy(tmp, (const char*)&directoryFrame[0xa], 12);
    info.id = tmp;
    memset(tmp, 0, sizeof(tmp));
    std::strncpy(tmp, (const char*)&directoryFrame[0x16], 16);
    info.name = tmp;

    uint32_t fileSize = 0;
    fileSize |= directoryFrame[4];
    fileSize |= directoryFrame[5] << 8;
    fileSize |= directoryFrame[6] << 16;
    fileSize |= directoryFrame[7] << 24;
    info.fileSize = fileSize;

    uint16_t nextBlock = 0;
    nextBlock |= directoryFrame[8];
    nextBlock |= directoryFrame[9] << 8;
    info.nextBlock = nextBlock == 0xffff ? -1 : (nextBlock + 1);

    // Check if the block is marked as free in the directory frame and adjust the name/filename if so
    if (info.isErased()) {
        info.reset();
        info.allocState = 0xa0;
        info.titleAscii = "Free Block";
        info.titleSjis = "Free Block";
        info.titleUtf8 = "Free Block";
    }
}

char* PCSX::SIO::getMcdData(int mcd) {
    switch (mcd) {
        case 1:
            return m_memoryCard[0].getMcdData();
        case 2:
            return m_memoryCard[1].getMcdData();
        default:
            throw std::runtime_error("Attempt to access invalid memory card");
            return nullptr;
            break;
    }
}

// Erase a memory card block by clearing it with 0s
// mcd: The memory card we want to use (1 or 2)
void PCSX::SIO::eraseMcdFile(const McdBlock& block) {
    char* data = getMcdData(block.mcd);

    // Set the block data to 0
    const size_t offset = block.number * c_blockSize;
    std::memset(data + offset, 0, c_blockSize);

    // Fix up the corresponding directory frame in block 0.
    const auto frame = (uint8_t*)data + block.number * c_sectorSize;
    frame[0] = 0xa0;                   // Code for a freshly formatted block
    for (auto i = 1; i < 0x7f; i++) {  // Zero the rest of the frame
        frame[i] = 0;
    }
    frame[0x7f] = 0xa0;  // xor checksum of frame

    if (block.isErased()) {
        return;
    }

    auto nextBlock = block.nextBlock;
    if ((nextBlock >= 1) && (nextBlock <= 15)) {
        McdBlock next;
        getMcdBlockInfo(block.mcd, nextBlock, next);
        eraseMcdFile(next);
    }
}

unsigned PCSX::SIO::getFreeSpace(int mcd) {
    unsigned count = 0;
    for (int i = 1; i < 16; i++) {
        McdBlock block;
        getMcdBlockInfo(mcd, i, block);
        if (block.isErased()) {
            count++;
        }
    }

    return count;
}

unsigned PCSX::SIO::getFileBlockCount(McdBlock block) {
    if (block.isErased()) {
        return 0;
    }

    std::bitset<16> walked;
    unsigned count = 1;

    while (true) {
        if ((block.nextBlock < 1) || (block.nextBlock > 15)) {
            return count;
        }
        if (walked.test(block.nextBlock)) {
            return count;
        }
        walked.set(block.nextBlock);
        getMcdBlockInfo(block.mcd, block.nextBlock, block);
        count++;
    }
}

int PCSX::SIO::findFirstFree(int mcd) {
    McdBlock block;
    for (int i = 1; i < 16; i++) {
        getMcdBlockInfo(mcd, i, block);
        if (block.isErased()) {
            return i;
        }
    }

    return -1;
}

bool PCSX::SIO::copyMcdFile(McdBlock block) {
    auto other = otherMcd(block);
    if (getFreeSpace(other) < getFileBlockCount(block)) {
        return false;
    }
    const auto data = getMcdData(block);
    const auto otherData = getMcdData(other);

    std::bitset<16> walked;
    int prevBlock = -1;

    while (true) {
        int dstBlock = findFirstFree(other);
        if (dstBlock < 1 || dstBlock > 16) {
            throw std::runtime_error("Inconsistent memory card state");
        }

        // copy block data
        size_t srcOffset = block.number * c_blockSize;
        size_t dstOffset = dstBlock * c_blockSize;
        std::memcpy(otherData + dstOffset, data + srcOffset, c_blockSize);

        // copy directory entry
        srcOffset = block.number * c_sectorSize;
        dstOffset = dstBlock * c_sectorSize;
        std::memcpy(otherData + dstOffset, data + srcOffset, c_sectorSize);

        // Fix up the corresponding directory frame in block 0.
        if (prevBlock != -1) {
            const auto frame = reinterpret_cast<uint8_t*>(otherData) + prevBlock * c_sectorSize;
            uint8_t crcFix = frame[8] ^ (dstBlock - 1);
            frame[8] = dstBlock - 1;
            frame[0x7f] ^= crcFix;
        }
        prevBlock = dstBlock;
        if (block.nextBlock == -1) {
            return true;
        }
        if ((block.nextBlock < 1) || (block.nextBlock > 15)) {
            return false;
        }
        if (walked.test(block.nextBlock)) {
            return false;
        }
        walked.set(block.nextBlock);
        getMcdBlockInfo(block.mcd, block.nextBlock, block);
    }
}

// Back up the entire memory card to a file
// mcd: The memory card to back up (1 or 2)
void PCSX::SIO::saveMcd(int mcd) {
    switch (mcd) {
        case 1: {
            const auto path = g_emulator->settings.get<Emulator::SettingMcd1>().string();
            m_memoryCard[0].saveMcd(path);
            break;
        }
        case 2: {
            const auto path = g_emulator->settings.get<Emulator::SettingMcd2>().string();
            m_memoryCard[1].saveMcd(path);
            break;
        }
    }
}

void PCSX::SIO::togglePocketstationMode() {
    if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingMcd1Pocketstation>()) {
        m_memoryCard[0].enablePocketstation();
    } else {
        m_memoryCard[0].disablePocketstation();
    }

    if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingMcd2Pocketstation>()) {
        m_memoryCard[1].enablePocketstation();
    } else {
        m_memoryCard[1].disablePocketstation();
    }
}

void PCSX::SIO::updateFIFOStatus() {
    if (m_rxFIFO.size() > 0) {
        m_regs.status |= StatusFlags::RX_FIFONOTEMPTY;
    } else {
        m_regs.status &= ~StatusFlags::RX_FIFONOTEMPTY;
    }
    g_emulator->m_mem->writeHardwareRegister<0x1044>(m_regs.status);
}
