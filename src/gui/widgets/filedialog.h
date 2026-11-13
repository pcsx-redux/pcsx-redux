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

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ImFileDialog/ImFileDialog.h"
#include "core/system.h"

namespace PCSX {
namespace Widgets {

class FileDialogBase : protected ifd::FileDialog {
  public:
    virtual void* CreateTexture(uint8_t* data, int w, int h, char fmt) override;
    FileDialogBase(std::vector<std::string>& favorites) : ifd::FileDialog(), m_favorites(favorites) {}

  protected:
    void setDeleteTexture();
    void restoreFavorites();
    void saveFavorites();

  private:
    std::vector<std::string>& m_favorites;
};

enum class FileDialogMode { Open, MultiSelect, Save, SelectDirectory };

template <FileDialogMode mode = FileDialogMode::Open>
class FileDialog : public FileDialogBase {
  public:
    FileDialog(std::function<const char*()> title, std::vector<std::string>& favorites)
        : FileDialogBase(favorites), m_title(title) {
        setToCurrentPath();
        setDeleteTexture();
    }
    virtual ~FileDialog() = default;
    void setToCurrentPath() { m_currentPath = std::filesystem::current_path(); }
    void openDialog() {
        restoreFavorites();
        if constexpr (mode == FileDialogMode::Open) {
            Open(m_title(), m_title(), "*.*", mode == FileDialogMode::MultiSelect,
                 reinterpret_cast<const char*>(m_currentPath.u8string().c_str()));
        } else if constexpr (mode == FileDialogMode::Save) {
            Save(m_title(), m_title(), "*.*", reinterpret_cast<const char*>(m_currentPath.u8string().c_str()));
        } else if constexpr (mode == FileDialogMode::SelectDirectory) {
            // An empty filter puts ImFileDialog in directory mode.
            Open(m_title(), m_title(), "", false, reinterpret_cast<const char*>(m_currentPath.u8string().c_str()));
        }
    }
    const std::vector<PCSX::u8string>& selected() const { return m_results; }
    bool draw() {
        bool done = IsDone(m_title());
        m_currentPath = CurrentDirectory();
        if (done) {
            auto results = GetResults();
            m_results.clear();
            m_results.reserve(results.size());
            for (auto& result : results) m_results.push_back(result.u8string());
            Close();
            saveFavorites();
        }
        return done;
    }
    std::filesystem::path m_currentPath;

  private:
    const std::function<const char*()> m_title;
    std::vector<PCSX::u8string> m_results;
};

}  // namespace Widgets
}  // namespace PCSX
