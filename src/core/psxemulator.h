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

/*
 * This file contains common definitions and includes for all parts of the
 * emulator core.
 */

#pragma once

// Windows include on top
#include "support/windowswrapper.h"

// Normal includes
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <zlib.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>

#include "support/settings.h"
#include "support/ssize_t.h"
#include "support/strings-helpers.h"

#ifndef MAXPATHLEN
#if defined(MAX_PATH)
#define MAXPATHLEN MAX_PATH
#elif defined(PATH_MAX)
#define MAXPATHLEN PATH_MAX
#endif
#endif

// Local includes from core - anything else from core is forbidden
#include "core/logger.h"
#include "core/system.h"

#if defined(__linux__) || defined(__MACOSX__)
#define strnicmp strncasecmp
#endif

namespace PCSX {

class AllocTracker;
class CallStacks;
class CDRom;
class CDRomLogger;
class Counters;
class CpuTrace;
class Debug;
class GdbServer;
class GPU;
class GPUDumper;
class GPUDumpPlayer;
class GPULogger;
class GTE;
class RAMLogger;
class SPULogger;
class HW;
class Lua;
class MDEC;
class Memory;
class Pads;
class PatchManager;
class R3000Acpu;
class ShmDisplay;
class SIO;
class SPUInterface;
class System;
class WebServer;
class SIO1;
class SIO1Server;
class SIO1Client;
class PIOCart;

class Emulator;
extern Emulator* g_emulator;

class Emulator {
  public:
    Emulator();
    ~Emulator();
    Emulator(Emulator&&) = delete;
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;
    enum VideoType { PSX_TYPE_NTSC = 0, PSX_TYPE_PAL };    // PSX Types
    enum CDDAType { CDDA_DISABLED = 0, CDDA_ENABLED_LE };  // CDDA Types
    struct DebugSettings {
        typedef Setting<bool, TYPESTRING("Debug")> Debug;
        typedef Setting<bool, TYPESTRING("Trace")> Trace;
        typedef Setting<bool, TYPESTRING("KernelLog")> KernelLog;
        typedef Setting<uint32_t, TYPESTRING("FirstChanceException"), 0x00001cf0> FirstChanceException;
        typedef Setting<bool, TYPESTRING("SkipISR")> SkipISR;
        typedef Setting<bool, TYPESTRING("LoggingCDROM"), false> LoggingCDROM;
        typedef Setting<bool, TYPESTRING("LoggingHWCDROM"), false> LoggingHWCDROM;
        typedef Setting<bool, TYPESTRING("GdbServer"), false> GdbServer;
        typedef Setting<bool, TYPESTRING("GdbManifest"), true> GdbManifest;
        enum class GdbLog {
            None,
            TTY,
            All,
        };
        typedef Setting<GdbLog, TYPESTRING("GdbLog"), GdbLog::TTY> GdbLogSetting;
        typedef Setting<int, TYPESTRING("GdbServerPort"), 3333> GdbServerPort;
        typedef Setting<bool, TYPESTRING("GdbServerTrace"), false> GdbServerTrace;
        typedef Setting<bool, TYPESTRING("WebServer"), false> WebServer;
        typedef Setting<int, TYPESTRING("WebServerPort"), 8080> WebServerPort;
        typedef Setting<uint32_t, TYPESTRING("KernelCallA0_00_1f"), 0xffffffff> KernelCallA0_00_1f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallA0_20_3f"), 0xffffffff> KernelCallA0_20_3f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallA0_40_5f"), 0xffffffff> KernelCallA0_40_5f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallA0_60_7f"), 0xffffffff> KernelCallA0_60_7f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallA0_80_9f"), 0xffffffff> KernelCallA0_80_9f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallA0_a0_bf"), 0xffffffff> KernelCallA0_a0_bf;
        typedef Setting<uint32_t, TYPESTRING("KernelCallB0_00_1f"), 0xffffffff> KernelCallB0_00_1f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallB0_20_3f"), 0xffffffff> KernelCallB0_20_3f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallB0_40_5f"), 0xffffffff> KernelCallB0_40_5f;
        typedef Setting<uint32_t, TYPESTRING("KernelCallC0_00_1f"), 0xffffffff> KernelCallC0_00_1f;
        typedef Setting<bool, TYPESTRING("DemangledSymbols"), true> DemangledSymbols;
        typedef Setting<bool, TYPESTRING("PCdrv"), false> PCdrv;
        typedef SettingPath<TYPESTRING("PCdrvBase")> PCdrvBase;
        typedef Setting<bool, TYPESTRING("PCdrvNet"), false> PCdrvNet;
        typedef SettingString<TYPESTRING("PCdrvNetAllow")> PCdrvNetAllow;
        typedef Setting<bool, TYPESTRING("SIO1Server"), false> SIO1Server;
        typedef Setting<int, TYPESTRING("SIO1ServerPort"), 6699> SIO1ServerPort;
        typedef Setting<bool, TYPESTRING("SIO1Client"), false> SIO1Client;
        typedef SettingString<TYPESTRING("SIO1Clienthost")> SIO1ClientHost;
        typedef Setting<int, TYPESTRING("SIO1ClientPort"), 6699> SIO1ClientPort;
        enum class SIO1Mode {
            Protobuf,
            Raw,
        };
        typedef Setting<SIO1Mode, TYPESTRING("SIO1Mode"), SIO1Mode::Protobuf> SIO1ModeSetting;
        typedef Settings<Debug, Trace, KernelLog, FirstChanceException, SkipISR, LoggingCDROM, LoggingHWCDROM,
                         GdbServer, GdbManifest, GdbLogSetting, GdbServerPort, GdbServerTrace, WebServer, WebServerPort,
                         KernelCallA0_00_1f, KernelCallA0_20_3f, KernelCallA0_40_5f, KernelCallA0_60_7f,
                         KernelCallA0_80_9f, KernelCallA0_a0_bf, KernelCallB0_00_1f, KernelCallB0_20_3f,
                         KernelCallB0_40_5f, KernelCallC0_00_1f, DemangledSymbols, PCdrv, PCdrvBase, PCdrvNet,
                         PCdrvNetAllow, SIO1Server, SIO1ServerPort, SIO1Client, SIO1ClientHost, SIO1ClientPort,
                         SIO1ModeSetting>
            type;
    };
    typedef SettingNested<TYPESTRING("Debug"), DebugSettings::type> SettingDebugSettings;
    typedef SettingPath<TYPESTRING("Mcd1")> SettingMcd1;
    typedef SettingPath<TYPESTRING("Mcd2")> SettingMcd2;
    // The memory cards in a multitap's slots B to D, port 1 then port 2. Slot A is Mcd1 or Mcd2.
    typedef SettingPath<TYPESTRING("Mcd1B")> SettingMcd1B;
    typedef SettingPath<TYPESTRING("Mcd1C")> SettingMcd1C;
    typedef SettingPath<TYPESTRING("Mcd1D")> SettingMcd1D;
    typedef SettingPath<TYPESTRING("Mcd2B")> SettingMcd2B;
    typedef SettingPath<TYPESTRING("Mcd2C")> SettingMcd2C;
    typedef SettingPath<TYPESTRING("Mcd2D")> SettingMcd2D;
    typedef SettingPath<TYPESTRING("Bios")> SettingBios;
    typedef SettingPath<TYPESTRING("PpfDir")> SettingPpfDir;
    typedef SettingPath<TYPESTRING("PsxExe")> SettingPsxExe;
    typedef Setting<bool, TYPESTRING("Xa"), true> SettingXa;
    typedef Setting<bool, TYPESTRING("SpuIrq")> SettingSpuIrq;
    typedef Setting<bool, TYPESTRING("BnWMdec")> SettingBnWMdec;
    typedef Setting<bool, TYPESTRING("AutoVideo"), true> SettingAutoVideo;
    typedef Setting<VideoType, TYPESTRING("Video"), PSX_TYPE_NTSC> SettingVideo;
    typedef Setting<bool, TYPESTRING("FastBoot"), false> SettingFastBoot;
    typedef Setting<bool, TYPESTRING("CDRomEntropySeed"), false> SettingCDRomEntropySeed;
    typedef Setting<bool, TYPESTRING("RCntFix")> SettingRCntFix;
    typedef SettingPath<TYPESTRING("IsoPath")> SettingIsoPath;
    typedef SettingString<TYPESTRING("Locale")> SettingLocale;
    typedef Setting<bool, TYPESTRING("Mcd1Inserted"), true> SettingMcd1Inserted;
    typedef Setting<bool, TYPESTRING("Mcd2Inserted"), true> SettingMcd2Inserted;
    typedef Setting<bool, TYPESTRING("Mcd1BInserted"), false> SettingMcd1BInserted;
    typedef Setting<bool, TYPESTRING("Mcd1CInserted"), false> SettingMcd1CInserted;
    typedef Setting<bool, TYPESTRING("Mcd1DInserted"), false> SettingMcd1DInserted;
    typedef Setting<bool, TYPESTRING("Mcd2BInserted"), false> SettingMcd2BInserted;
    typedef Setting<bool, TYPESTRING("Mcd2CInserted"), false> SettingMcd2CInserted;
    typedef Setting<bool, TYPESTRING("Mcd2DInserted"), false> SettingMcd2DInserted;
    typedef Setting<bool, TYPESTRING("Dynarec"), true> SettingDynarec;
    typedef Setting<bool, TYPESTRING("8Megs"), false> Setting8MB;
    typedef Setting<uint8_t, TYPESTRING("MemoryFillValue"), 0x00> SettingMemoryFillValue;
    // Rewind: capture a snapshot every N frames (0 disables), keeping the last M snapshots.
    typedef Setting<uint32_t, TYPESTRING("RewindInterval"), 0> SettingRewindInterval;
    typedef Setting<uint32_t, TYPESTRING("RewindCount"), 60> SettingRewindCount;
    typedef Setting<bool, TYPESTRING("2MBVRAM"), false> Setting2MBVRAM;
    typedef Setting<int, TYPESTRING("GUITheme"), 0> SettingGUITheme;
    typedef Setting<int, TYPESTRING("Dither"), 1> SettingDither;
    typedef Setting<bool, TYPESTRING("UseCachedDithering"), false> SettingCachedDithering;
    typedef Setting<bool, TYPESTRING("SoftGPUThreaded"), false> SettingSoftGPUThreaded;
    typedef Setting<bool, TYPESTRING("ReportGLErrors"), false> SettingGLErrorReporting;
    typedef Setting<int, TYPESTRING("ReportGLErrorsSeverity"), 1> SettingGLErrorReportingSeverity;
    typedef Setting<bool, TYPESTRING("FullCaching"), false> SettingFullCaching;
    typedef Setting<bool, TYPESTRING("HardwareRenderer"), false> SettingHardwareRenderer;
    typedef Setting<bool, TYPESTRING("ShownAutoUpdateConfig"), false> SettingShownAutoUpdateConfig;
    typedef Setting<bool, TYPESTRING("AutoUpdate"), false> SettingAutoUpdate;
    typedef Setting<int, TYPESTRING("MSAA"), 1> SettingMSAA;
    typedef Setting<bool, TYPESTRING("LinearFiltering"), true> SettingLinearFiltering;
    typedef Setting<bool, TYPESTRING("KioskMode"), false> SettingKioskMode;
    typedef Setting<bool, TYPESTRING("Mcd1Pocketstation"), false> SettingMcd1Pocketstation;
    typedef Setting<bool, TYPESTRING("Mcd2Pocketstation"), false> SettingMcd2Pocketstation;
    typedef SettingPath<TYPESTRING("BiosPath")> SettingBiosBrowsePath;
    typedef SettingPath<TYPESTRING("EXP1Filepath")> SettingEXP1Filepath;
    typedef SettingPath<TYPESTRING("EXP1BrowsePath")> SettingEXP1BrowsePath;
    typedef Setting<bool, TYPESTRING("PIOConnected")> SettingPIOConnected;
    typedef SettingPath<TYPESTRING("MapBrowsePath")> SettingMapBrowsePath;
    typedef SettingVector<std::string, TYPESTRING("OpenDialogFavorites")> SettingOpenDialogFavorites;

    Settings<SettingMcd1, SettingMcd2, SettingBios, SettingPpfDir, SettingPsxExe, SettingXa, SettingSpuIrq,
             SettingBnWMdec, SettingAutoVideo, SettingVideo, SettingFastBoot, SettingDebugSettings, SettingRCntFix,
             SettingIsoPath, SettingLocale, SettingMcd1Inserted, SettingMcd2Inserted, SettingDynarec, Setting8MB,
             SettingMemoryFillValue, Setting2MBVRAM, SettingGUITheme, SettingDither, SettingCachedDithering,
             SettingSoftGPUThreaded, SettingGLErrorReporting, SettingGLErrorReportingSeverity, SettingFullCaching,
             SettingHardwareRenderer, SettingShownAutoUpdateConfig, SettingAutoUpdate, SettingMSAA,
             SettingLinearFiltering, SettingKioskMode, SettingMcd1Pocketstation, SettingMcd2Pocketstation,
             SettingBiosBrowsePath, SettingEXP1Filepath, SettingEXP1BrowsePath, SettingPIOConnected,
             SettingMapBrowsePath, SettingOpenDialogFavorites, SettingRewindInterval, SettingRewindCount,
             SettingCDRomEntropySeed, SettingMcd1B, SettingMcd1C, SettingMcd1D, SettingMcd2B, SettingMcd2C,
             SettingMcd2D, SettingMcd1BInserted, SettingMcd1CInserted, SettingMcd1DInserted, SettingMcd2BInserted,
             SettingMcd2CInserted, SettingMcd2DInserted>
        settings;
    class PcsxConfig {
      public:
        bool VSyncWA = false;
        bool PerGameMcd = false;
        bool Widescreen = false;
        bool HideCursor = false;
        bool SaveWindowPos = false;
        int32_t WindowPos[2] = {0, 0};
        uint32_t AltSpeed1 = 0;  // Percent relative to natural speed.
        uint32_t AltSpeed2 = 0;
        bool OverClock = false;  // enable overclocking
        float PsxClock = 0.0f;
        // PGXP variables
        bool PGXP_GTE = false;
        bool PGXP_Cache = false;
        bool PGXP_Texture = false;
        uint32_t PGXP_Mode = 0;
    };

    // It is safe if these overflow
    uint32_t m_rewind_counter = 0;

    // Vsyncs raised by vsync() and not yet dispatched by dispatchVSync().
    uint32_t m_vsyncPending = 0;

    // In-memory rewind ring; defined in psxemulator.cc so this header does not pull in sstate.h.
    struct RewindRing;
    std::unique_ptr<RewindRing> m_rewindRing;
    bool m_rewindCaptureSuppressed = false;
    bool m_rewindCapturePending = false;
    // The pending capture comes from restartRewind(), see captureRewindState().
    bool m_rewindRestartPending = false;
    void captureRewindState(bool restarting = false);

    // Used for overclocking
    // Make the timing events trigger faster as we are currently assuming everything
    // takes one cycle, which is not the case on real hardware.
    // FIXME: Count the proper cycle and get rid of this
    static constexpr uint32_t m_psxClockSpeed = 33868800 /* 33.8688 MHz */;
    static constexpr uint32_t BIAS = 2;
    static constexpr uint32_t ROM_EXTRA_BIAS = 10;

    template <unsigned alignment = 1>
        requires((alignment == 1) || (alignment == 4))
    constexpr uint32_t getRamMask() {
        if constexpr (alignment == 1) {
            return settings.get<PCSX::Emulator::Setting8MB>() ? 0x7fffff : 0x1fffff;
        } else if constexpr (alignment == 4) {
            return settings.get<PCSX::Emulator::Setting8MB>() ? 0x7ffffc : 0x1ffffc;
        }
    }

    int init();
    void reset();
    void shutdown();
    void vsync();
    // Runs what vsync() deferred: the VSync event, the system update and asking for the rewind
    // snapshot.
    // Called from R3000Acpu::branchTest(), where m_regs.pc is the real next pc.
    void dispatchVSync();
    void setPGXPMode(uint32_t pgxpMode);

    // Rewind: ring of in-memory snapshots taken every RewindInterval frames, bounded to
    // RewindCount entries. Entries are SaveStates::capture()d messages, restored with
    // SaveStates::restore(), so neither direction goes through the protobuf encoding.
    // rewindState() steps back right away when nothing of the emulation is on the stack, and
    // queues the step for the main loop otherwise, like SaveStates::loadSafe(). Either way it
    // returns whether there was a snapshot to step back to. rewindStateNow() is the unsafe half
    // the main loop applies a queue with, and rewindStateCount() leaves out queued steps.
    // createRewindState() called from inside the emulation, which is where the ring's own
    // captures come from, only marks the snapshot as wanted: R3000Acpu::branchTest() takes it
    // with flushRewindCapture() once it is done, so restoring it doesn't skip any of its work.
    void createRewindState();
    void flushRewindCapture();
    bool rewindState();
    bool rewindStateNow(unsigned steps = 1);
    size_t rewindStateCount() const;

    // Random access into the same ring, for replaying from a snapshot rather than stepping back
    // to it, which is what the gdb server's reverse execution does. Index 0 is the oldest
    // snapshot. The cycle is m_regs.cycle at capture. restoreRewindSnapshot() leaves the
    // snapshot on the ring, and has the same constraint as rewindStateNow(): nothing of the
    // emulation may be on the stack. While a replay is suppressing captures, the vsyncs it runs
    // through don't push copies of what the ring already holds. dropRewindSnapshotsAfter()
    // discards the snapshots of a future that is about to be executed again.
    size_t rewindSnapshotCount() const;
    uint64_t rewindSnapshotCycle(size_t index) const;
    void restoreRewindSnapshot(size_t index);
    void dropRewindSnapshotsAfter(uint64_t cycle);
    void suppressRewindCapture(bool suppress) { m_rewindCaptureSuppressed = suppress; }

    // For a point in the emulation that a replay from an earlier snapshot can't reproduce, such as
    // the shell being hijacked. clearRewindRing() drops every snapshot and returns whether there
    // were any. restartRewind() does the same, then takes a snapshot if the ring was in use, or if
    // it is capturing on its own. Called from Execute(), it has to be between two blocks, where the
    // ShellReached event is signalled.
    bool clearRewindRing();
    void restartRewind(bool inUse = false);

    void setLua();

    PcsxConfig& config() { return m_config; }

    // Turbo multiplies the SPU::Speed sink multiplier; see SPU::SDLAudio::effectiveSpeed.
    void setTurbo(bool on) { m_turboFactor.store(on ? 2 : 1, std::memory_order_relaxed); }
    int getTurboFactor() const { return m_turboFactor.load(std::memory_order_relaxed); }

    std::unique_ptr<CallStacks> m_callStacks;
    std::unique_ptr<CDRom> m_cdrom;
    std::unique_ptr<CDRomLogger> m_cdromLogger;
    std::unique_ptr<Counters> m_counters;
    std::unique_ptr<CpuTrace> m_cpuTrace;
    std::unique_ptr<Debug> m_debug;
    std::unique_ptr<GdbServer> m_gdbServer;
    std::unique_ptr<GPU> m_gpu;
    std::unique_ptr<GPUDumper> m_gpuDumper;
    std::unique_ptr<GPUDumpPlayer> m_gpuDumpPlayer;
    std::unique_ptr<GPULogger> m_gpuLogger;
    std::unique_ptr<GTE> m_gte;
    std::unique_ptr<RAMLogger> m_ramLogger;
    std::unique_ptr<SPULogger> m_spuLogger;
    std::unique_ptr<HW> m_hw;
    std::unique_ptr<Lua> m_lua;
    std::unique_ptr<MDEC> m_mdec;
    std::unique_ptr<Memory> m_mem;
    std::unique_ptr<Pads> m_pads;
    std::unique_ptr<PatchManager> m_patchManager;
    std::unique_ptr<PIOCart> m_pioCart;
    std::unique_ptr<R3000Acpu> m_cpu;
    std::unique_ptr<SIO> m_sio;
    std::unique_ptr<SIO1> m_sio1;
    std::unique_ptr<SIO1Server> m_sio1Server;
    std::unique_ptr<SIO1Client> m_sio1Client;
    std::unique_ptr<ShmDisplay> m_shmDisplay;
    std::unique_ptr<SPUInterface> m_spu;
    std::unique_ptr<WebServer> m_webServer;
    // Last, so it is destroyed first: its teardown removes breakpoints from m_debug.
    std::unique_ptr<AllocTracker> m_allocTracker;

  private:
    PcsxConfig m_config;
    std::atomic<int> m_turboFactor{1};
};

}  // namespace PCSX
