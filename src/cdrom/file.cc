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

#include "cdrom/file.h"

#include <magic_enum/magic_enum_all.hpp>

#include "cdrom/cdriso.h"
#include "iec-60908b/edcecc.h"

PCSX::CDRIsoFile::CDRIsoFile(std::shared_ptr<CDRIso> iso, uint32_t lba, int32_t size, IEC60908b::SectorMode mode)
    : File(RW_SEEKABLE), m_iso(iso), m_lba(lba) {
    uint8_t* sector = m_cachedSector;
    if (iso->failed()) {
        m_failed = true;
        return;
    }
    if (mode == IEC60908b::SectorMode::GUESS) {
        mode = IEC60908b::SectorMode::RAW;
        do {
            m_cachedLBA = lba;
            iso->readSectors(lba, sector, 1);
            if (memcmp(sector, IEC60908b::SYNC, sizeof(IEC60908b::SYNC)) != 0) {
                break;
            }
            uint8_t bcd[3];
            IEC60908b::MSF(lba + 150).toBCD(bcd);
            if ((sector[12] != bcd[0]) || (sector[13] != bcd[1]) || (sector[14] != bcd[2])) {
                break;
            }
            switch (sector[15]) {
                case 1:
                    mode = IEC60908b::SectorMode::M1;
                    break;
                case 2: {
                    uint8_t* subheaders = sector + 16;
                    if ((subheaders[0] != subheaders[4]) || (subheaders[1] != subheaders[5]) ||
                        (subheaders[2] != subheaders[6]) || (subheaders[3] != subheaders[7])) {
                        break;
                    }
                    if (subheaders[2] & 32) {
                        mode = IEC60908b::SectorMode::M2_FORM2;
                    } else {
                        mode = IEC60908b::SectorMode::M2_FORM1;
                    }
                    break;
                }
            }
        } while (0);
    }

    m_mode = mode;

    if (size >= 0) {
        m_size = size;
        return;
    }

    if ((mode != IEC60908b::SectorMode::M2_FORM1) && (mode != IEC60908b::SectorMode::M2_FORM2)) {
        // can't detect file size on non-mode2 sectors
        m_failed = true;
        return;
    }

    size = 1;

    while (true) {
        if (m_cachedLBA != lba) {
            m_cachedLBA = lba;
            iso->readSectors(lba, sector, 1);
        }
        uint8_t* subheaders = sector + 16;
        if (subheaders[2] & 0x81) break;
        lba++;
        size++;
    }

    m_size = size * c_sectorSizes[magic_enum::enum_integer(mode)];
}

ssize_t PCSX::CDRIsoFile::rSeek(ssize_t pos, int wheel) {
    if (m_failed) return -1;
    switch (wheel) {
        case SEEK_SET:
            m_ptrR = pos;
            break;

        case SEEK_CUR:
            m_ptrR += pos;
            break;

        case SEEK_END:
            m_ptrR = m_size + pos;
            break;
    }
    if (m_ptrR < 0) m_ptrR = 0;

    if (m_ptrR > m_size) m_ptrR = m_size;

    return m_ptrR;
}

ssize_t PCSX::CDRIsoFile::wSeek(ssize_t pos, int wheel) {
    if (m_failed) return -1;
    switch (wheel) {
        case SEEK_SET:
            m_ptrW = pos;
            break;

        case SEEK_CUR:
            m_ptrW += pos;
            break;

        case SEEK_END:
            m_ptrW = m_size + pos;
            break;
    }
    if (m_ptrW < 0) m_ptrW = 0;

    if (m_ptrW > m_size) m_size = m_ptrW;

    return m_ptrW;
}

ssize_t PCSX::CDRIsoFile::read(void* buffer_, size_t size) {
    uint8_t* buffer = static_cast<uint8_t*>(buffer_);
    if (m_failed) return -1;
    if (m_ptrR >= m_size) return 0;
    if (m_ptrR + size > m_size) size = m_size - m_ptrR;

    auto modeIndex = magic_enum::enum_integer(m_mode);
    auto sectorSize = c_sectorSizes[modeIndex];

    uint32_t sectorOffset = m_ptrR % sectorSize;
    uint32_t toCopy = size;

    size_t actualSize = 0;
    uint32_t lba = m_lba + m_ptrR / sectorSize;

    while (toCopy != 0) {
        if (m_cachedLBA != lba) {
            m_cachedLBA = lba;
            auto res = m_iso->readSectors(lba, m_cachedSector, 1);
            if (res != 1) return -1;
        }
        size_t blocSize = std::min(toCopy, c_sectorSizes[modeIndex] - sectorOffset);
        memcpy(buffer + actualSize, m_cachedSector + c_sectorOffsets[modeIndex] + sectorOffset, blocSize);
        lba++;
        sectorOffset = 0;
        actualSize += blocSize;
        toCopy -= blocSize;
    }

    m_ptrR += actualSize;
    return actualSize;
}

ssize_t PCSX::CDRIsoFile::write(const void* buffer_, size_t size) {
    const char* buffer = static_cast<const char*>(buffer_);
    if (m_failed) return -1;
    if (m_ptrW + size > m_size) m_size = m_ptrW + size;
    auto ppf = m_iso->getPPF();

    auto modeIndex = magic_enum::enum_integer(m_mode);
    auto sectorSize = c_sectorSizes[modeIndex];

    uint32_t sectorOffset = m_ptrW % sectorSize;
    uint32_t toCopy = size;

    size_t actualSize = 0;
    uint32_t lba = m_lba + m_ptrW / sectorSize;
    IEC60908b::MSF msf(lba + 150);

    while (toCopy != 0) {
        if (m_cachedLBA != lba) {
            m_cachedLBA = lba;
            auto res = m_iso->readSectors(lba, m_cachedSector, 1);
            if (res != 1) return -1;
        }
        uint8_t patched[2352];
        memcpy(patched, m_cachedSector, 2352);
        size_t blocSize = std::min(toCopy, c_sectorSizes[modeIndex] - sectorOffset);
        memcpy(patched + c_sectorOffsets[modeIndex] + sectorOffset, buffer + actualSize, blocSize);
        switch (m_mode) {
            case IEC60908b::SectorMode::M2_FORM1:
            case IEC60908b::SectorMode::M2_FORM2:
                compute_edcecc(patched);
                break;
        }
        ppf->calculatePatch(m_cachedSector, patched, msf);
        msf++;
        lba++;
        sectorOffset = 0;
        actualSize += blocSize;
        toCopy -= blocSize;
    }

    m_cachedLBA = -1;
    m_ptrW += actualSize;
    return actualSize;
}
