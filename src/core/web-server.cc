/***************************************************************************
 *   Copyright (C) 2020 PCSX-Redux authors                                 *
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

// v1 wasm has no libuv, so no web server.
#ifndef __EMSCRIPTEN__

#include "core/web-server.h"

#include <llhttp.h>
#include <multipart_parser.h>
#include <uv.h>

#include <charconv>
#include <magic_enum/magic_enum_all.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>

#include "GL/gl3w.h"
#include "cdrom/cdriso.h"
#include "cdrom/file.h"
#include "cdrom/iso9660-reader.h"
#include "core/cdrom.h"
#include "core/debug.h"
#include "core/gpu.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/screenshot.h"
#include "core/system.h"
#include "gui/gui.h"
#include "lua/luawrapper.h"
#include "support/file.h"
#include "support/hashtable.h"
#include "support/strings-helpers.h"
#include "supportpsx/iso9660-builder.h"
#include "uriparser/Uri.h"

namespace {

static const std::unordered_set<std::string> stepFunctions = {"stepIn", "stepOver", "stepOut"};

class VramExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return urldata.path == "/api/v1/gpu/vram/raw";
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        if (request.method == PCSX::RequestData::Method::HTTP_HTTP_GET) {
            client->write(
                "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n");
            client->write(PCSX::g_emulator->m_gpu->getVRAM());

            return true;
        } else if (request.method == PCSX::RequestData::Method::HTTP_POST) {
            auto vars = parseQuery(request.urlData.query);
            auto ix = vars.find("x");
            auto iy = vars.find("y");
            auto iwidth = vars.find("width");
            auto iheight = vars.find("height");
            if ((ix == vars.end()) || (iy == vars.end()) || (iwidth == vars.end()) || (iheight == vars.end()) ||
                (!ix->second.has_value()) || (!iy->second.has_value()) || (!iwidth->second.has_value()) ||
                (!iheight->second.has_value())) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            auto x = std::stoi(ix->second.value());
            auto y = std::stoi(iy->second.value());
            auto width = std::stoi(iwidth->second.value());
            auto height = std::stoi(iheight->second.value());
            if ((x < 0) || (y < 0) || (width < 0) || (height < 0)) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            if ((x > 1024) || (y > 512) || ((x + width) > 1024) || ((y + height) > 512)) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            auto size = width * height * sizeof(uint16_t);
            if (size != request.body.size()) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }

            PCSX::g_emulator->m_gpu->partialUpdateVRAM(x, y, width, height, request.body.data<uint16_t>());
            client->write("HTTP/1.1 200 OK\r\n\r\n");
            return true;
        }
        return false;
    }

  public:
    VramExecutor() {}
    virtual ~VramExecutor() = default;
};

class RamExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return urldata.path == "/api/v1/cpu/ram/raw";
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        const auto& ram8M = PCSX::g_emulator->settings.get<PCSX::Emulator::Setting8MB>().value;
        if (request.method == PCSX::RequestData::Method::HTTP_HTTP_GET) {
            if (ram8M) {
                client->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 8388608\r\n\r\n");
            } else {
                client->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 2097152\r\n\r\n");
            }
            uint32_t size = 1024 * 1024 * (ram8M ? 8 : 2);
            uint8_t* data = (uint8_t*)malloc(size);
            memcpy(data, PCSX::g_emulator->m_mem->m_wram, size);
            PCSX::Slice slice;
            slice.acquire(data, size);
            client->write(std::move(slice));
            return true;
        } else if (request.method == PCSX::RequestData::Method::HTTP_POST) {
            const auto ramSize = (ram8M ? 8 : 2) * 1024 * 1024;
            auto vars = parseQuery(request.urlData.query);
            auto ioffset = vars.find("offset");
            auto isize = vars.find("size");
            if ((ioffset == vars.end()) || (isize == vars.end()) || (!isize->second.has_value()) ||
                (!ioffset->second.has_value())) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            auto offset = std::stoul(ioffset->second.value());
            auto size = std::stoul(isize->second.value());
            if ((offset >= ramSize) || (size > ramSize) || ((offset + size) > ramSize)) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            if (size != request.body.size()) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }

            memcpy(PCSX::g_emulator->m_mem->m_wram + offset, request.body.data<uint8_t>(), size);
            client->write("HTTP/1.1 200 OK\r\n\r\n");
            return true;
        }
        return false;
    }

  public:
    RamExecutor() = default;
    virtual ~RamExecutor() = default;
};

class AssemblyExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return urldata.path == "/api/v1/assembly/symbols";
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        if (request.method == PCSX::RequestData::Method::HTTP_POST) {
            auto vars = parseQuery(request.urlData.query);
            auto ifunction = vars.find("function");
            if (ifunction == vars.end()) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            std::string function = ifunction->second.value_or("");
            auto& cpu = PCSX::g_emulator->m_cpu;
            if (function.compare("reset") == 0) {
                cpu->m_symbols.clear();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            if (function.compare("upload") == 0) {
                auto& body = request.body;
                PCSX::IO<PCSX::File> file = new PCSX::BufferFile(std::move(body));
                while (file && !file->failed() && !file->eof()) {
                    auto line = file->gets();
                    auto tokens = PCSX::StringsHelpers::split(std::string_view(line), " ");
                    if (tokens.size() != 2) {
                        continue;
                    }
                    auto addressStr = tokens[0];
                    auto name = tokens[1];
                    uint32_t address;
                    auto result =
                        std::from_chars(addressStr.data(), addressStr.data() + addressStr.size(), address, 16);
                    if (result.ec == std::errc::invalid_argument) continue;

                    cpu->m_symbols[address] = name;
                }
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
            return true;
        }
        return false;
    }

  public:
    AssemblyExecutor() = default;
    virtual ~AssemblyExecutor() = default;
};

class CacheExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return urldata.path == "/api/v1/cpu/cache";
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        if (request.method == PCSX::RequestData::Method::HTTP_POST) {
            auto vars = parseQuery(request.urlData.query);
            auto ifunction = vars.find("function");
            if (ifunction == vars.end()) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            std::string function = ifunction->second.value_or("");
            if (function.compare("flush") == 0) {
                PCSX::g_emulator->m_cpu->invalidateCache();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
            return true;
        }
        return false;
    }

  public:
    CacheExecutor() = default;
    virtual ~CacheExecutor() = default;
};

class FlowExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return urldata.path == "/api/v1/execution-flow";
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        auto& debugSettings = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>();
        bool debugEnabled = debugSettings.get<PCSX::Emulator::DebugSettings::Debug>().value;

        if (request.method == PCSX::RequestData::Method::HTTP_HTTP_GET) {
            nlohmann::json j;
            j["running"] = PCSX::g_system->running();
            j["isDynarec"] = PCSX::g_emulator->m_cpu->isDynarec();
            j["8mb"] = PCSX::g_emulator->settings.get<PCSX::Emulator::Setting8MB>().value;
            j["debugger"] = debugEnabled;
            write200(client, j);
            return true;
        } else if (request.method == PCSX::RequestData::Method::HTTP_POST) {
            auto vars = parseQuery(request.urlData.query);
            auto ifunction = vars.find("function");
            if (ifunction == vars.end()) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            std::string function = ifunction->second.value_or("");
            if (function.compare("start") == 0) {
                PCSX::g_system->resume();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            if (function.compare("pause") == 0) {
                PCSX::g_system->pause();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            if (function.compare("resume") == 0) {
                PCSX::g_system->resume();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            if (stepFunctions.contains(function) && !debugEnabled) {
                client->write("HTTP/1.1 503 Debugger not enabled\r\n\r\n");
                return false;
            }
            if (function.compare("stepIn") == 0) {
                PCSX::g_emulator->m_debug->stepIn();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            if (function.compare("stepOver") == 0) {
                PCSX::g_emulator->m_debug->stepOver();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            if (function.compare("stepOut") == 0) {
                PCSX::g_emulator->m_debug->stepOut();
                client->write("HTTP/1.1 200 OK\r\n\r\n");
                return true;
            }
            /* Start of functions that requires a type */
            auto itype = vars.find("type");
            if (itype == vars.end()) {
                client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                return true;
            }
            std::string type = itype->second.value_or("");
            if (function.compare("reset") == 0) {
                if (type.compare("hard") == 0) {
                    PCSX::g_system->hardReset();
                    client->write("HTTP/1.1 200 OK\r\n\r\n");
                    return true;
                }
                if (type.compare("soft") == 0) {
                    PCSX::g_system->softReset();
                    client->write("HTTP/1.1 200 OK\r\n\r\n");
                    return true;
                }
            }
            client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
            return true;
        }
        return false;
    }

  public:
    FlowExecutor() = default;
    virtual ~FlowExecutor() = default;
};

class LuaExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return PCSX::StringsHelpers::startsWith(urldata.path, c_prefix);
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        auto L = *PCSX::g_emulator->m_lua;
        L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
        L.getfieldtable("WebServer");
        L.getfieldtable("Handlers");
        L.getfield(request.urlData.path.substr(c_prefix.length()));
        auto x = L.type();
        if (L.isfunction()) {
            L.newtable();
            L.push("urlData");
            L.newtable();
            L.push("schema");
            L.push(request.urlData.schema);
            L.settable();
            L.push("host");
            L.push(request.urlData.host);
            L.settable();
            L.push("port");
            L.push(request.urlData.port);
            L.settable();
            L.push("path");
            L.push(request.urlData.path);
            L.settable();
            L.push("query");
            L.push(request.urlData.query);
            L.settable();
            L.push("fragment");
            L.push(request.urlData.fragment);
            L.settable();
            L.push("userInfo");
            L.push(request.urlData.userInfo);
            L.settable();
            L.settable();
            L.push("method");
            L.push(magic_enum::enum_name(request.method).substr(5));
            L.settable();
            L.push("headers");
            L.newtable();
            for (auto& header : request.headers) {
                L.getfieldtable(header.first);
                L.push(lua_Number(L.length() + 1));
                L.push(header.second);
                L.settable();
                L.pop();
            }
            L.settable();
            L.push("form");
            L.newtable();
            for (auto& variable : request.form) {
                L.getfieldtable(variable.first);
                L.push(lua_Number(L.length() + 1));
                L.push(variable.second);
                L.settable();
                L.pop();
            }
            L.settable();
            L.push("body");
            L.push(request.body.asString());
            L.settable();
            try {
                L.pcall(1);
                if (L.isstring()) {
                    auto response = L.tostring();
                    if (PCSX::StringsHelpers::startsWith(response, "HTTP/")) {
                        client->write(std::move(response));
                    } else {
                        std::string message = std::string(
                                                  "HTTP/1.1 200 OK\r\n"
                                                  "Content-Length: ") +
                                              std::to_string(response.size()) + std::string("\r\n\r\n") + response;
                        client->write(std::move(message));
                    }
                } else {
                    client->write(
                        "HTTP/1.1 500 Internal Server Error\r\n\r\nThe Lua script didn't return a string.\r\n");
                }
            } catch (std::exception& e) {
                std::string message = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
                message += e.what();
                message += "\r\n";
                client->write(std::move(message));
            } catch (...) {
                client->write(
                    "HTTP/1.1 500 Internal Server Error\r\n\r\nAn unknown error occured while running Lua code.\r\n");
            }
        } else {
            client->write("HTTP/1.1 404 Not Found\r\n\r\nURL Not found.\r\n");
        }

        while (L.gettop()) L.pop();

        return true;
    }

  public:
    const std::string_view c_prefix = "/api/v1/lua/";
    LuaExecutor() = default;
    virtual ~LuaExecutor() = default;
};

class CDExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return PCSX::StringsHelpers::startsWith(urldata.path, c_prefix);
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        auto path = request.urlData.path.substr(c_prefix.length());
        auto& cdrom = PCSX::g_emulator->m_cdrom;
        auto iso = cdrom->getIso();
        PCSX::ISO9660Reader reader(iso);

        if (request.method == PCSX::RequestData::Method::HTTP_HTTP_GET) {
            if (path == "info") {
                nlohmann::json j;
                j["id"] = cdrom->getCDRomID();
                j["label"] = cdrom->getCDRomLabel();
                j["iso"]["TN"] = iso->getTN();
                for (unsigned t = 0; t <= iso->getTN(); t++) {
                    if (t != 0) {
                        j["iso"]["tracktype"][t] = magic_enum::enum_name(iso->getTrackType(t));
                    }
                    auto duration = iso->getTD(t);
                    j["iso"]["TD"][t]["m"] = duration.m;
                    j["iso"]["TD"][t]["s"] = duration.s;
                    j["iso"]["TD"][t]["f"] = duration.f;
                }
                write200(client, j);
                return true;
            } else if (path == "files") {
                auto vars = parseQuery(request.urlData.query);
                auto filename = vars.find("filename");
                if (filename == vars.end()) {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                    return true;
                }
                PCSX::IO<PCSX::File> file = reader.open(filename->second.value_or(""));
                if (file->failed()) {
                    std::string message = fmt::format(
                        "HTTP/1.1 404 File Not Found\r\n\r\nFile `{}` was not found in the currently loaded disc "
                        "image.",
                        filename->second.value_or(""));
                    client->write(std::move(message));
                    return true;
                }
                auto size = file->size();
                client->write(std::string("HTTP/1.1 200 OK\r\n"
                                          "Content-Type: application/octet-stream\r\n"
                                          "Content-Length: ") +
                              std::to_string(size) + std::string("\r\n\r\n"));
                auto buffer = file->read(size);
                client->write(std::move(buffer));
                return true;
            }
            return false;
        } else if (request.method == PCSX::RequestData::Method::HTTP_POST) {
            if (path == "patch") {
                auto vars = parseQuery(request.urlData.query);
                auto filename = vars.find("filename");
                auto sector = vars.find("sector");
                auto modeStr = vars.find("mode");
                PCSX::IEC60908b::SectorMode mode = PCSX::IEC60908b::SectorMode::GUESS;
                if ((filename == vars.end()) && (sector == vars.end())) {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                    return true;
                }
                if ((filename != vars.end()) && (sector != vars.end())) {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                    return true;
                }
                if (modeStr != vars.end()) {
                    auto modeCast = magic_enum::enum_cast<PCSX::IEC60908b::SectorMode>(modeStr->second.value_or(""));
                    if (modeCast.has_value()) {
                        mode = modeCast.value();
                    } else {
                        client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                        return true;
                    }
                }

                if (reader.failed()) {
                    client->write("HTTP/1.1 404 File Not Found\r\n\r\nNo disc image currently loaded.");
                    return true;
                }

                PCSX::IO<PCSX::File> file;

                if (filename != vars.end()) {
                    file = reader.open(filename->second.value_or(""));
                }

                if (sector != vars.end()) {
                    auto sectorNumber = std::stoul(sector->second.value_or(""));
                    file = new PCSX::CDRIsoFile(iso, sectorNumber, request.body.size(), mode);
                }

                if (file->failed()) {
                    std::string message = fmt::format(
                        "HTTP/1.1 404 File Not Found\r\n\r\nFile {} was not found in the currently loaded disc image.",
                        filename->second.value_or(""));
                    client->write(std::move(message));
                    return true;
                }

                file->write(request.body.data<uint8_t>(), request.body.size());
                client->write("HTTP/1.1 200 OK\r\n\r\nDisc image has been patched successfully.");
                return true;
            } else if (path == "ppf") {
                auto vars = parseQuery(request.urlData.query);
                auto function = vars.find("function");
                if (function == vars.end()) {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                    return true;
                }
                if (function->second == "save") {
                    iso->getPPF()->save(iso->getIsoPath());
                } else if (function->second == "clear") {
                    iso->getPPF()->clear();
                } else {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\nUnknown function.");
                    return true;
                }
                client->write("HTTP/1.1 200 OK\r\n\r\n");
            }
            return false;
        }
        return false;
    }

  public:
    const std::string_view c_prefix = "/api/v1/cd/";
    CDExecutor() = default;
    virtual ~CDExecutor() = default;
};

class StateExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return PCSX::StringsHelpers::startsWith(urldata.path, c_prefix);
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        if (PCSX::g_gui == nullptr) {
            client->write("HTTP/1.1 500 Internal Server Error\r\n\r\nSave states unavailable in CLI/no-UI mode.");
            return false;
        }
        auto path = request.urlData.path.substr(c_prefix.length());

        if (request.method == PCSX::RequestData::Method::HTTP_HTTP_GET) {
            if (path == "usage") {
                nlohmann::json j;
                for (uint32_t i = 0; i < 10; ++i) {
                    j["slots"][i] = PCSX::g_gui->getSaveStateExists(i);
                }
                const auto& namedSaves = PCSX::g_gui->getNamedSaveStates();
                for (uint32_t i = 0; i < namedSaves.size(); ++i) {
                    const auto& filenamePair = namedSaves[i];
                    j["named"][i]["name"] = filenamePair.second;
                    j["named"][i]["filepath"] = filenamePair.first.string();
                }
                write200(client, j);
                return true;
            } else if (path == "load" || path == "save" || path == "delete") {
                auto vars = parseQuery(request.urlData.query);
                auto islot = vars.find("slot");
                auto iname = vars.find("name");
                if ((islot == vars.end()) && (iname == vars.end())) {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                    return true;
                }
                if ((islot != vars.end()) && (iname != vars.end())) {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                    return true;
                }
                if (islot != vars.end()) {
                    std::string message;
                    int slot = -1;
                    if (!islot->second.has_value()) {
                        message = "HTTP/1.1 400 Bad Request\r\n\r\nState slot value is empty.";
                    } else {
                        try {
                            slot = std::stoul(islot->second.value());
                        } catch (std::exception const& ex) {
                            message =
                                fmt::format("HTTP/1.1 400 Bad Request\r\n\r\nFailed to parse state slot value \"{}\".",
                                            islot->second.value());
                            client->write(std::move(message));
                            return true;
                        }
                    }
                    if (slot < 0 || slot >= 10) {
                        message =
                            fmt::format("HTTP/1.1 400 Bad Request\r\n\r\nState slot index {} out of range 0-9.", slot);
                    } else {
                        bool success = false;
                        if (path == "load") {
                            success = PCSX::g_gui->loadSaveStateSlot(slot);
                        } else if (path == "save") {
                            success = PCSX::g_gui->saveSaveStateSlot(slot);
                        } else if (path == "delete") {
                            success = PCSX::g_gui->deleteSaveStateSlot(slot);
                        }
                        if (success) {
                            message =
                                fmt::format("HTTP/1.1 200 OK\r\n\r\nState slot index {} {} successful.", slot, path);
                        } else {
                            message = fmt::format(
                                "HTTP/1.1 500 Internal Server Error\r\n\r\nState slot index {} {} failed.", slot, path);
                        }
                    }
                    client->write(std::move(message));
                    return true;
                } else if (iname != vars.end()) {
                    std::string message;
                    auto name = iname->second.value_or("");
                    if (name.empty()) {
                        message = "HTTP/1.1 400 Bad Request\r\n\r\nState name is empty.";
                    } else if (name.length() > PCSX::Widgets::NamedSaveStates::NAMED_SAVE_STATE_LENGTH_MAX) {
                        message = fmt::format(
                            "HTTP/1.1 400 Bad Request\r\n\r\nState name \"{}\" exceeds {} characters in length.", name,
                            PCSX::Widgets::NamedSaveStates::NAMED_SAVE_STATE_LENGTH_MAX);
                    } else {
                        for (char c : name) {
                            if (!PCSX::Widgets::NamedSaveStates::TextFilters::isValid(c)) {
                                message = fmt::format(
                                    "HTTP/1.1 400 Bad Request\r\n\r\nState name \"{}\" includes invalid character(s).",
                                    name);
                                break;
                            }
                        }
                    }
                    if (message.empty()) {
                        std::filesystem::path saveFilepath =
                            PCSX::Widgets::NamedSaveStates::createSaveStatePath(PCSX::g_gui, name);
                        bool success = false;
                        if (path == "load") {
                            success = PCSX::g_gui->loadSaveState(saveFilepath);
                        } else if (path == "save") {
                            success = PCSX::g_gui->saveSaveState(saveFilepath);
                        } else if (path == "delete") {
                            success = PCSX::g_gui->deleteSaveState(saveFilepath);
                        }
                        if (success) {
                            message =
                                fmt::format("HTTP/1.1 200 OK\r\n\r\nState slot name \"{}\" {} successful.", name, path);
                        } else {
                            message = fmt::format(
                                "HTTP/1.1 500 Internal Server Error\r\n\r\nState slot name \"{}\" {} failed.", name,
                                path);
                        }
                    }
                    client->write(std::move(message));
                    return true;
                }
            }
        }
        return false;
    }

  public:
    const std::string_view c_prefix = "/api/v1/state/";
    StateExecutor() = default;
    virtual ~StateExecutor() = default;
};

class ScreenExecutor : public PCSX::WebExecutor {
    virtual bool match(PCSX::WebClient* client, const PCSX::UrlData& urldata) final {
        return PCSX::StringsHelpers::startsWith(urldata.path, c_prefix);
    }
    virtual bool execute(PCSX::WebClient* client, PCSX::RequestData& request) final {
        auto path = request.urlData.path.substr(c_prefix.length());

        if (request.method == PCSX::RequestData::Method::HTTP_HTTP_GET) {
            if (path == "save") {
                auto vars = parseQuery(request.urlData.query);
                auto ifilepath = vars.find("filepath");
                if (ifilepath == vars.end()) {
                    client->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                    return true;
                }
                std::string message;
                std::filesystem::path path = std::filesystem::path(ifilepath->second.value_or("").c_str());
                if (path.is_relative()) {
                    std::filesystem::path persistentDir = PCSX::g_system->getPersistentDir();
                    if (persistentDir.empty()) {
                        persistentDir = std::filesystem::current_path();
                    }
                    path = persistentDir / path;
                }
                auto screenshot = PCSX::g_emulator->m_gpu->takeScreenShot();
                clip::image img = PCSX::ScreenShot::convertScreenshotToImage(std::move(screenshot));
                bool success = PCSX::ScreenShot::writeImagePNG(path.string(), std::move(img));
                if (success) {
                    message =
                        fmt::format("HTTP/1.1 200 OK\r\n\r\nScreenshot saved successfully to \"{}\".", path.string());
                } else {
                    message =
                        fmt::format("HTTP/1.1 500 Internal Server Error\r\n\r\nFailed to save screenshot to \"{}\".",
                                    path.string());
                }
                client->write(std::move(message));
                return true;
            } else if (path == "still") {
                auto screenshot = PCSX::g_emulator->m_gpu->takeScreenShot();
                clip::image img = PCSX::ScreenShot::convertScreenshotToImage(std::move(screenshot));
                writeImagePNG(client, std::move(img));
                return true;
            }
        }
        return false;
    }
    bool writeImagePNG(PCSX::WebClient* client, clip::image&& img) {
        std::vector<uint8_t> pngData;
        bool success = img.export_to_png(pngData);
        if (!success) {
            client->write("HTTP/1.1 500 Internal Server Error\r\n\r\n");
            return false;
        }
        client->write(std::string("HTTP/1.1 200 OK\r\n"));
        client->write(std::string("Cache-Control: no-cache, must-revalidate\r\n"));
        client->write(std::string("Expires: Fri, 31 Dec 1999 23:59:59 GMT\r\n"));
        client->write(std::string("Content-Type: image/png\r\n"));
        client->write(std::string("Content-Length: " + std::to_string(pngData.size()) + "\r\n\r\n"));
        PCSX::Slice slice;
        slice.copy(pngData.data(), pngData.size());
        client->write(std::move(slice));
        return true;
    }

  public:
    const std::string_view c_prefix = "/api/v1/screen/";
    ScreenExecutor() = default;
    virtual ~ScreenExecutor() = default;
};

}  // namespace

std::multimap<std::string, std::optional<std::string>> PCSX::WebExecutor::parseQuery(std::string_view query) {
    UriQueryListA* queryList;
    int itemCount;
    std::multimap<std::string, std::optional<std::string>> ret;
    const char* queryStart = query.data();
    const char* queryEnd = query.data() + query.size();
    if (uriDissectQueryMallocA(&queryList, &itemCount, queryStart, queryEnd) == URI_SUCCESS) {
        auto item = queryList;
        for (int i = 0; i < itemCount; ++i) {
            ret.emplace(item->key, item->value ? std::optional<std::string>{item->value} : std::nullopt);
            item = item->next;
        }
        uriFreeQueryListA(queryList);
    }

    return ret;
}

void PCSX::WebExecutor::write200(PCSX::WebClient* client, const nlohmann::json& j) {
    std::string json = j.dump();
    std::string message = std::string(
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: ") +
                          std::to_string(json.size()) + std::string("\r\n\r\n") + json;
    client->write(std::move(message));
}

PCSX::WebServer::WebServer() : Network::Server("Web Server"), m_listener(g_system->m_eventBus) {
    m_executors.push_back(new VramExecutor());
    m_executors.push_back(new RamExecutor());
    m_executors.push_back(new AssemblyExecutor());
    m_executors.push_back(new CacheExecutor());
    m_executors.push_back(new FlowExecutor());
    m_executors.push_back(new LuaExecutor());
    m_executors.push_back(new CDExecutor());
    m_executors.push_back(new StateExecutor());
    m_executors.push_back(new ScreenExecutor());
    m_listener.listen<Events::SettingsLoaded>([this](const auto& event) {
        auto& debugSettings = g_emulator->settings.get<Emulator::SettingDebugSettings>();
        if (debugSettings.get<Emulator::DebugSettings::WebServer>() && !isRunning()) {
            start(g_system->getLoop(), debugSettings.get<Emulator::DebugSettings::WebServerPort>());
        }
    });
    m_listener.listen<Events::Quitting>([this](const auto& event) {
        if (isRunning()) stop();
    });
}

void PCSX::WebServer::onStopped() {
    // Covers an orderly stop and a failed bind alike. Closing a client
    // eventually deletes it, which unlinks it from this list.
    while (!m_clients.empty()) {
        auto client = m_clients.begin();
        client->close();
        if (!m_clients.empty() && (m_clients.begin() == client)) m_clients.erase(client);
    }
}

void PCSX::WebServer::onFailed() {
    g_system->printf(_("Web server: unable to listen on port %i: %s\n"), port(), lastError());
}

int PCSX::WebServer::configuredPort() const {
    return g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::WebServerPort>();
}

struct PCSX::WebClient::WebClientImpl {
    // The async lives here rather than in the client so the close callback can
    // find its way back after uv is done with the handle - UvFifo::setNotifier
    // owns the handle's data pointer.
    struct AsyncContext {
        uv_async_t m_async;
        WebClientImpl* m_impl;
    };

    WebClientImpl(WebServer* server, WebClient* parent, IO<File> connection, uv_loop_t* loop)
        : m_server(server), m_parent(parent), m_connection(connection) {
        m_asyncContext = new AsyncContext{{}, this};
        m_connection.asA<UvFifo>()->setNotifier(loop, &m_asyncContext->m_async, [this]() { onReadable(); });
        llhttp_settings_init(&m_httpParserSettings);
        m_httpParserSettings.on_message_begin = [](auto* parser) {
            return static_cast<WebClientImpl*>(parser->data)->onMessageBegin();
        };
        m_httpParserSettings.on_url = [](auto* parser, const char* data, size_t size) {
            Slice slice;
            slice.borrow(data, size);
            return static_cast<WebClientImpl*>(parser->data)->onUrl(slice);
        };
        m_httpParserSettings.on_status = [](auto* parser, const char* data, size_t size) {
            Slice slice;
            slice.borrow(data, size);
            return static_cast<WebClientImpl*>(parser->data)->onStatus(slice);
        };
        m_httpParserSettings.on_header_field = [](auto* parser, const char* data, size_t size) {
            Slice slice;
            slice.borrow(data, size);
            return static_cast<WebClientImpl*>(parser->data)->onHeaderField(slice);
        };
        m_httpParserSettings.on_header_value = [](auto* parser, const char* data, size_t size) {
            Slice slice;
            slice.borrow(data, size);
            return static_cast<WebClientImpl*>(parser->data)->onHeaderValue(slice);
        };
        m_httpParserSettings.on_headers_complete = [](auto* parser) {
            return static_cast<WebClientImpl*>(parser->data)->onHeadersComplete();
        };
        m_httpParserSettings.on_body = [](auto* parser, const char* data, size_t size) {
            Slice slice;
            slice.borrow(data, size);
            return static_cast<WebClientImpl*>(parser->data)->onBody(slice);
        };
        m_httpParserSettings.on_message_complete = [](auto* parser) {
            return static_cast<WebClientImpl*>(parser->data)->onMessageComplete();
        };
        m_httpParserSettings.on_chunk_header = [](auto* parser) {
            return static_cast<WebClientImpl*>(parser->data)->onChunkHeader();
        };
        m_httpParserSettings.on_chunk_complete = [](auto* parser) {
            return static_cast<WebClientImpl*>(parser->data)->onChunkComplete();
        };
        llhttp_init(&m_httpParser, HTTP_REQUEST, &m_httpParserSettings);
        m_httpParser.data = this;
    }
    void close() {
        if (m_status != OPEN) return;
        m_status = CLOSING;
        // Dropping the connection is enough: UvFifo::close() flushes whatever
        // is still queued before tearing the socket down, so a response cannot
        // be truncated. That guarantee is what retired the m_closeScheduled and
        // m_requests.size() bookkeeping this class used to carry.
        if (m_connection) {
            m_connection.asA<UvFifo>()->clearNotifier();
            m_connection.reset();
        }
        auto context = m_asyncContext;
        m_asyncContext = nullptr;
        if (!context) {
            delete m_parent;
            return;
        }
        // Deleting from here would free the object whose callback we are inside
        // of; let the async's own close callback do it.
        uv_close(reinterpret_cast<uv_handle_t*>(&context->m_async), [](uv_handle_t* handle) {
            auto context = reinterpret_cast<AsyncContext*>(handle);
            delete context->m_impl->m_parent;
            delete context;
        });
    }

    void onEOF() {
        auto error = llhttp_finish(&m_httpParser);
        if (error == HPE_PAUSED_UPGRADE) {
            onUpgrade();
        } else if (error != HPE_OK) {
            send400(magic_enum::enum_name(error));
        } else {
            scheduleClose();
        }
    }

    void onUpgrade() {}
    int onMessageBegin() { return 0; }
    int onUrl(const Slice& slice) {
        UriUriA uri;
        std::string urlString = slice.asString();
        const char* errorPos;
        if (uriParseSingleUriA(&uri, urlString.c_str(), &errorPos) != URI_SUCCESS) return 1;
        m_requestData.urlData.schema = uri.scheme.first ? std::string(uri.scheme.first, uri.scheme.afterLast) : "";
        m_requestData.urlData.host = uri.hostText.first ? std::string(uri.hostText.first, uri.hostText.afterLast) : "";
        m_requestData.urlData.port = uri.portText.first ? std::string(uri.portText.first, uri.portText.afterLast) : "";
        std::string path;
        auto pathFragment = uri.pathHead;
        while (pathFragment) {
            path += '/' + std::string(pathFragment->text.first, pathFragment->text.afterLast);
            pathFragment = pathFragment->next;
        }
        m_requestData.urlData.path = std::move(path);
        m_requestData.urlData.query = uri.query.first ? std::string(uri.query.first, uri.query.afterLast) : "";
        m_requestData.urlData.fragment =
            uri.fragment.first ? std::string(uri.fragment.first, uri.fragment.afterLast) : "";
        m_requestData.urlData.userInfo =
            uri.userInfo.first ? std::string(uri.userInfo.first, uri.userInfo.afterLast) : "";
        uriFreeUriMembersA(&uri);
        g_system->log(LogClass::WEBSERVER, "Received web api request, path: %s, query: %s\n",
                      m_requestData.urlData.path.c_str(), m_requestData.urlData.query.c_str());
        return findExecutor() ? 0 : 1;
    }
    int onStatus(const Slice& slice) { return 0; }
    void headerComplete() {
        m_requestData.headers.insert(std::pair(m_currentHeader, m_currentValue));
        m_currentHeader.clear();
        m_currentValue.clear();
    }
    int onHeaderField(const Slice& slice) {
        if (m_headerState == PARSING_VALUE) {
            headerComplete();
            m_headerState = PARSING_HEADER;
        }
        m_currentHeader += slice.asString();
        return 0;
    }
    int onHeaderValue(const Slice& slice) {
        m_headerState = PARSING_VALUE;
        m_currentValue += slice.asString();
        return 0;
    }
    void formHeaderComplete() {
        m_requestData.form.insert(std::pair(m_currentFormHeader, m_currentFormValue));
        m_currentFormHeader.clear();
        m_currentFormValue.clear();
    }
    int onFormHeaderField(const Slice& slice) {
        if (m_formHeaderState == PARSING_VALUE) {
            formHeaderComplete();
            m_formHeaderState = PARSING_HEADER;
        }
        m_currentFormHeader += slice.asString();
        return 0;
    }
    int onFormHeaderValue(const Slice& slice) {
        m_formHeaderState = PARSING_VALUE;
        m_currentFormValue += slice.asString();
        return 0;
    }
    int onHeadersComplete() {
        headerComplete();
        auto& ct = m_requestData.headers;
        auto it = ct.find("Content-Type");
        if (it != ct.end()) {
            std::string_view contentType = it->second;
            if (contentType.starts_with("multipart/form-data")) {
                auto pos = contentType.find("boundary=");
                if (pos != std::string::npos) {
                    std::string_view marker = contentType.substr(pos + 9);
                    if (marker.starts_with("\"") && marker.ends_with("\"") && marker.size() >= 2) {
                        marker = marker.substr(1, marker.size() - 2);
                    }
                    m_multipartBoundary = std::string("--");
                    m_multipartBoundary += marker;
                    m_multipart = true;
                    memset(&m_multipartParserCallbacks, 0, sizeof(multipart_parser_settings));
                    m_multipartParserCallbacks.on_header_field = [](multipart_parser* p, const char* at,
                                                                    size_t length) {
                        Slice slice;
                        slice.borrow(at, length);
                        return static_cast<WebClientImpl*>(multipart_parser_get_data(p))->onFormHeaderField(slice);
                    };
                    m_multipartParserCallbacks.on_header_value = [](multipart_parser* p, const char* at,
                                                                    size_t length) {
                        Slice slice;
                        slice.borrow(at, length);
                        return static_cast<WebClientImpl*>(multipart_parser_get_data(p))->onFormHeaderValue(slice);
                    };
                    m_multipartParserCallbacks.on_headers_complete = [](multipart_parser* p) {
                        return static_cast<WebClientImpl*>(multipart_parser_get_data(p))->onFormHeadersComplete();
                    };
                    m_multipartParserCallbacks.on_part_data = [](multipart_parser* p, const char* at, size_t length) {
                        Slice slice;
                        slice.borrow(at, length);
                        return static_cast<WebClientImpl*>(multipart_parser_get_data(p))->onFormData(slice);
                    };
                    m_multipartParser = multipart_parser_init(m_multipartBoundary.c_str(), &m_multipartParserCallbacks);
                    multipart_parser_set_data(m_multipartParser, this);
                }
            }
        }
        return 0;
    }

    int onFormHeadersComplete() {
        formHeaderComplete();
        return 0;
    }

    int onFormData(const Slice& slice) {
        m_requestData.body.concatenate(slice);
        return 0;
    }

    int onBody(const Slice& slice) {
        if (m_multipart) {
            multipart_parser_execute(m_multipartParser, slice.data<char>(), slice.size());
        } else {
            m_requestData.body.concatenate(slice);
        }
        return 0;
    }
    int onMessageComplete() {
        if (m_multipart) {
            multipart_parser_free(m_multipartParser);
        }
        executeRequest();
        return 0;
    }
    int onChunkHeader() { return 0; }
    int onChunkComplete() { return 0; }
    void onReadable() {
        uint8_t buffer[BUFFER_SIZE];
        while (m_connection && (m_status == OPEN) && (m_connection->size() > 0)) {
            auto got = m_connection->read(buffer, std::min(sizeof(buffer), m_connection->size()));
            if (got <= 0) break;
            Slice slice;
            slice.borrow(buffer, got);
            processData(slice);
        }
        // eof() is closed-and-drained, so a request that arrived alongside the
        // peer hanging up still gets parsed before the connection is finished.
        if (m_connection && (m_status == OPEN) && m_connection->eof()) onEOF();
    }
    void processData(const Slice& slice) {
        const char* ptr = reinterpret_cast<const char*>(slice.data());
        auto size = slice.size();

        auto error = llhttp_execute(&m_httpParser, ptr, size);
        if (m_status != OPEN) return;
        if (error == HPE_PAUSED_UPGRADE) {
            onUpgrade();
        } else if (error != HPE_OK) {
            send400(magic_enum::enum_name(error));
        }
    }

    template <size_t L>
    void write(const char (&str)[L]) {
        static_assert((L - 1) <= std::numeric_limits<uint32_t>::max());
        Slice slice;
        slice.borrow(str, L - 1);
        write(std::move(slice));
    }

    void write(Slice&& slice) {
        if (!m_connection || (m_status != OPEN)) return;
        m_connection->write(std::move(slice));
    }

    void write(std::string&& str) {
        Slice slice(std::move(str));
        write(std::move(slice));
    }

    void write(const std::string& str) {
        Slice slice(str);
        write(std::move(slice));
    }

    void send400(std::string_view code) {
        std::string str =
            fmt::format("HTTP/1.1 400 Bad Request\r\n\r\nRequest failed to parse properly. Error: {}\r\n", code);
        write(std::move(str));
        scheduleClose();
    }

    void send404() {
        write("HTTP/1.1 404 Not Found\r\n\r\nURL Not found.\r\n");
        scheduleClose();
    }

    bool findExecutor() {
        auto& list = m_server->m_executors;
        for (m_currentExecutor = list.begin(); m_currentExecutor != list.end(); m_currentExecutor++) {
            if (m_currentExecutor->match(m_parent, m_requestData.urlData)) return true;
        }
        send404();
        return false;
    }
    int executeRequest() {
        m_requestData.method = static_cast<RequestData::Method>(m_httpParser.method);
        m_currentExecutor->execute(m_parent, m_requestData);
        scheduleClose();
        return 0;
    }
    // The transport flushes on close now, so there is nothing left to wait for.
    void scheduleClose() { close(); }

    WebServer* m_server;
    IO<File> m_connection;
    AsyncContext* m_asyncContext = nullptr;
    static constexpr size_t BUFFER_SIZE = 256;
    enum { OPEN, CLOSING } m_status = OPEN;
    llhttp_settings_t m_httpParserSettings;
    llhttp_t m_httpParser;
    Intrusive::List<WebExecutor>::iterator m_currentExecutor;
    WebClient* m_parent;

    std::string m_currentHeader;
    std::string m_currentValue;
    std::string m_currentFormHeader;
    std::string m_currentFormValue;
    enum HeaderState {
        PARSING_HEADER,
        PARSING_VALUE,
    };
    HeaderState m_headerState = PARSING_HEADER;
    HeaderState m_formHeaderState = PARSING_HEADER;
    RequestData m_requestData;
    bool m_multipart = false;
    std::string m_multipartBoundary;
    multipart_parser* m_multipartParser;
    multipart_parser_settings m_multipartParserCallbacks;
};

PCSX::WebClient::WebClient(WebServer* server, IO<File> connection, uv_loop_t* loop)
    : m_impl(std::make_unique<WebClientImpl>(server, this, connection, loop)) {}
void PCSX::WebClient::close() { m_impl->close(); }
void PCSX::WebClient::write(Slice&& slice) { m_impl->write(std::move(slice)); }
void PCSX::WebClient::write(std::string&& str) { m_impl->write(std::move(str)); }
void PCSX::WebClient::write(const std::string& str) { m_impl->write(str); }

void PCSX::WebServer::onConnection(IO<File> connection) {
    m_clients.push_back(new WebClient(this, connection, g_system->getLoop()));
}

#endif
