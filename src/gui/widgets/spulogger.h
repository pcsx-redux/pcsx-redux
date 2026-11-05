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

#include <stdint.h>

#include <string>
#include <vector>

#include "core/spulogger.h"
#include "gui/widgets/filedialog.h"

namespace PCSX {

namespace Widgets {

class SPULogger {
  public:
    SPULogger(bool &show, bool &enabled, std::vector<std::string> &favorites)
        : m_show(show), m_enabled(enabled), m_exportDialog([]() { return _("Export SPU dump"); }, favorites) {}
    void draw(PCSX::SPULogger *logger, const char *title);
    // Pushes a checkbox change to the logger, and otherwise follows the logger, so a script
    // turning it on or off is reflected here rather than overridden on the next frame.
    void syncEnabled(PCSX::SPULogger *logger);

    bool &m_show;
    // Capture state is deliberately independent of window visibility, and persisted: closing
    // the window while a game runs must not silently stop recording.
    bool &m_enabled;

  private:
    void drawExportDialog(PCSX::SPULogger *logger);
    bool m_filterEnabled = false;
    // One bit per PCSX::SPULogger::Kind, all set: no filtering until asked.
    uint32_t m_kindMask = 0xffffffff;
    bool m_followTail = true;
    // Indices into the ring that survived the filter, rebuilt each frame and fed to the clipper.
    std::vector<uint32_t> m_visible;
    bool m_pushedEnabled = false;
    FileDialog<FileDialogMode::Save> m_exportDialog;
    std::string m_exportStatus;
};

}  // namespace Widgets

}  // namespace PCSX
