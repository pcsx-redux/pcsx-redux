/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
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

#include "core/commandline.h"

#include <memory>
#include <sstream>
#include <stdexcept>

#include "args/args.hxx"

namespace {

enum class Kind {
    Switch,    // -run
    Value,     // -loadexe file
    Number,    // -gdb-port 3333
    Optional,  // -portable, or -portable path
    List,      // -dofile a.lua -dofile b.lua
};

struct Option {
    const char* name;
    Kind kind;
    const char* help;
};

constexpr Option c_options[] = {
    {"version", Kind::Switch, "Print the version information as json and exit."},
    {"dumpproto", Kind::Switch, "Print the save state protobuf schema and exit."},

    {"no-ui", Kind::Switch, "Run without the graphical interface. Implies -stdout and -lua_stdout."},
    {"cli", Kind::Switch, "Same as -no-ui, and also implies -safe."},
    {"stdout", Kind::Switch, "Print the logs to stdout."},
    {"lua_stdout", Kind::Switch, "Print the Lua console output to stdout."},
    {"tui", Kind::Switch,
     "Run in the terminal, with the logs and a Lua console. Needs a terminal on both stdin and stdout."},
    {"no-gui-log", Kind::Switch, "Disable the logs window."},
    {"logfile", Kind::Value, "Write the logs to this file."},
    {"testmode", Kind::Switch,
     "Test mode: quit when the emulated software requests an exit, with its exit code, and don't load or save the "
     "settings."},

    {"portable", Kind::Optional, "Store the settings and other files next to the binary, or in the given directory."},
    {"no-portable", Kind::Switch, "Don't use portable mode, even if a pcsx.json file is found."},
    {"safe", Kind::Switch, "Don't load the pcsx.json settings file."},
    {"resetui", Kind::Switch, "Reset the window layout."},
    {"noshaders", Kind::Switch, "Disable the output shaders."},
    {"noupdate", Kind::Switch, "Disable the update check."},
    {"shmdisplay", Kind::Switch, "Share the display and the pad input with a host application through shared memory."},
    {"viewports", Kind::Switch, "Enable the multi-viewports support."},
    {"no-viewports", Kind::Switch, "Disable the multi-viewports support."},
    {"kiosk", Kind::Switch, "Enable kiosk mode."},
    {"no-kiosk", Kind::Switch, "Disable kiosk mode."},

    {"bios", Kind::Value, "Use this BIOS file."},
    {"memcard1", Kind::Value, "Use this file for the first memory card."},
    {"memcard2", Kind::Value, "Use this file for the second memory card."},
    {"iso", Kind::Value, "Load this disc image."},
    {"loadiso", Kind::Value, "Same as -iso."},
    {"disk", Kind::Value, "Same as -iso."},
    {"loadexe", Kind::Value, "Load this binary when the BIOS reaches the shell."},
    {"exe", Kind::Value, "Same as -loadexe."},
    {"run", Kind::Switch, "Start the emulation right away."},
    {"8mb", Kind::Switch, "Emulate 8MB of main RAM."},
    {"2mb", Kind::Switch, "Emulate 2MB of main RAM."},
    {"2mbvram", Kind::Switch, "Emulate 2MB of VRAM."},
    {"1mbvram", Kind::Switch, "Emulate 1MB of VRAM, as on retail consoles."},
    {"fastboot", Kind::Switch, "Skip the BIOS shell."},
    {"no-fastboot", Kind::Switch, "Don't skip the BIOS shell."},
    {"dynarec", Kind::Switch, "Use the dynamic recompiler."},
    {"interpreter", Kind::Switch, "Use the interpreter."},
    {"openglgpu", Kind::Switch, "Use the OpenGL GPU renderer."},
    {"softgpu", Kind::Switch, "Use the software GPU renderer."},
    {"wavdump", Kind::Value, "Write the SPU output to this WAV file."},
    {"softgputhreaded", Kind::Switch, "Run the software GPU renderer on a separate thread."},
    {"no-softgputhreaded", Kind::Switch, "Run the software GPU renderer on the emulation thread."},

    {"debugger", Kind::Switch, "Enable the debugger."},
    {"no-debugger", Kind::Switch, "Disable the debugger."},
    {"trace", Kind::Switch, "Enable the CPU trace."},
    {"no-trace", Kind::Switch, "Disable the CPU trace."},
    {"profile", Kind::Value,
     "Profile the CPU from boot and write a pprof profile to this file on exit (interpreter only)."},
    {"callgrind", Kind::Value,
     "Profile the CPU from boot and write a callgrind profile to this file on exit (interpreter only)."},
    {"gdb", Kind::Switch, "Enable the GDB server."},
    {"no-gdb", Kind::Switch, "Disable the GDB server."},
    {"gdb-port", Kind::Number, "Port for the GDB server."},
    {"webserver", Kind::Switch, "Enable the web server."},
    {"no-webserver", Kind::Switch, "Disable the web server."},
    {"webserver-port", Kind::Number, "Port for the web server."},
    {"pcdrv", Kind::Switch, "Enable the PCdrv host filesystem."},
    {"no-pcdrv", Kind::Switch, "Disable the PCdrv host filesystem."},
    {"pcdrvbase", Kind::Value, "Root directory for PCdrv."},
    {"pcdrvnet", Kind::Switch, "Let PCdrv open tcp://host:port network connections."},
    {"pcdrvnetallow", Kind::Value,
     "Comma separated host:port list that -pcdrvnet may connect to. Empty allows any destination."},

    {"archive", Kind::List, "Mount this zip archive in the Lua environment. Can be repeated."},
    {"dofile", Kind::List, "Run this Lua file on startup. Can be repeated."},
    {"exec", Kind::List, "Run this Lua code on startup, after the -dofile ones. Can be repeated."},
    {"luacov", Kind::Switch, "Collect Lua code coverage."},
};

const Option* lookupOption(std::string_view name) {
    for (auto& option : c_options) {
        if (name == option.name) return &option;
    }
    return nullptr;
}

// Historically, any number of leading dashes was accepted, so keep accepting --flag for
// the declared flags. Values are left alone, as Lua code passed to -exec may well start
// with two dashes.
std::vector<std::string> normalize(int argc, const char* const* argv) {
    std::vector<std::string> ret;
    bool expectingValue = false;
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        if (expectingValue) {
            expectingValue = false;
            ret.emplace_back(arg);
            continue;
        }
        auto dashes = arg.find_first_not_of('-');
        if (dashes == 0 || dashes == std::string_view::npos) {
            ret.emplace_back(arg);
            continue;
        }
        auto stripped = arg.substr(dashes);
        auto equal = stripped.find('=');
        auto name = stripped.substr(0, equal);
        if (name == "help" || name == "h") {
            ret.emplace_back("-" + std::string(stripped));
            continue;
        }
        auto option = lookupOption(name);
        if (!option) {
            ret.emplace_back(arg);
            continue;
        }
        expectingValue = equal == std::string_view::npos &&
                         (option->kind == Kind::Value || option->kind == Kind::Number || option->kind == Kind::List);
        ret.emplace_back("-" + std::string(stripped));
    }
    return ret;
}

const Option& findOption(std::string_view name) {
    auto option = lookupOption(name);
    if (!option) throw std::logic_error("Undeclared command line flag: " + std::string(name));
    return *option;
}

}  // namespace

PCSX::CommandLine::CommandLine(int argc, const char* const* argv) {
    args::ArgumentParser parser("PCSX-Redux, a PlayStation emulator and development tool.",
                                "Flags start with a single dash. Values can be given as the next argument, "
                                "or with an equal sign, as in -loadexe=file.ps-exe.");
    parser.Prog("pcsx-redux");
    parser.LongPrefix("-");
    parser.ShortPrefix("-");
    args::HelpFlag help(parser, "help", "Show this help and exit.", {"h", "help"});

    std::vector<std::unique_ptr<args::Flag>> switches;
    std::vector<std::unique_ptr<args::ValueFlag<std::string>>> strings;
    std::vector<std::unique_ptr<args::ValueFlag<int>>> numbers;
    std::vector<std::unique_ptr<args::ImplicitValueFlag<std::string>>> optionals;
    std::vector<std::unique_ptr<args::ValueFlagList<std::string>>> lists;
    for (auto& option : c_options) {
        switch (option.kind) {
            case Kind::Switch:
                switches.emplace_back(new args::Flag(parser, option.name, option.help, {option.name}));
                break;
            case Kind::Value:
                strings.emplace_back(new args::ValueFlag<std::string>(parser, option.name, option.help, {option.name}));
                break;
            case Kind::Number:
                numbers.emplace_back(new args::ValueFlag<int>(parser, option.name, option.help, {option.name}));
                break;
            case Kind::Optional:
                optionals.emplace_back(new args::ImplicitValueFlag<std::string>(
                    parser, option.name, option.help, {option.name}, std::string(), std::string()));
                break;
            case Kind::List:
                lists.emplace_back(
                    new args::ValueFlagList<std::string>(parser, option.name, option.help, {option.name}));
                break;
        }
    }
    args::PositionalList<std::string> positionals(parser, "", "", args::Options::Hidden);

    try {
        parser.ParseArgs(normalize(argc, argv));
    } catch (const args::Help&) {
        std::ostringstream str;
        str << parser;
        m_status = Status::Help;
        m_message = str.str();
        return;
    } catch (const args::Error& e) {
        m_status = Status::Error;
        m_message = std::string(e.what()) + "\nRun with -help for the list of flags.\n";
        return;
    }

    if (positionals) {
        m_status = Status::Error;
        m_message =
            "Unexpected argument: " + args::get(positionals).front() + "\nRun with -help for the list of flags.\n";
        return;
    }

    auto sw = switches.begin();
    auto st = strings.begin();
    auto nu = numbers.begin();
    auto op = optionals.begin();
    auto li = lists.begin();
    for (auto& option : c_options) {
        std::vector<std::string> values;
        bool present = false;
        switch (option.kind) {
            case Kind::Switch:
                present = !!**sw++;
                break;
            case Kind::Value:
                present = !!**st;
                if (present) values.push_back(args::get(**st));
                st++;
                break;
            case Kind::Number:
                present = !!**nu;
                if (present) values.push_back(std::to_string(args::get(**nu)));
                nu++;
                break;
            case Kind::Optional:
                present = !!**op;
                if (present) values.push_back(args::get(**op));
                op++;
                break;
            case Kind::List:
                present = !!**li;
                values = args::get(**li);
                li++;
                break;
        }
        if (present) m_values.emplace(option.name, std::move(values));
    }
}

bool PCSX::CommandLine::has(std::string_view name) const {
    findOption(name);
    return m_values.find(name) != m_values.end();
}

std::optional<std::string> PCSX::CommandLine::value(std::string_view name) const {
    findOption(name);
    auto it = m_values.find(name);
    if (it == m_values.end() || it->second.empty()) return std::nullopt;
    return it->second.back();
}

std::string PCSX::CommandLine::value(std::string_view name, std::string_view defaultValue) const {
    auto v = value(name);
    return v.has_value() ? *v : std::string(defaultValue);
}

std::optional<int> PCSX::CommandLine::number(std::string_view name) const {
    if (findOption(name).kind != Kind::Number) throw std::logic_error("Not a number flag: " + std::string(name));
    auto v = value(name);
    if (!v.has_value()) return std::nullopt;
    return std::stoi(*v);
}

std::vector<std::string> PCSX::CommandLine::values(std::string_view name) const {
    findOption(name);
    auto it = m_values.find(name);
    if (it == m_values.end()) return {};
    return it->second;
}
