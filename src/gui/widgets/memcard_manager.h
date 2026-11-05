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

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "GL/gl3w.h"
#include "clip/clip.h"
#include "core/sio.h"
#include "gui/widgets/filedialog.h"
#include "imgui.h"

namespace PCSX {

class GUI;
namespace Widgets {

class MemcardManager {
  public:
    MemcardManager(bool& show, std::vector<std::string>& favorites)
        : m_show(show),
          m_importMemoryCardDialog(l_("Import Memory Card file"), favorites),
          m_exportMemoryCardDialog(l_("Export Memory Card file"), favorites),
          m_pocketstationBiosDialog(l_("Select Pocketstation BIOS"), favorites) {}
    bool draw(GUI* gui, const char* title);
    bool& m_show;
    // The framecount from 0 to 59 inclusive. We need it to know which frame of multi-animation
    // icons to display.
    int m_frameCount = 0;

    void initTextures();

  private:
    int m_iconSize = 32;  // The width and length of the icon images
    bool m_drawPocketstationIcons = false;
    std::vector<std::pair<std::string, std::unique_ptr<uint8_t[]>>> m_undo;

    GLuint m_iconTextures[15] = {0};

    clip::image getIconRGBA8888(const SIO::McdBlock& block);

    void drawIcon(const SIO::McdBlock& block);
    void exportPNG(const SIO::McdBlock& block);
    void copyToClipboard(const SIO::McdBlock& block);
    void getPocketstationIcon(uint32_t* pixels, const SIO::McdBlock& block);

    void saveUndoBuffer(std::unique_ptr<uint8_t[]>&& tosave, const std::string& action);

    std::unique_ptr<uint8_t[]> getLatest() {
        std::unique_ptr<uint8_t[]> data = std::make_unique<uint8_t[]>(SIO::c_cardSize * 2);
        std::memcpy(data.get(), g_emulator->m_sio->getMcdData(1), SIO::c_cardSize);
        std::memcpy(data.get() + SIO::c_cardSize, g_emulator->m_sio->getMcdData(2), SIO::c_cardSize);

        return data;
    }

    int m_undoIndex = 0;
    std::unique_ptr<uint8_t[]> m_latest;
    Widgets::FileDialog<> m_importMemoryCardDialog;
    Widgets::FileDialog<FileDialogMode::Save> m_exportMemoryCardDialog;
    Widgets::FileDialog<> m_pocketstationBiosDialog;
    unsigned m_memoryCardImportExportIndex = 0;

    void clearUndoBuffer() {
        m_undo.clear();
        m_undoIndex = 0;
    }
};

}  // namespace Widgets
}  // namespace PCSX
