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

#include "gui/widgets/call_profiler.h"

#include <algorithm>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "support/gnu-c++-demangler.h"

void PCSX::Widgets::CallProfiler::draw(const char* title, PCSX::GUI* gui) {
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    auto& profiler = *g_emulator->m_profiler;
    if (g_emulator->m_cpu->isDynarec()) {
        ImGui::TextWrapped("%s", _("The profiler needs the interpreter. Disable the dynarec in "
                                   "Configuration->Emulation and restart."));
    }
    if (profiler.running()) {
        if (ImGui::Button(_("Stop"))) profiler.stop();
    } else {
        if (ImGui::Button(_("Start"))) profiler.start();
    }
    ImGui::SameLine();
    if (ImGui::Button(_("Clear"))) {
        profiler.clear();
        m_functions.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(_("Refresh"))) m_refresh = true;
    auto totals = profiler.totals();
    ImGui::SameLine();
    ImGui::Text(_("%llu instructions, %llu calls"), (unsigned long long)totals[::PCSX::Profiler::Instructions],
                (unsigned long long)totals[::PCSX::Profiler::Calls]);

    ImGui::InputText(_("File"), &m_path);
    if (ImGui::Button(_("Save pprof"))) {
        IO<File> out(new PosixFile(m_path, FileOps::TRUNCATE));
        if (out->failed()) {
            m_status = fmt::format(f_("Unable to create {}"), m_path);
        } else {
            auto data = profiler.serializePprof();
            out->write(data.data(), data.size());
            m_status = fmt::format(f_("Wrote {}"), m_path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(_("Save callgrind"))) {
        IO<File> out(new PosixFile(m_path, FileOps::TRUNCATE));
        if (out->failed()) {
            m_status = fmt::format(f_("Unable to create {}"), m_path);
        } else {
            auto data = profiler.serializeCallgrind();
            out->write(data.data(), data.size());
            m_status = fmt::format(f_("Wrote {}"), m_path);
        }
    }
    if (!m_status.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(m_status.c_str());
    }

    if (m_refresh) {
        m_functions = profiler.functions();
        m_refresh = false;
    }

    const uint64_t total = std::max<uint64_t>(totals[::PCSX::Profiler::Instructions], 1);
    static constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
                                             ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                             ImGuiTableFlags_BordersOuter;
    if (ImGui::BeginTable("functions", 5, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(_("Function"), ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn(_("Self"),
                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn(_("Self %"), ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn(_("Inclusive"), ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn(_("Calls"), ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableHeadersRow();
        if (auto sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0) {
            auto& spec = sort->Specs[0];
            auto value = [&](const ::PCSX::Profiler::Function& f) -> uint64_t {
                switch (spec.ColumnIndex) {
                    case 3:
                        return f.inclusive[::PCSX::Profiler::Instructions];
                    case 4:
                        return f.self[::PCSX::Profiler::Calls];
                }
                return f.self[::PCSX::Profiler::Instructions];
            };
            bool descending = spec.SortDirection == ImGuiSortDirection_Descending;
            std::stable_sort(m_functions.begin(), m_functions.end(), [&](const auto& a, const auto& b) {
                return descending ? value(a) > value(b) : value(a) < value(b);
            });
        }
        auto& debugSettings = g_emulator->settings.get<Emulator::SettingDebugSettings>();
        bool demangle = debugSettings.get<Emulator::DebugSettings::DemangledSymbols>();
        ImGuiListClipper clipper;
        clipper.Begin(m_functions.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                auto& f = m_functions[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                std::string name = demangle ? GNUDemangler::demangle(f.name) : f.name;
                ImGui::PushID(i);
                if (ImGui::Selectable(name.c_str())) {
                    g_system->m_eventBus->signal(::PCSX::Events::GUI::JumpToPC{f.address});
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)f.self[::PCSX::Profiler::Instructions]);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", 100.0 * f.self[::PCSX::Profiler::Instructions] / total);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)f.inclusive[::PCSX::Profiler::Instructions]);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)f.self[::PCSX::Profiler::Calls]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
