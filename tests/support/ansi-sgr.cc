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

#include "support/ansi-sgr.h"

#include "gtest/gtest.h"

using PCSX::AnsiSGR;

TEST(AnsiSGR, PlainText) {
    AnsiSGR sgr;
    std::vector<AnsiSGR::Span> spans;
    EXPECT_EQ(sgr.parse("hello world", spans), "hello world");
    EXPECT_TRUE(spans.empty());
}

TEST(AnsiSGR, CesterLine) {
    AnsiSGR sgr;
    std::vector<AnsiSGR::Span> spans;
    auto out = sgr.parse("\x1b[32m+ (PASSED)\x1b[0m test_foo", spans);
    EXPECT_EQ(out, "+ (PASSED) test_foo");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].offset, 0u);
    EXPECT_EQ(spans[0].color, 0x00cd00);
    EXPECT_EQ(spans[1].offset, 10u);
    EXPECT_EQ(spans[1].color, AnsiSGR::c_default);
}

TEST(AnsiSGR, BoldBrightensAndCarriesAcrossLines) {
    AnsiSGR sgr;
    std::vector<AnsiSGR::Span> spans;
    EXPECT_EQ(sgr.parse("a\x1b[1;31mb", spans), "ab");
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].offset, 1u);
    EXPECT_EQ(spans[0].color, 0xff0000);
    EXPECT_EQ(sgr.parse("next", spans), "next");
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].offset, 0u);
    EXPECT_EQ(spans[0].color, 0xff0000);
    EXPECT_EQ(sgr.parse("\x1b[mdone", spans), "done");
    EXPECT_TRUE(spans.empty());
}

TEST(AnsiSGR, ExtendedColours) {
    AnsiSGR sgr;
    std::vector<AnsiSGR::Span> spans;
    EXPECT_EQ(sgr.parse("\x1b[38;5;196mx\x1b[38;2;1;2;3my\x1b[48;5;21mz", spans), "xyz");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].color, 0xff0000);
    EXPECT_EQ(spans[1].offset, 1u);
    EXPECT_EQ(spans[1].color, 0x010203);
}

TEST(AnsiSGR, OtherSequencesAreStripped) {
    AnsiSGR sgr;
    std::vector<AnsiSGR::Span> spans;
    EXPECT_EQ(sgr.parse("\x1b[2Ka\x1b[?25lb\x1b"
                        "cc\x1b[3",
                        spans),
              "abc");
    EXPECT_TRUE(spans.empty());
}

TEST(AnsiSGR, StringSequencesAndCharsetEscapes) {
    AnsiSGR sgr;
    std::vector<AnsiSGR::Span> spans;
    EXPECT_EQ(sgr.parse("a\x1b]0;title\x07"
                        "b\x1b]8;;http://x\x1b\\c\x1b(B\x1b[md",
                        spans),
              "abcd");
    EXPECT_TRUE(spans.empty());
    EXPECT_EQ(sgr.parse("e\x1bPunterminated", spans), "e");
}

TEST(AnsiSGR, ColonFormColours) {
    AnsiSGR sgr;
    std::vector<AnsiSGR::Span> spans;
    EXPECT_EQ(sgr.parse("\x1b[38:2::255:0:0mr\x1b[38:2:0:255:0mg\x1b[38:5:21mb\x1b[4:3mu", spans), "rgbu");
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].color, 0xff0000);
    EXPECT_EQ(spans[1].color, 0x00ff00);
    EXPECT_EQ(spans[2].offset, 2u);
    EXPECT_EQ(spans[2].color, 0x0000ff);
}
