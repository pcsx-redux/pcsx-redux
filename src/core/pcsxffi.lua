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
typedef union {
    struct {
        uint32_t r0, at, v0, v1, a0, a1, a2, a3;
        uint32_t t0, t1, t2, t3, t4, t5, t6, t7;
        uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
        uint32_t t8, t9, k0, k1, gp, sp, s8, ra;
        uint32_t lo, hi;
    } n;
    uint32_t r[34];
} psxGPRRegs;

typedef union {
    uint32_t r[32];
} psxCP0Regs;

typedef union {
    uint32_t r[32];
} psxCP2Data;

typedef union {
    uint32_t r[32];
} psxCP2Ctrl;

typedef struct {
    psxGPRRegs GPR;
    psxCP0Regs CP0;
    psxCP2Data CP2D;
    psxCP2Ctrl CP2C;
    const uint32_t rawPC;
} psxRegisters;

enum BreakpointType { Exec, Read, Write };
typedef struct { uint8_t opaque[?]; } Breakpoint;

uint64_t getCPUCycles();
uint8_t* getMemPtr();
uint8_t* getParPtr();
uint8_t* getRomPtr();
uint8_t* getScratchPtr();
psxRegisters* getRegisters();
void setPC(uint32_t pc);
void setGPR(unsigned index, uint32_t value);
uint8_t** getReadLUT();
uint8_t** getWriteLUT();
Breakpoint* addBreakpoint(uint32_t address, enum BreakpointType type, unsigned width, const char* cause, bool (*invoker)(uint32_t address, unsigned width, const char* cause), const char* label);
void enableBreakpoint(Breakpoint*);
void disableBreakpoint(Breakpoint*);
bool breakpointEnabled(Breakpoint*);
void removeBreakpoint(Breakpoint*);
void pauseEmulator();
void resumeEmulator();
void softResetEmulator();
void hardResetEmulator();
void luaMessage(const char* msg, bool error);
void luaLog(const char* msg);
void jumpToPC(uint32_t address);
void jumpToMemory(uint32_t address, unsigned width);
void invalidateCache();

typedef struct {
    uint32_t pc, code, regRs, regRt, regRd, hilo, cop, memValue;
    uint64_t cycle;
} PCSX_TraceEntry;

uint64_t getCpuTraceSize();
void clearCpuTrace();
bool getCpuTraceEnabled();
void setCpuTraceEnabled(bool enabled);
uint64_t readCpuTrace(void* dest, uint64_t start, uint64_t count);
bool exportCpuTraceText(const char* path);
bool saveCpuTrace(const char* path);
bool loadCpuTrace(const char* path);

typedef enum { BPP_16, BPP_24 } ScreenShotBPP;

typedef struct {
    LuaSlice* data;
    uint16_t width, height;
    ScreenShotBPP bpp;
} LuaScreenShot;

LuaScreenShot takeScreenShot();
double getGuestFPS();
void setGPULogging(bool enabled, bool overdrawStats);
typedef struct {
    bool valid;
    uint64_t total;
    uint32_t pixels, max;
    uint32_t buckets[5];
} LuaOverdrawStats;
LuaOverdrawStats getOverdrawStats(bool read);
uint64_t getOverdrawSum(bool read, int x, int y, int w, int h);
void startGPUDump(LuaFile*);
void stopGPUDump();
bool isGPUDumpArmed();
bool isGPUDumpRecording();
uint64_t getGPUDumpFrames();
bool loadGPUDumpPlayer(LuaFile*);
void unloadGPUDumpPlayer();
bool stepGPUDumpPlayer();
void rewindGPUDumpPlayer();
uint64_t getGPUDumpPlayerFrame();
void setGPUDumpPlayerLogging(bool enabled);
uint32_t getGPUDumpPlayerLoggedCount();
LuaSlice* getGPUDumpPlayerVRAM();
LuaSlice* getVRAM();
void setSPULoggerEnabled(bool);
bool isSPULoggerEnabled();
void clearSPULogger();
uint64_t getSPULoggerEventCount();
bool prepareSPULoggerSPD(uint32_t tickRate, bool fullRam);
bool writeSPULoggerSPD(LuaFile*);
const char* getSPULoggerExportError();

LuaSlice* createSaveState();
void loadSaveStateFromSlice(LuaSlice*);
void loadSaveStateFromFile(LuaFile*);

void createRewindState();
bool rewindState();
uint32_t getRewindStateCount();

LuaFile* getMemoryAsFile();

void quit(int code);
]]

local C = ffi.load 'PCSX'

-- Writing pc goes through the cpu, which drops a branch still pending in a delay slot.
ffi.metatype('psxRegisters', {
    __index = function(regs, key)
        if key == 'pc' then return regs.rawPC end
        error('\'psxRegisters\' has no member named \'' .. tostring(key) .. '\'')
    end,
    __newindex = function(regs, key, value)
        if key == 'pc' then return C.setPC(value) end
        error('\'psxRegisters\' has no member named \'' .. tostring(key) .. '\'')
    end,
})

local function removeBreakpoint(bp)
    C.removeBreakpoint(ffi.gc(bp._wrapper, nil))
    bp._wrapper = ffi.cast('Breakpoint*', 0)
    if bp._invokercb ~= nil then
        bp._invokercb:free()
        bp._invokercb = nil
    end
end

local function defaultInvoker(address, width, cause)
    C.pauseEmulator()
    return true
end

local validBpTypes = { Exec = true, Read = true, Write = true }

local function addBreakpoint(address, bptype, width, cause, invoker, label)
    if type(address) ~= 'number' then error 'PCSX.addBreakpoint needs an address' end
    if bptype == nil then bptype = 'Exec' end
    if not validBpTypes[bptype] then error 'PCSX.addBreakpoint needs a valid breakpoint type' end
    if width == nil then width = 4 end
    if type(width) ~= 'number' then error 'PCSX.addBreakpoint needs a width that is a number' end
    if cause == nil then cause = '' end
    if type(cause) == 'function' and invoker == nil then
        invoker = cause
        cause = ''
    end
    if type(cause) ~= 'string' then error 'PCSX.addBreakpoint needs a cause that is a string' end
    local invokercb = defaultInvoker
    if invoker ~= nil then
        if type(invoker) ~= 'function' then error 'PCSX.addBreakpoint needs an invoker that is a function' end
        invokercb = function(address, width, cause)
            cause = ffi.string(cause)
            -- This runs as an FFI callback: a Lua error must not unwind out of it into C++.
            local ok, ret = pcall(invoker, address, width, cause)
            if not ok then
                local printed, msg = pcall(tostring, ret)
                if not printed then msg = '(error object could not be converted to a string)' end
                C.luaMessage('Lua breakpoint invoker failed, deleting breakpoint: ' .. msg, true)
                return false
            end
            if ret == false then
                return false
            else
                return true
            end
        end
    end
    if label == nil then label = '' end
    if type(label) ~= 'string' then error 'PCSX.addBreakpoint needs a label that is a string' end
    invokercb = ffi.cast('bool (*)(uint32_t address, unsigned width, const char* cause)', invokercb)
    local wrapper = C.addBreakpoint(address, bptype, width, cause, invokercb, label)
    local bp = {
        _wrapper = wrapper,
        _proxy = newproxy(),
        _invokercb = invokercb,
        enable = function(bp) C.enableBreakpoint(bp._wrapper) end,
        disable = function(bp) C.disableBreakpoint(bp._wrapper) end,
        isEnabled = function(bp) return C.breakpointEnabled(bp._wrapper) end,
        remove = function(bp) removeBreakpoint(bp) end,
    }
    -- Use a proxy instead of doing this on the wrapper directly using ffi.gc, because of a bug in LuaJIT,
    -- where circular references on finalizers using ffi.gc won't actually collect anything.
    debug.setmetatable(bp._proxy, { __gc = function() removeBreakpoint(bp) end })
    return bp
end

local function printLike(callback, ...)
    local s = ''
    for i, v in ipairs({ ... }) do s = s .. tostring(v) .. ' ' end
    callback(s)
end

local function jumpToPC(pc)
    if type(pc) ~= 'number' then error 'PCSX.GUI.jumpToPC requires a numeric address' end
    C.jumpToPC(pc)
end

local function jumpToMemory(address, width)
    if type(address) ~= 'number' then error 'PCSX.GUI.jumpToMemory requires a numeric address' end
    if width == nil then width = 1 end
    if type(width) ~= 'number' then error 'PCSX.GUI.jumpToMemory requires a numeric width' end
    C.jumpToMemory(address, width)
end

PCSX = {
    getCPUCycles = function() return C.getCPUCycles() end,
    getMemPtr = function() return C.getMemPtr() end,
    getParPtr = function() return C.getParPtr() end,
    getRomPtr = function() return C.getRomPtr() end,
    getScratchPtr = function() return C.getScratchPtr() end,
    getRegisters = function() return C.getRegisters() end,
    -- Unlike a store into getRegisters().GPR, also drops a load still pending for that register.
    setGPR = function(index, value)
        if type(index) ~= 'number' or index < 0 or index > 33 or index % 1 ~= 0 then
            error 'PCSX.setGPR needs a register index between 0 and 33'
        end
        C.setGPR(index, value)
    end,
    getReadLUT = function() return C.getReadLUT() end,
    getWriteLUT = function() return C.getWriteLUT() end,
    addBreakpoint = addBreakpoint,
    pauseEmulator = function() C.pauseEmulator() end,
    resumeEmulator = function() C.resumeEmulator() end,
    softResetEmulator = function() C.softResetEmulator() end,
    hardResetEmulator = function() C.hardResetEmulator() end,
    invalidateCache = function() C.invalidateCache() end,
    log = function(...) printLike(function(msg) C.luaLog(msg .. '\n') end, ...) end,
    GUI = { jumpToPC = jumpToPC, jumpToMemory = jumpToMemory },
    CPU = {
        -- Binary CPU trace. Records are fixed PCSX_TraceEntry structs; the store is
        -- chunked, so read() flattens a range into a fresh FFI array
        -- you can inspect, ffi.string(), or write to a File. getLine() renders the
        -- same annotated disassembly the viewer shows, reconstructed from the record.
        -- A record's cycle is the emulated cycle counter at capture; records loaded
        -- from a version 1 trace file carry 0xffffffffffffffffULL there.
        Trace = {
            size = function() return tonumber(C.getCpuTraceSize()) end,
            clear = function() C.clearCpuTrace() end,
            enabled = function() return C.getCpuTraceEnabled() end,
            enable = function() C.setCpuTraceEnabled(true) end,
            disable = function() C.setCpuTraceEnabled(false) end,
            getLine = function(idx)
                if type(idx) ~= 'number' then error 'PCSX.CPU.Trace.getLine requires a numeric index' end
                return PCSX.getCpuTraceLine(idx)
            end,
            read = function(start, count)
                start = start or 0
                local total = tonumber(C.getCpuTraceSize())
                if start >= total then return nil, 0 end
                if count == nil or count > total - start then count = total - start end
                if count <= 0 then return nil, 0 end
                local arr = ffi.new('PCSX_TraceEntry[?]', count)
                local got = tonumber(C.readCpuTrace(arr, start, count))
                return arr, got
            end,
            exportText = function(path) return C.exportCpuTraceText(path) end,
            save = function(path) return C.saveCpuTrace(path) end,
            load = function(path) return C.loadCpuTrace(path) end,
        },
    },
    nextTick = function(f)
        local oldCleanup = AfterPollingCleanup
        AfterPollingCleanup = function()
            if oldCleanup then oldCleanup() end
            f()
        end
    end,
    SPU = {
        Logger = {
            enable = function() C.setSPULoggerEnabled(true) end,
            disable = function() C.setSPULoggerEnabled(false) end,
            isEnabled = function() return C.isSPULoggerEnabled() end,
            clear = function() C.clearSPULogger() end,
            getEventCount = function() return tonumber(C.getSPULoggerEventCount()) end,
            -- Writes the captured register stream as an SPU dump. tickRate is 16.16 Hz and
            -- defaults to 1/16th of the hblank rate. Returns true, or false and the reason.
            exportSPD = function(file, tickRate, fullRam)
                local isName = type(file) == 'string'
                if not isName and (type(file) ~= 'table' or file._type ~= 'File') then
                    error('exportSPD: requires a File or a filename')
                end
                if not C.prepareSPULoggerSPD(tickRate or 0, fullRam and true or false) then
                    return false, ffi.string(C.getSPULoggerExportError())
                end
                if isName then file = Support.File.open(file, 'TRUNCATE') end
                if C.writeSPULoggerSPD(file._wrapper) then return true end
                return false, ffi.string(C.getSPULoggerExportError())
            end,
        },
    },
    GPU = {
        takeScreenShot = function()
            local ss = C.takeScreenShot()
            return {
                data = Support.File._createSliceWrapper(ss.data),
                width = ss.width,
                height = ss.height,
                bpp = ss.bpp,
            }
        end,
        getGuestFPS = function() return C.getGuestFPS() end,
        setLogging = function(enabled, overdrawStats) C.setGPULogging(enabled, overdrawStats == true) end,
        getOverdrawSum = function(read, x, y, w, h) return tonumber(C.getOverdrawSum(read == true, x, y, w, h)) end,
        getOverdrawStats = function(read)
            local s = C.getOverdrawStats(read == true)
            if not s.valid then return nil end
            local total = tonumber(s.total)
            return {
                total = total,
                pixels = s.pixels,
                max = s.max,
                mean = s.pixels > 0 and total / s.pixels or 0,
                buckets = { s.buckets[0], s.buckets[1], s.buckets[2], s.buckets[3], s.buckets[4] },
            }
        end,
        getVRAM = function() return Support.File._createSliceWrapper(C.getVRAM()) end,
        startDump = function(file)
            if type(file) == 'string' then file = Support.File.open(file, 'TRUNCATE') end
            if type(file) ~= 'table' or file._type ~= 'File' then
                error('startDump: requires a File or a filename')
            end
            C.startGPUDump(file._wrapper)
        end,
        stopDump = function() C.stopGPUDump() end,
        isDumpArmed = function() return C.isGPUDumpArmed() end,
        isDumping = function() return C.isGPUDumpRecording() end,
        getDumpFrames = function() return tonumber(C.getGPUDumpFrames()) end,
        DumpPlayer = {
            load = function(file)
                if type(file) == 'string' then file = Support.File.open(file) end
                if type(file) ~= 'table' or file._type ~= 'File' then
                    error('load: requires a File or a filename')
                end
                return C.loadGPUDumpPlayer(file._wrapper)
            end,
            unload = function() C.unloadGPUDumpPlayer() end,
            step = function() return C.stepGPUDumpPlayer() end,
            rewind = function() C.rewindGPUDumpPlayer() end,
            getFrame = function() return tonumber(C.getGPUDumpPlayerFrame()) end,
            -- The player's own GPU logger, holding the commands of the last frame played.
            setLogging = function(enabled) C.setGPUDumpPlayerLogging(enabled) end,
            getLoggedCount = function() return C.getGPUDumpPlayerLoggedCount() end,
            getVRAM = function() return Support.File._createSliceWrapper(C.getGPUDumpPlayerVRAM()) end,
        },
    },
    createSaveState = function()
        local slice = C.createSaveState()
        return Support.File._createSliceWrapper(slice)
    end,
    loadSaveState = function(obj)
        if type(obj) ~= 'table' then error('loadSaveState: requires an object as input') end
        if obj._type == 'Slice' then
            C.loadSaveStateFromSlice(obj._wrapper)
        elseif obj._type == 'File' then
            C.loadSaveStateFromFile(obj._wrapper)
        else
            error('loadSaveState: requires a Slice or File as input')
        end
    end,
    getMemoryAsFile = function() return Support.File._createFileWrapper(C.getMemoryAsFile()) end,
    createRewindState = function() C.createRewindState() end,
    rewindState = function() return C.rewindState() end,
    getRewindStateCount = function() return C.getRewindStateCount() end,
    quit = function(code) C.quit(code or 0) end,
}

print = function(...) printLike(function(s) C.luaMessage(s, false) end, ...) end
printError = function(...) printLike(function(s) C.luaMessage(s, true) end, ...) end
bit.extract = function(x, n, w) return bit.rshift(bit.band(x, bit.lshift(1, w) - 1), n) end

-- )EOF"
