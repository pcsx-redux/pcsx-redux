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

#include "gui/widgets/uvfiles.h"

#include "core/system.h"
#include "fmt/format.h"
#include "imgui.h"
#include "support/file.h"
#include "support/uvfile.h"

void PCSX::Widgets::UvFiles::draw() {
    if (ImGui::Begin(_("UvFiles"), &m_show)) {
        std::string rate;
        byteRateToString(UvFile::getReadRate(), rate);
        ImGui::Text(_("Read rate: %s"), rate.c_str());
        byteRateToString(UvFile::getWriteRate(), rate);
        ImGui::Text(_("Write rate: %s"), rate.c_str());
        byteRateToString(UvFile::getDownloadRate(), rate);
        ImGui::Text(_("Download rate: %s"), rate.c_str());
        if (ImGui::BeginTable("UvFiles", 2, ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(_("Caching"));
            ImGui::TableSetupColumn(_("Filename"));
            ImGui::TableHeadersRow();
            UvThreadOp::iterateOverAllOps([](UvThreadOp* f) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (f->caching()) {
                    ImGui::ProgressBar(f->cacheProgress());
                } else {
                    std::string label = fmt::format("Cache##{}", reinterpret_cast<void*>(f));
                    auto canCache = f->canCache();
                    if (!canCache) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Button(label.c_str()) && canCache) {
                        f->startCaching();
                    }
                    if (!canCache) {
                        ImGui::EndDisabled();
                    }
                }
                ImGui::TableSetColumnIndex(1);
                File* actual = dynamic_cast<File*>(f);
                ImGui::TextUnformatted(actual->filename().string().c_str());
            });
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void PCSX::Widgets::UvFiles::byteRateToString(float rate, std::string& str) {
    if (rate >= 1000000000) {
        str = fmt::format("{:.2f} GB/s", rate / 1000000000);
    } else if (rate >= 1000000) {
        str = fmt::format("{:.2f} MB/s", rate / 1000000);
    } else if (rate >= 1000) {
        str = fmt::format("{:.2f} KB/s", rate / 1000);
    } else {
        str = fmt::format("{:.2f} B/s", rate);
    }
}
