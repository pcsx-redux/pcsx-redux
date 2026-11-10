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

#include <string>
#include <vector>

#include "core/profiler.h"

namespace PCSX {

class GUI;

namespace Widgets {

class CallProfiler {
  public:
    CallProfiler(bool& show) : m_show(show) {}
    void draw(const char* title, GUI*);

    bool& m_show;

  private:
    std::vector<::PCSX::Profiler::Function> m_functions;
    std::string m_path = "profile.pb";
    std::string m_status;
    bool m_refresh = true;
};

}  // namespace Widgets

}  // namespace PCSX
