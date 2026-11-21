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

#include <stdio.h>
#include <zlib.h>

#include <filesystem>

#include "cdrom/ppf.h"
#include "core/psxemulator.h"
#include "supportpsx/iec-60908b.h"

namespace PCSX {

class CDRIso {
  public:
    // Out-of-lined so this header does not need uvfile.h, which pulls in libcurl
    // and libuv. UvFile was the only thing here that needed it - everything else
    // is IO<File>.
    CDRIso(const std::filesystem::path& path);
    CDRIso(IO<File> isoFile) : CDRIso() {
        m_isoPath = isoFile->filename();
        open(isoFile);
    }
    ~CDRIso() {
        close();
        inflateEnd(&m_zstr);
    }
    enum class TrackType { CLOSED = 0, DATA = 1, CDDA = 2 };
    TrackType getTrackType(unsigned track) { return m_ti[track].type; }
    const std::filesystem::path& getIsoPath() { return m_isoPath; }
    uint8_t getTN() { return std::max(m_numtracks, 1U); }
    IEC60908b::MSF getTD(uint8_t track);
    IEC60908b::MSF getLength(uint8_t track);
    IEC60908b::MSF getPregap(uint8_t track);
    bool getLocP(const IEC60908b::MSF msf, uint8_t locP[8]);
    unsigned getTrack(const IEC60908b::MSF msf);

    bool failed();

    /* Probably have to change those */
    bool readTrack(const IEC60908b::MSF time);
    unsigned readSectors(uint32_t lba, void* buffer, unsigned count);
    uint8_t* getBuffer();
    const IEC60908b::Sub* getBufferSub();
    bool readCDDA(const IEC60908b::MSF msf, unsigned char* buffer);
    PPF* getPPF() { return &m_ppf; }

    unsigned m_cdrIsoMultidiskCount;
    unsigned m_cdrIsoMultidiskSelect;

    bool CheckSBI(const uint8_t* time);

  private:
    CDRIso();
    bool open(IO<File> isoFile);
    void close();

    std::filesystem::path m_isoPath;
    typedef ssize_t (CDRIso::*read_func_t)(IO<File> f, unsigned int base, void* dest, int sector);

    bool m_useCompressed = false;
    z_stream m_zstr;

    IO<File> m_cdHandle;
    IO<File> m_subHandle;

    bool m_subChanMixed = false;
    bool m_subChanRaw = false;
    bool m_subChanMissing = false;

    bool m_multifile = false;
    bool m_isMode1ISO = false;  // TODO: use sector size/mode info from CUE also?

    uint8_t m_cdbuffer[2352];
    IEC60908b::Sub m_subbuffer;

    bool m_cddaBigEndian = false;
    /* Frame offset into CD image where pregap data would be found if it was there.
     * If a game seeks there we must *not* return subchannel data since it's
     * not in the CD image, so that cdrom code can fake subchannel data instead.
     * XXX: there could be multiple pregaps but PSX dumps only have one? */
    unsigned int m_pregapOffset;

    // compressed image stuff
    struct compr_img_t {
        unsigned char buff_raw[16][2352];
        unsigned char buff_compressed[2352 * 16 + 100];
        unsigned int* index_table;
        unsigned int index_len;
        unsigned int block_shift;
        unsigned int current_block;
        unsigned int sector_in_blk;
    }* m_compr_img = nullptr;

    read_func_t m_cdimg_read_func = nullptr;

    uint32_t m_len_decoded_ecm_buffer = 0;  // same as decoded ECM file length or 2x size
    uint32_t m_len_ecm_savetable = 0;       // same as sector count of decoded ECM file or 2x count

    uint32_t m_decoded_ecm_sectors = 0;  // disabled

    bool m_ecm_file_detected = false;
    uint32_t m_prevsector;

    IO<File> m_decoded_ecm = nullptr;
    void* m_decoded_ecm_buffer = nullptr;

    // Function that is used to read CD normally
    read_func_t m_cdimg_read_func_o = nullptr;

    struct ECMFILELUT {
        int32_t sector;
        int32_t filepos;
    };

    ECMFILELUT* m_ecm_savetable = nullptr;

    static inline const size_t ECM_SECTOR_SIZE[4] = {1, 2352, 2336, 2336};

    struct trackinfo {
        TrackType type = TrackType::CLOSED;
        IEC60908b::MSF pregap;
        IEC60908b::MSF start;
        IEC60908b::MSF length;
        IO<File> handle = nullptr;                                       // for multi-track images CDDA
        enum CDDAType { NONE = 0, BIN = 1, CCDDA = 2 } cddatype = NONE;  // BIN, WAV, MP3, APE
        uint32_t start_offset = 0;                                       // byte offset from start of above file
        uint32_t pregapInFile = 0;  // sectors of the pregap stored in handle ahead of INDEX 01
    };

    static constexpr unsigned MAXTRACKS = 100; /* How many tracks can a CD hold? */

    unsigned m_numtracks = 0;
    struct trackinfo m_ti[MAXTRACKS];

    // redump.org SBI files
    uint8_t sbitime[256][3], sbicount;
    PPF m_ppf;

    void decodeRawSubData();
    bool parsetoc(const char* isofile);
    bool parsecue(const char* isofile);
    bool parseccd(const char* isofile);
    bool parsemds(const char* isofile);
    bool handlepbp(const char* isofile);
    bool handlecbin(const char* isofile);
    bool handleecm(const char* isoname, IO<File> cdh, int32_t* accurate_length);
    bool opensubfile(const char* isoname);
    bool opensbifile(const char* isoname);

    bool LoadSBI(const char* filename);

    ssize_t cdread_normal(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t cdread_sub_mixed(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t cdread_compressed(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t cdread_2048(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t ecmDecode(IO<File> f, unsigned int base, void* dest, int sector);

    void printTracks();
    void UnloadSBI();
};

}  // namespace PCSX
