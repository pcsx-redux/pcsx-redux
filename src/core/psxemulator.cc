/***************************************************************************
 *   Copyright (C) 2023 PCSX-Redux authors                                 *
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

#include "core/psxemulator.h"

#include "core/alloc-tracker.h"
#include "core/callstacks.h"
#include "core/cdrom.h"
#include "core/cdromlogger.h"
#include "core/cputrace.h"
#include "core/debug.h"
#include "core/eventslua.h"
#include "core/gdb-server.h"
#include "core/gpu.h"
#include "core/gpudump.h"
#include "core/gpulogger.h"
#include "core/gte.h"
#include "core/luaiso.h"
#include "core/mdec.h"
#include "core/pad.h"
#include "core/patchmanager.h"
#include "core/pcsxlua.h"
#include "core/pio-cart.h"
#include "core/r3000a.h"
#include "core/ramlogger.h"
#include "core/shmdisplay.h"
#include "core/sio.h"
#include "core/sio1-server.h"
#include "core/sio1.h"
#include "core/spulogger.h"
#include "core/sstate.h"
#include "core/web-server.h"
#include "gpu/soft/interface.h"
#include "lua/compressionffi.h"
#include "lua/extra.h"
#include "lua/luafile.h"
#include "lua/luawrapper.h"
#include "lua/luv-glue.h"
#include "lua/zlibffi.h"
#include "luafilesystem/src/lfs.h"
#include "spu/interface.h"
#include "supportpsx/adpcmlua.h"
#include "supportpsx/assembler.h"
#include "supportpsx/binlua.h"

extern "C" int luaopen_lpeg(lua_State* L);

struct PCSX::Emulator::RewindRing {
    // front() is the oldest snapshot, back() the most recent. Entries popped off the ring are
    // parked in m_spare and reused, so steady-state capture does not reallocate the messages.
    struct Entry {
        std::unique_ptr<SaveStates::SaveState> state;
        uint64_t cycle;
    };
    std::deque<Entry> m_states;
    std::vector<std::unique_ptr<SaveStates::SaveState>> m_spare;

    std::unique_ptr<SaveStates::SaveState> acquire() {
        if (!m_spare.empty()) {
            auto state = std::move(m_spare.back());
            m_spare.pop_back();
            return state;
        }
        return std::unique_ptr<SaveStates::SaveState>(new SaveStates::SaveState(SaveStates::constructSaveState()));
    }
    void clear() {
        while (!m_states.empty()) {
            m_spare.push_back(std::move(m_states.back().state));
            m_states.pop_back();
        }
    }
};

PCSX::Emulator::Emulator()
    : m_callStacks(new PCSX::CallStacks),
      m_cdrom(PCSX::CDRom::factory()),
      m_cdromLogger(new PCSX::CDRomLogger()),
      m_counters(new PCSX::Counters()),
      m_cpuTrace(new PCSX::CpuTrace()),
      m_debug(new PCSX::Debug()),
      m_gdbServer(new PCSX::GdbServer()),
      m_gpuDumper(new PCSX::GPUDumper()),
      m_gpuDumpPlayer(new PCSX::GPUDumpPlayer()),
      m_gpuLogger(new PCSX::GPULogger()),
      m_gte(new PCSX::GTE()),
      m_ramLogger(new PCSX::RAMLogger()),
      m_spuLogger(new PCSX::SPULogger()),
      m_hw(new PCSX::HW()),
      m_lua(new PCSX::Lua()),
      m_mdec(new PCSX::MDEC()),
      m_mem(new PCSX::Memory()),
      m_pads(PCSX::Pads::factory()),
      m_patchManager(new PatchManager()),
      m_pioCart(new PCSX::PIOCart),
      m_sio(new PCSX::SIO()),
      m_sio1(new PCSX::SIO1()),
      m_sio1Server(new PCSX::SIO1Server()),
      m_sio1Client(new PCSX::SIO1Client()),
      m_spu(new PCSX::SPU::impl()),
      m_webServer(new PCSX::WebServer()),
      m_rewindRing(new RewindRing()),
      m_allocTracker(new PCSX::AllocTracker) {
    auto L = *m_lua;
    L.openlibs();
}

void PCSX::Emulator::setLua() {
    auto L = *m_lua;
    L.declareFunc("t_", [](lua_State* L_) -> int {
        Lua L(L_);
        if (L.gettop() != 1) {
            return L.error("t_ expects 1 argument");
        }
        if (!L.isstring(1)) {
            return L.error("t_ expects a string");
        }
        auto str = L.tostring(1);
        L.push(g_system->getStr(djb::hash(str), str.c_str()));
        return 1;
    });
    L.load("ffi = require('ffi')", "internal:setffi.lua");
    // Before anything that consumes `bit`. src/core/pcsxffi.lua defines
    // bit.extract at file scope, so on a backend without the LuaJIT bit library
    // that is an index of a nil value and the whole chunk dies at startup.
    // No-op where `bit` already exists.
    {
        // The lualoader / paren dance is load-bearing: the .lua file is valid
        // Lua AND a C++ raw string literal at once, and the leading `--` of its
        // first line becomes a pre-decrement discarded by the comma operator.
        // Without the parentheses it is a syntax error, not a string.
        static int lualoader = 1;
        static const char* bitshim = (
#include "lua/bitshim.lua"
        );
        L.load(bitshim, "src:lua/bitshim.lua");
    }

    // The _CLIBS adapter, before ANY ffi file: it wraps ffi.cdef to accumulate
    // declarations, and a library cdef'd before the wrapper is installed would
    // be invisible to it. No-op on LuaJIT, where lj_clib.c does this in C.
    {
        static int lualoader = 1;
        static const char* clibs = (
#include "lua/clibs.lua"
        );
        L.load(clibs, "src:lua/clibs.lua");
    }
    LuaFFI::open_zlib(L);
    openLuv(L);
    luaopen_lfs(L.getState());
    L.pop(3);
    luaopen_lpeg(L.getState());
    L.pop(2);
    LuaFFI::open_file(L);
    LuaFFI::open_pcsx(L);
    LuaFFI::open_iso(L);
    LuaFFI::open_extra(L);
    LuaBindings::open_events(L);
    LuaFFI::open_compression(L);
    LuaSupportPSX::open_adpcm(L);
    LuaSupportPSX::open_assembler(L);
    LuaSupportPSX::open_binaries(L);

    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.getfieldtable("settings");
    L.push("emulator");
    settings.pushValue(L);
    L.settable();
    L.pop();
    L.pop();

    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.getfieldtable("CONSTS");
    L.getfieldtable("CPU");
    L.push(lua_Number(m_psxClockSpeed));
    L.setfield("CLOCKSPEED");
    L.pop();
    L.pop();
    L.pop();

    m_pads->setLua(L);

    assert(L.gettop() == 0);
}

PCSX::Emulator::~Emulator() {
    // TODO: move Lua to g_system.
    m_lua->close();
}

int PCSX::Emulator::init() {
    assert(g_system);
    if (m_mem->init() == -1) return -1;
    int ret = R3000Acpu::psxInit();

    const auto& args = g_system->getArgs();

    m_gpu = settings.get<SettingHardwareRenderer>() ? GPU::getOpenGL() : GPU::getSoft();
    m_gpu->setDumper(m_gpuDumper.get());
    if (args.isShmDisplayEnabled()) m_shmDisplay = std::make_unique<ShmDisplay>();

    setPGXPMode(m_config.PGXP_Mode);
    m_sio->init();
    return ret;
}

void PCSX::Emulator::reset() {
    m_mem->reset();
    m_spu->resetCaptureBuffer();
    m_cpu->psxReset();
    m_gpu->reset();
    m_pads->shutdown();
    m_pads->init();
    m_pads->reset();
    m_sio->reset();
    m_sio1->reset();
    m_rewindRing->clear();
    m_rewindCapturePending = false;
    m_rewindRestartPending = false;
    m_vsyncPending = 0;
}

void PCSX::Emulator::shutdown() {
    m_mem->shutdown();
    m_cpu->psxShutdown();

    m_pads->shutdown();
}

void PCSX::Emulator::vsync() {
    m_gpuDumper->vsync(m_gpu.get(), m_cpu->m_regs.cycle);
    m_gpu->vblank();
    // vsync() can be reached from inside an instruction, through a root counter read or write
    // calling Counters::update(). There, the recompilers haven't written back m_regs.pc yet, and
    // the end of the block overwrites whatever a listener puts in it. The listeners and everything
    // else that may touch the cpu state run from branchTest() instead, between blocks. Linked
    // blocks skip branchTest(), so more than one vsync can be pending by the time it runs.
    m_vsyncPending++;
}

void PCSX::Emulator::dispatchVSync() {
    while (m_vsyncPending) {
        m_vsyncPending--;
        g_system->m_eventBus->signal<Events::GPU::VSync>({});
        g_system->update(true);
        // A frame has been produced, so let Execute() unwind at the next block
        // boundary. Unconditional rather than wasm-only: on desktop this costs one
        // extra return and re-entry per frame against a few million instruction
        // dispatches, and one code path is worth more than that.
        m_cpu->m_frameDone = true;

        const uint32_t interval = settings.get<SettingRewindInterval>().value;
        if (interval > 0 && !(++m_rewind_counter % interval)) {
            createRewindState();
        }
    }
}

void PCSX::Emulator::createRewindState() {
    // From inside the emulation, the snapshot waits for the end of the current branchTest(),
    // see flushRewindCapture().
    if (g_system->inExecute()) {
        m_rewindCapturePending = true;
        return;
    }
    captureRewindState();
}

void PCSX::Emulator::flushRewindCapture() {
    if (!m_rewindCapturePending) return;
    const bool restarting = m_rewindRestartPending;
    m_rewindCapturePending = false;
    m_rewindRestartPending = false;
    captureRewindState(restarting);
}

void PCSX::Emulator::captureRewindState(bool restarting) {
    // A queued rewind is about to throw this state away, and would land on this very snapshot
    // rather than the one it was asked for. Unless the ring was just restarted: then this snapshot
    // is the oldest one that can still be replayed, and the right place for that rewind to land.
    if (g_system->pendingRewinds() && !restarting) return;
    if (m_rewindCaptureSuppressed) return;
    auto& ring = *m_rewindRing;
    auto state = ring.acquire();
    SaveStates::capture(*state);
    ring.m_states.push_back({std::move(state), m_cpu->m_regs.cycle});
    // Bound the ring. RewindCount == 0 means unbounded (use with care).
    const uint32_t count = settings.get<SettingRewindCount>().value;
    if (count > 0) {
        while (ring.m_states.size() > count) {
            ring.m_spare.push_back(std::move(ring.m_states.front().state));
            ring.m_states.pop_front();
        }
        // Never hold more messages than the ring can use, or lowering the count strands them.
        while (!ring.m_spare.empty() && ring.m_states.size() + ring.m_spare.size() > count) {
            ring.m_spare.pop_back();
        }
    }
}

bool PCSX::Emulator::rewindState() {
    if (!g_system->inExecute()) return rewindStateNow();
    // Same as SaveStates::loadSafe(): the Lua bindings, the menu and the hotkey all get here from
    // callbacks in the middle of Counters::update() and branchTest(), so restoring now would
    // swap the state under them. Each queued step holds on to a snapshot, see rewindStateCount().
    if (rewindStateCount() == 0) return false;
    g_system->scheduleRewind();
    return true;
}

bool PCSX::Emulator::rewindStateNow(unsigned steps) {
    auto& ring = *m_rewindRing;
    if (ring.m_states.empty() || steps == 0) return false;
    // Each step consumes a snapshot, but only the one we land on needs restoring.
    while (--steps && ring.m_states.size() > 1) {
        ring.m_spare.push_back(std::move(ring.m_states.back().state));
        ring.m_states.pop_back();
    }
    // Step back to the most recent snapshot and consume it, so successive calls walk further
    // into the past. Take it off the ring first: restore() signals SaveStateLoaded, and a
    // listener that rewinds again has to step past this snapshot, not land on it a second time.
    // restore() invalidates the code cache itself, since it does not reset the cpu.
    auto state = std::move(ring.m_states.back().state);
    ring.m_states.pop_back();
    SaveStates::restore(*state);
    ring.m_spare.push_back(std::move(state));
    return true;
}

size_t PCSX::Emulator::rewindStateCount() const {
    const size_t size = m_rewindRing->m_states.size();
    const size_t pending = g_system->pendingRewinds();
    return size > pending ? size - pending : 0;
}

bool PCSX::Emulator::clearRewindRing() {
    const bool hadSnapshots = !m_rewindRing->m_states.empty();
    m_rewindRing->clear();
    m_rewindCapturePending = false;
    m_rewindRestartPending = false;
    return hadSnapshots;
}

void PCSX::Emulator::restartRewind(bool inUse) {
    if (clearRewindRing()) inUse = true;
    if (settings.get<SettingRewindInterval>().value > 0) inUse = true;
    if (!inUse) return;
    // The interpreter signals ShellReached from hasToRun(), after the branchTest() of the block
    // that got there. The recompilers signal it from the start of the next block and then run
    // a branchTest(), so the snapshot waits for the end of that one, see flushRewindCapture().
    if (g_system->inExecute() && m_cpu->isDynarec()) {
        m_rewindCapturePending = true;
        m_rewindRestartPending = true;
        return;
    }
    captureRewindState(true);
}

size_t PCSX::Emulator::rewindSnapshotCount() const { return m_rewindRing->m_states.size(); }

uint64_t PCSX::Emulator::rewindSnapshotCycle(size_t index) const { return m_rewindRing->m_states[index].cycle; }

void PCSX::Emulator::restoreRewindSnapshot(size_t index) {
    // A capture queued before the restore asked for a state that no longer exists.
    m_rewindCapturePending = false;
    m_rewindRestartPending = false;
    SaveStates::restore(*m_rewindRing->m_states[index].state);
}

void PCSX::Emulator::dropRewindSnapshotsAfter(uint64_t cycle) {
    auto& ring = *m_rewindRing;
    while (!ring.m_states.empty() && ring.m_states.back().cycle > cycle) {
        ring.m_spare.push_back(std::move(ring.m_states.back().state));
        ring.m_states.pop_back();
    }
}

void PCSX::Emulator::setPGXPMode(uint32_t pgxpMode) { m_cpu->psxSetPGXPMode(pgxpMode); }

PCSX::Emulator* PCSX::g_emulator;
