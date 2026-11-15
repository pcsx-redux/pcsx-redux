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

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "core/callprofiler.h"

namespace PCSX {

class GUI;

namespace Widgets {

class CallProfiler {
  public:
    CallProfiler(bool& show) : m_show(show) {}
    void draw(const char* title, GUI*);

    bool& m_show;

    // When the function table is rebuilt: as the window appears, as the profiler stops,
    // and while it runs, at most once a second and never more often than keeps the
    // rebuild under 5% of the time, so a table that is slow to build does not stall
    // the UI. Times are in seconds.
    struct RefreshPolicy {
        bool due(double now, bool appeared, bool running) {
            bool stopped = m_wasRunning && !running;
            m_wasRunning = running;
            if (appeared || stopped) return true;
            return running && now - m_last >= std::max(1.0, 20.0 * m_cost);
        }
        void done(double now, double cost) {
            m_last = now;
            m_cost = cost;
        }

      private:
        double m_last = -std::numeric_limits<double>::infinity();
        double m_cost = 0.0;
        bool m_wasRunning = false;
    };

  private:
    std::vector<::PCSX::Profiler::Function> m_functions;
    // The totals the table was built against, so its percentages stay consistent.
    ::PCSX::Profiler::Counters m_snapshotTotals = {};
    RefreshPolicy m_refresh;
    int m_lastFrame = -2;
    std::string m_path = "profile.pb";
    std::string m_status;
};

}  // namespace Widgets

}  // namespace PCSX
