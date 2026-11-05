-- lualoader, R"EOF(--
--   Copyright (C) 2022 PCSX-Redux authors
--
--   This program is free software; you can redistribute it and/or modify
--   it under the terms of the GNU General Public License as published by
--   the Free Software Foundation; either version 2 of the License, or
--   (at your option) any later version.
--
--   This program is distributed in the hope that it will be useful,
--   but WITHOUT ANY WARRANTY; without even the implied warranty of
--   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--   GNU General Public License for more details.
--
--   You should have received a copy of the GNU General Public License
--   along with this program; if not, write to the
--   Free Software Foundation, Inc.,
--   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
ffi.cdef [[

enum SectorMode {
    GUESS,     // will try and guess the sector mode based on flags found in the first sector
    RAW,       // 2352 bytes per sector
    M1,        // 2048 bytes per sector
    M2_RAW,    // 2336 bytes per sector, includes subheader; cannot be guessed
    M2_FORM1,  // 2048 bytes per sector
    M2_FORM2,  // 2324 bytes per sector
};

typedef struct { char opaque[?]; } LuaIso;
typedef struct { char opaque[?]; } IsoReader;
typedef struct { uint8_t m, s, f; } MSF;

void deleteIso(LuaIso* wrapper);
bool isIsoFailed(LuaIso* wrapper);
void isoClearPPF(LuaIso* wrapper);
void isoSavePPF(LuaIso* wrapper);
LuaIso* getCurrentIso();
LuaIso* openIso(const char* path);
LuaIso* openIsoFromFile(LuaFile* wrapper);
void cdromSetIso(LuaIso* wrapper);
void cdromEjectIso();
void cdromOpenLid();
void cdromCloseLid();
void cdromScheduleCloseLid();
bool cdromIsLidOpen();

IsoReader* createIsoReader(LuaIso* wrapper);
void deleteIsoReader(IsoReader* isoReader);
bool isReaderFailed(IsoReader* reader);
LuaFile* readerOpen(IsoReader* reader, const char* path);
LuaFile* fileisoOpen(LuaIso* wrapper, uint32_t lba, uint32_t size, enum SectorMode mode);
uint32_t getIsoTN(LuaIso* wrapper);
MSF getIsoTD(LuaIso* wrapper, uint32_t tn);

typedef struct { char opaque[?]; } DirEntries;
DirEntries* readerListDir(IsoReader* reader, const char* path);
void deleteDirEntries(DirEntries* entries);
uint32_t dirEntriesCount(DirEntries* entries);
const char* dirEntryName(DirEntries* entries, uint32_t index);
uint32_t dirEntryLBA(DirEntries* entries, uint32_t index);
uint32_t dirEntrySize(DirEntries* entries, uint32_t index);
bool dirEntryIsDir(DirEntries* entries, uint32_t index);

typedef struct { char opaque[?]; } GapList;
GapList* readerFindGaps(IsoReader* reader);
void deleteGapList(GapList* list);
uint32_t gapListCount(GapList* list);
uint32_t gapEntryLBA(GapList* list, uint32_t index);
uint32_t gapEntrySectors(GapList* list, uint32_t index);

typedef struct { char opaque[?]; } ISO9660Builder;
ISO9660Builder* createIsoBuilder(LuaFile* out);
void deleteIsoBuilder(ISO9660Builder* builder);
bool isoBuilderFailed(ISO9660Builder* builder);
void isoBuilderWriteLicense(ISO9660Builder* builder, LuaFile*);
uint32_t isoBuilderWriteSector(ISO9660Builder* builder, const uint8_t* sectorData, uint32_t dataSize, enum SectorMode mode);
uint32_t isoBuilderWriteSectorAt(ISO9660Builder* builder, const uint8_t* sectorData, uint32_t dataSize, uint32_t lba, enum SectorMode mode);
uint32_t isoBuilderGetCurrentLBA(ISO9660Builder* builder);
const char* isoBuilderClose(ISO9660Builder* builder, uint32_t threadCount);

// High-level filesystem-aware ISO builder
typedef struct { char opaque[?]; } IsoDirTree;

// PVD field getters (copy trimmed string to caller buffer)
void hlPvdGetSystemIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetVolumeIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetVolSetIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetPublisherIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetDataPreparerIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetApplicationIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetCopyrightFileIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetAbstractFileIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);
void hlPvdGetBibliographicFileIdent(ISO9660Builder* b, char* buf, uint32_t bufSize);

// PVD field setters (pad with spaces)
void hlPvdSetSystemIdent(ISO9660Builder* b, const char* val);
void hlPvdSetVolumeIdent(ISO9660Builder* b, const char* val);
void hlPvdSetVolSetIdent(ISO9660Builder* b, const char* val);
void hlPvdSetPublisherIdent(ISO9660Builder* b, const char* val);
void hlPvdSetDataPreparerIdent(ISO9660Builder* b, const char* val);
void hlPvdSetApplicationIdent(ISO9660Builder* b, const char* val);
void hlPvdSetCopyrightFileIdent(ISO9660Builder* b, const char* val);
void hlPvdSetAbstractFileIdent(ISO9660Builder* b, const char* val);
void hlPvdSetBibliographicFileIdent(ISO9660Builder* b, const char* val);

// DirTree creation
IsoDirTree* hlCreateRoot(ISO9660Builder* b, uint32_t sectorCount);
IsoDirTree* hlCreateDir(ISO9660Builder* b, IsoDirTree* parent, const char* name, uint32_t sectorCount);
IsoDirTree* hlCreateFile(ISO9660Builder* b, IsoDirTree* parent, const char* name, LuaFile* content);

// DirTree property access
const char* dirTreeGetName(IsoDirTree* node);
uint32_t dirTreeGetSize(IsoDirTree* node);
uint32_t dirTreeGetLBA(IsoDirTree* node);
bool dirTreeIsDir(IsoDirTree* node);
bool dirTreeIsHidden(IsoDirTree* node);
void dirTreeSetHidden(IsoDirTree* node, bool val);
bool dirTreeShouldSkip(IsoDirTree* node);
void dirTreeSetSkip(IsoDirTree* node, bool val);
bool dirTreeHasAnchorLBA(IsoDirTree* node);
uint32_t dirTreeGetAnchorLBA(IsoDirTree* node);
void dirTreeSetAnchorLBA(IsoDirTree* node, uint32_t lba);
void dirTreeClearAnchorLBA(IsoDirTree* node);
bool dirTreeHasDeclaredSize(IsoDirTree* node);
uint32_t dirTreeGetDeclaredSize(IsoDirTree* node);
void dirTreeSetDeclaredSize(IsoDirTree* node, uint32_t size);
void dirTreeClearDeclaredSize(IsoDirTree* node);
bool dirTreeHasXA(IsoDirTree* node);
void dirTreeSetHasXA(IsoDirTree* node, bool val);
enum SectorMode dirTreeGetSectorMode(IsoDirTree* node);
void dirTreeSetSectorMode(IsoDirTree* node, enum SectorMode mode);
uint16_t dirTreeGetXAAttribs(IsoDirTree* node);
void dirTreeSetXAAttribs(IsoDirTree* node, uint16_t val);
uint8_t dirTreeGetXAFileNum(IsoDirTree* node);
void dirTreeSetXAFileNum(IsoDirTree* node, uint8_t val);

// DirTree navigation
IsoDirTree* dirTreeParent(IsoDirTree* node);
IsoDirTree* dirTreeFirstChild(IsoDirTree* node);
IsoDirTree* dirTreeNextSibling(IsoDirTree* node);

// DirTree date access
void dirTreeSetDate(IsoDirTree* node, uint8_t year, uint8_t month, uint8_t day,
                    uint8_t hour, uint8_t minute, uint8_t second, uint8_t offset);
uint8_t dirTreeGetDateYear(IsoDirTree* node);
uint8_t dirTreeGetDateMonth(IsoDirTree* node);
uint8_t dirTreeGetDateDay(IsoDirTree* node);

]]

local C = ffi.load 'CORE_ISO'

local function createIsoReaderWrapper(isoReader)
    local reader = {
        _wrapper = ffi.gc(isoReader, C.deleteIsoReader),
        open = function(self, fname) return Support.File._createFileWrapper(C.readerOpen(self._wrapper, fname)) end,
        findGaps = function(self)
            local gapList = ffi.gc(C.readerFindGaps(self._wrapper), C.deleteGapList)
            local count = C.gapListCount(gapList)
            local result = {}
            for i = 0, count - 1 do
                table.insert(result, { lba = C.gapEntryLBA(gapList, i), sectors = C.gapEntrySectors(gapList, i) })
            end
            return result
        end,
        listDir = function(self, path)
            if path == nil then path = '' end
            local entries = ffi.gc(C.readerListDir(self._wrapper, path), C.deleteDirEntries)
            local count = C.dirEntriesCount(entries)
            local result = {}
            for i = 0, count - 1 do
                local name = ffi.string(C.dirEntryName(entries, i))
                table.insert(result, {
                    name = name,
                    lba = C.dirEntryLBA(entries, i),
                    size = C.dirEntrySize(entries, i),
                    isDir = C.dirEntryIsDir(entries, i),
                })
            end
            return result
        end,
    }
    return reader
end

local function createIsoWrapper(wrapper)
    local iso = {
        _wrapper = ffi.gc(wrapper, C.deleteIso),
        failed = function(self) return C.isIsoFailed(self._wrapper) end,
        createReader = function(self) return createIsoReaderWrapper(C.createIsoReader(self._wrapper)) end,
        clearPPF = function(self) C.isoClearPPF(self._wrapper) end,
        savePPF = function(self) C.isoSavePPF(self._wrapper) end,
        open = function(self, lba, size, mode)
            if type(size) == 'string' and mode == nil then
                mode = size
                size = -1
            end
            if size == nil then size = -1 end
            if mode == nil then mode = 'GUESS' end
            return Support.File._createFileWrapper(C.fileisoOpen(self._wrapper, lba, size, mode))
        end,
        getTD = function(self, tn) return C.getIsoTD(self._wrapper, tn) end,
        getTN = function(self) return C.getIsoTN(self._wrapper) end,
    }
    return iso
end

-- High-level ISO builder (filesystem-aware)
local pvdBuf = ffi.new('char[?]', 513) -- max CString size (512 for ApplicationUse) + 1

-- XA attribute bits of a directory record. The low bits pair up as read/execute for owner, group and world; the
-- odd bits in between are reserved.
local xaPermissionBits = {
    ownerRead = 0x0001,
    ownerExecute = 0x0004,
    groupRead = 0x0010,
    groupExecute = 0x0040,
    worldRead = 0x0100,
    worldExecute = 0x0400,
}
local xaFlagBits = { interleaved = 0x2000, cdda = 0x4000, directory = 0x8000 }
local xaFormBits = { 0x0800, 0x1000 } -- form 1 = bit 11, form 2 = bit 12
local xaFormMask = 0x1800

local function sectorModeValue(mode) return tonumber(ffi.cast('enum SectorMode', mode)) end
local sectorModeNames = { 'GUESS', 'RAW', 'M1', 'M2_RAW', 'M2_FORM1', 'M2_FORM2' }
-- M1 file contents are written as Mode 2 Form 1 sectors.
local formOfSectorMode = {
    [sectorModeValue('M1')] = 1,
    [sectorModeValue('M2_FORM1')] = 1,
    [sectorModeValue('M2_FORM2')] = 2,
}

-- The XA form a file written in this sector mode carries. Raw modes hand over whole sectors whose subheaders say
-- it per sector, so the directory record can't be derived from them.
local function xaFormForMode(mode)
    local value = sectorModeValue(mode)
    local form = formOfSectorMode[value]
    if form == nil then
        error('the XA form of a ' .. (sectorModeNames[value + 1] or tostring(mode)) ..
                  ' file cannot be derived from its sector mode, pass form = 1 or 2 to setXA')
    end
    return form
end

-- Computes the XA attributes for a setXA{} field table. Returns the attributes, and whether the form bits were
-- derived from the node's sector mode and should keep following it.
local function xaAttribsFromFields(node, fields)
    for name, value in pairs(fields) do
        if name == 'form' then
            if value ~= 1 and value ~= 2 then error('setXA: form must be 1 or 2, got ' .. tostring(value)) end
        elseif xaPermissionBits[name] or xaFlagBits[name] then
            if type(value) ~= 'boolean' then error('setXA: field ' .. name .. ' must be a boolean') end
        else
            error('setXA: unknown field ' .. tostring(name))
        end
    end
    local isDir = C.dirTreeIsDir(node)
    local attribs = 0
    for name, mask in pairs(xaPermissionBits) do
        if fields[name] ~= false then attribs = bit.bor(attribs, mask) end
    end
    local flags = { interleaved = false, cdda = false, directory = isDir }
    for name, mask in pairs(xaFlagBits) do
        local value = fields[name]
        if value == nil then value = flags[name] end
        if value then attribs = bit.bor(attribs, mask) end
    end
    local form, following = fields.form, false
    if form == nil then
        if isDir then
            form = 1
        elseif not fields.cdda then
            form = xaFormForMode(C.dirTreeGetSectorMode(node))
            following = true
        end
    end
    if form then attribs = bit.bor(attribs, xaFormBits[form]) end
    return attribs, following
end

local function nodeKey(node) return tonumber(ffi.cast('uintptr_t', node)) end

local function createDirTreeWrapper(node, xaFollowing)
    if node == nil then return nil end
    local wrapper = {
        _node = node,
        _xaFollowing = xaFollowing,
        getName = function(self) return ffi.string(C.dirTreeGetName(self._node)) end,
        getSize = function(self) return C.dirTreeGetSize(self._node) end,
        getLBA = function(self) return C.dirTreeGetLBA(self._node) end,
        isDir = function(self) return C.dirTreeIsDir(self._node) end,
        isHidden = function(self) return C.dirTreeIsHidden(self._node) end,
        setHidden = function(self, val) C.dirTreeSetHidden(self._node, val) end,
        shouldSkip = function(self) return C.dirTreeShouldSkip(self._node) end,
        setSkip = function(self, val) C.dirTreeSetSkip(self._node, val) end,
        hasAnchorLBA = function(self) return C.dirTreeHasAnchorLBA(self._node) end,
        getAnchorLBA = function(self) return C.dirTreeGetAnchorLBA(self._node) end,
        setAnchorLBA = function(self, lba) C.dirTreeSetAnchorLBA(self._node, lba) end,
        clearAnchorLBA = function(self) C.dirTreeClearAnchorLBA(self._node) end,
        hasDeclaredSize = function(self) return C.dirTreeHasDeclaredSize(self._node) end,
        getDeclaredSize = function(self) return C.dirTreeGetDeclaredSize(self._node) end,
        setDeclaredSize = function(self, size) C.dirTreeSetDeclaredSize(self._node, size) end,
        clearDeclaredSize = function(self) C.dirTreeClearDeclaredSize(self._node) end,
        hasXA = function(self) return C.dirTreeHasXA(self._node) end,
        setXA = function(self, fields)
            local key = nodeKey(self._node)
            if fields == false then
                self._xaFollowing[key] = nil
                C.dirTreeSetHasXA(self._node, false)
                return
            end
            if fields == true then fields = {} end
            if type(fields) ~= 'table' then error('setXA expects a boolean or a table of fields') end
            local attribs, following = xaAttribsFromFields(self._node, fields)
            C.dirTreeSetHasXA(self._node, true)
            C.dirTreeSetXAAttribs(self._node, attribs)
            self._xaFollowing[key] = following or nil
        end,
        getSectorMode = function(self) return sectorModeNames[tonumber(C.dirTreeGetSectorMode(self._node)) + 1] end,
        setSectorMode = function(self, mode)
            if self._xaFollowing[nodeKey(self._node)] then
                local form = xaFormForMode(mode)
                local attribs = C.dirTreeGetXAAttribs(self._node)
                C.dirTreeSetXAAttribs(self._node, bit.bor(bit.band(attribs, bit.bnot(xaFormMask)), xaFormBits[form]))
            end
            C.dirTreeSetSectorMode(self._node, mode)
        end,
        getXAAttribs = function(self) return C.dirTreeGetXAAttribs(self._node) end,
        setXAAttribs = function(self, val)
            self._xaFollowing[nodeKey(self._node)] = nil
            C.dirTreeSetXAAttribs(self._node, val)
        end,
        getXAFileNum = function(self) return C.dirTreeGetXAFileNum(self._node) end,
        setXAFileNum = function(self, val) C.dirTreeSetXAFileNum(self._node, val) end,
        parent = function(self) return createDirTreeWrapper(C.dirTreeParent(self._node), self._xaFollowing) end,
        firstChild = function(self) return createDirTreeWrapper(C.dirTreeFirstChild(self._node), self._xaFollowing) end,
        nextSibling = function(self) return createDirTreeWrapper(C.dirTreeNextSibling(self._node), self._xaFollowing) end,
        setDate = function(self, year, month, day, hour, minute, second, offset)
            C.dirTreeSetDate(self._node, year or 0, month or 0, day or 0, hour or 0, minute or 0, second or 0,
                             offset or 0)
        end,
    }
    return wrapper
end

-- Amount of data a raw sector write consumes, per mode. Modes absent from this table can't be written.
local sectorDataSize = { RAW = 2352, M2_RAW = 2336, M2_FORM1 = 2048, M2_FORM2 = 2324 }

-- Normalizes the (data[, size]) pair of the raw sector writers. Strings carry their own size; anything
-- else is a pointer and needs one spelled out.
local function sectorData(data, size, mode)
    if size == nil then
        if type(data) ~= 'string' then error('a size is required when writing a sector from a pointer') end
        size = #data
    end
    local needed = sectorDataSize[mode]
    if needed == nil then error('sector mode ' .. tostring(mode) .. ' cannot be written') end
    if size < needed then error('sector mode ' .. mode .. ' needs ' .. needed .. ' bytes of data, got ' .. size) end
    return ffi.cast('const uint8_t*', data), size
end

local function createIsoBuilderWrapper(wrapper)
    local function pvdGetter(getter)
        return function(self)
            getter(self._wrapper, pvdBuf, 513)
            return ffi.string(pvdBuf)
        end
    end
    local builder = {
        _wrapper = ffi.gc(wrapper, C.deleteIsoBuilder),
        -- Nodes whose XA form bits were derived by setXA and follow their sector mode, keyed by node address.
        _xaFollowing = {},
        failed = function(self) return C.isoBuilderFailed(self._wrapper) end,
        getCurrentLBA = function(self) return C.isoBuilderGetCurrentLBA(self._wrapper) end,
        writeSector = function(self, data, a, b)
            local size, mode
            if type(a) == 'number' then
                size, mode = a, b
            else
                size, mode = nil, a
            end
            if mode == nil then mode = 'M2_FORM1' end
            local ptr, len = sectorData(data, size, mode)
            return C.isoBuilderWriteSector(self._wrapper, ptr, len, mode)
        end,
        writeSectorAt = function(self, data, a, b, c)
            local size, lba, mode
            if type(b) == 'number' then
                size, lba, mode = a, b, c
            else
                size, lba, mode = nil, a, b
            end
            if mode == nil then mode = 'M2_FORM1' end
            local ptr, len = sectorData(data, size, mode)
            return C.isoBuilderWriteSectorAt(self._wrapper, ptr, len, lba, mode)
        end,
        writeLicense = function(self, file)
            if file then
                C.isoBuilderWriteLicense(self._wrapper, file._wrapper)
            else
                C.isoBuilderWriteLicense(self._wrapper, nil)
            end
        end,
        -- PVD getters
        getSystemIdent = pvdGetter(C.hlPvdGetSystemIdent),
        getVolumeIdent = pvdGetter(C.hlPvdGetVolumeIdent),
        getVolSetIdent = pvdGetter(C.hlPvdGetVolSetIdent),
        getPublisherIdent = pvdGetter(C.hlPvdGetPublisherIdent),
        getDataPreparerIdent = pvdGetter(C.hlPvdGetDataPreparerIdent),
        getApplicationIdent = pvdGetter(C.hlPvdGetApplicationIdent),
        getCopyrightFileIdent = pvdGetter(C.hlPvdGetCopyrightFileIdent),
        getAbstractFileIdent = pvdGetter(C.hlPvdGetAbstractFileIdent),
        getBibliographicFileIdent = pvdGetter(C.hlPvdGetBibliographicFileIdent),
        -- PVD setters
        setSystemIdent = function(self, val) C.hlPvdSetSystemIdent(self._wrapper, val) end,
        setVolumeIdent = function(self, val) C.hlPvdSetVolumeIdent(self._wrapper, val) end,
        setVolSetIdent = function(self, val) C.hlPvdSetVolSetIdent(self._wrapper, val) end,
        setPublisherIdent = function(self, val) C.hlPvdSetPublisherIdent(self._wrapper, val) end,
        setDataPreparerIdent = function(self, val) C.hlPvdSetDataPreparerIdent(self._wrapper, val) end,
        setApplicationIdent = function(self, val) C.hlPvdSetApplicationIdent(self._wrapper, val) end,
        setCopyrightFileIdent = function(self, val) C.hlPvdSetCopyrightFileIdent(self._wrapper, val) end,
        setAbstractFileIdent = function(self, val) C.hlPvdSetAbstractFileIdent(self._wrapper, val) end,
        setBibliographicFileIdent = function(self, val) C.hlPvdSetBibliographicFileIdent(self._wrapper, val) end,
        -- Tree building
        createRoot = function(self, sectorCount)
            return createDirTreeWrapper(C.hlCreateRoot(self._wrapper, sectorCount or 1), self._xaFollowing)
        end,
        createDir = function(self, parent, name, sectorCount)
            return createDirTreeWrapper(C.hlCreateDir(self._wrapper, parent._node, name, sectorCount or 1),
                                        self._xaFollowing)
        end,
        createFile = function(self, parent, name, fileHandle)
            return createDirTreeWrapper(C.hlCreateFile(self._wrapper, parent._node, name, fileHandle._wrapper),
                                        self._xaFollowing)
        end,
        close = function(self, threadCount)
            local err = C.isoBuilderClose(self._wrapper, threadCount or 0)
            if err ~= nil then error(ffi.string(err)) end
        end,
    }
    return builder
end

PCSX.getCurrentIso = function() return createIsoWrapper(C.getCurrentIso()) end
PCSX.openIso = function(arg)
    if type(arg) == 'string' then
        return createIsoWrapper(C.openIso(arg))
    else
        return createIsoWrapper(C.openIsoFromFile(arg._wrapper))
    end
end
PCSX.CDRom = {
    -- Takes a path, a File, or an iso returned by PCSX.openIso.
    setIso = function(arg)
        if type(arg) ~= 'table' or arg._wrapper == nil or arg.createReader == nil then arg = PCSX.openIso(arg) end
        C.cdromSetIso(arg._wrapper)
    end,
    ejectIso = function() C.cdromEjectIso() end,
    openLid = function() C.cdromOpenLid() end,
    closeLid = function() C.cdromCloseLid() end,
    -- Opens the lid now and closes it one emulated second later.
    scheduleCloseLid = function() C.cdromScheduleCloseLid() end,
    isLidOpen = function() return C.cdromIsLidOpen() end,
}
PCSX.isoBuilder = function(file) return createIsoBuilderWrapper(C.createIsoBuilder(file._wrapper)) end
PCSX.isoTools = {
    fromBCD = function(bcd)
        local dec = 0
        local mul = 1
        while bcd ~= 0 do
            local digit = bcd % 16
            if digit >= 10 then error('Invalid BCD digit: ' .. digit) end
            dec = dec + mul * digit
            mul = mul * 10
            bcd = math.floor(bcd / 16)
        end
        return dec
    end,
    toBCD = function(dec)
        local bcd = 0
        local mul = 1
        while dec ~= 0 do
            local digit = dec % 10
            bcd = bcd + mul * digit
            mul = mul * 16
            dec = math.floor(dec / 10)
        end
        return bcd
    end,
    fromMSF = function(m, s, f)
        m = PCSX.isoTools.fromBCD(m)
        s = PCSX.isoTools.fromBCD(s)
        f = PCSX.isoTools.fromBCD(f)
        if s >= 60 then error('Invalid MSF seconds: ' .. s) end
        if f >= 75 then error('Invalid MSF frames: ' .. f) end
        return (m * 60 + s) * 75 + f - 150
    end,
    toMSF = function(lba)
        lba = lba + 150
        local f = lba % 75
        lba = lba - f
        lba = lba / 75
        local s = lba % 60
        lba = lba - s
        local m = lba / 60
        return m, s, f
    end,
}

-- )EOF"
