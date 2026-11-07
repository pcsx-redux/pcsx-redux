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

#include "gui/widgets/scope_profiler.h"

#include <algorithm>
#include <vector>

#include "core/psxcounters.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "imgui.h"
#include "mips/common/hardware/pcsxprof.h"

namespace {

// How many vsyncs of history to keep.
constexpr size_t c_history = 600;
constexpr double c_cyclesPerUs = 33.8688;

uint32_t peek32(uint32_t address) {
    // Aligned, so the four bytes never straddle the end of a mapped page.
    if (address & 3) return 0;
    auto* ptr = PCSX::g_emulator->m_mem->getPointer<uint32_t>(address);
    return ptr ? *ptr : 0;
}

std::string peekString(uint32_t address) {
    std::string str;
    for (unsigned i = 0; i < 256; i++) {
        auto* ptr = PCSX::g_emulator->m_mem->getPointer<char>(address + i);
        if (!ptr || !*ptr) break;
        str += *ptr;
    }
    return str;
}

}  // namespace

PCSX::Widgets::ScopeProfiler::ScopeProfiler(bool& show) : m_show(show), m_listener(g_system->m_eventBus) {
    m_listener.listen<PCSX::Events::GPU::VSync>([this](const auto& event) {
        if (m_frozen) return;
        drain();
        if (!m_header) return;
        m_vsyncs.push_back(g_emulator->m_cpu->m_regs.cycle);
        while (m_vsyncs.size() > c_history) m_vsyncs.pop_front();
        uint64_t oldest = m_vsyncs.front();
        while (!m_events.empty() && m_events.front().end < oldest) m_events.pop_front();
    });
    m_listener.listen<PCSX::Events::ExecutionFlow::Reset>([this](const auto& event) {
        clear();
        m_frozen = false;
    });
    // A loaded state holds another run's buffer and counters: start over from it.
    m_listener.listen<PCSX::Events::ExecutionFlow::SaveStateLoaded>([this](const auto& event) {
        clear();
        m_frozen = false;
    });
}

void PCSX::Widgets::ScopeProfiler::clear() {
    m_events.clear();
    m_vsyncs.clear();
    m_zones.clear();
    m_header = 0;
    m_cursor = 0;
    m_dropped = 0;
    m_maxDepth = 0;
}

void PCSX::Widgets::ScopeProfiler::drain() {
    uint32_t header = g_emulator->m_mem->m_profilerHeader;
    if (header != m_header) {
        clear();
        m_header = header;
        if (header) m_cursor = peek32(header + offsetof(pcsx_ProfHeader, written));
    }
    if (!header) return;
    uint32_t magic = peek32(header + offsetof(pcsx_ProfHeader, magic));
    uint32_t sizes = peek32(header + offsetof(pcsx_ProfHeader, version));
    uint32_t entrySize = sizes >> 16;
    uint32_t capacity = peek32(header + offsetof(pcsx_ProfHeader, capacity));
    uint32_t entries = peek32(header + offsetof(pcsx_ProfHeader, entries));
    uint32_t written = peek32(header + offsetof(pcsx_ProfHeader, written));
    m_cyclesPer16Lines = peek32(header + offsetof(pcsx_ProfHeader, cyclesPer16Lines));
    if (!pcsx_profHeaderValid(magic, sizes & 0xffff, entrySize, capacity, entries)) return;

    // Reinstalled in place: the count restarted below where we left it.
    if (int32_t(written - m_cursor) < 0) m_cursor = 0;
    // After a freeze, whatever piled up is too old to place on the cycle axis.
    if (m_resync) {
        m_resync = false;
        m_cursor = written;
    }
    uint32_t pending = written - m_cursor;
    if (pending > capacity) {
        m_dropped += pending - capacity;
        m_cursor = written - capacity;
    }

    // Anchor: the stamp the guest would read right now, and the cycle it
    // corresponds to. Every entry is placed by its distance back from here.
    uint64_t now = g_emulator->m_cpu->m_regs.cycle;
    uint32_t nowStamp = (g_emulator->m_counters->peekCounter(1) << 16) | g_emulator->m_counters->peekCounter(2);

    for (; m_cursor != written; m_cursor++) {
        uint32_t entry = entries + (m_cursor & (capacity - 1)) * entrySize;
        Event event;
        event.zone = peek32(entry + offsetof(pcsx_ProfEntry, zone));
        uint32_t start = peek32(entry + offsetof(pcsx_ProfEntry, start));
        uint32_t end = peek32(entry + offsetof(pcsx_ProfEntry, end));
        uint32_t packed = peek32(entry + offsetof(pcsx_ProfEntry, vsync));
        event.depth = (packed >> 16) & 0xff;
        event.stackDepth = packed >> 24;
        event.start = now - pcsx_profElapsed(start, nowStamp, m_cyclesPer16Lines);
        event.end = now - pcsx_profElapsed(end, nowStamp, m_cyclesPer16Lines);
        if (event.start > event.end) event.start = event.end;
        m_maxDepth = std::max(m_maxDepth, unsigned(event.depth));
        m_events.push_back(event);
    }
}

const PCSX::Widgets::ScopeProfiler::Zone& PCSX::Widgets::ScopeProfiler::zone(uint32_t address) {
    auto it = m_zones.find(address);
    if (it != m_zones.end()) return it->second;
    Zone zone;
    zone.name = peekString(peek32(address + offsetof(pcsx_ProfZone, name)));
    zone.file = peekString(peek32(address + offsetof(pcsx_ProfZone, file)));
    zone.line = peek32(address + offsetof(pcsx_ProfZone, line));
    if (zone.name.empty()) zone.name = fmt::format("{:08x}", address);
    uint32_t hash = address * 2654435761u;
    float hue = float(hash >> 8) / float(1 << 24);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, r, g, b);
    zone.color = ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
    return m_zones.emplace(address, std::move(zone)).first->second;
}

void PCSX::Widgets::ScopeProfiler::draw(const char* title, GUI* gui) {
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }
    if (!m_frozen) drain();

    if (!m_header) {
        ImGui::TextWrapped(
            "%s", _("No profiler registered. A psyqo program registers one by calling psyqo::Profiler::install()."));
        ImGui::End();
        return;
    }

    if (ImGui::Checkbox(_("Freeze"), &m_frozen) && !m_frozen) m_resync = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt(_("VSyncs shown"), &m_window, 1, 60);
    int available = int(m_vsyncs.size()) - 1;
    if (m_frozen && (available > m_window)) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderInt(_("Back"), &m_scrollBack, 0, available - m_window);
    } else {
        m_scrollBack = 0;
    }
    ImGui::Text(_("%zu scopes held, %llu lost to buffer overruns, %u cycles per 16 lines"), m_events.size(),
                (unsigned long long)m_dropped, m_cyclesPer16Lines);

    if (m_vsyncs.size() < 2) {
        ImGui::TextUnformatted(_("Waiting for vsyncs..."));
        ImGui::End();
        return;
    }

    int last = int(m_vsyncs.size()) - 1 - m_scrollBack;
    int first = std::max(0, last - m_window);
    uint64_t t0 = m_vsyncs[first];
    uint64_t t1 = m_vsyncs[last];
    double span = double(t1 - t0);

    const float rowHeight = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
    const float rulerHeight = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width = std::max(100.0f, ImGui::GetContentRegionAvail().x);
    float height = rulerHeight + rowHeight * (m_maxDepth + 1);
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(24, 24, 24, 255));
    drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);

    auto toX = [&](uint64_t t) { return origin.x + float(double(int64_t(t - t0)) / span * width); };

    for (int i = first; i <= last; i++) {
        float x = toX(m_vsyncs[i]);
        drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), IM_COL32(90, 90, 90, 255));
        if (i < last) {
            auto label = fmt::format("vsync -{}", last - i + m_scrollBack);
            drawList->AddText(ImVec2(x + 3.0f, origin.y), IM_COL32(150, 150, 150, 255), label.c_str());
        }
    }

    const Event* hoveredEvent = nullptr;
    for (const auto& event : m_events) {
        if ((event.end < t0) || (event.start > t1)) continue;
        float x0 = toX(std::max(event.start, t0));
        float x1 = std::max(x0 + 1.0f, toX(std::min(event.end, t1)));
        float y0 = origin.y + rulerHeight + rowHeight * event.depth;
        float y1 = y0 + rowHeight - 2.0f;
        const Zone& z = zone(event.zone);
        drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), z.color);
        ImVec2 textSize = ImGui::CalcTextSize(z.name.c_str());
        if (x1 - x0 > textSize.x + 4.0f) {
            drawList->AddText(ImVec2(x0 + 2.0f, y0 + 1.0f), IM_COL32(0, 0, 0, 255), z.name.c_str());
        }
        if (hovered && (mouse.x >= x0) && (mouse.x < x1) && (mouse.y >= y0) && (mouse.y < y1)) hoveredEvent = &event;
    }
    drawList->PopClipRect();

    if (hoveredEvent) {
        const Zone& z = zone(hoveredEvent->zone);
        uint64_t cycles = hoveredEvent->end - hoveredEvent->start;
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(z.name.c_str());
        ImGui::Text("%s:%u", z.file.c_str(), z.line);
        ImGui::Text(_("%llu cycles, %.1f us"), (unsigned long long)cycles, double(cycles) / c_cyclesPerUs);
        ImGui::Text(_("depth %u, call stack depth %u"), hoveredEvent->depth, hoveredEvent->stackDepth);
        ImGui::EndTooltip();
    }

    // Per-zone totals over the scopes that end inside the window.
    struct Stats {
        uint32_t zone;
        uint64_t calls = 0;
        uint64_t total = 0;
        uint64_t min = UINT64_MAX;
        uint64_t max = 0;
    };
    std::unordered_map<uint32_t, Stats> stats;
    for (const auto& event : m_events) {
        if ((event.end < t0) || (event.end > t1)) continue;
        auto& s = stats[event.zone];
        s.zone = event.zone;
        uint64_t cycles = event.end - event.start;
        s.calls++;
        s.total += cycles;
        s.min = std::min(s.min, cycles);
        s.max = std::max(s.max, cycles);
    }
    std::vector<Stats> sorted;
    for (auto& [address, s] : stats) sorted.push_back(s);
    std::sort(sorted.begin(), sorted.end(), [](const Stats& a, const Stats& b) { return a.total > b.total; });

    gui->useMonoFont();
    unsigned vsyncs = last - first;
    if (ImGui::BeginTable(
            "##zones", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(_("Zone"));
        ImGui::TableSetupColumn(_("Calls"));
        ImGui::TableSetupColumn(_("Cycles"));
        ImGui::TableSetupColumn(_("Per vsync"));
        ImGui::TableSetupColumn(_("Mean"));
        ImGui::TableSetupColumn(_("Min"));
        ImGui::TableSetupColumn(_("Max"));
        ImGui::TableHeadersRow();
        for (const auto& s : sorted) {
            const Zone& z = zone(s.zone);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(z.color), "%s", z.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s:%u", z.file.c_str(), z.line);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)s.calls);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)s.total);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)(s.total / vsyncs));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)(s.total / s.calls));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)s.min);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)s.max);
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ImGui::End();
}
