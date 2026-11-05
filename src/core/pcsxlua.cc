/***************************************************************************
 *   Copyright (C) 2021 PCSX-Redux authors                                 *
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

#include "core/pcsxlua.h"

#include <algorithm>
#include <vector>

#include "core/callstacks.h"
#include "core/cputrace.h"
#include "core/debug.h"
#include "core/disr3000a.h"
#include "core/gpu.h"
#include "core/gpudump.h"
#include "core/gpulogger.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/spulogger.h"
#include "core/sstate.h"
#include "lua/luafile.h"
#include "lua/luawrapper.h"

namespace {

struct LuaBreakpoint {
    PCSX::Debug::BreakpointUserListType wrapper;
};

uint64_t getCPUCycles() { return PCSX::g_emulator->m_cpu->m_regs.cycle; }
void* getMemPtr() { return PCSX::g_emulator->m_mem->m_wram; }
void* getParPtr() { return PCSX::g_emulator->m_mem->m_exp1; }
void* getRomPtr() { return PCSX::g_emulator->m_mem->m_bios; }
void* getScratchPtr() { return PCSX::g_emulator->m_mem->m_hard; }
void* getRegisters() { return &PCSX::g_emulator->m_cpu->m_regs; }
void setPC(uint32_t pc) { PCSX::g_emulator->m_cpu->setPC(pc); }
void setGPR(unsigned index, uint32_t value) { PCSX::g_emulator->m_cpu->setGPR(index, value); }
void* getReadLUT() { return PCSX::g_emulator->m_mem->m_readLUT; }
void* getWriteLUT() { return PCSX::g_emulator->m_mem->m_writeLUT; }

LuaBreakpoint* addBreakpoint(uint32_t address, PCSX::Debug::BreakpointType type, unsigned width, const char* cause,
                             bool (*invoker)(uint32_t address, unsigned width, const char* cause), const char* label) {
    if (!PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
             .get<PCSX::Emulator::DebugSettings::Debug>()) {
        PCSX::g_system->luaMessage(
            "Warning: the debugger is disabled, this breakpoint will not trigger until it is enabled (use -debugger)",
            false);
    }
    LuaBreakpoint* ret = new LuaBreakpoint();
    auto* bp = PCSX::g_emulator->m_debug->addBreakpoint(
        address, type, width, std::string("Lua Breakpoint"), std::string(label && *label ? label : cause),
        [invoker](const PCSX::Debug::Breakpoint* self, uint32_t address, unsigned width, const char* cause) {
            try {
                return invoker(address, width, cause);
            } catch (...) {
                PCSX::g_system->luaMessage("Lua Breakpoint invoker threw an exception, deleting breakpoint", true);
                return false;
            }
        });

    ret->wrapper.push_back(bp);
    return ret;
}
void enableBreakpoint(LuaBreakpoint* wrapper) {
    if (wrapper->wrapper.size() == 0) return;
    wrapper->wrapper.begin()->enable();
}
void disableBreakpoint(LuaBreakpoint* wrapper) {
    if (wrapper->wrapper.size() == 0) return;
    wrapper->wrapper.begin()->disable();
}
bool breakpointEnabled(LuaBreakpoint* wrapper) {
    if (wrapper->wrapper.size() == 0) return false;
    return wrapper->wrapper.begin()->enabled();
}
void removeBreakpoint(LuaBreakpoint* wrapper) {
    if (!wrapper) return;
    wrapper->wrapper.destroyAll();
    delete wrapper;
}
void pauseEmulator() { PCSX::g_system->pause(); }
void resumeEmulator() { PCSX::g_system->resume(); }
void softResetEmulator() { PCSX::g_system->softReset(); }
void hardResetEmulator() { PCSX::g_system->hardReset(); }
void luaMessage(const char* msg, bool error) { PCSX::g_system->luaMessage(msg, error); }
void luaLog(const char* msg) { PCSX::g_system->log(PCSX::LogClass::LUA, msg); }
void jumpToPC(uint32_t pc) { PCSX::g_system->m_eventBus->signal(PCSX::Events::GUI::JumpToPC{pc}); }
void jumpToMemory(uint32_t address, unsigned width) {
    PCSX::g_system->m_eventBus->signal(PCSX::Events::GUI::JumpToMemory{address, width});
}
void invalidateCache() { PCSX::g_emulator->m_cpu->invalidateCache(); }

uint64_t getCpuTraceSize() { return PCSX::g_emulator->m_cpuTrace->size(); }
void clearCpuTrace() { PCSX::g_emulator->m_cpuTrace->clear(); }
bool getCpuTraceEnabled() {
    return PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
        .get<PCSX::Emulator::DebugSettings::Trace>()
        .value;
}
void setCpuTraceEnabled(bool enabled) {
    PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
        .get<PCSX::Emulator::DebugSettings::Trace>()
        .value = enabled;
}
// Bulk-copy `count` fixed records starting at `start` into a caller-provided flat
// buffer (an FFI PCSX_TraceEntry[]). The store is chunked, so this is the export
// path that flattens it for Lua: alloc an array, pull a range, dump/diff/inspect.
// Returns the number actually copied (clamped to what exists).
uint64_t readCpuTrace(void* dest, uint64_t start, uint64_t count) {
    auto& trace = *PCSX::g_emulator->m_cpuTrace;
    uint64_t total = trace.size();
    if (start >= total) return 0;
    if (count > total - start) count = total - start;
    auto* out = reinterpret_cast<PCSX::TraceEntry*>(dest);
    for (uint64_t i = 0; i < count; i++) out[i] = trace[start + i];
    return count;
}

bool exportCpuTraceText(const char* path) {
    PCSX::IO<PCSX::File> file = new PCSX::PosixFile(path, PCSX::FileOps::TRUNCATE);
    if (file->failed()) return false;
    return PCSX::g_emulator->m_cpuTrace->exportText(file);
}
bool saveCpuTrace(const char* path) {
    PCSX::IO<PCSX::File> file = new PCSX::PosixFile(path, PCSX::FileOps::TRUNCATE);
    if (file->failed()) return false;
    return PCSX::g_emulator->m_cpuTrace->save(file);
}
bool loadCpuTrace(const char* path) {
    PCSX::IO<PCSX::File> file = new PCSX::PosixFile(path);
    if (file->failed()) return false;
    return PCSX::g_emulator->m_cpuTrace->load(file);
}

struct LuaScreenShot {
    PCSX::Slice* data;
    uint16_t width, height;
    decltype(PCSX::GPU::ScreenShot::bpp) bpp;
};

LuaScreenShot takeScreenShot() {
    LuaScreenShot ret;
    auto ss = PCSX::g_emulator->m_gpu->takeScreenShot();
    ret.data = new PCSX::Slice(std::move(ss.data));
    ret.width = ss.width;
    ret.height = ss.height;
    ret.bpp = ss.bpp;
    return ret;
}

double getGuestFPS() { return PCSX::g_emulator->m_gpuLogger->getGuestFPS(); }

void setGPULogging(bool enabled, bool overdrawStats) {
    auto logger = PCSX::g_emulator->m_gpuLogger.get();
    logger->setEnabled(enabled);
    logger->m_overdrawStats = overdrawStats;
}

struct LuaOverdrawStats {
    bool valid;
    uint64_t total;
    uint32_t pixels, max;
    uint32_t buckets[5];
};

uint64_t getOverdrawSum(bool read, int x, int y, int w, int h) {
    return PCSX::g_emulator->m_gpuLogger->getOverdrawSum(read, x, y, w, h);
}

LuaOverdrawStats getOverdrawStats(bool read) {
    LuaOverdrawStats ret = {};
    auto logger = PCSX::g_emulator->m_gpuLogger.get();
    if (!logger->hasOverdrawStats()) return ret;
    auto& stats = read ? logger->getReadOverdraw() : logger->getWrittenOverdraw();
    ret.valid = true;
    ret.total = stats.total;
    ret.pixels = stats.pixels;
    ret.max = stats.max;
    for (unsigned i = 0; i < 5; i++) ret.buckets[i] = stats.buckets[i];
    return ret;
}

void startGPUDump(PCSX::LuaFFI::LuaFile* file) { PCSX::g_emulator->m_gpuDumper->start(file->file); }
void stopGPUDump() { PCSX::g_emulator->m_gpuDumper->stop(); }
bool isGPUDumpArmed() { return PCSX::g_emulator->m_gpuDumper->armed(); }
bool isGPUDumpRecording() { return PCSX::g_emulator->m_gpuDumper->recording(); }
uint64_t getGPUDumpFrames() { return PCSX::g_emulator->m_gpuDumper->frames(); }

bool loadGPUDumpPlayer(PCSX::LuaFFI::LuaFile* file) { return PCSX::g_emulator->m_gpuDumpPlayer->load(file->file); }
void unloadGPUDumpPlayer() { PCSX::g_emulator->m_gpuDumpPlayer->unload(); }
bool stepGPUDumpPlayer() { return PCSX::g_emulator->m_gpuDumpPlayer->step(); }
void rewindGPUDumpPlayer() { PCSX::g_emulator->m_gpuDumpPlayer->rewind(); }
uint64_t getGPUDumpPlayerFrame() { return PCSX::g_emulator->m_gpuDumpPlayer->frame(); }
uint64_t getGPUDumpPlayerFrameCount() { return PCSX::g_emulator->m_gpuDumpPlayer->frameCount(); }
bool seekGPUDumpPlayer(uint64_t frame) { return PCSX::g_emulator->m_gpuDumpPlayer->seek(frame); }
void setGPUDumpPlayerLogging(bool enabled) { PCSX::g_emulator->m_gpuDumpPlayer->logger()->setLogging(enabled); }
uint32_t getGPUDumpPlayerLoggedCount() { return PCSX::g_emulator->m_gpuDumpPlayer->logger()->loggedCount(); }
PCSX::Slice* getGPUDumpPlayerVRAM() {
    auto gpu = PCSX::g_emulator->m_gpuDumpPlayer->gpu();
    if (!gpu) return new PCSX::Slice();
    return new PCSX::Slice(gpu->getVRAM(PCSX::GPU::Ownership::ACQUIRE));
}
void setSPULoggerEnabled(bool enabled) { PCSX::g_emulator->m_spuLogger->setEnabled(enabled); }
bool isSPULoggerEnabled() { return PCSX::g_emulator->m_spuLogger->enabled(); }
void clearSPULogger() { PCSX::g_emulator->m_spuLogger->clear(); }
uint64_t getSPULoggerEventCount() { return PCSX::g_emulator->m_spuLogger->size(); }
static std::string s_spuLoggerExportError;
static std::vector<uint8_t> s_spuLoggerExport;
// Two steps, so a filename is only opened, and truncated, once the capture is known to export.
bool prepareSPULoggerSPD(uint32_t tickRate, bool fullRam) {
    PCSX::SPD::Options options;
    options.tickRate = tickRate;
    options.fullRam = fullRam;
    s_spuLoggerExport.clear();
    s_spuLoggerExportError = PCSX::g_emulator->m_spuLogger->exportSPD(s_spuLoggerExport, options);
    return s_spuLoggerExportError.empty();
}
bool writeSPULoggerSPD(PCSX::LuaFFI::LuaFile* file) {
    if (file->file->failed()) {
        s_spuLoggerExportError = "unable to open the SPD file";
        s_spuLoggerExport.clear();
        return false;
    }
    auto size = static_cast<ssize_t>(s_spuLoggerExport.size());
    bool ok = file->file->write(s_spuLoggerExport.data(), s_spuLoggerExport.size()) == size;
    if (!ok) s_spuLoggerExportError = "failed to write the whole SPD file";
    s_spuLoggerExport.clear();
    return ok;
}
const char* getSPULoggerExportError() { return s_spuLoggerExportError.c_str(); }

PCSX::Slice* getVRAM() { return new PCSX::Slice(PCSX::g_emulator->m_gpu->getVRAM(PCSX::GPU::Ownership::ACQUIRE)); }

PCSX::Slice* createSaveState() {
    auto ss = PCSX::SaveStates::save();
    return new PCSX::Slice(std::move(ss));
}

void loadSaveStateFromSlice(PCSX::Slice* data) { PCSX::SaveStates::loadSafe(std::string(data->asStringView())); }

void loadSaveStateFromFile(PCSX::LuaFFI::LuaFile* file) {
    auto data = file->file->readAt(64 * 1024 * 1024, 0);
    PCSX::SaveStates::loadSafe(std::string(data.asStringView()));
}

void createRewindState() { PCSX::g_emulator->createRewindState(); }
bool rewindState() { return PCSX::g_emulator->rewindState(); }
uint32_t getRewindStateCount() { return static_cast<uint32_t>(PCSX::g_emulator->rewindStateCount()); }

PCSX::LuaFFI::LuaFile* getMemoryAsFile() {
    return new PCSX::LuaFFI::LuaFile(PCSX::g_emulator->m_mem->getMemoryAsFile());
}

void quit(int code) { PCSX::g_system->quit(code); }

/* Built on first use, since constructSaveState() binds references to live emulator members and
   there is no point holding twenty megabytes of message for a session that never snapshots.
   Those references die with the emulator, so open_pcsx() drops it for each new Lua state. */
std::unique_ptr<PCSX::SaveStates::SaveState> s_scratchState;
// Whether captureState() has filled the slot yet: restoring the empty message would zero the machine.
bool s_scratchStateCaptured = false;
PCSX::SaveStates::SaveState& scratchState() {
    if (!s_scratchState) {
        s_scratchState.reset(new PCSX::SaveStates::SaveState(PCSX::SaveStates::constructSaveState()));
    }
    return *s_scratchState;
}
// callGuest's full isolation has its own, so a guest call doesn't replace what captureState() took.
std::unique_ptr<PCSX::SaveStates::SaveState> s_guestState;
PCSX::SaveStates::SaveState& guestState() {
    if (!s_guestState) {
        s_guestState.reset(new PCSX::SaveStates::SaveState(PCSX::SaveStates::constructSaveState()));
    }
    return *s_guestState;
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
    L.push("PCSX");
    L.newtable();
    REGISTER(L, getCPUCycles);
    REGISTER(L, getMemPtr);
    REGISTER(L, getParPtr);
    REGISTER(L, getRomPtr);
    REGISTER(L, getScratchPtr);
    REGISTER(L, getRegisters);
    REGISTER(L, setPC);
    REGISTER(L, setGPR);
    REGISTER(L, getReadLUT);
    REGISTER(L, getWriteLUT);
    REGISTER(L, addBreakpoint);
    REGISTER(L, enableBreakpoint);
    REGISTER(L, disableBreakpoint);
    REGISTER(L, breakpointEnabled);
    REGISTER(L, removeBreakpoint);
    REGISTER(L, pauseEmulator);
    REGISTER(L, resumeEmulator);
    REGISTER(L, softResetEmulator);
    REGISTER(L, hardResetEmulator);
    REGISTER(L, luaMessage);
    REGISTER(L, luaLog);
    REGISTER(L, jumpToPC);
    REGISTER(L, jumpToMemory);
    REGISTER(L, invalidateCache);
    REGISTER(L, getCpuTraceSize);
    REGISTER(L, clearCpuTrace);
    REGISTER(L, getCpuTraceEnabled);
    REGISTER(L, setCpuTraceEnabled);
    REGISTER(L, readCpuTrace);
    REGISTER(L, exportCpuTraceText);
    REGISTER(L, saveCpuTrace);
    REGISTER(L, loadCpuTrace);
    REGISTER(L, takeScreenShot);
    REGISTER(L, getGuestFPS);
    REGISTER(L, setGPULogging);
    REGISTER(L, getOverdrawStats);
    REGISTER(L, getOverdrawSum);
    REGISTER(L, startGPUDump);
    REGISTER(L, setSPULoggerEnabled);
    REGISTER(L, isSPULoggerEnabled);
    REGISTER(L, clearSPULogger);
    REGISTER(L, getSPULoggerEventCount);
    REGISTER(L, prepareSPULoggerSPD);
    REGISTER(L, writeSPULoggerSPD);
    REGISTER(L, getSPULoggerExportError);
    REGISTER(L, stopGPUDump);
    REGISTER(L, isGPUDumpArmed);
    REGISTER(L, isGPUDumpRecording);
    REGISTER(L, getGPUDumpFrames);
    REGISTER(L, loadGPUDumpPlayer);
    REGISTER(L, unloadGPUDumpPlayer);
    REGISTER(L, stepGPUDumpPlayer);
    REGISTER(L, rewindGPUDumpPlayer);
    REGISTER(L, getGPUDumpPlayerFrame);
    REGISTER(L, getGPUDumpPlayerFrameCount);
    REGISTER(L, seekGPUDumpPlayer);
    REGISTER(L, setGPUDumpPlayerLogging);
    REGISTER(L, getGPUDumpPlayerLoggedCount);
    REGISTER(L, getGPUDumpPlayerVRAM);
    REGISTER(L, getVRAM);
    REGISTER(L, createSaveState);
    REGISTER(L, loadSaveStateFromSlice);
    REGISTER(L, loadSaveStateFromFile);
    REGISTER(L, createRewindState);
    REGISTER(L, rewindState);
    REGISTER(L, getRewindStateCount);
    REGISTER(L, getMemoryAsFile);
    REGISTER(L, quit);
    L.settable();
    L.pop();
}

void PCSX::LuaFFI::open_pcsx(Lua L) {
    static int lualoader = 1;
    static const char* pcsxFFI = (
#include "core/pcsxffi.lua"
    );
    registerAllSymbols(L);
    s_scratchState.reset();
    s_scratchStateCaptured = false;
    s_guestState.reset();
    L.load(pcsxFFI, "src:core/pcsxffi.lua");
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.push("execSlots");
    L.newtable();
    L.settable();
    /* One reusable snapshot slot, so the measurement is of capture() and not of building a
       20MB message. This is the shape isolate wants too: a harness snapshots into the same
       place thousands of times. */
    L.declareFunc(
        "captureState",
        [](lua_State* L_) -> int {
            // The slot is what a queued restoreState() is going to bring back.
            if (g_system->hasPendingRestore()) {
                Lua L(L_);
                return L.error("captureState: a restoreState() is still waiting for the main loop");
            }
            SaveStates::capture(scratchState());
            s_scratchStateCaptured = true;
            return 0;
        },
        -1);
    L.declareFunc(
        "restoreState",
        [](lua_State* L_) -> int {
            if (!s_scratchStateCaptured) {
                Lua L(L_);
                return L.error("restoreState: nothing to restore, call captureState() first");
            }
            // Same as SaveStates::loadSafe(): from a callback on the emulation stack, restoring
            // now would swap the state under the frames that called it, so the main loop does it.
            if (g_system->inExecute()) {
                g_system->scheduleRestore([]() { SaveStates::restore(scratchState()); });
            } else {
                SaveStates::restore(scratchState());
            }
            return 0;
        },
        -1);
    L.declareFunc(
        "callGuest",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if ((L.gettop() != 1) || !L.istable(1)) {
                return L.error("callGuest takes a single table argument");
            }
            auto field = [&L](const char* name, uint32_t def, bool* present = nullptr) -> uint32_t {
                L.getfield(name, 1);
                uint32_t ret = def;
                if (L.isnumber()) {
                    ret = uint32_t(int64_t(L.tonumber()));
                    if (present) *present = true;
                }
                L.pop();
                return ret;
            };

            bool hasPC = false;
            const uint32_t pc = field("pc", 0, &hasPC);
            if (!hasPC) return L.error("callGuest needs a pc to call");
            if (pc & 3) return L.error("callGuest: pc 0x%08x isn't aligned", pc);
            /* The sentinel only ever gets compared against, never fetched from, so it just
               has to be aligned and somewhere the callee will never legitimately jump. */
            const uint32_t ra = field("ra", 0x8f000000);
            if (ra & 3) return L.error("callGuest: ra sentinel 0x%08x isn't aligned", ra);
            const uint64_t cycles = field("cycles", 100000000);

            auto& regs = g_emulator->m_cpu->m_regs;
            /* Default to the top of RAM, which is where the BIOS leaves the stack anyway.
               Callers with a live program should hand us something of their own. */
            uint32_t sp = field("sp", ((g_emulator->getRamMask<1>() + 1) - 16) | 0x80000000);
            if (sp & 7) return L.error("callGuest: sp 0x%08x isn't 8-byte aligned", sp);

            std::vector<uint32_t> args;
            L.getfield("args", 1);
            if (L.istable()) {
                size_t n = L.length();
                for (size_t i = 1; i <= n; i++) {
                    L.rawgeti(i);
                    args.push_back(uint32_t(int64_t(L.tonumber())));
                    L.pop();
                }
            } else if (!L.isnil()) {
                L.pop();
                return L.error("callGuest: args has to be a table");
            }
            L.pop();

            /* Full machine by default when isolation is asked for at all. "ram" is the lever
               that says "I know this callee only touches memory" - cheaper, and blind to
               anything else it turns out to touch. nonRamAccesses in the result is how you
               check that claim rather than trusting it. */
            enum { NoIsolation, FullIsolation, RamIsolation } isolation = NoIsolation;
            L.getfield("isolate", 1);
            if (L.isnumber()) {
                return L.error("callGuest: isolate takes true, false, 'full' or 'ram'");
            } else if (L.isstring()) {
                auto mode = L.tostring();
                if (mode == "ram") {
                    isolation = RamIsolation;
                } else if (mode == "full") {
                    isolation = FullIsolation;
                } else {
                    L.pop();
                    return L.error("callGuest: isolate takes true, false, 'full' or 'ram'");
                }
            } else if (L.toboolean()) {
                isolation = FullIsolation;
            }
            L.pop();
            const bool isolate = isolation == RamIsolation;
            L.getfield("dirty", 1);
            const bool wantDirty = L.toboolean();
            L.pop();

            /* Snapshot before anything mutates state. Memory::write32 bumps m_regs.cycle by
               one per access, so staging the stack arguments first would leak into the
               clock we're about to promise we left alone. */
            const uint32_t ramSize = g_emulator->getRamMask<1>() + 1;
            const auto savedGPR = regs.GPR;
            const auto savedCP0 = regs.CP0;
            const auto savedCP2D = regs.CP2D;
            const auto savedCP2C = regs.CP2C;
            const uint32_t savedPC = regs.pc;
            const uint32_t savedCode = regs.code;
            const uint64_t savedCycle = regs.cycle;
            /* Read-only peek at the callstack monitor. We deliberately don't drive it: a frame
               is opened by the callee spilling $ra, not by us, and CallStacks::setSP has a
               branch that destroys non-matching stacks, so a naive save/restore is unsafe. */
            auto& callStacks = g_emulator->m_callStacks;
            const unsigned depthBefore = callStacks->hasCurrent() ? callStacks->getCurrent().calls.size() : 0;

            /* o32: a0-a3 in registers, the rest on the stack, and the caller owes the callee
               a 16-byte argument save area whether it uses it or not. Argument n lands at
               sp + 4 * (n - 1), so the fifth is the first one that actually goes to memory. */
            size_t frame = std::max<size_t>(16, args.size() * 4);
            frame = (frame + 7) & ~size_t(7);
            sp -= frame;
            /* Arguments past the fourth go through the bus, so the frame has to sit in RAM or
               the scratchpad; anywhere else those writes would land on hardware registers. */
            if (args.size() > 4) {
                const uint32_t phys = sp & 0x1fffffff;
                const bool inRam = uint64_t(phys) + frame <= 0x00800000;
                const bool inScratchpad = phys >= 0x1f800000 && uint64_t(phys) + frame <= 0x1f800400;
                if (!inRam && !inScratchpad) {
                    return L.error("callGuest: stack frame at 0x%08x isn't in RAM or the scratchpad", sp);
                }
            }

            /* Check every stage and fetch range before touching anything, so a bad one can't
               leave half the inputs in place, or a guest run that no rollback follows. */
            std::vector<std::pair<uint32_t, std::string>> stages;
            L.getfield("stage", 1);
            if (L.istable()) {
                size_t n = L.length();
                for (size_t i = 1; i <= n; i++) {
                    L.rawgeti(i);
                    L.getfield("addr");
                    uint32_t addr = uint32_t(int64_t(L.tonumber()));
                    L.pop();
                    L.getfield("data");
                    auto data = L.tostring();
                    L.pop();
                    L.pop();
                    uint32_t off = addr & (ramSize - 1);
                    if (uint64_t(off) + data.size() > ramSize) {
                        L.pop();
                        return L.error("callGuest: stage at 0x%08x runs off the end of RAM", addr);
                    }
                    stages.emplace_back(off, std::move(data));
                }
            }
            L.pop();
            std::vector<std::pair<uint32_t, uint32_t>> fetches;
            L.getfield("fetch", 1);
            if (L.istable()) {
                size_t n = L.length();
                for (size_t i = 1; i <= n; i++) {
                    L.rawgeti(i);
                    L.getfield("addr");
                    uint32_t addr = uint32_t(int64_t(L.tonumber()));
                    L.pop();
                    L.getfield("size");
                    uint32_t size = uint32_t(int64_t(L.tonumber()));
                    L.pop();
                    L.pop();
                    uint32_t off = addr & (ramSize - 1);
                    if (uint64_t(off) + size > ramSize) {
                        L.pop();
                        return L.error("callGuest: fetch at 0x%08x runs off the end of RAM", addr);
                    }
                    fetches.emplace_back(off, size);
                }
            }
            L.pop();

            /* Full isolation snapshots before the inputs go in, so the restore takes them back
               out along with whatever the callee did. */
            if (isolation == FullIsolation) SaveStates::capture(guestState());

            std::vector<uint32_t> stackBefore;
            for (size_t i = 4; i < args.size(); i++) {
                stackBefore.push_back(g_emulator->m_mem->read32(sp + i * 4));
                g_emulator->m_mem->write32(sp + i * 4, args[i]);
            }

            /* Stage inputs straight into wram rather than through write32: this is the host
               placing a buffer, not the guest storing to one, so it has no business being
               gated on the guest's cache-isolation state. Keep each range's previous contents
               so the rollback can undo the staging too - otherwise "the machine comes back
               clear" would quietly mean "clear except for whatever I just put in it". */
            std::vector<std::pair<uint32_t, std::string>> stagedBefore;
            auto* wram = g_emulator->m_mem->m_wram;
            for (const auto& [off, data] : stages) {
                stagedBefore.emplace_back(off, std::string(reinterpret_cast<const char*>(wram + off), data.size()));
                memcpy(wram + off, data.data(), data.size());
            }
            g_emulator->m_cpu->invalidateCache();
            /* Takes the inputs back out, newest first since stage ranges may overlap. */
            auto undoInputs = [&]() {
                for (auto it = stagedBefore.rbegin(); it != stagedBefore.rend(); it++) {
                    memcpy(wram + it->first, it->second.data(), it->second.size());
                }
                for (size_t i = 0; i < stackBefore.size(); i++) {
                    g_emulator->m_mem->write32(sp + (i + 4) * 4, stackBefore[i]);
                }
                g_emulator->m_cpu->invalidateCache();
            };

            /* Snapshot RAM AFTER staging, so the dirty-page report is purely what the CALLEE
               touched and not an echo of the input we just placed. Reused across calls: a
               harness makes thousands of these, and reallocating a couple of megabytes each
               time is pure waste. */
            const uint64_t nonRamBefore = g_emulator->m_mem->m_nonRamAccesses;

            static std::vector<uint8_t> ramSnapshot;
            static std::vector<uint8_t> scratchSnapshot;
            if (isolate || wantDirty) {
                ramSnapshot.resize(ramSize);
                scratchSnapshot.resize(0x400);
                memcpy(ramSnapshot.data(), g_emulator->m_mem->m_wram, ramSize);
                memcpy(scratchSnapshot.data(), g_emulator->m_mem->m_hard, 0x400);
            }

            for (size_t i = 0; i < 4; i++) {
                regs.GPR.r[4 + i] = i < args.size() ? args[i] : 0;
            }
            regs.GPR.n.sp = sp;
            regs.GPR.n.ra = ra;
            bool hasGP = false;
            uint32_t gp = field("gp", 0, &hasGP);
            if (hasGP) regs.GPR.n.gp = gp;
            regs.pc.raw() = pc;

            auto outcome = g_emulator->m_cpu->RunUntil(ra, cycles);
            if (outcome == R3000Acpu::RunUntilResult::Reentered) {
                undoInputs();
                regs.GPR = savedGPR;
                regs.pc.raw() = savedPC;
                regs.cycle = savedCycle;
                return L.error(
                    "callGuest can't be nested: this one was called from inside another guest call, from a Lua "
                    "handler run by one of its accesses, such as a PCSX.execSlots function. An ExecutionFlow event "
                    "listener is a fine place to call from; the middle of an instruction is not.");
            }
            if (outcome == R3000Acpu::RunUntilResult::Unsupported) {
                undoInputs();
                regs.GPR = savedGPR;
                regs.pc.raw() = savedPC;
                regs.cycle = savedCycle;
                return L.error(
                    "callGuest needs the interpreter: the recompilers emit no per-instruction checks, so they can't "
                    "be stopped on an arbitrary pc. Start with -interpreter, or turn the dynarec off in Emulation "
                    "settings and reboot the emulator.");
            }

            const uint32_t v0 = regs.GPR.n.v0;
            const uint32_t v1 = regs.GPR.n.v1;
            const uint64_t spent = regs.cycle - savedCycle;
            const unsigned depthAfter = callStacks->hasCurrent() ? callStacks->getCurrent().calls.size() : 0;
            const uint32_t faultPC = regs.pc;
            const uint32_t cause = regs.CP0.n.Cause;
            const uint32_t epc = regs.CP0.n.EPC;
            const uint32_t badVAddr = regs.CP0.n.BadVAddr;

            /* Read outputs out before the rollback, or isolation would eat the answer. */
            std::vector<std::string> fetched;
            for (const auto& [off, size] : fetches) {
                fetched.emplace_back(reinterpret_cast<const char*>(wram + off), size);
            }

            /* The changed-page list is the interesting half: it answers "did the callee write
               anywhere it had no business writing", which for a routine with no destination
               bounds check is the whole question. The compare is a byte or two of work per
               page on top of a rollback that has to touch the memory anyway. */
            const uint64_t nonRamAccesses = g_emulator->m_mem->m_nonRamAccesses - nonRamBefore;

            std::vector<uint32_t> dirty;
            if (isolate || wantDirty) {
                /* Compare and restore in one pass, and only touch the pages that actually
                   moved. A blanket restore of all of RAM costs the same whether the callee
                   wrote one page or every page, and it is nearly always one. */
                for (uint32_t page = 0; page < ramSize; page += 0x10000) {
                    if (memcmp(g_emulator->m_mem->m_wram + page, ramSnapshot.data() + page, 0x10000) != 0) {
                        dirty.push_back(0x80000000 | page);
                        if (isolate) memcpy(g_emulator->m_mem->m_wram + page, ramSnapshot.data() + page, 0x10000);
                    }
                }
            }
            if (isolate) {
                memcpy(g_emulator->m_mem->m_hard, scratchSnapshot.data(), 0x400);
                undoInputs();
            } else if (isolation == FullIsolation) {
                SaveStates::restore(guestState());
            }

            regs.GPR = savedGPR;
            regs.CP0 = savedCP0;
            regs.CP2D = savedCP2D;
            regs.CP2C = savedCP2C;
            regs.pc.raw() = savedPC;
            regs.code = savedCode;
            regs.cycle = savedCycle;

            L.newtable();
            L.push("status");
            switch (outcome) {
                case R3000Acpu::RunUntilResult::Reached:
                    L.push("returned");
                    break;
                case R3000Acpu::RunUntilResult::OutOfCycles:
                    L.push("cycles");
                    break;
                case R3000Acpu::RunUntilResult::Exception:
                    L.push("exception");
                    break;
                default:
                    L.push("unknown");
                    break;
            }
            L.settable();
            L.push("v0");
            L.push(lua_Number(v0));
            L.settable();
            L.push("v1");
            L.push(lua_Number(v1));
            L.settable();
            L.push("cycles");
            L.push(lua_Number(spent));
            L.settable();
            /* A leaf callee never spills $ra, so it never opens a frame and this stays 0.
               A non-zero delta after a clean return means the callee unwound badly. */
            L.push("depth");
            L.push(lua_Number(int32_t(depthAfter) - int32_t(depthBefore)));
            L.settable();
            /* setLuts() nulls the whole RAM write LUT whenever the BIU says the caches are
               isolated, which is also the state a cold emulator boots into. Every guest store
               then vanishes, and the unknown-address log that would have said so is gated on
               the same predicate, so it vanishes quietly. Worth saying out loud to anyone
               using this to check what a routine WROTE. */
            L.push("storesDropped");
            L.push(g_emulator->m_mem->m_writeLUT[0x8000] == nullptr);
            L.settable();
            /* Zero means the callee provably touched nothing but RAM, so isolate = 'ram' is
               sound for it. Non-zero means it isn't, whatever the caller believed. */
            L.push("nonRamAccesses");
            L.push(lua_Number(nonRamAccesses));
            L.settable();
            L.push("out");
            L.newtable();
            for (size_t i = 0; i < fetched.size(); i++) {
                L.push(fetched[i].data(), fetched[i].size());
                L.rawseti(i + 1);
            }
            L.settable();
            L.push("dirty");
            L.newtable();
            for (size_t i = 0; i < dirty.size(); i++) {
                L.push(lua_Number(dirty[i]));
                L.rawseti(i + 1);
            }
            L.settable();
            if (outcome == R3000Acpu::RunUntilResult::Exception) {
                L.push("exceptionCode");
                L.push(lua_Number((cause >> 2) & 0x1f));
                L.settable();
                L.push("epc");
                L.push(lua_Number(epc));
                L.settable();
                L.push("badVAddr");
                L.push(lua_Number(badVAddr));
                L.settable();
            } else if (outcome == R3000Acpu::RunUntilResult::OutOfCycles) {
                L.push("pc");
                L.push(lua_Number(faultPC));
                L.settable();
            }
            return 1;
        },
        -1);
    L.declareFunc(
        "getSaveStateProtoSchema",
        [](lua_State* L_) -> int {
            Lua L(L_);
            std::ostringstream os;
            SaveStates::ProtoFile::dumpSchema(os);
            L.push(os.str());
            return 1;
        },
        -1);
    L.declareFunc(
        "getCpuTraceLine",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() != 1) {
                return L.error("Wrong number of arguments to getCpuTraceLine");
            }
            uint64_t idx = L.checknumber(1);
            auto& trace = *g_emulator->m_cpuTrace;
            if (idx >= trace.size()) {
                return L.error("getCpuTraceLine: index out of range");
            }
            const TraceEntry& e = trace[idx];
            PlaybackValueSource source(e);
            L.push(Disasm::asString(e.code, 0, e.pc, nullptr, true, &source));
            return 1;
        },
        -1);
    L.declareFunc(
        "insertSymbol",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() != 2) {
                return L.error("Wrong number of arguments to insertSymbol");
            }
            uint32_t address = L.checknumber(1);
            auto name = L.tostring(2);
            g_emulator->m_cpu->m_symbols[address] = name;
            return 0;
        },
        -1);
    L.declareFunc(
        "removeSymbol",
        [](lua_State* L_) -> int {
            auto& symbols = g_emulator->m_cpu->m_symbols;
            Lua L(L_);
            if (L.gettop() != 1) {
                return L.error("Wrong number of arguments to insertSymbol");
            }
            auto i = symbols.begin();
            if (L.isnumber()) {
                i = symbols.find(L.tonumber());
            } else {
                auto name = L.tostring();
                while (i != symbols.end()) {
                    if (i->second == name) break;
                    i++;
                }
            }
            if (i != symbols.end()) symbols.erase(i);
            return 0;
        },
        -1);
    L.declareFunc(
        "iterateSymbols",
        [](lua_State* L_) -> int {
            Lua L(L_);
            L.push([](lua_State* L_) -> int {
                auto& symbols = g_emulator->m_cpu->m_symbols;
                Lua L(L_);
                if (L.gettop() != 2) {
                    return L.error("Wrong number of arguments");
                }
                auto iter = symbols.begin();
                if (L.isnumber(-1)) {
                    iter = symbols.find(L.tonumber(-1));
                    if (iter != symbols.end()) iter++;
                }
                if (iter != symbols.end()) {
                    L.push(lua_Number(iter->first));
                    L.push(iter->second);
                    return 2;
                }
                return 0;
            });
            L.push();
            L.push();
            return 3;
        },
        -1);
    L.pop();
}
