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

#include "GL/gl3w.h"
#include "gui/widgets/filedialog.h"
#include "gui/widgets/gpulogger.h"

namespace PCSX {
namespace Widgets {

class GPUDump {
  public:
    GPUDump(bool& show, std::vector<std::string>& favorites);
    ~GPUDump();
    void draw(const char* title);

    bool& m_show;

  private:
    void drawRecorder();
    void drawPlayer();
    void updateTexture();

    FileDialog<FileDialogMode::Save> m_saveDialog;
    FileDialog<FileDialogMode::Open> m_openDialog;
    GLuint m_texture = 0;
    std::vector<uint32_t> m_pixels;
    bool m_playing = false;
    bool m_loop = false;
    bool m_displayOnly = true;
    float m_zoom = 1.0f;
    std::string m_error;
    bool m_showLogger = false;
    GPULogger m_logger{m_showLogger};
};

}  // namespace Widgets
}  // namespace PCSX
