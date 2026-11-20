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

#include "lua/extra.h"

#include <filesystem>
#include <string_view>
#include <tuple>
#include <vector>

#include "lua-protobuf/pb.h"
#include "lua/luafile.h"
#include "lua/luawrapper.h"
#include "support/djbhash.h"
#include "support/file.h"
#include "support/strings-helpers.h"
#include "support/zip.h"

namespace {

std::vector<PCSX::ZipArchive> s_archives;
std::tuple<PCSX::File*, std::string> load(std::string_view name, std::string_view from, bool inArchives = true) {
    bool doRelative = false;
    if (!from.empty() && (from[0] == '@')) {
        from = from.substr(1);
        doRelative = true;
    }
    std::filesystem::path fromPath(from);
    std::filesystem::path absolutePath(name);
    // Archive member names are not filesystem paths, so they are normalised lexically: what exists on
    // disk must not change them. The on-disk fallback resolves through the filesystem, symlinks included.
    std::filesystem::path relativePath(fromPath.parent_path() / name);
    std::filesystem::path relativeMember = relativePath.lexically_normal();

    PCSX::File* file = nullptr;

    if (inArchives) {
        for (auto archivei = s_archives.rbegin(); archivei != s_archives.rend(); archivei++) {
            auto& archive = *archivei;
            if (doRelative) {
                file = archive.openFile(relativeMember.string());
                if (!file->failed()) return {file, relativeMember.generic_string()};
                delete file;
            }
            file = archive.openFile(absolutePath.string());
            if (!file->failed()) return {file, absolutePath.generic_string()};
            delete file;
        }
    } else {
        for (auto archivei = s_archives.rbegin(); archivei != s_archives.rend(); archivei++) {
            auto& archive = *archivei;
            std::filesystem::path fromPath(from);
            std::filesystem::path relativeMember((archive.archiveFilename().parent_path() / name).lexically_normal());
            file = archive.openFile(relativeMember.string());
            if (!file->failed()) return {file, relativeMember.generic_string()};
            delete file;
            file = archive.openFile(absolutePath.string());
            if (!file->failed()) return {file, absolutePath.generic_string()};
            delete file;
        }
    }

    if (doRelative) {
        relativePath = std::filesystem::weakly_canonical(relativePath);
        file = new PCSX::PosixFile(relativePath);
        if (!file->failed()) return {file, relativePath.generic_string()};
        delete file;
    }
    return {new PCSX::PosixFile(absolutePath), absolutePath.generic_string()};
}

uint64_t djbHash(const char* str, size_t len) { return PCSX::djb::hash(str, len); }

// The file a relative name is resolved against: the caller's own chunk, read off the stack, unless the
// Lua wrapper passes the running file explicitly because the caller forwarded with a tail call.
std::string relativeBase(PCSX::Lua L, const std::string& caller) {
    if (L.gettop() >= 2 && L.isstring(2)) return "@" + L.tostring(2);
    return caller;
}

template <typename T, size_t S>
void registerSymbol(PCSX::Lua L, const char (&name)[S], const T ptr) {
    L.push<S>(name);
    L.push((void*)ptr);
    L.settable();
}

#define REGISTER(L, s) registerSymbol(L, #s, s)

void registerAllSymbols(PCSX::Lua L) {
    L.getfieldtable("_CLIBS", LUA_REGISTRYINDEX);
    L.push("SUPPORT_EXTRA");
    L.newtable();

    REGISTER(L, djbHash);

    L.settable();
    L.pop();
}

}  // namespace

void closeArchivesForTests() { s_archives.clear(); }

PCSX::ZipArchive& PCSX::LuaFFI::addArchive(Lua L, IO<File> file) {
    auto& newArchive = s_archives.emplace_back(file);
    if (newArchive.failed()) {
        s_archives.pop_back();
        throw std::runtime_error("Invalid zip file");
    }
    IO<File> autoexec = newArchive.openFile("autoexec.lua");
    if (!autoexec->failed()) {
        std::string code = autoexec->readString(autoexec->size());
        L.load(code, fmt::format("{}:@autoexec.lua", file->filename().string()).c_str());
    }
    return newArchive;
}

void PCSX::LuaFFI::open_extra(Lua L) {
    L.getfieldtable("_LOADED", LUA_REGISTRYINDEX);
    luaopen_pb(L.getState());
    L.setfield("pb", -3);
    L.pop();
    luaopen_pb_io(L.getState());
    L.setfield("pb.io");
    luaopen_pb_conv(L.getState());
    L.setfield("pb.conv");
    luaopen_pb_slice(L.getState());
    L.setfield("pb.slice");
    luaopen_pb_buffer(L.getState());
    L.setfield("pb.buffer");
    luaopen_pb_unsafe(L.getState());
    L.setfield("pb.unsafe");

    static int lualoader = 8;
    static const char* pprint = (
#include "pprint.lua/pprint.lua"
    );
    static const char* pprint_internals = (
#include "pprint.lua/pprint-internals.lua"
    );
    static const char* reflectFFI = (
#include "ffi-reflect/reflect.lua"
    );
    static const char* protobufLexer = (
#include "lua-protobuf/lexer.lua"
    );
    static const char* protobufTopLevel = (
#include "lua-protobuf/toplevel.lua"
    );
    static const char* descriptorPB = (
#include "lua-protobuf/descriptor.pb.lua"
    );
    static const char* protoc = (
#include "lua-protobuf/protoc.lua"
    );
    static const char* extra = (
#include "lua/extra.lua"
    );
    registerAllSymbols(L);
    L.load(pprint, "third_party:pprint.lua/pprint.lua");
    L.load(pprint_internals, "third_party:pprint.lua/pprint-internals.lua");
    L.load(reflectFFI, "third_party:ffi-reflect/reflect.lua");

    L.load(protobufLexer, "third_party:lua-protobuf/lexer.lua");
    L.setfield("pb.Lexer");

    L.load(protobufTopLevel, "third_party:lua-protobuf/toplevel.lua");
    L.setfield("pb.TopLevel");

    L.load(descriptorPB, "third_party:lua-protobuf/descriptor.pb.lua");
    L.setfield("pb.Descriptor");

    L.load(protoc, "third_party:lua-protobuf/protoc.lua");
    L.setfield("protoc");

    L.load(extra, "src:lua/extra.lua");

    L.getfieldtable("Support", LUA_GLOBALSINDEX);
    L.getfieldtable("extra");
    L.declareFunc(
        "addArchive",
        [](lua_State* L_) -> int {
            Lua L(L_);
            auto ar = L.getinfo("S");
            auto name = L.tostring();
            IO<File> file;
            std::string actualName;
            std::tie(file, actualName) = load(name, ar.has_value() ? ar->source : "");
            if (file->failed()) return L.error("Unable to locate archive file");
            addArchive(L, file);
            return 0;
        },
        -1);
    L.pop();
    L.getfieldtable("_internal");
    L.declareFunc(
        "open",
        [](lua_State* L_) -> int {
            Lua L(L_);
            auto ar = L.getinfo("S", 2);
            auto name = L.tostring(1);
            IO<File> file;
            std::string actualName;
            std::tie(file, actualName) = load(name, relativeBase(L, ar.has_value() ? ar->source : ""));
            L.push(new LuaFile(file));
            return 1;
        },
        -1);
    L.declareFunc(
        "loadfile",
        [](lua_State* L_) -> int {
            Lua L(L_);
            auto ar = L.getinfo("S", 2);
            auto name = L.tostring(1);
            IO<File> file;
            std::string actualName;
            std::tie(file, actualName) = load(name, relativeBase(L, ar.has_value() ? ar->source : ""));
            if (file->failed()) return L.error("Unable to locate file %s", name.c_str());
            L.push(file->readString(file->size()));
            L.push(actualName);
            return 2;
        },
        -1);
    L.pop();
    L.pop();

    L.pop();
}
