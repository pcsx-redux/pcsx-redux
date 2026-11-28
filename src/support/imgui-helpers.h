/*

MIT License

Copyright (c) 2022 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"

namespace PCSX {
namespace ImGuiHelpers {

static void normalizeDimensions(ImVec2& vec, float ratio) {
    float r = vec.y / vec.x;
    if (r > ratio) {
        vec.y = vec.x * ratio;
    } else {
        vec.x = vec.y / ratio;
    }
    vec.x = roundf(vec.x);
    vec.y = roundf(vec.y);
    vec.x = std::max(vec.x, 1.0f);
    vec.y = std::max(vec.y, 1.0f);
}

static void ShowHelpMarker(const char* desc) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted("(?)");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The OpenGL backend binds its own linear sampler object, which overrides the texture's filter
// parameters, so an image meant to stay pixelated has to ask for nearest sampling around its draw.
static void ImageNearest(ImTextureRef texture, const ImVec2& size, const ImVec2& uv0 = ImVec2(0, 0),
                         const ImVec2& uv1 = ImVec2(1, 1)) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    ImGui::Image(texture, size, uv0, uv1);
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
}

}  // namespace ImGuiHelpers
}  // namespace PCSX
