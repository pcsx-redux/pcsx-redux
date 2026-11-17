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

#include "cdrom/cdriso.h"
#include "core/cdrom.h"

static constexpr unsigned ECM_HEADER_SIZE = 4;

/* Adapted from ecm.c:unecmify() (C) Neill Corlett */
ssize_t PCSX::CDRIso::ecmDecode(IO<File> f, unsigned int base, void *dest, int sector) {
    uint32_t b = 0, writebytecount = 0, num;
    uint32_t sectorcount = 0;
    int8_t type = 0;  // mode type 0 (META) or 1, 2 or 3 for CDROM type
    uint8_t sector_buffer[PCSX::IEC60908b::FRAMESIZE_RAW];
    // this flag tells if to decode all sectors or just skip to wanted sector
    bool processsectors = (bool)m_decoded_ecm_sectors;
    ECMFILELUT *pos = &(m_ecm_savetable[0]);  // points always to beginning of ECM DATA

    // If not pointing to ECM file but CDDA file or some other track
    if (f != m_cdHandle) {
        // printf("BASETR %i %i\n", base, sector);
        return (*this.*m_cdimg_read_func_o)(f, base, dest, sector);
    }
    // When sector exists in decoded ECM file buffer
    else if (m_decoded_ecm_sectors && sector < m_decoded_ecm_sectors) {
        // printf("ReadSector %i %i\n", sector, savedsectors);
        return (*this.*m_cdimg_read_func_o)(m_decoded_ecm, base, dest, sector);
    }
    // To prevent invalid seek
    /* else if (sector > m_len_ecm_savetable) {
            PCSX::g_system->printf("ECM: invalid sector requested\n");
            return -1;
    }*/
    // printf("SeekSector %i %i %i %i\n", sector, pos->sector, m_prevsector, base);

    if (sector <= m_len_ecm_savetable) {
        // get sector from LUT which points to wanted sector or close to
        // TODO: What would be optimal maximum to search near sector?
        //       Might cause slowdown if too small but too big also..
        for (sectorcount = sector; ((sectorcount > 0) && ((sector - sectorcount) <= 50000)); sectorcount--) {
            if (m_ecm_savetable[sectorcount].filepos >= ECM_HEADER_SIZE) {
                pos = &(m_ecm_savetable[sectorcount]);
                // printf("LUTSector %i %i %i %i\n", sector, pos->sector, m_prevsector, base);
                break;
            }
        }
        // if suitable sector was not found from LUT use last sector if less than wanted sector
        if (pos->filepos <= ECM_HEADER_SIZE && sector > m_prevsector) pos = &(m_ecm_savetable[m_prevsector]);
    }

    auto reconstructSector = [](uint8_t *sector, int8_t type) {
        auto ref32 = [sector](uint32_t offset) -> uint32_t & { return *reinterpret_cast<uint32_t *>(sector + offset); };
        memcpy(sector, IEC60908b::SYNC, sizeof(IEC60908b::SYNC));

        switch (type) {
            case 1:
                // Mode
                sector[0x00f] = 0x01;
                // Empty
                sector[0x814] = 0x00;
                sector[0x815] = 0x00;
                sector[0x816] = 0x00;
                sector[0x817] = 0x00;
                sector[0x818] = 0x00;
                sector[0x819] = 0x00;
                sector[0x81a] = 0x00;
                sector[0x81b] = 0x00;
                break;
            case 2:
            case 3:
                // Mode
                sector[0x00f] = 0x02;
                // Subheaders
                sector[0x010] = sector[0x014];
                sector[0x011] = sector[0x015];
                sector[0x012] = sector[0x016];
                sector[0x013] = sector[0x017];
                break;
        }

        IEC60908b::computeEDCECC(sector);
    };

    writebytecount = pos->sector * PCSX::IEC60908b::FRAMESIZE_RAW;
    sectorcount = pos->sector;
    if (m_decoded_ecm_sectors) m_decoded_ecm->rSeek(writebytecount, SEEK_SET);  // rewind to last pos
    f->rSeek(/*base+*/ pos->filepos, SEEK_SET);
    while (sector >= sectorcount) {  // decode ecm file until we are past wanted sector
        int c = f->getc();
        int bits = 5;
        if (c == EOF) {
            goto error_in;
        }
        type = c & 3;
        num = (c >> 2) & 0x1F;
        // printf("ECM1 file; count %x\n", c);
        while (c & 0x80) {
            c = f->getc();
            // printf("ECM2 file; count %x\n", c);
            if (c == EOF) {
                goto error_in;
            }
            if ((bits > 31) || ((uint32_t)(c & 0x7F)) >= (((uint32_t)0x80000000LU) >> (bits - 1))) {
                // PCSX::g_system->message(_("Corrupt ECM file; invalid sector count\n"));
                goto error;
            }
            num |= ((uint32_t)(c & 0x7F)) << bits;
            bits += 7;
        }
        if (num == 0xFFFFFFFF) {
            // End indicator
            m_len_decoded_ecm_buffer = writebytecount;
            m_len_ecm_savetable = m_len_decoded_ecm_buffer / PCSX::IEC60908b::FRAMESIZE_RAW;
            break;
        }
        num++;
        while (num) {
            if (!processsectors && sectorcount >= (sector - 1)) {  // ensure that we read the sector we are supposed to
                processsectors = true;
                // printf("Saving at %i\n", sectorcount);
            } else if (processsectors && sectorcount > sector) {
                // printf("Terminating at %i\n", sectorcount);
                break;
            }
            /*printf("Type %i Num %i SeekSector %i ProcessedSectors %i(%i) Bytecount %i Pos %li Write %u\n",
                            type, num, sector, sectorcount, pos->sector, writebytecount, ftell(f),
               processsectors);*/
            switch (type) {
                case 0:  // META
                    b = num;
                    if (b > sizeof(sector_buffer)) {
                        b = sizeof(sector_buffer);
                    }
                    writebytecount += b;
                    if (!processsectors) {
                        f->rSeek(b, SEEK_CUR);
                        break;
                    }  // seek only
                    if (f->read(sector_buffer, b) != b) {
                        goto error_in;
                    }
                    // output_edc = edc_compute(output_edc, sector_buffer, b);
                    if (m_decoded_ecm_sectors &&
                        m_decoded_ecm->write(sector_buffer, b) != b) {  // just seek or write also
                        goto error_out;
                    }
                    break;
                case 1:  // Mode 1
                    b = 1;
                    writebytecount += ECM_SECTOR_SIZE[type];
                    if (f->read(sector_buffer + 0x00C, 0x003) != 0x003) {
                        goto error_in;
                    }
                    if (f->read(sector_buffer + 0x010, 0x800) != 0x800) {
                        goto error_in;
                    }
                    if (!processsectors) break;  // seek only
                    reconstructSector(sector_buffer, type);
                    // output_edc = edc_compute(output_edc, sector_buffer, ECM_SECTOR_SIZE[type]);
                    if (m_decoded_ecm_sectors &&
                        m_decoded_ecm->write(sector_buffer, ECM_SECTOR_SIZE[type]) != ECM_SECTOR_SIZE[type]) {
                        goto error_out;
                    }
                    break;
                case 2:  // Mode 2 (XA), form 1
                    b = 1;
                    writebytecount += ECM_SECTOR_SIZE[type];
                    if (!processsectors) {
                        f->rSeek(0x804, SEEK_CUR);
                        break;
                    }  // seek only
                    if (f->read(sector_buffer + 0x014, 0x804) != 0x804) {
                        goto error_in;
                    }
                    reconstructSector(sector_buffer, type);
                    // output_edc = edc_compute(output_edc, sector_buffer + 0x10, ECM_SECTOR_SIZE[type]);
                    if (m_decoded_ecm_sectors &&
                        m_decoded_ecm->write(sector_buffer + 0x10, ECM_SECTOR_SIZE[type]) != ECM_SECTOR_SIZE[type]) {
                        goto error_out;
                    }
                    break;
                case 3:  // Mode 2 (XA), form 2
                    b = 1;
                    writebytecount += ECM_SECTOR_SIZE[type];
                    if (!processsectors) {
                        f->rSeek(0x918, SEEK_CUR);
                        break;
                    }  // seek only
                    if (f->read(sector_buffer + 0x014, 0x918) != 0x918) {
                        goto error_in;
                    }
                    reconstructSector(sector_buffer, type);
                    // output_edc = edc_compute(output_edc, sector_buffer + 0x10, ECM_SECTOR_SIZE[type]);
                    if (m_decoded_ecm_sectors &&
                        m_decoded_ecm->write(sector_buffer + 0x10, ECM_SECTOR_SIZE[type]) != ECM_SECTOR_SIZE[type]) {
                        goto error_out;
                    }
                    break;
            }
            sectorcount = ((writebytecount / PCSX::IEC60908b::FRAMESIZE_RAW) - 0);
            num -= b;
        }
        if (type && sectorcount > 0 && m_ecm_savetable[sectorcount].filepos <= ECM_HEADER_SIZE) {
            m_ecm_savetable[sectorcount].filepos = f->rTell() /*-base*/;
            m_ecm_savetable[sectorcount].sector = sectorcount;
            // printf("Marked %i at pos %i\n", m_ecm_savetable[sectorcount].sector,
            // m_ecm_savetable[sectorcount].filepos);
        }
    }

    if (m_decoded_ecm_sectors) {
        m_decoded_ecm->rSeek(-1 * PCSX::IEC60908b::FRAMESIZE_RAW, SEEK_CUR);
        num = m_decoded_ecm->read(sector_buffer, PCSX::IEC60908b::FRAMESIZE_RAW);
        m_decoded_ecm_sectors = std::max(m_decoded_ecm_sectors, sectorcount);
    } else {
        num = PCSX::IEC60908b::FRAMESIZE_RAW;
    }

    memcpy(dest, sector_buffer, PCSX::IEC60908b::FRAMESIZE_RAW);
    m_prevsector = sectorcount;
    // printf("OK: Frame decoded %i %i\n", sectorcount-1, writebytecount);
    return num;

error_in:
error:
error_out:
    // memset(dest, 0x0, PCSX::CDRomCD_FRAMESIZE_RAW);
    PCSX::g_system->printf("Error decoding ECM image: WantedSector %i Type %i Base %i Sectors %i(%i) Pos %i(%li)\n",
                           sector, type, base, sectorcount, pos->sector, writebytecount, f->rTell());
    return -1;
}

bool PCSX::CDRIso::handleecm(const char *isoname, IO<File> cdh, int32_t *accurate_length) {
    // Rewind to start and check ECM header and filename suffix validity
    cdh->rSeek(0, SEEK_SET);
    if ((cdh->getc() == 'E') && (cdh->getc() == 'C') && (cdh->getc() == 'M') && (cdh->getc() == 0x00) &&
        (strncmp((isoname + strlen(isoname) - 5), ".ecm", 4))) {
        // Function used to read CD normally
        // TODO: detect if 2048 and use it
        m_cdimg_read_func_o = &CDRIso::cdread_normal;

        // Function used to decode ECM data
        m_cdimg_read_func = &CDRIso::ecmDecode;

        // Last accessed sector
        m_prevsector = 0;

        // Already analyzed during this session, use cached results
        if (m_ecm_file_detected) {
            if (accurate_length) *accurate_length = m_len_ecm_savetable;
            return 0;
        }

        PCSX::g_system->printf(_("\nDetected ECM file with proper header and filename suffix.\n"));

        // Reserve maximum known sector ammount for LUT (80MIN CD)
        m_len_ecm_savetable = 75 * 80 * 60;  // 2*(accurate_length/PCSX::CDRomCD_FRAMESIZE_RAW);

        // Index 0 always points to beginning of ECM data
        m_ecm_savetable = (ECMFILELUT *)calloc(m_len_ecm_savetable, sizeof(ECMFILELUT));  // calloc returns nulled data
        m_ecm_savetable[0].filepos = ECM_HEADER_SIZE;

        if (accurate_length || m_decoded_ecm_sectors) {
            uint8_t tbuf1[PCSX::IEC60908b::FRAMESIZE_RAW];
            m_len_ecm_savetable = 0;             // indicates to cdread_ecm_decode that no lut has been built yet
            ecmDecode(cdh, 0U, tbuf1, INT_MAX);  // builds LUT completely
            if (accurate_length) *accurate_length = m_len_ecm_savetable;
        }

        // Full image decoded? Needs fmemopen()

        m_ecm_file_detected = true;

        return true;
    }
    return false;
}
