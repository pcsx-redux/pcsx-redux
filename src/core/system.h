/***************************************************************************
 *   Copyright (C) 2018 PCSX-Redux authors                                 *
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

#include <stdarg.h>
// libuv only appears here as an opaque handle. Including <uv.h> from this
// header dragged it into 110 of 251 translation units, of which only eight
// actually use it; the ones that do include it themselves.
struct uv_loop_s;
typedef struct uv_loop_s uv_loop_t;

#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/arguments.h"
#include "fmt/format.h"
#include "fmt/printf.h"
#include "imgui.h"
#include "support/djbhash.h"
#include "support/eventbus.h"
#include "support/version-info.h"

namespace PCSX {

enum class LogClass : unsigned;

// a hack, until C++-20 is fully adopted everywhere.
typedef decltype(std::filesystem::path().u8string()) u8string;
#define MAKEU8(x) reinterpret_cast<const decltype(PCSX::u8string::value_type()) *>(x)

// another hack, until C++-20 properly gets std::chrono::clock_cast
template <typename DstTP, typename SrcTP, typename DstClk = typename DstTP::clock,
          typename SrcClk = typename SrcTP::clock>
DstTP ClockCast(const SrcTP tp) {
    const SrcTP srcNow = SrcClk::now();
    const DstTP dstNow = DstClk::now();
    return std::chrono::time_point_cast<typename DstClk::duration>(tp - srcNow + dstNow);
}

namespace Events {
// While the event bus can handle any type as event, we only use the ones that
// are within this namespace. Also, any new event should also be added to the
// Lua bindings, in the file eventslua.cc.
struct SettingsLoaded {
    bool safe = false;
};
struct Quitting {};
struct LogMessage {
    LogClass logClass;
    std::string message;
};
struct IsoMounted {};
namespace GPU {
struct VSync {};
}  // namespace GPU
namespace ExecutionFlow {
struct ShellReached {};
struct Run {};
struct Pause {
    bool exception = false;
};
struct Reset {
    bool hard = false;
};
struct SaveStateLoaded {};
}  // namespace ExecutionFlow
namespace GUI {
struct JumpToPC {
    uint32_t pc;
};
struct JumpToMemory {
    uint32_t address;
    unsigned size;
    unsigned editorNum;
};
struct SelectClut {
    unsigned x, y;
};
enum VRAMMode : int {
    VRAM_4BITS,
    VRAM_8BITS,
    VRAM_16BITS,
    VRAM_24BITS,
};
struct VRAMFocus {
    int x1, y1;
    int x2, y2;
    VRAMMode vramMode = VRAM_16BITS;
};
struct VRAMHover {
    float x, y;
    VRAMMode vramMode;
};
struct VRAMClick {
    float x, y;
    VRAMMode vramMode;
};
struct RAMFocus {
    uint32_t address;
    uint32_t size;
};
}  // namespace GUI
// action: 0 = released, 1 = pressed, 2 = held key auto-repeating. Lua listeners
// receive these values as-is.
struct Keyboard {
    int key, scancode, action, mods;
};
namespace Memory {
struct SetLuts {};
}  // namespace Memory
}  // namespace Events

class System {
  public:
    System();
    virtual ~System();
    // Requests a system reset
    virtual void softReset() = 0;
    virtual void hardReset() = 0;
    // Putc used by bios syscalls
    virtual void biosPutc(int c) = 0;
    virtual const Arguments &getArgs() const = 0;

    // Legacy printf stuff; needs to be replaced with loggers
    template <typename... Args>
    void printf(const char *format, const Args &...args) {
        std::string s = fmt::sprintf(format, args...);
        printf(std::move(s));
    }
    virtual void printf(std::string &&) = 0;
    // Add a log line
    template <typename... Args>
    void log(LogClass logClass, const char *format, const Args &...args) {
        std::string s = fmt::sprintf(format, args...);
        log(logClass, std::move(s));
    }
    virtual void log(LogClass, std::string &&) = 0;
    // Display a popup message to the user
    template <typename... Args>
    void message(const char *format, const Args &...args) {
        std::string s = fmt::sprintf(format, args...);
        message(std::move(s));
    }
    virtual void message(std::string &&) = 0;
    // For the Lua output
    virtual void luaMessage(const std::string &, bool error) = 0;
    // Called periodically; if vsync = true, this while the emulated hardware vsyncs
    virtual void update(bool vsync = false) = 0;
    // Close mem and plugins
    virtual void close() = 0;
    virtual void purgeAllEvents() = 0;
    bool running() {
        std::atomic_signal_fence(std::memory_order_relaxed);
        return m_running && !m_quitting;
    }
    const bool *runningPtr() { return &m_running; }
    const bool *quittingPtr() { return &m_quitting; }
    bool quitting() { return m_quitting; }
    int exitCode() { return m_exitCode; }
    bool emergencyExit() { return m_emergencyExit; }
    [[gnu::cold]] void pause(bool exception = false) {
        if (m_hasPendingSaveStateLoad) {
            // m_running is already false because a deferred save state load
            // asked the CPU to unwind, but the emulator isn't meant to be
            // stopped yet, so this pause is a real one and has to be honoured.
            m_resumeAfterPendingLoad = false;
            m_eventBus->signal(Events::ExecutionFlow::Pause{exception});
            return;
        }
        if (!m_running) return;
        m_running = false;
        m_eventBus->signal(Events::ExecutionFlow::Pause{exception});
    }
    void resume() {
        if (m_hasPendingSaveStateLoad) {
            // The CPU is unwinding for a deferred load; run once it's applied.
            m_resumeAfterPendingLoad = true;
            m_eventBus->signal(Events::ExecutionFlow::Run{});
            return;
        }
        if (m_running) return;
        m_running = true;
        m_eventBus->signal(Events::ExecutionFlow::Run{});
    }
    // Queues a save state to be loaded by the main loop, and asks the CPU to
    // return out of Execute(). Loading one directly from a callback that runs
    // on the emulation stack, such as a Lua GPU::Vsync listener or the ImGui
    // menu, replaces the emulator state underneath frames that are still
    // holding pre-load values, and those frames then keep going. This doesn't
    // signal a pause, since the emulation isn't stopping, it's only unwinding.
    void scheduleSaveStateLoad(std::string &&data) {
        m_pendingSaveStateLoad = std::move(data);
        m_pendingRestore = nullptr;
        // A load also replaces any rewind queued before it.
        m_pendingRewinds = 0;
        // A second load in the same window replaces the first, and must not
        // read the m_running the first one already cleared.
        if (!m_hasPendingSaveStateLoad) m_resumeAfterPendingLoad = m_running;
        m_hasPendingSaveStateLoad = true;
        m_running = false;
    }
    // Same thing for a step back through the emulator's rewind ring, which
    // replaces the state just as much as a load does. Rewinds queued in the same
    // window add up, one snapshot each, while a rewind queued after a load
    // replaces it, like a second load would.
    void scheduleRewind() {
        m_pendingSaveStateLoad.clear();
        m_pendingRestore = nullptr;
        m_pendingRewinds++;
        if (!m_hasPendingSaveStateLoad) m_resumeAfterPendingLoad = m_running;
        m_hasPendingSaveStateLoad = true;
        m_running = false;
    }
    // And for a snapshot already in memory, such as the one Lua's restoreState()
    // brings back. It replaces anything queued before it, like a load would.
    void scheduleRestore(std::function<void()> &&restore) {
        m_pendingSaveStateLoad.clear();
        m_pendingRewinds = 0;
        m_pendingRestore = std::move(restore);
        if (!m_hasPendingSaveStateLoad) m_resumeAfterPendingLoad = m_running;
        m_hasPendingSaveStateLoad = true;
        m_running = false;
    }
    // A reset issued before the main loop applied a queued load wins over it.
    void cancelPendingSaveStateLoad() {
        if (!m_hasPendingSaveStateLoad) return;
        m_hasPendingSaveStateLoad = false;
        m_running = m_resumeAfterPendingLoad;
        m_pendingSaveStateLoad.clear();
        m_pendingRewinds = 0;
        m_pendingRestore = nullptr;
    }
    // True while the main loop is inside the CPU's Execute(). Anything that
    // runs then, including a Pause listener fired from a breakpoint, is on the
    // emulation stack, whether or not m_running is still set.
    bool inExecute() const { return m_inExecute; }
    void setInExecute(bool inExecute) { m_inExecute = inExecute; }
    bool hasPendingSaveStateLoad() const { return m_hasPendingSaveStateLoad; }
    // When non-zero, the queued load is this many rewinds rather than a save state.
    unsigned pendingRewinds() const { return m_pendingRewinds; }
    // When set, the queued load is this restore rather than a save state.
    bool hasPendingRestore() const { return static_cast<bool>(m_pendingRestore); }
    // Hands over the queued save state and puts the emulation back the way it
    // was. Only ever call this from the main loop, with nothing of the
    // emulation left on the stack.
    std::string takePendingSaveStateLoad() {
        m_hasPendingSaveStateLoad = false;
        m_running = m_resumeAfterPendingLoad;
        m_pendingRewinds = 0;
        m_pendingRestore = nullptr;
        return std::move(m_pendingSaveStateLoad);
    }
    // Same, for a queued restore. Main loop only, like takePendingSaveStateLoad().
    std::function<void()> takePendingRestore() {
        auto restore = std::move(m_pendingRestore);
        takePendingSaveStateLoad();
        return restore;
    }
    virtual void testQuit(int code) = 0;
    // This needs to only mutate variables, as it requires to be signal-safe.
    [[gnu::cold]] void quit(int code = 0) {
        m_quitting = true;
        m_exitCode = code;
    }

    std::shared_ptr<EventBus::EventBus> m_eventBus = std::make_shared<EventBus::EventBus>();

    const char *getStr(uint64_t hash, const char *str) const {
        auto ret = m_i18n.find(hash);
        if (ret == m_i18n.end()) return str;
        return ret->second.c_str();
    }

    bool findResource(std::function<bool(const std::filesystem::path &path)> walker, const std::filesystem::path &name,
                      const std::filesystem::path &releasePath, const std::filesystem::path &sourcePath);
    void loadAllLocales() {
        for (auto &l : LOCALES) {
            findResource([name = l.first, this](std::filesystem::path filename) { return loadLocale(name, filename); },
                         l.second.filename, "i18n", "i18n");
        }
    }

    bool loadLocale(const std::string &name, const std::filesystem::path &path);
    void activateLocale(const std::string &name) {
        if (name == "English") {
            m_currentLocale = "English";
            m_i18n = {};
            return;
        }
        auto locale = m_locales.find(name);
        if (locale == m_locales.end()) return;
        m_i18n = locale->second;
        m_currentLocale = name;
    }
    std::string localeName() const { return m_currentLocale; }
    std::vector<PCSX::u8string> getLocaleExtra() {
        auto localeInfo = LOCALES.find(m_currentLocale);
        if (localeInfo == LOCALES.end()) return {};
        return localeInfo->second.extraFonts;
    }
    std::vector<std::string> localesNames() {
        std::vector<std::string> locales;
        for (auto &l : m_locales) {
            locales.push_back(l.first);
        }
        return locales;
    }

    std::filesystem::path getBinDir() const { return m_binDir; }
    std::filesystem::path getPersistentDir() const;
    const VersionInfo &getVersion() const { return m_version; }

    // Attaches a tag to any crash report sent from this point on. This is a no-op
    // unless the platform's crash reporter installed a setter at startup.
    static void setCrashReportTag(const char *key, const char *value) {
        if (s_crashReportTagSetter) s_crashReportTagSetter(key, value);
    }
    static inline void (*s_crashReportTagSetter)(const char *key, const char *value) = nullptr;

    uv_loop_t *getLoop();

  private:
    uv_loop_t *m_loop;
    std::map<uint64_t, std::string> m_i18n;
    std::map<std::string, decltype(m_i18n)> m_locales;
    std::string m_currentLocale;
    // If true, indicates that the emulator is currently capturing the main loop
    // and actively emulates the PSX hardware. If false, the emulator is paused,
    // waiting for user input or other events inside the UI. The way the UI
    // is refreshed is by calling update() periodically, so this boolean affects
    // the moment when and how update() is called.
    bool m_running = false;
    // If true, indicates that the emulator is quitting. This can be set by a
    // number of events, including the user pressing the quit button or the
    // emulator itself requesting a quit due to testing for instance. This will
    // cause the two main loop to exit: the inner one being the emulator itself,
    // and the outer one being the main.cc loop.
    bool m_quitting = false;
    // Set while a save state load has been queued from the emulation stack and
    // the main loop hasn't picked it up yet. See scheduleSaveStateLoad().
    bool m_hasPendingSaveStateLoad = false;
    bool m_resumeAfterPendingLoad = false;
    bool m_inExecute = false;
    std::string m_pendingSaveStateLoad;
    unsigned m_pendingRewinds = 0;
    std::function<void()> m_pendingRestore;
    int m_exitCode = 0;
    struct LocaleInfo {
        const std::string filename;
        const std::vector<PCSX::u8string> extraFonts;
    };
    static const std::map<std::string, LocaleInfo> LOCALES;

  protected:
    std::filesystem::path m_binDir;
    PCSX::VersionInfo m_version;
    bool m_emergencyExit = false;
};

extern System *g_system;

}  // namespace PCSX

// i18n macros
// Normal string lookup to const char *
#define _(str) PCSX::g_system->getStr(PCSX::djb::ctHash(str), str)
// Formatting string lookup to use with fmt::format or fmt::printf
#define f_(str) fmt::runtime(PCSX::g_system->getStr(PCSX::djb::ctHash(str), str))
// Lambda string lookup to use with static arrays of strings
#define l_(str) []() { return PCSX::g_system->getStr(PCSX::djb::ctHash(str), str); }
// Same as _() and l_(), with a translation context for strings whose meaning
// depends on where they're used, e.g. C_("Menu", "Update"). The context shows up
// as msgctxt in the .pot file, and both arguments need to be string literals.
#define C_(ctx, str) PCSX::g_system->getStr(PCSX::djb::ctHash(ctx "\004" str), str)
#define lC_(ctx, str) []() { return PCSX::g_system->getStr(PCSX::djb::ctHash(ctx "\004" str), str); }
