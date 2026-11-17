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

#include "core/system.h"
#include "gui/gui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "support/imgui-helpers.h"

void PCSX::GUI::drawOutputWindow(ImGuiContext* context, ImGuiID dockspaceId, ImGuiDockNode* dockspaceNode, int w, int h,
                                 float renderRatio) {
    if (m_fullWindowRender && !dockspaceNode->IsEmpty()) {
        m_fullWindowRender = false;
        ImGui::SetNextWindowDockID(dockspaceId);
    }
    m_outputWindowFocused = false;
    if (m_fullWindowRender) {
        ImTextureID texture = m_offscreenTextures[m_currentTexture];
        const auto basePos = ImGui::GetMainViewport()->Pos;
        const auto displayFramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        const auto logicalRenderSize =
            ImVec2(m_renderSize[0] / displayFramebufferScale[0], m_renderSize[1] / displayFramebufferScale[1]);
        ImGui::SetNextWindowPos(ImVec2((w - m_renderSize.x) / 2.0f / displayFramebufferScale[0] + basePos.x,
                                       (h - m_renderSize.y) / 2.0f / displayFramebufferScale[0] + basePos.y));
        ImGui::SetNextWindowSize(logicalRenderSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("FullWindowRender", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (g_system->getArgs().isShadersDisabled()) {
            ImGui::Image(texture, logicalRenderSize, ImVec2(0, 0), ImVec2(1, 1));
        } else {
            m_outputShaderEditor.renderWithImgui(this, texture, m_renderSize, logicalRenderSize);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    } else {
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
        bool outputWindowShown = true;
        if (ImGui::Begin(
                _("Output"), &outputWindowShown,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse)) {
            m_outputWindowFocused = ImGui::IsWindowFocused();
            ImGuiDockNode* outputNode = ImGui::GetWindowDockNode();
            if (outputNode && dockspaceNode->OnlyNodeWithWindows == outputNode &&
                (!outputNode->TabBar || outputNode->TabBar->Tabs.size() == 1)) {
                // if output is the only visible window in dockspace, switch to full window render mode automatically
                outputWindowShown = false;
            }
            ImVec2 contentRegion = ImGui::GetContentRegionAvail();
            ImVec2 textureSize = contentRegion;
            if ((m_outputWindowSize.x != textureSize.x) || (m_outputWindowSize.y != textureSize.y)) {
                m_outputWindowSize = textureSize;
                m_setupScreenSize = true;
            }
            ImGuiHelpers::normalizeDimensions(textureSize, renderRatio);
            ImGui::SetCursorPos(ImGui::GetCursorPos() + (contentRegion - textureSize) * 0.5f);
            ImTextureID texture = m_offscreenTextures[m_currentTexture];
            if (g_system->getArgs().isShadersDisabled()) {
                ImGui::Image(texture, textureSize, ImVec2(0, 0), ImVec2(1, 1));
            } else {
                m_outputShaderEditor.renderWithImgui(this, texture, m_renderSize, textureSize);
            }
        }
        ImGui::End();
        if (!outputWindowShown) {
            m_fullWindowRender = true;
            // full window render mode can't have anything docked in the dockspace
            ImGui::DockContextClearNodes(context, dockspaceId, true);
        }
    }
}
