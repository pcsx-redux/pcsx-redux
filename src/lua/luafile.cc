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

#include "lua/luafile.h"

#include "core/system.h"
#include "lua-protobuf/pb.h"
#include "lua/luawrapper.h"
#include "support/ffmpeg-audio-file.h"
#include "support/mem4g.h"
#include "support/uvfile.h"
#include "support/zfile.h"

namespace {

using LuaFile = PCSX::LuaFFI::LuaFile;

enum FileOps {
    READ,
    TRUNCATE,
    CREATE,
    READWRITE,
    DOWNLOAD_URL,
};

void deleteFile(LuaFile* wrapper) { delete wrapper; }

LuaFile* openFile(const char* filename, FileOps type) {
    switch (type) {
        case READ:
            return new LuaFile(new PCSX::UvFile(filename));
        case TRUNCATE:
            return new LuaFile(new PCSX::UvFile(filename, PCSX::FileOps::TRUNCATE));
        case CREATE:
            return new LuaFile(new PCSX::UvFile(filename, PCSX::FileOps::CREATE));
        case READWRITE:
            return new LuaFile(new PCSX::UvFile(filename, PCSX::FileOps::READWRITE));
        case DOWNLOAD_URL:
            return new LuaFile(new PCSX::UvFile(filename, PCSX::UvFile::DOWNLOAD_URL));
    }

    return nullptr;
}

LuaFile* openFileWithCallback(const char* url, void (*callback)()) {
    return new LuaFile(
        new PCSX::UvFile(url, [callback]() { callback(); }, PCSX::g_system->getLoop(), PCSX::UvFile::DOWNLOAD_URL));
}

LuaFile* bufferFileReadOnly(void* data, uint64_t size) { return new LuaFile(new PCSX::BufferFile(data, size)); }
LuaFile* bufferFile(void* data, uint64_t size) {
    return new LuaFile(new PCSX::BufferFile(data, size, PCSX::FileOps::READWRITE));
}
LuaFile* bufferFileAcquire(void* data, uint64_t size) {
    return new LuaFile(new PCSX::BufferFile(data, size, PCSX::BufferFile::ACQUIRE));
}
LuaFile* bufferFileEmpty() { return new LuaFile(new PCSX::BufferFile(PCSX::FileOps::READWRITE)); }
LuaFile* subFile(LuaFile* wrapper, uint64_t start, int64_t size) {
    return new LuaFile(new PCSX::SubFile(wrapper->file, start, size));
}
LuaFile* uvFifo(const char* address, int port) { return new LuaFile(new PCSX::UvFifo(address, port)); }
bool uvFifoIsConnecting(LuaFile* wrapper) { return wrapper->file.asA<PCSX::UvFifo>()->isConnecting(); }
LuaFile* failedFile() { return new LuaFile(new PCSX::FailedFile()); }

void closeFile(LuaFile* wrapper) { wrapper->file->close(); }

uint64_t readFileRawPtr(LuaFile* wrapper, void* dst, uint64_t size) { return wrapper->file->read(dst, size); }
uint64_t readFileBuffer(LuaFile* wrapper, void* buffer) {
    uint32_t* pSize = reinterpret_cast<uint32_t*>(buffer);
    uint8_t* data = reinterpret_cast<uint8_t*>(pSize + 1);
    const ssize_t got = wrapper->file->read(data, *pSize);
    return *pSize = got < 0 ? 0 : got;
}
PCSX::Slice* readFileToSlice(LuaFile* wrapper, uint64_t size) {
    return new PCSX::Slice(std::move(wrapper->file->read(size)));
}
uint64_t readFileToExistingSlice(LuaFile* wrapper, PCSX::Slice* slice, uint64_t size) {
    return wrapper->file->read(slice->mutableData(), size);
}

uint64_t writeFileRawPtr(LuaFile* wrapper, const uint8_t* data, uint64_t size) {
    return wrapper->file->write(data, size);
}
uint64_t writeFileBuffer(LuaFile* wrapper, const void* buffer) {
    const uint32_t* pSize = reinterpret_cast<const uint32_t*>(buffer);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(pSize + 1);
    return wrapper->file->write(data, *pSize);
}

void writeFileMoveSlice(LuaFile* wrapper, PCSX::Slice* slice) { wrapper->file->write(std::move(*slice)); }

int64_t rSeek(LuaFile* wrapper, int64_t pos, PCSX::LuaFFI::SeekWheel wheel) {
    return wrapper->file->rSeek(pos, PCSX::LuaFFI::wheelConv(wheel));
}
int64_t rTell(LuaFile* wrapper) { return wrapper->file->rTell(); }
int64_t wSeek(LuaFile* wrapper, int64_t pos, PCSX::LuaFFI::SeekWheel wheel) {
    return wrapper->file->wSeek(pos, PCSX::LuaFFI::wheelConv(wheel));
}
int64_t wTell(LuaFile* wrapper) { return wrapper->file->wTell(); }

uint64_t getFileSize(LuaFile* wrapper) { return wrapper->file->size(); }

uint64_t readFileAtRawPtr(LuaFile* wrapper, void* dst, uint64_t size, uint64_t pos) {
    return wrapper->file->readAt(dst, size, pos);
}
uint64_t readFileAtBuffer(LuaFile* wrapper, void* buffer, uint64_t pos) {
    uint32_t* pSize = reinterpret_cast<uint32_t*>(buffer);
    uint8_t* data = reinterpret_cast<uint8_t*>(pSize + 1);
    const ssize_t got = wrapper->file->readAt(data, *pSize, pos);
    return *pSize = got < 0 ? 0 : got;
}
PCSX::Slice* readFileAtToSlice(LuaFile* wrapper, uint64_t size, uint64_t pos) {
    return new PCSX::Slice(std::move(wrapper->file->readAt(size, pos)));
}
uint64_t readFileAtToExistingSlice(LuaFile* wrapper, PCSX::Slice* slice, uint64_t size, uint64_t pos) {
    return wrapper->file->readAt(slice->mutableData(), size, pos);
}

uint64_t writeFileAtRawPtr(LuaFile* wrapper, const uint8_t* data, uint64_t size, uint64_t pos) {
    return wrapper->file->writeAt(data, size, pos);
}
uint64_t writeFileAtBuffer(LuaFile* wrapper, const void* buffer, uint64_t pos) {
    const uint32_t* pSize = reinterpret_cast<const uint32_t*>(buffer);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(pSize + 1);
    return wrapper->file->writeAt(data, *pSize, pos);
}

void writeFileAtMoveSlice(LuaFile* wrapper, PCSX::Slice* slice, uint64_t pos) {
    wrapper->file->writeAt(std::move(*slice), pos);
}

bool isFileSeekable(LuaFile* wrapper) { return wrapper->file->seekable(); }
bool isFileWritable(LuaFile* wrapper) { return wrapper->file->writable(); }
bool isFileEOF(LuaFile* wrapper) { return wrapper->file->eof(); }
bool isFileFailed(LuaFile* wrapper) { return wrapper->file->failed(); }
bool isFileCacheable(LuaFile* wrapper) { return wrapper->file.isA<PCSX::UvFile>(); }
bool isFileCaching(LuaFile* wrapper) {
    PCSX::IO<PCSX::UvFile> file = wrapper->file.asA<PCSX::UvFile>();
    if (file) return file->caching();
    return false;
}
float fileCacheProgress(LuaFile* wrapper) {
    PCSX::IO<PCSX::UvFile> file = wrapper->file.asA<PCSX::UvFile>();
    if (file) return file->cacheProgress();
    return 0.0f;
}
void startFileCaching(LuaFile* wrapper) {
    PCSX::IO<PCSX::UvFile> file = wrapper->file.asA<PCSX::UvFile>();
    if (file) file->startCaching();
}
bool startFileCachingWithCallback(LuaFile* wrapper, void (*callback)()) {
    PCSX::IO<PCSX::UvFile> file = wrapper->file.asA<PCSX::UvFile>();
    if (file) {
        file->startCaching([callback]() { callback(); }, PCSX::g_system->getLoop());
        return true;
    } else {
        return false;
    }
}

LuaFile* dupFile(LuaFile* wrapper) { return new LuaFile(wrapper->file->dup()); }

LuaFile* zReader(LuaFile* wrapper, int64_t size, bool raw) {
    return new LuaFile(raw ? new PCSX::ZReader(wrapper->file, size, PCSX::ZReader::RAW)
                           : new PCSX::ZReader(wrapper->file, size));
}

PCSX::Slice* createEmptySlice() { return new PCSX::Slice(); }
uint64_t getSliceSize(PCSX::Slice* slice) { return slice->size(); }
const void* getSliceData(PCSX::Slice* slice) { return slice->data(); }
void* getSliceMutableData(PCSX::Slice* slice) { return slice->mutableData(); }
void resizeSlice(PCSX::Slice* slice, uint32_t size) { slice->resize(size); }
void destroySlice(PCSX::Slice* slice) { delete slice; }

int readFileUserData(PCSX::Lua L) {
    if (L.gettop() != 3) return L.error("Invalid number of arguments to readFileUserData");

    if (!L.iscdata(1)) return L.error("readFileUserData: arg 1 not a cdata");
    if (!L.isuserdata(2)) return L.error("readFileUserData: arg 2 not a userdata");
    if (!L.isnumber(3)) return L.error("readFileUserData: arg 3 not a number");

    LuaFile* wrapper = *L.topointer<LuaFile*>(1);

    if (!lpb_isbuffer(L.getState(), 2)) return L.error("readFileUserData: unknown userdata type");
    pb_Buffer* buffer = lpb_checkbuffer(L.getState(), 2);
    size_t len = L.tonumber(3);
    auto ptr = pb_prepbuffsize(buffer, len);

    auto ret = wrapper->file->read(ptr, len);
    if (ret >= 0) pb_addsize(buffer, ret);
    L.push(lua_Number(ret));
    return 1;
}

int readFileUserDataAt(PCSX::Lua L) {
    if (L.gettop() != 4) return L.error("Invalid number of arguments to readFileUserDataAt");

    if (!L.iscdata(1)) return L.error("readFileUserDataAt: arg 1 not a cdata");
    if (!L.isuserdata(2)) return L.error("readFileUserDataAt: arg 2 not a userdata");
    if (!L.isnumber(3)) return L.error("readFileUserDataAt: arg 3 not a number");
    if (!L.isnumber(4)) return L.error("readFileUserDataAt: arg 4 not a number");

    LuaFile* wrapper = *L.topointer<LuaFile*>(1);

    if (!lpb_isbuffer(L.getState(), 2)) return L.error("readFileUserDataAt: unknown userdata type");
    pb_Buffer* buffer = lpb_checkbuffer(L.getState(), 2);
    size_t len = L.tonumber(3);
    auto ptr = pb_prepbuffsize(buffer, len);

    auto ret = wrapper->file->readAt(ptr, len, L.tonumber(4));
    if (ret >= 0) pb_addsize(buffer, ret);
    L.push(lua_Number(ret));
    return 1;
}

int readFileGets(PCSX::Lua L) {
    if (L.gettop() != 1) return L.error("Invalid number of arguments to readFileGets");

    if (!L.iscdata(1)) return L.error("readFileGets: arg 1 not a cdata");

    LuaFile* wrapper = *L.topointer<LuaFile*>(1);

    L.push(wrapper->file->gets());
    return 1;
}

int writeFileUserData(PCSX::Lua L) {
    if (L.gettop() != 2) return L.error("Invalid number of arguments to writeFileUserData");

    if (!L.iscdata(1)) return L.error("writeFileUserData: arg 1 not a cdata");
    if (!L.isuserdata(2)) return L.error("writeFileUserData: arg 2 not a userdata");

    LuaFile* wrapper = *L.topointer<LuaFile*>(1);

    if (!lpb_isslice(L.getState(), 2)) return L.error("writeFileUserData: unknown userdata type");
    pb_Slice s = lpb_checkslice(L.getState(), 2);

    auto ret = wrapper->file->write(s.p, s.end - s.p);
    L.push(lua_Number(ret));
    return 1;
}

int writeFileUserDataAt(PCSX::Lua L) {
    if (L.gettop() != 3) return L.error("Invalid number of arguments to writeFileUserDataAt");

    if (!L.iscdata(1)) return L.error("writeFileUserDataAt: arg 1 not a cdata");
    if (!L.isuserdata(2)) return L.error("writeFileUserDataAt: arg 2 not a userdata");
    if (!L.isnumber(3)) return L.error("writeFileUserDataAt: arg 3 not a number");

    LuaFile* wrapper = *L.topointer<LuaFile*>(1);

    if (!lpb_isslice(L.getState(), 2)) return L.error("writeFileUserDataAt: unknown userdata type");
    pb_Slice s = lpb_checkslice(L.getState(), 2);

    auto ret = wrapper->file->writeAt(s.p, s.end - s.p, L.tonumber(3));
    L.push(lua_Number(ret));
    return 1;
}

int createPBSliceFromBuffer(PCSX::Lua L) {
    if (L.gettop() != 1) return L.error("Invalid number of arguments to createPBSliceFromBuffer");
    if (!L.iscdata(1)) return L.error("createPBSliceFromBuffer: arg 1 is not a pointer");

    uint32_t* ptr = L.topointer<uint32_t>(1);

    return lpb_newslice(L.getState(), reinterpret_cast<char*>(ptr + 1), *ptr);
}

LuaFile* mem4g() { return new LuaFile(new PCSX::Mem4G()); }
uint32_t mem4gLowestAddress(LuaFile* file) { return file->file.asA<PCSX::Mem4G>()->lowestAddress(); }
uint32_t mem4gHighestAddress(LuaFile* file) { return file->file.asA<PCSX::Mem4G>()->highestAddress(); }
uint32_t mem4gActualSize(LuaFile* file) { return file->file.asA<PCSX::Mem4G>()->actualSize(); }

LuaFile* ffmpegAudioFile(LuaFile* file, PCSX::FFmpegAudioFile::Channels channels,
                         PCSX::FFmpegAudioFile::Endianness endianness, PCSX::FFmpegAudioFile::SampleFormat sampleFormat,
                         unsigned frequency) {
    return new LuaFile(new PCSX::FFmpegAudioFile(file->file, channels, endianness, sampleFormat, frequency));
}

}  // namespace

template <typename T, size_t S>
static void registerSymbol(PCSX::Lua L, const char (&name)[S], const T ptr) {
    L.push<S>(name);
    L.push((void*)ptr);
    L.settable();
}

#define REGISTER(L, s) registerSymbol(L, #s, s)

static void registerAllSymbols(PCSX::Lua L) {
    L.getfieldtable("_CLIBS", LUA_REGISTRYINDEX);
    L.push("SUPPORT_FILE");
    L.newtable();

    REGISTER(L, deleteFile);

    REGISTER(L, openFile);
    REGISTER(L, openFileWithCallback);
    REGISTER(L, bufferFileReadOnly);
    REGISTER(L, bufferFile);
    REGISTER(L, bufferFileAcquire);
    REGISTER(L, bufferFileEmpty);
    REGISTER(L, subFile);
    REGISTER(L, uvFifo);
    REGISTER(L, uvFifoIsConnecting);
    REGISTER(L, failedFile);

    REGISTER(L, closeFile);

    REGISTER(L, readFileRawPtr);
    REGISTER(L, readFileBuffer);
    REGISTER(L, readFileToSlice);
    REGISTER(L, readFileToExistingSlice);

    REGISTER(L, writeFileRawPtr);
    REGISTER(L, writeFileBuffer);
    REGISTER(L, writeFileMoveSlice);

    REGISTER(L, rSeek);
    REGISTER(L, rTell);
    REGISTER(L, wSeek);
    REGISTER(L, wTell);

    REGISTER(L, getFileSize);

    REGISTER(L, readFileAtRawPtr);
    REGISTER(L, readFileAtBuffer);
    REGISTER(L, readFileAtToSlice);
    REGISTER(L, readFileAtToExistingSlice);

    REGISTER(L, writeFileAtRawPtr);
    REGISTER(L, writeFileAtBuffer);
    REGISTER(L, writeFileAtMoveSlice);

    REGISTER(L, isFileSeekable);
    REGISTER(L, isFileWritable);
    REGISTER(L, isFileEOF);
    REGISTER(L, isFileFailed);
    REGISTER(L, isFileCacheable);
    REGISTER(L, isFileCaching);
    REGISTER(L, fileCacheProgress);
    REGISTER(L, startFileCaching);
    REGISTER(L, startFileCachingWithCallback);

    REGISTER(L, dupFile);

    REGISTER(L, zReader);

    REGISTER(L, createEmptySlice);
    REGISTER(L, getSliceSize);
    REGISTER(L, getSliceData);
    REGISTER(L, getSliceMutableData);
    REGISTER(L, resizeSlice);
    REGISTER(L, destroySlice);

    REGISTER(L, mem4g);
    REGISTER(L, mem4gLowestAddress);
    REGISTER(L, mem4gHighestAddress);
    REGISTER(L, mem4gActualSize);

    REGISTER(L, ffmpegAudioFile);

    L.settable();
    L.pop();
}

void PCSX::LuaFFI::open_file(Lua L) {
    static int lualoader = 3;
    static const char* fileFFICDef = (
#include "lua/fileffi-cdef.lua"
    );
    static const char* fileFFI = (
#include "lua/fileffi.lua"
    );
    static const char* fileFFIMeta = (
#include "lua/fileffimeta.lua"
    );
    registerAllSymbols(L);
    L.getfieldtable("Support", LUA_GLOBALSINDEX);
    L.getfieldtable("_internal");
    L.declareFunc("readFileUserData", readFileUserData, -1);
    L.declareFunc("readFileUserDataAt", readFileUserDataAt, -1);
    L.declareFunc("readFileGets", readFileGets, -1);
    L.declareFunc("writeFileUserData", writeFileUserData, -1);
    L.declareFunc("writeFileUserDataAt", writeFileUserDataAt, -1);
    L.declareFunc("createPBSliceFromBuffer", createPBSliceFromBuffer, -1);
    L.pop();
    L.declareFunc(
        "sliceToPBSlice",
        [](Lua L) -> int {
            if (L.gettop() != 1) return L.error("Invalid number of arguments to sliceToPBSlice");
            if (!L.istable(1)) return L.error("sliceToPBSlice: arg 1 is not a Slice");
            L.getfieldtable("_type");
            if (!L.isstring() || (L.tostring() != "Slice")) return L.error("sliceToPBSlice: arg 1 is not a Slice");
            L.getfieldtable("_wrapper", -2);
            Slice* slice = *L.topointer<Slice*>();
            if (!slice) return L.error("sliceToPBSlice: arg 1 is corrupted");
            return lpb_newslice(L.getState(), slice->data<char>(), slice->size());
        },
        -1);
    L.pop();
    L.load(fileFFICDef, "src:lua/fileffi-cdef.lua");
    L.load(fileFFI, "src:lua/fileffi.lua");
    L.load(fileFFIMeta, "src:lua/fileffimeta.lua");
}
