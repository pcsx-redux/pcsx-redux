/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include <uv.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <functional>
#endif

#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "core/arguments.h"
#include "core/cdrom.h"
#include "core/gpu.h"
#include "core/logger.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/sio.h"
#include "core/sstate.h"
#include "core/ui.h"
#include "fmt/chrono.h"
#include "gui/gui.h"
#include "lua/extra.h"
#include "lua/luawrapper.h"
#include "main/terminalui.h"
#include "main/textui.h"
#include "spu/interface.h"
#include "support/binpath.h"
#include "support/uvfile.h"
#include "support/version.h"
#include "tracy/Tracy.hpp"

static PCSX::UI *s_ui;

class SystemImpl final : public PCSX::System {
    virtual void biosPutc(int c) final override {
        if (c == '\r') return;
        m_putcharBuffer += std::string(1, c);
        if (c == '\n') {
            log(PCSX::LogClass::MIPS, std::move(m_putcharBuffer));
            m_putcharBuffer.clear();
        }
    }
    // stdout is fully buffered when it is a pipe, and the tools that capture it
    // want every line as soon as it is written.
    static void writeStdout(const std::string &s) {
        ::fputs(s.c_str(), stdout);
        ::fflush(stdout);
    }
    virtual void message(std::string &&s) final override {
        if (m_args.isGUILogsEnabled()) s_ui->addNotification(s.c_str());
        if (s_ui->addLog(PCSX::LogClass::UI, s)) {
            if (m_args.isStdoutEnabled()) writeStdout(s);
            if (m_logfile) m_logfile->write(std::move(s));
            m_eventBus->signal(PCSX::Events::LogMessage{PCSX::LogClass::UI, s});
        }
    }

    virtual void log(PCSX::LogClass logClass, std::string &&s) final override {
        if (m_args.isGUILogsEnabled()) {
            if (!s_ui->addLog(logClass, s)) return;
        }
        if (m_args.isStdoutEnabled()) writeStdout(s);
        if (m_logfile) m_logfile->write(std::move(s));
        m_eventBus->signal(PCSX::Events::LogMessage{logClass, s});
    }

    virtual void printf(std::string &&s) final override {
        if (m_args.isGUILogsEnabled()) {
            if (!s_ui->addLog(PCSX::LogClass::UNCATEGORIZED, s)) return;
        }
        if (m_args.isStdoutEnabled()) writeStdout(s);
        if (m_logfile) m_logfile->write(std::move(s));
        m_eventBus->signal(PCSX::Events::LogMessage{PCSX::LogClass::UNCATEGORIZED, s});
    }

    virtual void luaMessage(const std::string &s, bool error) final override {
        if (m_args.isGUILogsEnabled()) {
            s_ui->addLuaLog(s, error);
        }
        if (m_args.isTuiEnabled()) return;
        if ((error && m_inStartup) || m_args.isLuaStdoutEnabled()) {
            if (error) {
                fputs(s.c_str(), stderr);
                fputc('\n', stderr);
            } else {
                puts(s.c_str());
                fflush(stdout);
            }
        }
    }

    virtual void update(bool vsync = false) final override {
        // called on vblank to update states
        s_ui->update(vsync);
    }

    virtual void softReset() final override {
        // debugger or UI is requesting a reset
        cancelPendingSaveStateLoad();
        PCSX::g_emulator->m_cpu->psxReset();
        m_eventBus->signal(PCSX::Events::ExecutionFlow::Reset{});
    }

    virtual void hardReset() final override {
        // debugger or UI is requesting a reset
        cancelPendingSaveStateLoad();
        PCSX::g_emulator->reset();
        m_eventBus->signal(PCSX::Events::ExecutionFlow::Reset{true});
    }

    virtual void close() final override {
        // emulator is requesting a shutdown of the emulation
    }

    virtual void testQuit(int code) final override {
        if (m_args.isTestModeEnabled()) {
            quit(code);
        } else {
            PCSX::System::log(PCSX::LogClass::UI, "PSX software requested an exit with code %i\n", code);
            pause();
        }
    }

    virtual const PCSX::Arguments &getArgs() const final override { return m_args; }

    std::string m_putcharBuffer;
    PCSX::IO<PCSX::UvFile> m_logfile;
    const PCSX::Arguments m_args;

  public:
    virtual void purgeAllEvents() final override { uv_run(getLoop(), UV_RUN_DEFAULT); }

    void setBinDir(std::filesystem::path path) {
        m_binDir = path;
        m_version.loadFromFile(new PCSX::PosixFile(path / "version.json"));
        if (m_version.failed()) {
            m_version.loadFromFile(
                new PCSX::PosixFile(path / ".." / "share" / "pcsx-redux" / "resources" / "version.json"));
        }
        if (m_version.failed()) {
            m_version.loadFromFile(
                new PCSX::PosixFile(path / ".." / "Resources" / "share" / "pcsx-redux" / "resources" / "version.json"));
        }
    }

    explicit SystemImpl(const PCSX::CommandLine &args) : m_args(args) {}
    ~SystemImpl() {}

    void setEmergencyExit() { m_emergencyExit = true; }

    void useLogfile(const PCSX::u8string &filename) {
        m_logfile.setFile(new PCSX::UvFile(filename, PCSX::FileOps::TRUNCATE));
    }
    bool m_inStartup = true;
};

struct Cleaner {
    Cleaner(std::function<void()> &&f) : f(std::move(f)) {}
    ~Cleaner() {
        if (f) f();
    }
    // When the browser owns the loop, pcsxMain returns while the emulator, the
    // UI and the GL context are all still very much in use. The teardown has to
    // be switched off explicitly rather than avoided by never leaving the scope,
    // because leaving the scope is exactly what we now do.
    void disarm() { f = nullptr; }

  private:
    std::function<void()> f;
};

void handleSignal(int signal) { PCSX::g_system->quit(-1); }

// Files given on the command line are otherwise only opened much later, and a missing one
// is either silently replaced (bios), or merely logged (iso, exe). Check them up front instead.
// Memory cards are created on demand, and dofile goes through the archive lookup, so neither
// is checked here.
static bool checkCommandLinePaths(const PCSX::CommandLine &args) {
    bool ok = true;
    std::string messages;
    auto checkPath = [&args, &ok, &messages](const char *name, bool directory) {
        for (auto value : args.values(name)) {
            if (value.empty()) continue;
            std::string str(value);
            std::error_code ec;
            std::filesystem::path path = PCSX::u8string(MAKEU8(str.c_str()));
            bool found =
                directory ? std::filesystem::is_directory(path, ec) : std::filesystem::is_regular_file(path, ec);
            if (found) continue;
            ok = false;
            std::string message;
            if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory) {
                message = fmt::format("-{}: unable to access '{}': {}\n", name, str, ec.message());
            } else {
                message = fmt::format("-{}: {} '{}' not found\n", name, directory ? "directory" : "file", str);
            }
            fmt::print(stderr, "{}", message);
            messages += message;
        }
    };
    for (auto name : {"bios", "iso", "loadiso", "disk", "loadexe", "exe", "archive"}) checkPath(name, false);
    checkPath("pcdrvbase", true);
#if defined(_WIN32) || defined(_WIN64)
    // A plain GUI launch (no -stdout / -no-ui / -cli) has no console, so the stderr output above
    // goes nowhere and the process would otherwise just exit with no visible trace of why.
    if (!ok && GetConsoleWindow() == NULL) {
        MessageBoxA(NULL, messages.c_str(), "pcsx-redux", MB_ICONERROR | MB_OK);
    }
#endif
    return ok;
}

// Returns nullptr when -tui is asked for without a terminal to draw on.
static PCSX::UI *createUI(const PCSX::CommandLine &args, std::vector<std::string> &favorites) {
    if (args.has("tui")) {
        if (!PCSX::TerminalUI::isTerminal()) return nullptr;
        return new PCSX::TerminalUI();
    }
    if (args.has("no-ui") || args.has("cli")) return new PCSX::TUI();
    return new PCSX::GUI(favorites);
}

static void applyPCdrvNetArgs(const PCSX::CommandLine &args, auto &debugSettings) {
    if (args.has("pcdrvnet")) {
        debugSettings.template get<PCSX::Emulator::DebugSettings::PCdrvNet>() = true;
    }
    auto argPCdrvNetAllow = args.value("pcdrvnetallow");
    if (argPCdrvNetAllow.has_value()) {
        debugSettings.template get<PCSX::Emulator::DebugSettings::PCdrvNetAllow>() = argPCdrvNetAllow.value();
    }
}

int pcsxMain(int argc, char **argv) {
    ZoneScoped;
    // Command line arguments are parsed after this point.
    const PCSX::CommandLine args(argc, argv);
    if (args.status() != PCSX::CommandLine::Status::Ok) {
        bool error = args.status() == PCSX::CommandLine::Status::Error;
#if defined(_WIN32) || defined(_WIN64)
        // We're a Windows subsystem application. If the caller redirected our output, it's
        // already usable. Otherwise there's no console to print to, unless we were started
        // from one.
        HANDLE out = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        bool redirected = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
        if (!redirected) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                MessageBoxA(nullptr, args.message().c_str(), "PCSX-Redux", error ? MB_ICONERROR : MB_ICONINFORMATION);
                return error ? 1 : 0;
            }
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
#endif
        fputs(args.message().c_str(), error ? stderr : stdout);
        return error ? 1 : 0;
    }
    // The UvFile and UvFifo should work past this point.
    PCSX::UvThreadOp::UvThread uvThread;

#if defined(_WIN32) || defined(_WIN64)
    if (args.has("stdout") || args.has("no-ui") || args.has("cli") || args.has("tui")) {
        // A parent that captures our output hands us a pipe or a file; keep it
        // instead of opening a console window nobody reads.
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        bool redirected = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
        if (!redirected && AllocConsole()) {
            freopen("CONIN$", "r", stdin);
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
    }
#endif

    // This is an easy early-out.
    if (args.has("dumpproto")) {
        PCSX::SaveStates::ProtoFile::dumpSchema(std::cout);
        return 0;
    }

    // The version query never opens any of these files.
    if (!args.has("version") && !checkCommandLinePaths(args)) return 1;

    // Creating the "system" global object first, making sure anything logging-related is
    // enabled as much as possible.
    SystemImpl *system = new SystemImpl(args);
    PCSX::g_system = system;
    auto sigint = std::signal(SIGINT, handleSignal);
    auto sigterm = std::signal(SIGTERM, handleSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    const auto &logfileArgOpt = args.value("logfile");
    const PCSX::u8string logfileArg = MAKEU8(logfileArgOpt.has_value() ? logfileArgOpt->c_str() : "");
    if (!logfileArg.empty()) system->useLogfile(logfileArg);
    std::filesystem::path self = PCSX::BinPath::getExecutablePath();
    std::filesystem::path binDir = std::filesystem::absolute(self).parent_path();
    system->setBinDir(binDir);
    system->loadAllLocales();

    // This is another early out, which can only be done once we have a system object.
    if (args.has("version")) {
        auto &version = system->getVersion();
        if (version.failed()) {
            fmt::print("Failed to load version.json\n");
            return 1;
        }
        fmt::print(
            "{{\n  \"version\": \"{}\",\n  \"changeset\": \"{}\",\n  \"timestamp\": \"{}\",\n  \"timestampDecoded\": "
            "\"{:%Y-%m-%d %H:%M:%S}\"\n}}\n",
            version.version, version.changeset, version.timestamp, *std::localtime(&version.timestamp));
        return 0;
    }

    // At this point, we're committed to run the emulator, so we first create it, and the UI next.
    PCSX::Emulator *emulator = new PCSX::Emulator();
    PCSX::g_emulator = emulator;
    auto &favorites = emulator->settings.get<PCSX::Emulator::SettingOpenDialogFavorites>().value;

    s_ui = createUI(args, favorites);
    if (!s_ui) {
        fmt::print(stderr, "-tui needs a terminal on both stdin and stdout; use -no-ui for headless runs.\n");
        return 1;
    }
    // Settings will be loaded after this initialization.
    s_ui->init([&emulator, &args, &system]() {
        // Start tweaking / sanitizing settings a bit, while continuing to parse the command line
        // to handle overrides properly.
        auto &emuSettings = emulator->settings;
        auto &debugSettings = emuSettings.get<PCSX::Emulator::SettingDebugSettings>();
        if (emuSettings.get<PCSX::Emulator::SettingMcd1>().empty()) {
            emuSettings.get<PCSX::Emulator::SettingMcd1>() = MAKEU8(u8"memcard1.mcd");
        }

        if (emuSettings.get<PCSX::Emulator::SettingMcd2>().empty()) {
            emuSettings.get<PCSX::Emulator::SettingMcd2>() = MAKEU8(u8"memcard2.mcd");
        }

        auto argPath1 = args.value("memcard1");
        auto argPath2 = args.value("memcard2");
        if (argPath1.has_value()) emuSettings.get<PCSX::Emulator::SettingMcd1>() = argPath1.value();
        if (argPath2.has_value()) emuSettings.get<PCSX::Emulator::SettingMcd2>() = argPath2.value();
        PCSX::u8string path1 = emuSettings.get<PCSX::Emulator::SettingMcd1>().string();
        PCSX::u8string path2 = emuSettings.get<PCSX::Emulator::SettingMcd2>().string();

        emulator->m_sio->loadMcds(path1, path2);
        // The multitap slot B to D cards, port 1 then port 2. SIO reads each one when it is first selected.
        const auto defaultTapMcd = [](auto &setting, const PCSX::u8string &defaultPath) {
            if (setting.empty()) setting = defaultPath;
        };
        defaultTapMcd(emuSettings.get<PCSX::Emulator::SettingMcd1B>(), MAKEU8(u8"memcard1b.mcd"));
        defaultTapMcd(emuSettings.get<PCSX::Emulator::SettingMcd1C>(), MAKEU8(u8"memcard1c.mcd"));
        defaultTapMcd(emuSettings.get<PCSX::Emulator::SettingMcd1D>(), MAKEU8(u8"memcard1d.mcd"));
        defaultTapMcd(emuSettings.get<PCSX::Emulator::SettingMcd2B>(), MAKEU8(u8"memcard2b.mcd"));
        defaultTapMcd(emuSettings.get<PCSX::Emulator::SettingMcd2C>(), MAKEU8(u8"memcard2c.mcd"));
        defaultTapMcd(emuSettings.get<PCSX::Emulator::SettingMcd2D>(), MAKEU8(u8"memcard2d.mcd"));
        auto biosCfg = args.value("bios");
        if (biosCfg.has_value()) emuSettings.get<PCSX::Emulator::SettingBios>() = biosCfg.value();

        system->activateLocale(emuSettings.get<PCSX::Emulator::SettingLocale>());

        if (args.has("debugger")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::Debug>() = true;
        }
        if (args.has("no-debugger")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::Debug>() = false;
        }

        if (args.has("trace")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::Trace>() = true;
        }
        if (args.has("no-trace")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::Trace>() = false;
        }

        if (args.has("8mb")) {
            emuSettings.get<PCSX::Emulator::Setting8MB>() = true;
        }
        if (args.has("2mb")) {
            emuSettings.get<PCSX::Emulator::Setting8MB>() = false;
        }
        // Note: -2mb/-8mb above select main RAM size; the VRAM fitment is a
        // separate axis (1MB retail vs 2MB second-bank boards).
        if (args.has("2mbvram")) {
            emuSettings.get<PCSX::Emulator::Setting2MBVRAM>() = true;
        }
        if (args.has("1mbvram")) {
            emuSettings.get<PCSX::Emulator::Setting2MBVRAM>() = false;
        }

        if (args.has("fastboot")) {
            emuSettings.get<PCSX::Emulator::SettingFastBoot>() = true;
        }
        if (args.has("no-fastboot")) {
            emuSettings.get<PCSX::Emulator::SettingFastBoot>() = false;
        }

        if (args.has("gdb")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::GdbServer>() = true;
        }
        if (args.has("no-gdb")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::GdbServer>() = false;
        }

        if (args.number("gdb-port")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::GdbServerPort>() = args.number("gdb-port").value();
        }

        if (args.has("webserver")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::WebServer>() = true;
        }
        if (args.has("no-webserver")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::WebServer>() = false;
        }

        if (args.number("webserver-port")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::WebServerPort>() = args.number("webserver-port").value();
        }

        auto argPCdrvBase = args.value("pcdrvbase");
        if (args.has("pcdrv")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::PCdrv>() = true;
        }
        if (args.has("no-pcdrv")) {
            debugSettings.get<PCSX::Emulator::DebugSettings::PCdrv>() = false;
        }
        if (argPCdrvBase.has_value()) {
            debugSettings.get<PCSX::Emulator::DebugSettings::PCdrvBase>() = argPCdrvBase.value();
        }
        applyPCdrvNetArgs(args, debugSettings);

        if (args.has("dynarec")) {
            emuSettings.get<PCSX::Emulator::SettingDynarec>() = true;
        }
        if (args.has("interpreter")) {
            emuSettings.get<PCSX::Emulator::SettingDynarec>() = false;
        }

        if (args.has("openglgpu")) {
            emuSettings.get<PCSX::Emulator::SettingHardwareRenderer>() = true;
        }

        if (args.has("softgpu")) {
            emuSettings.get<PCSX::Emulator::SettingHardwareRenderer>() = false;
        }

        if (args.has("softgputhreaded")) {
            emuSettings.get<PCSX::Emulator::SettingSoftGPUThreaded>() = true;
        }
        if (args.has("no-softgputhreaded")) {
            emuSettings.get<PCSX::Emulator::SettingSoftGPUThreaded>() = false;
        }

        if (args.has("kiosk")) {
            emuSettings.get<PCSX::Emulator::SettingKioskMode>() = true;
        }
        if (args.has("no-kiosk")) {
            emuSettings.get<PCSX::Emulator::SettingKioskMode>() = false;
        }
    });

    // Now it's time to mount our iso filesystem
    std::filesystem::path isoToOpen = args.value("iso", "");
    if (isoToOpen.empty()) isoToOpen = args.value("loadiso", "");
    if (isoToOpen.empty()) isoToOpen = args.value("disk", "");
    if (!isoToOpen.empty()) emulator->m_cdrom->setIso(new PCSX::CDRIso(isoToOpen));
    emulator->m_cdrom->parseIso();

    // After settings are loaded, we're fine setting the SPU part of the emulation.
    emulator->m_spu->init();
    auto wavDump = args.value("wavdump", "");
    if (!wavDump.empty() && !emulator->m_spu->setWavDump(PCSX::u8string(MAKEU8(wavDump.c_str())))) {
        fmt::print(stderr, "-wavdump: unable to create '{}'\n", wavDump);
    }

    // Make sure the Lua environment is set.
    bool luacovEnabled = false;
    if (args.has("luacov")) {
        auto L = *emulator->m_lua;
        L.load(
            "package.path = package.path .. "
            "';./lua_modules/share/lua/5.1/?.lua;../../../third_party/luacov/src/?.lua;./third_party/luacov/src/?.lua'",
            "internal:package.path.lua");
        try {
            L.load(R"(
local runner = require 'luacov.runner'
runner.init({
    nameparser = function(name)
        if name:sub(1, 4) == 'src:' then
            return 'src/' .. name:sub(5)
        elseif name:sub(1, 12) == 'third_party:' then
            return 'third_party/' .. name:sub(13)
        end
        return nil
    end,
}))",
                   "internal:luacov.lua");
            luacovEnabled = true;
        } catch (...) {
            luacovEnabled = false;
        }
    }
    emulator->setLua();
    s_ui->setLua(*emulator->m_lua);
    emulator->m_spu->setLua(*emulator->m_lua);
    assert(emulator->m_lua->gettop() == 0);

    // Starting up the whole emulator; we delay setting the GPU only now because why not.
    auto &emuSettings = emulator->settings;
    emulator->m_spu->open();
    emulator->init();
    emulator->m_gpu->init(s_ui);
    emulator->m_gpu->setDither(emuSettings.get<PCSX::Emulator::SettingDither>());
    emulator->m_gpu->setCachedDithering(emuSettings.get<PCSX::Emulator::SettingCachedDithering>());
    emulator->m_gpu->setLinearFiltering();
    emulator->reset();

    // Looking at setting up what to run exactly within the emulator, if requested.
    if (args.has("run")) system->resume();
    s_ui->m_exeToLoad.set(MAKEU8(args.value("loadexe", "").c_str()));
    if (s_ui->m_exeToLoad.empty()) s_ui->m_exeToLoad.set(MAKEU8(args.value("exe", "").c_str()));

    // And finally, let's run things.
    int exitCode = 0;
    {
        // First, set up a closer. This makes sure that everything is shut down gracefully,
        // in the right order, once we exit the scope. This is because of how we're still
        // allowing exceptions to occur.
        Cleaner cleaner([&emulator, &system, &exitCode, luacovEnabled, sigint, sigterm]() {
            emulator->m_spu->close();
            emulator->m_cdrom->clearIso();

            emulator->m_spu->shutdown();
            emulator->m_gpu->shutdown();
            emulator->shutdown();
            s_ui->close();
            delete s_ui;

            if (luacovEnabled) {
                auto L = *emulator->m_lua;
                L.load("(require 'luacov.runner').shutdown()", "internal:luacov-shutdown.lua");
            }

            delete emulator;
            PCSX::g_emulator = nullptr;

            exitCode = system->exitCode();
            std::signal(SIGINT, sigint);
            std::signal(SIGTERM, sigterm);
            delete system;
            PCSX::g_system = nullptr;
        });
        try {
            auto &L = emulator->m_lua;
            // Before going into the main loop, let's first load all of the Lua files
            // from the command-line specified using the -dofile switch.
            auto archives = args.values("archive");
            for (auto &archive : archives) {
                PCSX::IO<PCSX::File> file = new PCSX::PosixFile(archive);
                if (file->failed()) {
                    throw std::runtime_error(fmt::format("Couldn't load file {}", archive));
                }
                PCSX::LuaFFI::addArchive(*L, file);
            }
            auto dofiles = args.values("dofile");
            auto luaexecs = args.values("exec");
            if (!dofiles.empty() || !luaexecs.empty()) PCSX::System::setCrashReportTag("user_lua", "cmdline");
            L->load("return function(name) Support.extra.dofile(name) end", "internal:dofile.lua");
            for (auto &dofile : dofiles) {
                L->copy(-1);
                L->push(dofile);
                L->pcall(1);
            }
            L->pop();

            // Then run all of the Lua "exec" commands.
            for (auto &luaexec : luaexecs) {
                L->load(std::string(luaexec), "cmdline:");
            }

            system->m_inStartup = false;

            // And finally, main loop. One pass is one frame either way: Execute()
            // unwinds at the first block boundary after the emulated vblank, and
            // update() is already a whole frame.
            //
            // Captured BY VALUE. Both are raw pointers and s_ui is file scope, so
            // this outlives pcsxMain's frame - which it has to, because on wasm
            // the callbacks keep running long after this function returns.
            auto oneIteration = [system, emulator]() {
                if (system->hasPendingSaveStateLoad()) {
                    // Somebody asked for a save state from within a callback the
                    // emulator itself made, so we deferred it to here, where the
                    // emulation stack has unwound and nothing is holding values
                    // from before the load. Rewinds come through here too.
                    if (system->hasPendingRestore()) {
                        system->takePendingRestore()();
                    } else if (unsigned rewinds = system->pendingRewinds()) {
                        system->takePendingSaveStateLoad();
                        emulator->rewindStateNow(rewinds);
                    } else if (!PCSX::SaveStates::load(system->takePendingSaveStateLoad())) {
                        PCSX::g_system->message(_("Failed to load save state\n"));
                    }
                } else if (system->running()) {
                    // Runs until the emulated hardware vsyncs, or until paused.
                    system->setInExecute(true);
                    emulator->m_cpu->Execute();
                    system->setInExecute(false);
                } else {
                    // The "update" method will be called periodically by the emulator while
                    // it's running, meaning if we want our UI to work, we have to manually
                    // call "update" when the emulator is paused.
                    s_ui->update();
                    // Core is not executing, so branchTest (and with it stepPocketstation) is not
                    // running. Advance any enabled PocketStation off real wall-clock time so it runs
                    // fully standalone (keeps time, runs its GUI, sleeps cheaply via CLK_STOP). This
                    // branch is mutually exclusive with the running() one above, so the device is
                    // never double-clocked.
                    emulator->m_sio->stepPocketstationWallClock();
                }
            };

#ifdef __EMSCRIPTEN__
            // A browser will not let us keep the thread: the page only paints,
            // delivers input, or runs anything of its own once we return. So the
            // loop belongs to emscripten and we hand it one frame at a time.
            //
            // simulate_infinite_loop is FALSE, and that is the whole trick. The
            // true form escapes its caller by THROWING, and this scope has both
            // a catch (...) below and the Cleaner teardown above - so it unwinds
            // straight through them, closes the SPU, shuts down the GPU and
            // deletes the UI, and the first callback then runs against a
            // destroyed emulator. From the outside that looks nothing like a
            // teardown: "Cannot read properties of undefined (deleteProgram)".
            //
            // So: register, switch the teardown off, and return normally.
            // EXIT_RUNTIME is 0 and a main loop is registered, so the runtime
            // stays alive with everything intact. Nothing below runs on wasm -
            // no pause(), no Quitting signal - which is the honest shape for a
            // tab, since a tab is closed rather than quit.
            static std::function<void()> s_frame;
            s_frame = oneIteration;
            emscripten_set_main_loop([]() { s_frame(); }, 0, false);
            cleaner.disarm();
            return 0;
#else
            while (!system->quitting()) oneIteration();
#endif
            system->pause();
            system->m_eventBus->signal(PCSX::Events::Quitting{});
            system->purgeAllEvents();
        } catch (...) {
            // This will ensure we don't do certain cleanups that are awaiting other tasks,
            // which could result in deadlocks on exit in case we encountered a serious problem.
            // This may cause data loss when writing files, but that's life when encountering
            // a serious problem in a software. An error from the command-line archives or
            // Lua scripts happens before anything is running, so that one still shuts down
            // normally; skipping the uv thread's join there aborts the process on exit.
            if (!system->m_inStartup) {
                system->setEmergencyExit();
                uvThread.setEmergencyExit();
            }
            throw;
        }
    }

    return exitCode;
}
