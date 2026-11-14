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

#include "support/uvfile.h"
#include "supportpsx/iec-60908b.h"

////////////////////////////////////////////////////////////////////////////////
//
// Sector types
//
// Mode 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 01
// 0010h [---DATA...
// ...
// 0800h                                     ...DATA---]
// 0810h [---EDC---] 00 00 00 00 00 00 00 00 [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0810h             ...DATA---] [---EDC---] [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 2
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0920h                         ...DATA---] [---EDC---]
// -----------------------------------------------------
//
// ADDR:  Sector address, encoded as minutes:seconds:frames in BCD
// FLAGS: Used in Mode 2 (XA) sectors describing the type of sector; repeated
//        twice for redundancy
// DATA:  Area of the sector which contains the actual data itself
// EDC:   Error Detection Code
// ECC:   Error Correction Code
//

PCSX::CDRIso::CDRIso() {
    m_zstr.next_in = Z_NULL;
    m_zstr.avail_in = 0;
    m_zstr.zalloc = Z_NULL;
    m_zstr.zfree = Z_NULL;
    m_zstr.opaque = Z_NULL;
    auto ret = inflateInit2(&m_zstr, -15);
    if (ret != Z_OK) throw("Unable to initialize zlib context");
}

// this function tries to get the .sub file of the given .img
bool PCSX::CDRIso::opensubfile(const char *isoname) {
    char subname[MAXPATHLEN];

    // copy name of the iso and change extension from .img to .sub
    strncpy(subname, isoname, sizeof(subname));
    subname[MAXPATHLEN - 1] = '\0';

    if (strlen(subname) >= 4) {
        strcpy(subname + strlen(subname) - 4, ".sub");
    }

    m_subHandle.setFile(new UvFile(subname));
    if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
        m_subHandle.asA<UvFile>()->startCaching();
    }
    if (!m_subHandle->failed()) return true;
    m_subHandle.reset();

    if (strlen(subname) >= 8) {
        strcpy(subname + strlen(subname) - 8, ".sub");
    }

    m_subHandle.setFile(new UvFile(subname));
    if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
        m_subHandle.asA<UvFile>()->startCaching();
    }
    if (m_subHandle->failed()) {
        m_subHandle.reset();
        return false;
    }

    return true;
}

ssize_t PCSX::CDRIso::cdread_normal(IO<File> f, unsigned int base, void *dest, int sector) {
    return f->readAt(dest, IEC60908b::FRAMESIZE_RAW, base + sector * IEC60908b::FRAMESIZE_RAW);
}

ssize_t PCSX::CDRIso::cdread_sub_mixed(IO<File> f, unsigned int base, void *dest, int sector) {
    int ret;

    ret = f->readAt(dest, IEC60908b::FRAMESIZE_RAW,
                    base + sector * (IEC60908b::FRAMESIZE_RAW + IEC60908b::SUB_FRAMESIZE));
    f->readAt(m_subbuffer.raw, IEC60908b::SUB_FRAMESIZE,
              base + sector * (IEC60908b::FRAMESIZE_RAW + IEC60908b::SUB_FRAMESIZE) + IEC60908b::FRAMESIZE_RAW);

    if (m_subChanRaw) decodeRawSubData();

    return ret;
}

static int uncompress2_internal(void *out, unsigned long *out_size, void *in, unsigned long in_size, z_stream *z) {
    int ret = 0;

    ret = inflateReset(z);
    if (ret != Z_OK) return ret;

    z->next_in = reinterpret_cast<Bytef *>(in);
    z->avail_in = in_size;
    z->next_out = reinterpret_cast<Bytef *>(out);
    z->avail_out = *out_size;

    ret = inflate(z, Z_NO_FLUSH);

    *out_size -= z->avail_out;
    return ret == 1 ? 0 : ret;
}

ssize_t PCSX::CDRIso::cdread_compressed(IO<File> f, unsigned int base, void *dest, int sector) {
    unsigned long cdbuffer_size, cdbuffer_size_expect;
    unsigned int start_byte, size;
    int is_compressed;
    int ret, block;

    if (base) sector += base / 2352;

    block = sector >> m_compr_img->block_shift;
    m_compr_img->sector_in_blk = sector & ((1 << m_compr_img->block_shift) - 1);

    if (block == m_compr_img->current_block) {
        // printf("hit sect %d\n", sector);
        goto finish;
    }

    if (sector >= m_compr_img->index_len * 16) {
        PCSX::g_system->printf("sector %d is past img end\n", sector);
        return -1;
    }

    start_byte = m_compr_img->index_table[block] & 0x7fffffff;
    if (m_cdHandle->rSeek(start_byte, SEEK_SET) != 0) {
        PCSX::g_system->printf("seek error for block %d at %x: ", block, start_byte);
        perror(NULL);
        return -1;
    }

    is_compressed = !(m_compr_img->index_table[block] & 0x80000000);
    size = (m_compr_img->index_table[block + 1] & 0x7fffffff) - start_byte;
    if (size > sizeof(m_compr_img->buff_compressed)) {
        PCSX::g_system->printf("block %d is too large: %u\n", block, size);
        return -1;
    }

    if (m_cdHandle->read(is_compressed ? m_compr_img->buff_compressed : m_compr_img->buff_raw[0], size) != size) {
        PCSX::g_system->printf("read error for block %d at %x: ", block, start_byte);
        perror(NULL);
        return -1;
    }

    if (is_compressed) {
        cdbuffer_size_expect = sizeof(m_compr_img->buff_raw[0]) << m_compr_img->block_shift;
        cdbuffer_size = cdbuffer_size_expect;
        ret =
            uncompress2_internal(m_compr_img->buff_raw[0], &cdbuffer_size, m_compr_img->buff_compressed, size, &m_zstr);
        if (ret != 0) {
            PCSX::g_system->printf("uncompress failed with %d for block %d, sector %d\n", ret, block, sector);
            return -1;
        }
        if (cdbuffer_size != cdbuffer_size_expect)
            PCSX::g_system->printf("cdbuffer_size: %lu != %lu, sector %d\n", cdbuffer_size, cdbuffer_size_expect,
                                   sector);
    }

    // done at last!
    m_compr_img->current_block = block;

finish:
    if (dest != m_cdbuffer)  // copy avoid HACK
        memcpy(dest, m_compr_img->buff_raw[m_compr_img->sector_in_blk], IEC60908b::FRAMESIZE_RAW);
    return IEC60908b::FRAMESIZE_RAW;
}

ssize_t PCSX::CDRIso::cdread_2048(IO<File> f, unsigned int base, void *dest_, int sector) {
    uint8_t *dest = reinterpret_cast<uint8_t *>(dest_);
    int ret;

    ret = f->readAt(dest + 12 + 4 + 8, 2048, base + sector * 2048);

    memcpy(dest, IEC60908b::SYNC, sizeof(IEC60908b::SYNC));
    IEC60908b::MSF(sector + 150).toBCD(dest + 12);
    dest[15] = 2;
    dest[16] = dest[20] = 0;
    dest[17] = dest[21] = 0;
    dest[18] = dest[22] = 8;
    dest[19] = dest[23] = 0;

    IEC60908b::computeEDCECC(dest);

    return ret;
}

uint8_t *PCSX::CDRIso::getBuffer() {
    if (m_useCompressed) {
        return m_compr_img->buff_raw[m_compr_img->sector_in_blk] + 12;
    } else {
        return m_cdbuffer + 12;
    }
}

void PCSX::CDRIso::printTracks() {
    for (int i = 1; i <= m_numtracks; i++) {
        PCSX::g_system->printf(_("Track %.2d (%s) - Start %.2d:%.2d:%.2d, Length %.2d:%.2d:%.2d\n"), i,
                               (m_ti[i].type == TrackType::DATA        ? "DATA"
                                : m_ti[i].cddatype == trackinfo::CCDDA ? "CZDA"
                                                                       : "CDDA"),
                               m_ti[i].start.m, m_ti[i].start.s, m_ti[i].start.f, m_ti[i].length.m, m_ti[i].length.s,
                               m_ti[i].length.f);
    }
}

bool PCSX::CDRIso::open(IO<File> isoFile) {
    m_cdHandle = isoFile;
    if (m_cdHandle->failed()) {
        m_cdHandle.reset();
        return false;
    }
    if (g_emulator->settings.get<Emulator::SettingFullCaching>() && m_cdHandle.isA<UvFile>()) {
        m_cdHandle.asA<UvFile>()->startCaching();
    }

    PCSX::g_system->printf(_("Loaded CD Image: %s"), m_isoPath.string());

    m_cddaBigEndian = false;
    m_subChanMixed = false;
    m_subChanRaw = false;
    m_pregapOffset = 0;
    m_cdrIsoMultidiskCount = 1;
    m_multifile = false;

    m_useCompressed = false;
    m_cdimg_read_func = &CDRIso::cdread_normal;

    for (auto &i : m_ti) {
        i = {};
    }

    if (parsecue(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+cue]");
    } else if (parsetoc(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+toc]");
    } else if (parseccd(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+ccd]");
    } else if (parsemds(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+mds]");
    }
    // TODO Is it possible that cue/ccd+ecm? otherwise use else if below to supressn extra checks
    if (handlepbp(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[pbp]");
        m_useCompressed = true;
        m_cdimg_read_func = &CDRIso::cdread_compressed;
    } else if (handlecbin(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[cbin]");
        m_useCompressed = true;
        m_cdimg_read_func = &CDRIso::cdread_compressed;
    } else if ((handleecm(reinterpret_cast<const char *>(m_isoPath.string().c_str()), m_cdHandle, NULL))) {
        PCSX::g_system->printf("[+ecm]");
    }

    if (!m_subChanMixed && opensubfile(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+sub]");
    }
    if (opensbifile(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+sbi]");
    }

    if (!m_ecm_file_detected) {
        // guess whether it is mode1/2048
        if (m_cdHandle->size() % 2048 == 0) {
            unsigned int modeTest = m_cdHandle->readAt<uint32_t>(0);
            if (modeTest != 0xffffff00) {
                PCSX::g_system->printf("[2048]");
                m_isMode1ISO = true;
            }
        }
        m_cdHandle->rSeek(0, SEEK_SET);
    }

    if (m_numtracks == 0) {
        // We got no track information, just an iso file, so let's fill in very basic data
        m_numtracks = 1;
        m_ti[1].type = TrackType::DATA;
        m_ti[1].start = IEC60908b::MSF(0, 2, 0);
        m_ti[1].pregap = IEC60908b::MSF(0, 0, 0);
        m_ti[1].handle = m_cdHandle;
        m_ti[1].length = IEC60908b::MSF(m_ti[1].handle->size() / (m_isMode1ISO ? 2048 : 2352));
    }

    if (m_ppf.load(m_isoPath)) {
        PCSX::g_system->printf("[+ppf]");
    }
    PCSX::g_system->printf(".\n");

    printTracks();

    if (m_subChanMixed && (m_cdimg_read_func == &CDRIso::cdread_normal)) {
        m_cdimg_read_func = &CDRIso::cdread_sub_mixed;
    } else if (m_isMode1ISO && (m_cdimg_read_func == &CDRIso::cdread_normal)) {
        m_cdimg_read_func = &CDRIso::cdread_2048;
    }

    // make sure we have another handle open for cdda
    if (m_numtracks > 1 && !m_ti[1].handle) {
        m_ti[1].handle.setFile(new UvFile(m_isoPath));
        if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
            m_ti[1].handle.asA<UvFile>()->startCaching();
        }
    }

    return true;
}

void PCSX::CDRIso::close() {
    m_cdHandle.reset();
    m_subHandle.reset();

    if (m_compr_img) {
        free(m_compr_img->index_table);
        free(m_compr_img);
        m_compr_img = nullptr;
    }

    for (int i = 1; i <= m_numtracks; i++) {
        if (m_ti[i].handle) {
            m_ti[i].handle.reset();
            m_ti[i].cddatype = trackinfo::NONE;
        }
    }
    m_numtracks = 0;
    m_ti[1].type = TrackType::CLOSED;

    memset(m_cdbuffer, 0, sizeof(m_cdbuffer));
    m_useCompressed = false;
    // ECM LUT
    free(m_ecm_savetable);
    m_ecm_savetable = nullptr;

    if (m_decoded_ecm) {
        m_decoded_ecm.reset();
        free(m_decoded_ecm_buffer);
        m_decoded_ecm_buffer = nullptr;
    }
    m_ecm_file_detected = false;
}

PCSX::IEC60908b::MSF PCSX::CDRIso::getTD(uint8_t track) {
    if (track == 0) {
        unsigned int sect;
        sect = m_ti[m_numtracks].start.toLBA() + m_ti[m_numtracks].length.toLBA() - m_ti[m_numtracks].pregap.toLBA();
        return IEC60908b::MSF(sect);
    } else if (m_numtracks > 1 && track <= m_numtracks) {
        return IEC60908b::MSF(m_ti[track].start.toLBA());
    }
    return IEC60908b::MSF(0, 2, 0);
}

PCSX::IEC60908b::MSF PCSX::CDRIso::getLength(uint8_t track) {
    if (track == 0) {
        unsigned int sect;
        sect = m_ti[m_numtracks].start.toLBA() + m_ti[m_numtracks].length.toLBA();
        return IEC60908b::MSF(sect);
    } else if (m_numtracks > 0 && track <= m_numtracks) {
        return m_ti[track].length;
    }
    return IEC60908b::MSF(0, 0, 0);
}

PCSX::IEC60908b::MSF PCSX::CDRIso::getPregap(uint8_t track) {
    if (track <= 1) {
        return IEC60908b::MSF(0, 0, 0);
    } else if (m_numtracks > 0 && track <= m_numtracks) {
        return m_ti[track].pregap;
    }
    return IEC60908b::MSF(0, 2, 0);
}

bool PCSX::CDRIso::getLocP(const PCSX::IEC60908b::MSF msf, uint8_t locP[8]) {
    // TODO: actually check subchannels from iso.
    auto length = getTD(0) + IEC60908b::MSF{0, 2, 0};
    if (msf >= length) return false;
    unsigned track;
    for (track = m_numtracks; track != 1; track--) {
        if (m_ti[track].start <= msf) break;
    }

    bool inPregap = false;
    IEC60908b::MSF nextPregapStart;
    if (msf < m_ti[track].start) {
        // Before track 1 starts: its pregap, index 00, counting down.
        inPregap = true;
    } else if (track != m_numtracks) {
        if (msf >= (m_ti[track + 1].start - m_ti[track + 1].pregap)) {
            track++;
            inPregap = true;
        }
    }
    locP[0] = IEC60908b::itob(track);
    locP[1] = inPregap ? 0 : 1;
    IEC60908b::MSF relative;
    if (inPregap) {
        relative = m_ti[track].start - msf;
    } else {
        relative = msf - m_ti[track].start;
    }
    relative.toBCD(locP + 2);
    msf.toBCD(locP + 5);

    return true;
}

unsigned PCSX::CDRIso::getTrack(const PCSX::IEC60908b::MSF msf) {
    auto length = getTD(0) + IEC60908b::MSF{0, 2, 0};
    if (msf >= length) return 0;
    unsigned track;
    for (track = m_numtracks; track != 1; track--) {
        if (m_ti[track].start <= msf) break;
    }
    IEC60908b::MSF nextPregapStart;
    if (track != m_numtracks) {
        if (msf >= (m_ti[track + 1].start - m_ti[track + 1].pregap)) {
            track++;
        }
    }
    return track;
}

// Decode 'raw' subchannel data from being packed bitwise.
// Essentially is a bitwise matrix transposition.
void PCSX::CDRIso::decodeRawSubData() {
    unsigned char subQData[12];
    memset(subQData, 0, sizeof(subQData));

    for (int i = 0; i < 8 * 12; i++) {
        if (m_subbuffer.raw[i] & (1 << 6)) {  // only subchannel Q is needed
            subQData[i >> 3] |= (1 << (7 - (i & 7)));
        }
    }

    memcpy(&m_subbuffer.Q, subQData, 12);
}

// read track
bool PCSX::CDRIso::readTrack(const IEC60908b::MSF time) {
    int sector = time.toLBA() - 150;
    long ret;

    if (!m_cdHandle || m_cdHandle->failed()) {
        return false;
    }
    if (m_pregapOffset) {
        m_subChanMissing = false;
        if (sector >= m_pregapOffset) {
            sector -= 2 * 75;
            if (sector < m_pregapOffset) m_subChanMissing = true;
        }
    }

    ret = (*this.*m_cdimg_read_func)(m_cdHandle, 0, m_cdbuffer, sector);
    if (ret < 0) return false;

    if (m_subHandle) {
        m_subHandle->rSeek(sector * IEC60908b::SUB_FRAMESIZE, SEEK_SET);
        m_subHandle->read(m_subbuffer.raw, IEC60908b::SUB_FRAMESIZE);

        if (m_subChanRaw) decodeRawSubData();
    }

    m_ppf.maybePatchSector(m_cdbuffer, time);

    return true;
}

unsigned PCSX::CDRIso::readSectors(uint32_t lba, void *buffer_, unsigned count) {
    unsigned actual = 0;
    uint8_t *buffer = reinterpret_cast<uint8_t *>(buffer_);

    if (m_cdHandle->failed()) {
        return 0;
    }

    for (unsigned i = 0; i < count; i++) {
        auto ptr = buffer + actual * IEC60908b::FRAMESIZE_RAW;
        if (lba < m_ti[1].length.toLBA()) {
            IEC60908b::MSF time(lba + 150);
            long ret = (*this.*m_cdimg_read_func)(m_cdHandle, 0, ptr, lba++);
            m_ppf.maybePatchSector(ptr, time);
            if (ret < 0) return actual;
        } else {
            if (!readCDDA(IEC60908b::MSF(lba++), ptr)) return actual;
        }
        actual++;
    }

    return actual;
}

// gets subchannel data
const PCSX::IEC60908b::Sub *PCSX::CDRIso::getBufferSub() {
    if ((m_subHandle || m_subChanMixed) && !m_subChanMissing) {
        return &m_subbuffer;
    }

    return nullptr;
}

// read CDDA sector into buffer
bool PCSX::CDRIso::readCDDA(IEC60908b::MSF msf, unsigned char *buffer) {
    unsigned int file, track, track_start = 0;
    int ret;

    uint32_t lba = msf.toLBA();

    // find current track index
    for (track = m_numtracks;; track--) {
        track_start = m_ti[track].start.toLBA();
        if (track_start <= lba) break;
        if (track == 1) break;
    }

    // data tracks play silent
    if (m_ti[track].type != TrackType::CDDA) {
        memset(buffer, 0, IEC60908b::FRAMESIZE_RAW);
        return true;
    }

    // if above the track's end (likely next track's pregap), play silence
    if (lba >= (track_start + m_ti[track].length.toLBA())) {
        memset(buffer, 0, IEC60908b::FRAMESIZE_RAW);
        return true;
    }

    file = 1;
    if (m_multifile) {
        // find the file that contains this track
        for (file = track; file > 1; file--) {
            if (m_ti[file].handle) break;
        }
    }

    ret = (*this.*m_cdimg_read_func)(m_ti[file].handle, m_ti[track].start_offset, buffer, lba - track_start);
    if (ret != IEC60908b::FRAMESIZE_RAW) {
        memset(buffer, 0, IEC60908b::FRAMESIZE_RAW);
        return false;
    }

    if (m_cddaBigEndian) {
        unsigned char tmp;

        for (int i = 0; i < IEC60908b::FRAMESIZE_RAW / 2; i++) {
            tmp = buffer[i * 2];
            buffer[i * 2] = buffer[i * 2 + 1];
            buffer[i * 2 + 1] = tmp;
        }
    }

    return true;
}

bool PCSX::CDRIso::failed() { return !m_cdHandle && !m_ecm_savetable && !m_decoded_ecm; }

PCSX::CDRIso::CDRIso(const std::filesystem::path &path) : CDRIso() {
    m_isoPath = path;
    open(new UvFile(m_isoPath));
}
