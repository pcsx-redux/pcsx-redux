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

#include "core/luaiso.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "cdrom/cdriso.h"
#include "cdrom/file.h"
#include "cdrom/iso9660-reader.h"
#include "core/cdrom.h"
#include "lua/luafile.h"
#include "lua/luawrapper.h"
#include "support/strings-helpers.h"
#include "supportpsx/iec-60908b.h"
#include "supportpsx/iso9660-builder.h"

namespace {

struct LuaIso {
    LuaIso(std::shared_ptr<PCSX::CDRIso> iso) : iso(iso) {}
    std::shared_ptr<PCSX::CDRIso> iso;
};

struct MSF {
    uint8_t m, s, f;
};

void deleteIso(LuaIso* wrapper) { delete wrapper; }
bool isIsoFailed(LuaIso* wrapper) { return wrapper->iso->failed(); }
void isoClearPPF(LuaIso* wrapper) { wrapper->iso->getPPF()->clear(); }
void isoSavePPF(LuaIso* wrapper) { wrapper->iso->getPPF()->save(wrapper->iso->getIsoPath()); }
LuaIso* getCurrentIso() { return new LuaIso(PCSX::g_emulator->m_cdrom->getIso()); }
LuaIso* openIso(const char* path) { return new LuaIso(std::make_shared<PCSX::CDRIso>(path)); }
LuaIso* openIsoFromFile(PCSX::LuaFFI::LuaFile* wrapper) {
    return new LuaIso(std::make_shared<PCSX::CDRIso>(wrapper->file));
}

// Same sequence as the File menu: swap the image, then re-read its TOC.
void cdromSetIso(LuaIso* wrapper) {
    PCSX::g_emulator->m_cdrom->setIso(wrapper->iso);
    PCSX::g_emulator->m_cdrom->parseIso();
}
void cdromEjectIso() {
    PCSX::g_emulator->m_cdrom->setIso(new PCSX::CDRIso(new PCSX::FailedFile));
    PCSX::g_emulator->m_cdrom->parseIso();
}
void cdromOpenLid() { PCSX::g_emulator->m_cdrom->openLid(); }
void cdromCloseLid() { PCSX::g_emulator->m_cdrom->closeLid(); }
void cdromScheduleCloseLid() { PCSX::g_emulator->m_cdrom->scheduleCloseLid(); }
bool cdromIsLidOpen() { return PCSX::g_emulator->m_cdrom->isLidOpen(); }

PCSX::ISO9660Reader* createIsoReader(LuaIso* wrapper) { return new PCSX::ISO9660Reader(wrapper->iso); }
void deleteIsoReader(PCSX::ISO9660Reader* isoReader) { delete isoReader; }

bool isReaderFailed(PCSX::ISO9660Reader* reader) { return reader->failed(); }
PCSX::LuaFFI::LuaFile* readerOpen(PCSX::ISO9660Reader* reader, const char* path) {
    return new PCSX::LuaFFI::LuaFile(reader->open(path));
}
PCSX::LuaFFI::LuaFile* fileisoOpen(LuaIso* wrapper, uint32_t lba, uint32_t size, PCSX::IEC60908b::SectorMode mode) {
    return new PCSX::LuaFFI::LuaFile(new PCSX::CDRIsoFile(wrapper->iso, lba, size, mode));
}
uint32_t getIsoTN(LuaIso* wrapper) { return wrapper->iso->getTN(); }
MSF getIsoTD(LuaIso* wrapper, uint32_t tn) {
    auto ret = wrapper->iso->getTD(tn);
    return {ret.m, ret.s, ret.f};
}

struct DirEntries {
    std::vector<PCSX::ISO9660Reader::FullDirEntry> entries;
};

// Drop ISO9660 "." (\0) and ".." (\1) sentinel entries from a listing.
static std::vector<PCSX::ISO9660Reader::FullDirEntry> stripSelfParent(
    std::vector<PCSX::ISO9660Reader::FullDirEntry>&& entries) {
    std::vector<PCSX::ISO9660Reader::FullDirEntry> out;
    out.reserve(entries.size());
    for (auto& e : entries) {
        const auto& name = e.first.get<PCSX::ISO9660LowLevel::DirEntry_Filename>().value;
        if (name.size() == 1 && (name[0] == '\0' || name[0] == '\1')) continue;
        out.push_back(std::move(e));
    }
    return out;
}

DirEntries* readerListDir(PCSX::ISO9660Reader* reader, const char* path) {
    if (reader->failed()) return new DirEntries{};
    auto root = reader->getRootDirEntry();
    if (path == nullptr || path[0] == '\0') {
        return new DirEntries{stripSelfParent(reader->listAllEntriesFrom(root))};
    }
    // Walk the path using listAllEntriesFrom. ISO9660 directory entries
    // don't carry version suffixes (only files do), so exact match on each
    // path component is correct for directory listing.
    auto parts = PCSX::StringsHelpers::split(std::string_view(path), "/");
    PCSX::ISO9660LowLevel::DirEntry current = root;
    for (auto& part : parts) {
        auto entries = reader->listAllEntriesFrom(current);
        bool found = false;
        for (auto& [entry, xa] : entries) {
            if (entry.get<PCSX::ISO9660LowLevel::DirEntry_Filename>().value == part) {
                current = entry;
                found = true;
                break;
            }
        }
        if (!found) return new DirEntries{};
    }
    // Only return children if the target is actually a directory.
    if ((current.get<PCSX::ISO9660LowLevel::DirEntry_Flags>().value & 2) == 0) {
        return new DirEntries{};
    }
    return new DirEntries{stripSelfParent(reader->listAllEntriesFrom(current))};
}

void deleteDirEntries(DirEntries* entries) { delete entries; }
uint32_t dirEntriesCount(DirEntries* entries) { return entries->entries.size(); }
const char* dirEntryName(DirEntries* entries, uint32_t index) {
    if (index >= entries->entries.size()) return "";
    return entries->entries[index].first.get<PCSX::ISO9660LowLevel::DirEntry_Filename>().value.c_str();
}
uint32_t dirEntryLBA(DirEntries* entries, uint32_t index) {
    if (index >= entries->entries.size()) return 0;
    return entries->entries[index].first.get<PCSX::ISO9660LowLevel::DirEntry_LBA>();
}
uint32_t dirEntrySize(DirEntries* entries, uint32_t index) {
    if (index >= entries->entries.size()) return 0;
    return entries->entries[index].first.get<PCSX::ISO9660LowLevel::DirEntry_Size>();
}
bool dirEntryIsDir(DirEntries* entries, uint32_t index) {
    if (index >= entries->entries.size()) return false;
    return (entries->entries[index].first.get<PCSX::ISO9660LowLevel::DirEntry_Flags>().value & 2) != 0;
}

struct GapEntry {
    uint32_t lba;
    uint32_t sectors;
};

struct GapList {
    std::vector<GapEntry> gaps;
};

// XA attribute bits (CD-XA spec, stored big-endian in directory record).
// Bit 11 (0x0800): Mode 2 Form 1 data file
// Bit 12 (0x1000): Mode 2 Form 2 interleaved audio/video
static constexpr uint16_t XA_ATTR_FORM2 = 0x1000;

static void collectAllEntries(PCSX::ISO9660Reader* reader, const PCSX::ISO9660LowLevel::DirEntry& dir,
                              std::vector<std::pair<uint32_t, uint32_t>>& out,
                              std::unordered_set<uint32_t>& visitedDirs) {
    auto entries = reader->listAllEntriesFrom(dir);
    for (auto& [entry, xa] : entries) {
        const auto& name = entry.get<PCSX::ISO9660LowLevel::DirEntry_Filename>().value;
        if (name.size() == 1 && (name[0] == '\0' || name[0] == '\1')) continue;
        uint32_t lba = entry.get<PCSX::ISO9660LowLevel::DirEntry_LBA>();
        uint32_t size = entry.get<PCSX::ISO9660LowLevel::DirEntry_Size>();
        uint16_t attribs = xa.get<PCSX::ISO9660LowLevel::DirEntry_XA_Attribs>();
        uint32_t sectorSize = (attribs & XA_ATTR_FORM2) ? 2324 : 2048;
        uint32_t sectors = (size + sectorSize - 1) / sectorSize;
        // Skip zero-length extents: they don't consume sectors, and emitting
        // them would confuse the gap aggregation pass.
        if (sectors != 0) out.push_back({lba, sectors});
        bool isDir = (entry.get<PCSX::ISO9660LowLevel::DirEntry_Flags>().value & 2) != 0;
        // Guard against malformed ISOs with directory cycles.
        if (isDir && visitedDirs.insert(lba).second) {
            collectAllEntries(reader, entry, out, visitedDirs);
        }
    }
}

GapList* readerFindGaps(PCSX::ISO9660Reader* reader) {
    if (reader->failed()) return new GapList{};
    std::vector<std::pair<uint32_t, uint32_t>> allFiles;

    // Account for ISO9660 system structures
    allFiles.push_back({0, 16});  // License/system area
    auto& pvd = reader->getPVD();
    uint32_t vdEnd = reader->getVDEnd();
    allFiles.push_back({16, vdEnd > 16 ? vdEnd - 16 : 1});  // Volume descriptors including terminator
    uint32_t lPathLoc = pvd.get<PCSX::ISO9660LowLevel::PVD_LPathTableLocation>();
    uint32_t pathTableSize = pvd.get<PCSX::ISO9660LowLevel::PVD_PathTableSize>();
    uint32_t pathTableSectors = (pathTableSize + 2047) / 2048;
    allFiles.push_back({lPathLoc, pathTableSectors});
    uint32_t lPathOptLoc = pvd.get<PCSX::ISO9660LowLevel::PVD_LPathTableOptLocation>();
    if (lPathOptLoc != 0) allFiles.push_back({lPathOptLoc, pathTableSectors});
    uint32_t mPathLoc = pvd.get<PCSX::ISO9660LowLevel::PVD_MPathTableLocation>();
    allFiles.push_back({mPathLoc, pathTableSectors});
    uint32_t mPathOptLoc = pvd.get<PCSX::ISO9660LowLevel::PVD_MPathTableOptLocation>();
    if (mPathOptLoc != 0) allFiles.push_back({mPathOptLoc, pathTableSectors});
    auto& rootDir = reader->getRootDirEntry();
    uint32_t rootLBA = rootDir.get<PCSX::ISO9660LowLevel::DirEntry_LBA>();
    uint32_t rootSize = rootDir.get<PCSX::ISO9660LowLevel::DirEntry_Size>();
    allFiles.push_back({rootLBA, (rootSize + 2047) / 2048});

    std::unordered_set<uint32_t> visitedDirs;
    visitedDirs.insert(rootLBA);
    collectAllEntries(reader, reader->getRootDirEntry(), allFiles, visitedDirs);
    std::sort(allFiles.begin(), allFiles.end());

    auto* result = new GapList{};
    uint32_t nextExpected = 0;
    for (auto& [lba, sectors] : allFiles) {
        if (lba > nextExpected) {
            result->gaps.push_back({nextExpected, lba - nextExpected});
        }
        uint32_t end = lba + sectors;
        if (end > nextExpected) nextExpected = end;
    }
    // Trailing gap: anything between the last occupied extent and the disc end.
    uint32_t volumeSpaceSize = pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSpaceSize>();
    if (volumeSpaceSize > nextExpected) {
        result->gaps.push_back({nextExpected, volumeSpaceSize - nextExpected});
    }
    return result;
}

void deleteGapList(GapList* list) { delete list; }
uint32_t gapListCount(GapList* list) { return list->gaps.size(); }
uint32_t gapEntryLBA(GapList* list, uint32_t index) {
    if (index >= list->gaps.size()) return 0;
    return list->gaps[index].lba;
}
uint32_t gapEntrySectors(GapList* list, uint32_t index) {
    if (index >= list->gaps.size()) return 0;
    return list->gaps[index].sectors;
}

PCSX::ISO9660Builder* createIsoBuilder(PCSX::LuaFFI::LuaFile* wrapper) {
    return new PCSX::ISO9660Builder(wrapper->file);
}
void deleteIsoBuilder(PCSX::ISO9660Builder* builder) { delete builder; }
bool isoBuilderFailed(PCSX::ISO9660Builder* builder) { return builder->failed(); }
void isoBuilderWriteLicense(PCSX::ISO9660Builder* builder, PCSX::LuaFFI::LuaFile* licenseWrapper) {
    builder->writeLicense(licenseWrapper ? licenseWrapper->file : nullptr);
}
// Sentinel returned to Lua when a sector write couldn't be performed.
static constexpr uint32_t c_badLBA = 0xffffffff;

// Amount of data writeSectorAt will read from the caller's buffer for a given mode. Modes that can't
// be written to return 0.
static uint32_t sectorDataSize(PCSX::IEC60908b::SectorMode mode) {
    switch (mode) {
        case PCSX::IEC60908b::SectorMode::RAW:
            return PCSX::IEC60908b::FRAMESIZE_RAW;
        case PCSX::IEC60908b::SectorMode::M2_RAW:
            return 2336;
        case PCSX::IEC60908b::SectorMode::M2_FORM1:
            return 2048;
        case PCSX::IEC60908b::SectorMode::M2_FORM2:
            return 2324;
        default:
            return 0;
    }
}

// The LBAs here are relative to the start of the image, matching the rest of the iso API, while the
// builder works in MSF. Sector 0 of the image is MSF 00:02:00.
static uint32_t msfToImageLBA(PCSX::IEC60908b::MSF msf) {
    uint32_t lba = msf.toLBA();
    return lba < 150 ? c_badLBA : lba - 150;
}

uint32_t isoBuilderWriteSectorAt(PCSX::ISO9660Builder* builder, const uint8_t* sectorData, uint32_t dataSize,
                                 uint32_t lba, PCSX::IEC60908b::SectorMode mode) {
    uint32_t needed = sectorDataSize(mode);
    if ((needed == 0) || (dataSize < needed)) return c_badLBA;
    return msfToImageLBA(builder->writeSectorAt(sectorData, PCSX::IEC60908b::MSF(lba + 150), mode));
}
uint32_t isoBuilderWriteSector(PCSX::ISO9660Builder* builder, const uint8_t* sectorData, uint32_t dataSize,
                               PCSX::IEC60908b::SectorMode mode) {
    uint32_t needed = sectorDataSize(mode);
    if ((needed == 0) || (dataSize < needed)) return c_badLBA;
    return msfToImageLBA(builder->writeSector(sectorData, mode));
}
uint32_t isoBuilderGetCurrentLBA(PCSX::ISO9660Builder* builder) { return msfToImageLBA(builder->getCurrentLocation()); }
const char* isoBuilderClose(PCSX::ISO9660Builder* builder, uint32_t threadCount) {
    static thread_local std::string error;
    try {
        builder->close(threadCount);
    } catch (const std::exception& e) {
        error = e.what();
        return error.c_str();
    }
    return nullptr;
}

// PVD string field helpers
static void trimCStringTo(const char* src, size_t maxLen, char* dst, uint32_t dstSize) {
    size_t len = strnlen(src, maxLen);
    // Trim trailing spaces and nulls.
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\0')) len--;
    size_t toCopy = len < dstSize - 1 ? len : dstSize - 1;
    memcpy(dst, src, toCopy);
    dst[toCopy] = '\0';
}

// PVD getters
void hlPvdGetSystemIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_SystemIdent>().value, 32, buf, bufSize);
}
void hlPvdGetVolumeIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_VolumeIdent>().value, 32, buf, bufSize);
}
void hlPvdGetVolSetIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_VolSetIdent>().value, 128, buf, bufSize);
}
void hlPvdGetPublisherIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_PublisherIdent>().value, 128, buf, bufSize);
}
void hlPvdGetDataPreparerIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_DataPreparerIdent>().value, 128, buf, bufSize);
}
void hlPvdGetApplicationIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_ApplicationIdent>().value, 128, buf, bufSize);
}
void hlPvdGetCopyrightFileIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_CopyrightFileIdent>().value, 37, buf, bufSize);
}
void hlPvdGetAbstractFileIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_AbstractFileIdent>().value, 37, buf, bufSize);
}
void hlPvdGetBibliographicFileIdent(PCSX::ISO9660Builder* b, char* buf, uint32_t bufSize) {
    trimCStringTo(b->getPVD().get<PCSX::ISO9660LowLevel::PVD_BibliographicFileIdent>().value, 37, buf, bufSize);
}

// PVD setters
void hlPvdSetSystemIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_SystemIdent>().set(val, ' ');
}
void hlPvdSetVolumeIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_VolumeIdent>().set(val, ' ');
}
void hlPvdSetVolSetIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_VolSetIdent>().set(val, ' ');
}
void hlPvdSetPublisherIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_PublisherIdent>().set(val, ' ');
}
void hlPvdSetDataPreparerIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_DataPreparerIdent>().set(val, ' ');
}
void hlPvdSetApplicationIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_ApplicationIdent>().set(val, ' ');
}
void hlPvdSetCopyrightFileIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_CopyrightFileIdent>().set(val, ' ');
}
void hlPvdSetAbstractFileIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_AbstractFileIdent>().set(val, ' ');
}
void hlPvdSetBibliographicFileIdent(PCSX::ISO9660Builder* b, const char* val) {
    b->getPVD().get<PCSX::ISO9660LowLevel::PVD_BibliographicFileIdent>().set(val, ' ');
}

// DirTree wrappers
PCSX::ISO9660::DirTree* hlCreateRoot(PCSX::ISO9660Builder* b, uint32_t sectorCount) {
    return b->createRoot(sectorCount);
}
PCSX::ISO9660::DirTree* hlCreateDir(PCSX::ISO9660Builder* b, PCSX::ISO9660::DirTree* parent, const char* name,
                                    uint32_t sectorCount) {
    return b->createDir(parent, name, sectorCount);
}
PCSX::ISO9660::DirTree* hlCreateFile(PCSX::ISO9660Builder* b, PCSX::ISO9660::DirTree* parent, const char* name,
                                     PCSX::LuaFFI::LuaFile* content) {
    return b->createFile(parent, name, content->file);
}

const char* dirTreeGetName(PCSX::ISO9660::DirTree* node) { return node->getName().data(); }
uint32_t dirTreeGetSize(PCSX::ISO9660::DirTree* node) { return node->getSize(); }
uint32_t dirTreeGetLBA(PCSX::ISO9660::DirTree* node) { return node->getLBA(); }
bool dirTreeIsDir(PCSX::ISO9660::DirTree* node) { return node->isDir(); }
bool dirTreeIsHidden(PCSX::ISO9660::DirTree* node) { return node->isHidden(); }
void dirTreeSetHidden(PCSX::ISO9660::DirTree* node, bool val) { node->setHidden(val); }
bool dirTreeShouldSkip(PCSX::ISO9660::DirTree* node) { return node->shouldSkip(); }
void dirTreeSetSkip(PCSX::ISO9660::DirTree* node, bool val) { node->setSkip(val); }
bool dirTreeHasAnchorLBA(PCSX::ISO9660::DirTree* node) { return node->hasAnchorLBA(); }
uint32_t dirTreeGetAnchorLBA(PCSX::ISO9660::DirTree* node) { return node->getAnchorLBA(); }
void dirTreeSetAnchorLBA(PCSX::ISO9660::DirTree* node, uint32_t lba) { node->setAnchorLBA(lba); }
void dirTreeClearAnchorLBA(PCSX::ISO9660::DirTree* node) { node->clearAnchorLBA(); }
bool dirTreeHasDeclaredSize(PCSX::ISO9660::DirTree* node) { return node->hasDeclaredSize(); }
uint32_t dirTreeGetDeclaredSize(PCSX::ISO9660::DirTree* node) { return node->getDeclaredSize(); }
void dirTreeSetDeclaredSize(PCSX::ISO9660::DirTree* node, uint32_t size) { node->setDeclaredSize(size); }
void dirTreeClearDeclaredSize(PCSX::ISO9660::DirTree* node) { node->clearDeclaredSize(); }
bool dirTreeHasXA(PCSX::ISO9660::DirTree* node) { return node->hasXA(); }
void dirTreeSetHasXA(PCSX::ISO9660::DirTree* node, bool val) { node->setHasXA(val); }
PCSX::IEC60908b::SectorMode dirTreeGetSectorMode(PCSX::ISO9660::DirTree* node) { return node->getSectorMode(); }
void dirTreeSetSectorMode(PCSX::ISO9660::DirTree* node, PCSX::IEC60908b::SectorMode mode) { node->setSectorMode(mode); }
uint16_t dirTreeGetXAAttribs(PCSX::ISO9660::DirTree* node) {
    return node->getXA().get<PCSX::ISO9660LowLevel::DirEntry_XA_Attribs>().value;
}
void dirTreeSetXAAttribs(PCSX::ISO9660::DirTree* node, uint16_t val) {
    node->getXA().get<PCSX::ISO9660LowLevel::DirEntry_XA_Attribs>().value = val;
}
uint8_t dirTreeGetXAFileNum(PCSX::ISO9660::DirTree* node) {
    return node->getXA().get<PCSX::ISO9660LowLevel::DirEntry_XA_FileNum>().value;
}
void dirTreeSetXAFileNum(PCSX::ISO9660::DirTree* node, uint8_t val) {
    node->getXA().get<PCSX::ISO9660LowLevel::DirEntry_XA_FileNum>().value = val;
}

// DirTree navigation
PCSX::ISO9660::DirTree* dirTreeParent(PCSX::ISO9660::DirTree* node) { return node->parent(); }
PCSX::ISO9660::DirTree* dirTreeFirstChild(PCSX::ISO9660::DirTree* node) { return node->firstChild(); }
PCSX::ISO9660::DirTree* dirTreeNextSibling(PCSX::ISO9660::DirTree* node) { return node->nextSibling(); }

// DirTree date access
void dirTreeSetDate(PCSX::ISO9660::DirTree* node, uint8_t year, uint8_t month, uint8_t day, uint8_t hour,
                    uint8_t minute, uint8_t second, uint8_t offset) {
    auto& date = node->getDate();
    date.get<PCSX::ISO9660LowLevel::ShortDate_Year>().value = year;
    date.get<PCSX::ISO9660LowLevel::ShortDate_Month>().value = month;
    date.get<PCSX::ISO9660LowLevel::ShortDate_Day>().value = day;
    date.get<PCSX::ISO9660LowLevel::ShortDate_Hour>().value = hour;
    date.get<PCSX::ISO9660LowLevel::ShortDate_Minute>().value = minute;
    date.get<PCSX::ISO9660LowLevel::ShortDate_Second>().value = second;
    date.get<PCSX::ISO9660LowLevel::ShortDate_Offset>().value = offset;
}
uint8_t dirTreeGetDateYear(PCSX::ISO9660::DirTree* node) {
    return node->getDate().get<PCSX::ISO9660LowLevel::ShortDate_Year>().value;
}
uint8_t dirTreeGetDateMonth(PCSX::ISO9660::DirTree* node) {
    return node->getDate().get<PCSX::ISO9660LowLevel::ShortDate_Month>().value;
}
uint8_t dirTreeGetDateDay(PCSX::ISO9660::DirTree* node) {
    return node->getDate().get<PCSX::ISO9660LowLevel::ShortDate_Day>().value;
}

}  // namespace

template <typename T, size_t S>
static void registerSymbol(PCSX::Lua L, const char (&name)[S], const T ptr) {
    L.push<S>(name);
    L.push((void*)ptr);
    L.settable();
}

#define REGISTER(L, s) registerSymbol(L, #s, s)

static void registerCDRomSymbols(PCSX::Lua L) {
    REGISTER(L, cdromSetIso);
    REGISTER(L, cdromEjectIso);
    REGISTER(L, cdromOpenLid);
    REGISTER(L, cdromCloseLid);
    REGISTER(L, cdromScheduleCloseLid);
    REGISTER(L, cdromIsLidOpen);
}

static void registerPvdSymbols(PCSX::Lua L) {
    // PVD getters
    REGISTER(L, hlPvdGetSystemIdent);
    REGISTER(L, hlPvdGetVolumeIdent);
    REGISTER(L, hlPvdGetVolSetIdent);
    REGISTER(L, hlPvdGetPublisherIdent);
    REGISTER(L, hlPvdGetDataPreparerIdent);
    REGISTER(L, hlPvdGetApplicationIdent);
    REGISTER(L, hlPvdGetCopyrightFileIdent);
    REGISTER(L, hlPvdGetAbstractFileIdent);
    REGISTER(L, hlPvdGetBibliographicFileIdent);

    // PVD setters
    REGISTER(L, hlPvdSetSystemIdent);
    REGISTER(L, hlPvdSetVolumeIdent);
    REGISTER(L, hlPvdSetVolSetIdent);
    REGISTER(L, hlPvdSetPublisherIdent);
    REGISTER(L, hlPvdSetDataPreparerIdent);
    REGISTER(L, hlPvdSetApplicationIdent);
    REGISTER(L, hlPvdSetCopyrightFileIdent);
    REGISTER(L, hlPvdSetAbstractFileIdent);
    REGISTER(L, hlPvdSetBibliographicFileIdent);
}

static void registerDirTreeSymbols(PCSX::Lua L) {
    REGISTER(L, dirTreeGetName);
    REGISTER(L, dirTreeGetSize);
    REGISTER(L, dirTreeGetLBA);
    REGISTER(L, dirTreeIsDir);
    REGISTER(L, dirTreeIsHidden);
    REGISTER(L, dirTreeSetHidden);
    REGISTER(L, dirTreeShouldSkip);
    REGISTER(L, dirTreeSetSkip);
    REGISTER(L, dirTreeHasAnchorLBA);
    REGISTER(L, dirTreeGetAnchorLBA);
    REGISTER(L, dirTreeSetAnchorLBA);
    REGISTER(L, dirTreeClearAnchorLBA);
    REGISTER(L, dirTreeHasDeclaredSize);
    REGISTER(L, dirTreeGetDeclaredSize);
    REGISTER(L, dirTreeSetDeclaredSize);
    REGISTER(L, dirTreeClearDeclaredSize);
    REGISTER(L, dirTreeHasXA);
    REGISTER(L, dirTreeSetHasXA);
    REGISTER(L, dirTreeGetSectorMode);
    REGISTER(L, dirTreeSetSectorMode);
    REGISTER(L, dirTreeGetXAAttribs);
    REGISTER(L, dirTreeSetXAAttribs);
    REGISTER(L, dirTreeGetXAFileNum);
    REGISTER(L, dirTreeSetXAFileNum);
    REGISTER(L, dirTreeParent);
    REGISTER(L, dirTreeFirstChild);
    REGISTER(L, dirTreeNextSibling);
    REGISTER(L, dirTreeSetDate);
    REGISTER(L, dirTreeGetDateYear);
    REGISTER(L, dirTreeGetDateMonth);
    REGISTER(L, dirTreeGetDateDay);
}

static void registerAllSymbols(PCSX::Lua L) {
    L.getfieldtable("_CLIBS", LUA_REGISTRYINDEX);
    L.push("CORE_ISO");
    L.newtable();

    REGISTER(L, deleteIso);
    REGISTER(L, isIsoFailed);
    REGISTER(L, isoClearPPF);
    REGISTER(L, isoSavePPF);
    REGISTER(L, getCurrentIso);
    REGISTER(L, openIso);
    REGISTER(L, openIsoFromFile);
    registerCDRomSymbols(L);
    REGISTER(L, getIsoTD);
    REGISTER(L, getIsoTN);
    REGISTER(L, createIsoReader);
    REGISTER(L, deleteIsoReader);
    REGISTER(L, isReaderFailed);
    REGISTER(L, readerOpen);
    REGISTER(L, fileisoOpen);

    REGISTER(L, readerListDir);
    REGISTER(L, deleteDirEntries);
    REGISTER(L, dirEntriesCount);
    REGISTER(L, dirEntryName);
    REGISTER(L, dirEntryLBA);
    REGISTER(L, dirEntrySize);
    REGISTER(L, dirEntryIsDir);

    REGISTER(L, readerFindGaps);
    REGISTER(L, deleteGapList);
    REGISTER(L, gapListCount);
    REGISTER(L, gapEntryLBA);
    REGISTER(L, gapEntrySectors);

    REGISTER(L, createIsoBuilder);
    REGISTER(L, deleteIsoBuilder);
    REGISTER(L, isoBuilderFailed);
    REGISTER(L, isoBuilderWriteLicense);
    REGISTER(L, isoBuilderWriteSector);
    REGISTER(L, isoBuilderWriteSectorAt);
    REGISTER(L, isoBuilderGetCurrentLBA);
    REGISTER(L, isoBuilderClose);

    registerPvdSymbols(L);

    // DirTree
    REGISTER(L, hlCreateRoot);
    REGISTER(L, hlCreateDir);
    REGISTER(L, hlCreateFile);
    registerDirTreeSymbols(L);

    L.settable();
    L.pop();
}

void PCSX::LuaFFI::open_iso(Lua L) {
    static int lualoader = 1;
    static const char* isoFFI = (
#include "core/isoffi.lua"
    );
    registerAllSymbols(L);
    L.load(isoFFI, "src:core/isoffi.lua");
}
