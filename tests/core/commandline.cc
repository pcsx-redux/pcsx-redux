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

#include <initializer_list>
#include <stdexcept>
#include <vector>

#include "gtest/gtest.h"

static PCSX::CommandLine parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"pcsx-redux"};
    argv.insert(argv.end(), args);
    return PCSX::CommandLine(static_cast<int>(argv.size()), argv.data());
}

TEST(CommandLine, TestRunner) {
    auto args = parse({"-no-ui", "-run", "-pcdrv", "-pcdrvbase", ".", "-bios", "src/mips/openbios/openbios.bin",
                       "-testmode", "-interpreter", "-luacov", "-loadexe", "src/mips/tests/tooling/pcdrv/pcdrv.ps-exe"});
    ASSERT_EQ(args.status(), PCSX::CommandLine::Status::Ok) << args.message();
    EXPECT_TRUE(args.has("no-ui"));
    EXPECT_TRUE(args.has("run"));
    EXPECT_TRUE(args.has("testmode"));
    EXPECT_FALSE(args.has("dynarec"));
    EXPECT_EQ(args.value("pcdrvbase"), ".");
    EXPECT_EQ(args.value("bios"), "src/mips/openbios/openbios.bin");
    EXPECT_EQ(args.value("loadexe"), "src/mips/tests/tooling/pcdrv/pcdrv.ps-exe");
    EXPECT_EQ(args.value("exe"), std::nullopt);
    EXPECT_EQ(args.value("exe", "fallback"), "fallback");
}

TEST(CommandLine, LuaScripts) {
    auto args = parse({"-cli", "-dofile", "mkarchive.lua", "-exec", "mkarchive('index.json', 'output.arc') PCSX.Quit()",
                       "-exec", "-- a comment", "-dofile", "second.lua"});
    ASSERT_EQ(args.status(), PCSX::CommandLine::Status::Ok) << args.message();
    EXPECT_EQ(args.values("dofile"), (std::vector<std::string>{"mkarchive.lua", "second.lua"}));
    EXPECT_EQ(args.values("exec"),
              (std::vector<std::string>{"mkarchive('index.json', 'output.arc') PCSX.Quit()", "-- a comment"}));
    EXPECT_TRUE(args.values("archive").empty());
}

TEST(CommandLine, DoubleDashAndEqual) {
    auto args = parse({"--run", "--loadexe=a.ps-exe", "-gdb-port=3333", "-exec", "--run"});
    ASSERT_EQ(args.status(), PCSX::CommandLine::Status::Ok) << args.message();
    EXPECT_TRUE(args.has("run"));
    EXPECT_EQ(args.value("loadexe"), "a.ps-exe");
    EXPECT_EQ(args.number("gdb-port"), 3333);
    EXPECT_EQ(args.values("exec"), (std::vector<std::string>{"--run"}));
}

TEST(CommandLine, DisplayAndPCdrvNet) {
    auto args = parse({"-2mbvram", "-softgputhreaded", "-shmdisplay", "-tui", "-pcdrvnet", "-pcdrvnetallow",
                       "127.0.0.1:8080,localhost:1234"});
    ASSERT_EQ(args.status(), PCSX::CommandLine::Status::Ok) << args.message();
    EXPECT_TRUE(args.has("2mbvram"));
    EXPECT_FALSE(args.has("1mbvram"));
    EXPECT_TRUE(args.has("softgputhreaded"));
    EXPECT_FALSE(args.has("no-softgputhreaded"));
    EXPECT_TRUE(args.has("shmdisplay"));
    EXPECT_TRUE(args.has("tui"));
    EXPECT_TRUE(args.has("pcdrvnet"));
    EXPECT_EQ(args.value("pcdrvnetallow"), "127.0.0.1:8080,localhost:1234");

    auto negated = parse({"-1mbvram", "-no-softgputhreaded"});
    ASSERT_EQ(negated.status(), PCSX::CommandLine::Status::Ok) << negated.message();
    EXPECT_TRUE(negated.has("1mbvram"));
    EXPECT_TRUE(negated.has("no-softgputhreaded"));
    EXPECT_FALSE(negated.has("pcdrvnet"));
    EXPECT_EQ(negated.value("pcdrvnetallow"), std::nullopt);
}

TEST(CommandLine, LastValueWins) {
    auto args = parse({"-loadexe", "a.ps-exe", "-loadexe", "b.ps-exe"});
    ASSERT_EQ(args.status(), PCSX::CommandLine::Status::Ok) << args.message();
    EXPECT_EQ(args.value("loadexe"), "b.ps-exe");
}

TEST(CommandLine, OptionalValue) {
    auto bare = parse({"-portable", "-run"});
    ASSERT_EQ(bare.status(), PCSX::CommandLine::Status::Ok) << bare.message();
    EXPECT_TRUE(bare.has("portable"));
    EXPECT_EQ(bare.value("portable"), "");
    EXPECT_TRUE(bare.has("run"));

    auto withPath = parse({"-portable", "/tmp/redux"});
    ASSERT_EQ(withPath.status(), PCSX::CommandLine::Status::Ok) << withPath.message();
    EXPECT_EQ(withPath.value("portable"), "/tmp/redux");

    auto absent = parse({});
    EXPECT_FALSE(absent.has("portable"));
    EXPECT_EQ(absent.value("portable"), std::nullopt);
}

TEST(CommandLine, UnknownFlag) {
    auto args = parse({"-lodaexe", "a.ps-exe"});
    EXPECT_EQ(args.status(), PCSX::CommandLine::Status::Error);
    EXPECT_NE(args.message().find("lodaexe"), std::string::npos) << args.message();
}

TEST(CommandLine, StrayArgument) {
    // A switch doesn't take a value, so the file here isn't attached to anything.
    auto args = parse({"-run", "a.ps-exe"});
    EXPECT_EQ(args.status(), PCSX::CommandLine::Status::Error);
    EXPECT_NE(args.message().find("a.ps-exe"), std::string::npos) << args.message();
}

TEST(CommandLine, MissingValue) {
    auto args = parse({"-loadexe"});
    EXPECT_EQ(args.status(), PCSX::CommandLine::Status::Error);
}

TEST(CommandLine, BadNumber) {
    auto args = parse({"-gdb-port", "abc"});
    EXPECT_EQ(args.status(), PCSX::CommandLine::Status::Error);
}

TEST(CommandLine, Help) {
    for (auto flag : {"-help", "-h", "--help"}) {
        auto args = parse({flag});
        EXPECT_EQ(args.status(), PCSX::CommandLine::Status::Help) << flag;
        EXPECT_NE(args.message().find("-loadexe"), std::string::npos) << args.message();
    }
}

TEST(CommandLine, UndeclaredQuery) {
    auto args = parse({});
    EXPECT_THROW(args.has("lodaexe"), std::logic_error);
    EXPECT_THROW(args.number("loadexe"), std::logic_error);
}
