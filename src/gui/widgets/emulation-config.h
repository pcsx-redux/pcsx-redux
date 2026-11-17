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

#include "gui/widgets/filedialog.h"

namespace PCSX {

class GUI;

namespace Widgets {

class EmulationConfig {
  public:
    EmulationConfig(bool& show, int& idleSwapInterval, std::vector<std::string>& favorites);
    bool draw(GUI* gui);

    bool& m_show;

  private:
    int& m_idleSwapInterval;
    Widgets::FileDialog<> m_selectBiosDialog;
    Widgets::FileDialog<Widgets::FileDialogMode::SelectDirectory> m_selectScreenshotDirDialog;
    Widgets::FileDialog<Widgets::FileDialogMode::SelectDirectory> m_selectSaveStateDirDialog;
};

}  // namespace Widgets

}  // namespace PCSX
