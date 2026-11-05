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

#include "gui/widgets/spulogger.h"

#include "core/system.h"
#include "imgui.h"
#include "support/file.h"

void PCSX::Widgets::SPULogger::draw(PCSX::SPULogger *logger, const char *title) {
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox(_("Enable"), &m_enabled);
    ImGui::SameLine();
    if (ImGui::Button(_("Clear"))) logger->clear();
    ImGui::SameLine();
    ImGui::Checkbox(_("Follow"), &m_followTail);
    ImGui::SameLine();
    if (ImGui::Button(_("Export SPD..."))) m_exportDialog.openDialog();
    if (!m_exportStatus.empty()) ImGui::TextUnformatted(m_exportStatus.c_str());

    const size_t count = logger->size();
    const uint64_t seen = logger->seen();
    // seen counts everything since the last clear, so the difference is exactly what the ring
    // has dropped. Saying so beats letting a truncated log read as a complete one.
    if (seen > count) {
        ImGui::TextUnformatted(fmt::format(f_("{} events, {} dropped"), count, seen - count).c_str());
    } else {
        ImGui::TextUnformatted(fmt::format(f_("{} events"), count).c_str());
    }

    if (ImGui::TreeNode(_("Filter"))) {
        ImGui::Checkbox(_("Filter by kind"), &m_filterEnabled);
        for (unsigned i = 0; i <= unsigned(PCSX::SPULogger::Kind::RegWrite); i++) {
            bool on = m_kindMask & (1u << i);
            if (ImGui::Checkbox(PCSX::SPULogger::kindName(PCSX::SPULogger::Kind(i)), &on)) {
                if (on) {
                    m_kindMask |= (1u << i);
                } else {
                    m_kindMask &= ~(1u << i);
                }
            }
        }
        ImGui::TreePop();
    }

    ImGui::Separator();

    if (ImGui::BeginChild("SPUEvents")) {
        if (ImGui::BeginTable("SPULog", 7,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(_("Cycle"));
            ImGui::TableSetupColumn(_("PC"));
            ImGui::TableSetupColumn(_("Event"));
            ImGui::TableSetupColumn(_("Voice"));
            ImGui::TableSetupColumn(_("SPU addr"));
            ImGui::TableSetupColumn(_("Length"));
            ImGui::TableSetupColumn(_("Value"));
            ImGui::TableHeadersRow();

            // The ring holds tens of thousands of entries and every one of them would otherwise
            // emit ImGui calls per frame. Collect the surviving indices first - a mask test is
            // nearly free - then let the clipper draw only the rows on screen.
            m_visible.clear();
            m_visible.reserve(count);
            for (size_t i = 0; i < count; i++) {
                const auto &ev = (*logger)[i];
                if (m_filterEnabled && !(m_kindMask & (1u << unsigned(ev.kind)))) continue;
                m_visible.push_back(uint32_t(i));
            }

            ImGuiListClipper clipper;
            clipper.Begin(int(m_visible.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    const size_t i = m_visible[row];
                    const auto &ev = (*logger)[i];

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(fmt::format("{}", ev.cycle).c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(int(i));
                    if (ImGui::SmallButton(fmt::format("{:08x}", ev.pc).c_str())) {
                        g_system->m_eventBus->signal(Events::GUI::JumpToPC{ev.pc});
                    }
                    ImGui::PopID();

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(PCSX::SPULogger::kindName(ev.kind));

                    ImGui::TableSetColumnIndex(3);
                    if (ev.voice != PCSX::SPULogger::c_noVoice) {
                        ImGui::TextUnformatted(fmt::format("{}", ev.voice).c_str());
                    }

                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(fmt::format("{:05x}", ev.address).c_str());

                    ImGui::TableSetColumnIndex(5);
                    if (ev.length) ImGui::TextUnformatted(fmt::format("{}", ev.length).c_str());

                    ImGui::TableSetColumnIndex(6);
                    ImGui::TextUnformatted(fmt::format("{:04x}", ev.value).c_str());
                }
            }

            // Only meaningful once the clipper has run, so it stays outside the row loop.
            if (m_followTail) ImGui::SetScrollHereY(1.0f);
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::End();
    drawExportDialog(logger);
}

void PCSX::Widgets::SPULogger::drawExportDialog(PCSX::SPULogger *logger) {
    if (!m_exportDialog.draw()) return;
    auto &selected = m_exportDialog.selected();
    if (selected.empty()) return;
    std::vector<uint8_t> data;
    std::string error = logger->exportSPD(data, {});
    if (!error.empty()) {
        m_exportStatus = error;
        return;
    }
    IO<File> file = new PosixFile(selected[0], FileOps::TRUNCATE);
    if (file->failed()) {
        m_exportStatus = _("Unable to create the SPD file.");
        return;
    }
    if (file->write(data.data(), data.size()) != static_cast<ssize_t>(data.size())) {
        m_exportStatus = _("Unable to write the whole SPD file.");
        return;
    }
    m_exportStatus = fmt::format(f_("Exported {} bytes."), data.size());
}

void PCSX::Widgets::SPULogger::syncEnabled(PCSX::SPULogger *logger) {
    if (m_enabled != m_pushedEnabled) {
        logger->setEnabled(m_enabled);
        m_pushedEnabled = m_enabled;
    } else if (logger->enabled() != m_enabled) {
        m_enabled = m_pushedEnabled = logger->enabled();
    }
}
