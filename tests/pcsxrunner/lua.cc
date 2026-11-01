/***************************************************************************
 *   Copyright (C) 2024 PCSX-Redux authors                                 *
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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

#include "gtest/gtest.h"
#include "main/main.h"

static const char prefix[] = R"(
package.path = package.path
    .. ';../../../third_party/luaunit/?.lua'
    .. ';./third_party/luaunit/?.lua'
    .. ';../../../?.lua'
    .. ';./?.lua'
)";

static const char suffix[] = R"(
coroutine.resume(coroutine.create(function()
    PCSX.quit(require('luaunit').LuaUnit.new():runSuite '--verbose')
end))
)";

template <typename... Args>
int runLuaInt(Args... args) {
    MainInvoker invoker("-no-ui", "-cli", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter", "-luacov",
                        "-exec", prefix, args..., "-exec", suffix);
    return invoker.invoke();
}

template <typename... Args>
int runLuaDyn(Args... args) {
    MainInvoker invoker("-no-ui", "-cli", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec", "-luacov",
                        "-exec", prefix, args..., "-exec", suffix);
    return invoker.invoke();
}

static int runLuaIntTest(const char* name) {
    std::string req = "require '";
    req += name;
    req += "'";
    return runLuaInt("-exec", req.c_str());
}

static int runLuaDynTest(const char* name) {
    std::string req = "require '";
    req += name;
    req += "'";
    return runLuaDyn("-exec", req.c_str());
}

TEST(LuaBasic, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.basic"), 0); }
TEST(LuaBasic, Dynarec) { EXPECT_EQ(runLuaDynTest("tests.lua.basic"), 0); }
TEST(LuaFile, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.file"), 0); }
TEST(LuaFile, Dynarec) { EXPECT_EQ(runLuaDynTest("tests.lua.file"), 0); }
TEST(LuaAdpcm, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.adpcm"), 0); }
TEST(LuaAdpcm, Dynarec) { EXPECT_EQ(runLuaDynTest("tests.lua.adpcm"), 0); }
TEST(LuaAudioPlayback, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.audioplayback"), 0); }
TEST(LuaCallGuest, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.callguest"), 0); }
TEST(LuaCallGuest, Dynarec) { EXPECT_EQ(runLuaDynTest("tests.lua.callguest"), 0); }
TEST(LuaCompression, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.compression"), 0); }
TEST(LuaCompression, Dynarec) { EXPECT_EQ(runLuaDynTest("tests.lua.compression"), 0); }
TEST(LuaRewind, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.rewind"), 0); }
TEST(LuaRewind, Dynarec) { EXPECT_EQ(runLuaDynTest("tests.lua.rewind"), 0); }
TEST(LuaGdbReverse, Interpreter) {
    EXPECT_EQ(runLuaInt("-gdb", "-gdb-port", "3462", "-exec", "require 'tests.lua.gdb-reverse'"), 0);
}
static int runGdbReverseShell(bool dynarec, const char* mode, const char* exe = nullptr) {
    std::string select = "GDB_REVERSE_SHELL = '";
    select += mode;
    select += "'";
    auto run = [dynarec](auto... args) { return dynarec ? runLuaDyn(args...) : runLuaInt(args...); };
    if (!exe) {
        return run("-gdb", "-gdb-port", "3463", "-exec", select.c_str(), "-exec",
                   "require 'tests.lua.gdb-reverse-shell'");
    }
    // -run, or the binary is loaded with the emulator paused and stays paused after the load.
    return run("-gdb", "-gdb-port", "3463", "-run", "-loadexe", exe, "-exec", select.c_str(), "-exec",
               "require 'tests.lua.gdb-reverse-shell'");
}
// Named per run, so concurrent test processes don't share it. Entry and load address 0x80010000:
// li $t2, 0x1234 once, then addiu $t0, $t0, 1 in a loop.
static std::filesystem::path writeGdbReverseShellExe() {
    std::random_device random;
    auto exe = std::filesystem::temp_directory_path() /
               ("pcsx-gdb-reverse-shell-" + std::to_string(random()) + "-" + std::to_string(random()) + ".ps-exe");
    std::string image(4096, '\0');
    auto put32 = [&image](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; i++) image[offset + i] = char((value >> (i * 8)) & 0xff);
    };
    image.replace(0, 8, "PS-X EXE");
    put32(0x10, 0x80010000);
    put32(0x18, 0x80010000);
    put32(0x1c, 2048);
    put32(0x30, 0x801ffff0);
    const uint32_t code[] = {0x240a1234, 0x25080001, 0x1000fffe, 0x00000000};
    for (unsigned i = 0; i < 4; i++) put32(2048 + i * 4, code[i]);
    std::ofstream out(exe, std::ios::binary);
    out.write(image.data(), image.size());
    if (!out.good()) return {};
    return exe;
}
// -loadexe wants the file to exist already.
static int runGdbReverseShellExe(bool dynarec) {
    auto exe = writeGdbReverseShellExe();
    if (exe.empty()) return -1;
    int ret = runGdbReverseShell(dynarec, "exe", exe.string().c_str());
    std::filesystem::remove(exe);
    return ret;
}
TEST(LuaGdbReverseShell, FastBoot) { EXPECT_EQ(runGdbReverseShell(false, "fastboot"), 0); }
TEST(LuaGdbReverseShell, FastBootDynarec) { EXPECT_EQ(runGdbReverseShell(true, "fastboot"), 0); }
TEST(LuaGdbReverseShell, LoadExe) { EXPECT_EQ(runGdbReverseShellExe(false), 0); }
TEST(LuaGdbReverseShell, LoadExeDynarec) { EXPECT_EQ(runGdbReverseShellExe(true), 0); }
TEST(LuaGdbReverseShell, ShellHalt) { EXPECT_EQ(runGdbReverseShell(false, "shellhalt"), 0); }
TEST(LuaPCdrv, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.pcdrv"), 0); }
TEST(LuaPCWrite, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.pcwrite"), 0); }
TEST(LuaSaveStateBranchSlot, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.sstate-branchslot"), 0); }
TEST(LuaSaveStateSPUCnt, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.sstate-spucnt"), 0); }
TEST(LuaSPULogger, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.spulogger"), 0); }
TEST(LuaGdbServer, Interpreter) {
    EXPECT_EQ(runLuaInt("-gdb", "-gdb-port", "34567", "-exec", "require 'tests.lua.gdbserver'"), 0);
}
TEST(LuaIsoBuilder, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.isobuilder"), 0); }
TEST(LuaIsoBuilder, Dynarec) { EXPECT_EQ(runLuaDynTest("tests.lua.isobuilder"), 0); }
TEST(LuaMds, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.mds"), 0); }
TEST(LuaBinary, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.binary"), 0); }
TEST(LuaCDRomApi, Interpreter) { EXPECT_EQ(runLuaIntTest("tests.lua.cdromapi"), 0); }

TEST(LuaStartup, DofileError) {
    MainInvoker failing("-no-ui", "-cli", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-dofile", "tests/lua/startup-error.lua");
    EXPECT_THROW(failing.invoke(), std::runtime_error);
    // The failed startup still has to shut down cleanly, or nothing else can start afterwards.
    MainInvoker next("-no-ui", "-cli", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter", "-exec",
                     "PCSX.quit(0)");
    EXPECT_EQ(next.invoke(), 0);
}
