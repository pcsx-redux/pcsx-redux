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

#define IMGUI_DEFINE_MATH_OPERATORS
#include "gui/widgets/zoomable-image.h"

#include <cmath>

#include "imgui.h"
#include "imgui_internal.h"

void PCSX::Widgets::ZoomableImage::zoom(float factor, ImVec2 centerUV) {
    ImVec2 dimensions = m_cornerBR - m_cornerTL;
    ImVec2 newDimensions = dimensions * factor;
    ImVec2 dimensionsDiff = newDimensions - dimensions;
    m_cornerTL -= dimensionsDiff * centerUV;
    m_cornerBR = m_cornerTL + newDimensions;
}

void PCSX::Widgets::ZoomableImage::resetView() {
    m_cornerTL = {0.0f, 0.0f};
    m_cornerBR = defaultViewSize() * m_DPI;
}

void PCSX::Widgets::ZoomableImage::moveTo(ImVec2 pos) {
    ImVec2 dimensions = m_cornerBR - m_cornerTL;
    m_cornerTL = pos;
    m_cornerBR = m_cornerTL + dimensions;
}

void PCSX::Widgets::ZoomableImage::handlePanZoom(const ImGuiIO &io, ImVec2 dimensions) {
    if (io.MouseWheel != 0.0f) {
        static const float increment = 1.2f;
        // Exponential in the wheel delta, so fractional deltas from high-resolution
        // wheels and touchpads (SDL3 on Wayland, axis_value120) zoom proportionally.
        const float step = std::pow(increment, io.MouseWheel);
        zoom(step, m_mouseUV);
    } else if (io.MouseDown[2] || (io.MouseDown[0] && io.MouseDown[1])) {
        m_cornerTL.x += io.MouseDelta.x;
        m_cornerTL.y += io.MouseDelta.y;
        m_cornerBR = m_cornerTL + dimensions;
    }
}
