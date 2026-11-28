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

#pragma once

#include <cstdio>
#include <string>

#include "gtest/gtest.h"
#include "main/main.h"

// bank-probe passes on any board where some gate polarity opens the upper
// bank, including one where the bank stays reachable with the gate closed
// (verdict=both-polarities-open-upper, or arcade-gate0-opens-upper on a 1MB
// board). Each fitment has exactly one right answer. With 2MB fitted the
// closed gate mirrors the lower bank and only the open gate reaches the
// second chip, as measured on the 573. With 1MB fitted the closed gate
// mirrors and the open gate reads open bus, as measured on retail boards.
// This runs the probe with -stdout, captures what it prints and checks its
// summary line for the expected decode. The invoker has to carry -stdout.
static constexpr char kBankProbe1MB[] = " gate0=mirror gate1=open-bus";
static constexpr char kBankProbe2MB[] = " gate0=mirror gate1=real";

static void runBankProbeStrict(MainInvoker& invoker, const char* expected) {
    testing::internal::CaptureStdout();
    int ret = invoker.invoke();
    std::string out = testing::internal::GetCapturedStdout();
    // Hand the run's log back to the test output, as the other probes leave it.
    fputs(out.c_str(), stdout);
    EXPECT_EQ(ret, 0);

    static const char kSummary[] = "RESULT bank-probe summary ";
    auto start = out.find(kSummary);
    ASSERT_NE(start, std::string::npos) << "bank-probe printed no summary line";
    auto end = out.find('\n', start);
    std::string summary = out.substr(start, end == std::string::npos ? std::string::npos : end - start);
    EXPECT_TRUE(summary.ends_with(expected)) << summary;
}
