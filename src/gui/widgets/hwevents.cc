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

#include "gui/widgets/hwevents.h"

#include <filesystem>

#include "core/psxemulator.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "imgui.h"

PCSX::Widgets::HWEvents::HWEvents(bool& show, std::vector<std::string>& favorites)
    : m_show(show),
      m_saveDialog(l_("Save hardware events"), favorites),
      m_loadDialog(l_("Load hardware events"), favorites) {}

void PCSX::Widgets::HWEvents::draw(GUI* gui, const char* title) {
    if (m_show) {
        if (ImGui::Begin(title, &m_show)) {
            auto& events = *g_emulator->m_hwEvents;
            ImGui::TextUnformatted(_("Record:"));
            uint32_t mask = events.mask();
            for (unsigned source = 0; source < 32; source++) {
                const char* name = PCSX::HWEvents::sourceName(source);
                if (!name) continue;
                ImGui::SameLine();
                bool armed = mask & (1u << source);
                if (ImGui::Checkbox(fmt::format("{}##arm{}", name, source).c_str(), &armed)) {
                    events.setMask(armed ? (mask | (1u << source)) : (mask & ~(1u << source)));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(_("Clear"))) {
                events.clear();
                m_live.followTail = false;
            }
            ImGui::SameLine();
            if (ImGui::Button(_("Save..."))) m_saveDialog.openDialog();
            ImGui::SameLine();
            if (ImGui::Button(_("Load..."))) m_loadDialog.openDialog();
            if (events.full()) ImGui::TextUnformatted(_("Limit reached, recording stopped."));
            if (!m_error.empty()) ImGui::TextUnformatted(m_error.c_str());

            drawView(gui, events, m_live, true);
        }
        ImGui::End();
    }

    if (m_saveDialog.draw()) {
        auto& selected = m_saveDialog.selected();
        if (!selected.empty()) {
            IO<File> file = new PosixFile(selected[0], FileOps::TRUNCATE);
            if (file->failed()) {
                m_error = _("Unable to open the file for writing.");
            } else if (g_emulator->m_hwEvents->save(file)) {
                m_error.clear();
            } else {
                m_error = _("Writing the file failed.");
            }
        }
    }
    if (m_loadDialog.draw()) {
        auto& selected = m_loadDialog.selected();
        if (!selected.empty()) {
            IO<File> file = new PosixFile(selected[0]);
            auto& loaded = m_loaded.emplace_back();
            if (file->failed() || !loaded.events.load(file)) {
                m_loaded.pop_back();
                m_error = _("Not a hardware events file.");
            } else {
                loaded.name = std::filesystem::path(selected[0]).filename().string();
                m_error.clear();
            }
        }
    }

    for (auto it = m_loaded.begin(); it != m_loaded.end();) {
        auto& loaded = *it;
        std::string windowTitle = fmt::format("{}: {}###hwevents{}", title, loaded.name, static_cast<void*>(&loaded));
        ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(windowTitle.c_str(), &loaded.show)) {
            drawView(gui, loaded.events, loaded.view, false);
        }
        ImGui::End();
        if (!loaded.show) {
            it = m_loaded.erase(it);
        } else {
            ++it;
        }
    }
}

void PCSX::Widgets::HWEvents::drawView(GUI* gui, const PCSX::HWEvents& events, View& view, bool live) {
    const size_t count = events.size();
    ImGui::TextUnformatted(fmt::format(f_("{} events"), count).c_str());
    ImGui::TextUnformatted(_("Show:"));
    for (unsigned source = 0; source < 32; source++) {
        const char* name = PCSX::HWEvents::sourceName(source);
        if (!name) continue;
        ImGui::SameLine();
        bool shown = view.shown & (1u << source);
        if (ImGui::Checkbox(fmt::format("{}##show{}", name, source).c_str(), &shown)) {
            view.shown = shown ? (view.shown | (1u << source)) : (view.shown & ~(1u << source));
        }
    }
    if (live) {
        ImGui::SameLine();
        ImGui::Checkbox(_("Follow"), &view.followTail);
    }

    // A cleared or reloaded store, or a new filter, starts the row list over.
    if (view.rowsFilter != view.shown || view.rowsStore != &events || view.rowsGeneration != events.generation() ||
        view.scanned > count) {
        view.rows.clear();
        view.scanned = 0;
        view.rowsFilter = view.shown;
        view.rowsStore = &events;
        view.rowsGeneration = events.generation();
    }
    for (; view.scanned < count; view.scanned++) {
        // A loaded file can hold any type; sources past the mask's 32 bits are always shown.
        unsigned source = events[view.scanned].type >> 8;
        if (source >= 32 || (view.shown & (1u << source))) view.rows.push_back(view.scanned);
    }

    ImGui::Separator();
    gui->useMonoFont();
    ImGui::BeginChild("##hweventsScroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (count == 0) {
        ImGui::TextUnformatted(live ? _("No events recorded. Tick a source and run.") : _("Empty store."));
    } else {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(view.rows.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                uint32_t idx = view.rows[row];
                const HWEvent& e = events[idx];
                ImGui::TextUnformatted(fmt::format("{:8}: {:12} {:08x}  {}", idx, e.cycle, e.pc,
                                                   PCSX::HWEvents::describe(e))
                                           .c_str());
            }
        }
        if (view.followTail) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
    ImGui::EndChild();
    ImGui::PopFont();
}
