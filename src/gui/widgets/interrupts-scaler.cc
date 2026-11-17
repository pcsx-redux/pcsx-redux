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

#include "gui/widgets/interrupts-scaler.h"

#include <iterator>
#include <type_traits>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "imgui.h"

void PCSX::Widgets::InterruptsScaler::draw() {
    static const char* names[] = {
        "SIO",          "SIO1",    "CDR FIFO",    "CDR Command", "CDR Reads", "GPU DMA",
        "MDEC Out DMA", "SPU DMA", "MDEC In DMA", "GPU OTC DMA", "CDR DMA",
    };
    static_assert(std::size(names) == std::extent_v<decltype(R3000Acpu::m_scheduleScales)>);
    if (ImGui::Begin(_("Scheduler Scaler"), &m_show)) {
        if (ImGui::Button(_("Reset all"))) {
            for (auto& scale : g_emulator->m_cpu->m_scheduleScales) {
                scale = 1.0f;
            }
        }
        unsigned counter = 0;
        for (auto& scale : g_emulator->m_cpu->m_scheduleScales) {
            ImGui::SliderFloat(names[counter], &scale, 0.0f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            counter++;
        }
    }
    ImGui::End();
}
