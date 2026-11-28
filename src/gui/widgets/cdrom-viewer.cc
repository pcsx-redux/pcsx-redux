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

#include "gui/widgets/cdrom-viewer.h"

#include <algorithm>
#include <cmath>

#include "GL/gl3w.h"
#include "core/cdromlogger.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "gui/gui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "supportpsx/iec-60908b.h"

// Polar-mode inner-hole radius as a fraction of the disc radius. Physically the
// CD program area runs from r_in = 25 mm to r_out ~= 58 mm on a full disc
// (IEC 60908 / Red Book), so the data band starts at r_in / r_out ~= 0.43 of the
// outer radius. This is deliberately a fixed geometric constant: deriving it
// from the live disc sector count made the polar shape change on a hard reset,
// because getTD(0) momentarily reads 0 mid-reset before the tracks are
// re-parsed. Disc length still shows through where the data lands in the annulus
// and via the extent backdrop - not through the hole size.
static constexpr float c_innerHole = 0.43f;

static const GLchar *s_defaultVertexShader = GL_SHADER_VERSION R"(
precision highp float;

in vec2 i_position;
in vec2 i_texUV;

uniform mat4 u_projMatrix;

out vec2 fragUV;

void main() {
    fragUV = i_texUV;
    gl_Position = u_projMatrix * vec4(i_position.xy, 0.0f, 1.0f);
}
)";

static const GLchar *s_defaultPixelShader = GL_SHADER_VERSION R"(
precision highp float;
#if __VERSION__ == 300
// On MacOS, with shader version 410, these are illegal, but on
// Windows, with shader version 300 es and old drivers, these
// are mandatory.
precision highp usampler2D;
#endif

uniform usampler2D u_dataHeatmap;
uniform usampler2D u_audioHeatmap;
uniform usampler2D u_seekHeatmap;

// Per-radius ring-max textures (640x1), sampled in polar mode for the whole-ring highlight.
uniform usampler2D u_dataRing;
uniform usampler2D u_audioRing;
uniform usampler2D u_seekRing;

uniform vec4 u_dataColor;
uniform vec4 u_audioColor;
uniform vec4 u_seekColor;

uniform bool u_showData;
uniform bool u_showAudio;
uniform bool u_showSeek;

uniform uint u_currentCycle;
uniform float u_decayHalfLife;

uniform bool u_polarMode;
uniform float u_innerHole;   // polar inner hole radius, fraction of disc radius
uniform uint u_discSectors;  // lead-out LBA; 0 = unknown
uniform float u_side;        // grid side (640)

in vec2 fragUV;
out vec4 outColor;

const float PI = 3.14159265358979;

float heatFromAge(uint stamp) {
    float age = float(u_currentCycle - stamp);
    return exp2(-age / u_decayHalfLife);
}

void main() {
    vec2 uv = fragUV;

    // Map the fragment to a heatmap texel coordinate (heatUV) and a linear LBA.
    // This is the ONLY thing that differs between raster and polar modes; the
    // textures, decay, and compositing below are identical.
    vec2 heatUV;
    bool inDisc;
    uint lba;

    if (!u_polarMode) {
        // Square raster: the image IS the row-major LBA grid.
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            outColor = vec4(0.04, 0.04, 0.05, 1.0);
            return;
        }
        heatUV = uv;
        uint col = uint(clamp(floor(uv.x * u_side), 0.0, u_side - 1.0));
        uint row = uint(clamp(floor(uv.y * u_side), 0.0, u_side - 1.0));
        lba = row * uint(u_side) + col;
        inDisc = (u_discSectors == 0u) || (lba < u_discSectors);
    } else {
        // Polar disc: a laser-position view. Radius follows the CLV area law
        // (reads inside-out: low LBA at the hub, high at the rim). We light the
        // WHOLE ring at each radius - the per-radius most-recent read, from the
        // ring-max textures sampled below - rather than a thin rotating arc,
        // because at the physical rotation rate (~200-460 rpm) the arc is an
        // untrackable blur. Angle is purely decorative. A seek lights the ring
        // at its target radius (the head flying across the disc).
        vec2 p = uv - vec2(0.5);
        float rho = length(p) / 0.5;          // 0 at center, 1 at inscribed rim
        if (rho < u_innerHole || rho > 1.0) {
            // Inner spindle hole or beyond the outer edge: bare platter.
            outColor = vec4(0.04, 0.04, 0.05, 1.0);
            return;
        }
        float fRadial = (rho * rho - u_innerHole * u_innerHole) /
                        (1.0 - u_innerHole * u_innerHole);       // 0..1 = radius -> ring
        heatUV = vec2(fRadial, 0.5);
        float nmax = u_side * u_side;
        lba = uint(clamp(fRadial * nmax, 0.0, nmax - 1.0));
        inDisc = (u_discSectors == 0u) || (lba < u_discSectors);
    }

    // Faint backdrop so the disc extent is visible even with no recent activity.
    vec3 baseColor = inDisc ? vec3(0.11, 0.11, 0.13) : vec3(0.045, 0.045, 0.05);

    // Raster samples the full 2D heatmap; polar samples the per-radius ring-max
    // textures so the whole ring at a read radius lights up.
    uint dataStamp, audioStamp, seekStamp;
    if (u_polarMode) {
        dataStamp = texture(u_dataRing, heatUV).r;
        audioStamp = texture(u_audioRing, heatUV).r;
        seekStamp = texture(u_seekRing, heatUV).r;
    } else {
        dataStamp = texture(u_dataHeatmap, heatUV).r;
        audioStamp = texture(u_audioHeatmap, heatUV).r;
        seekStamp = texture(u_seekHeatmap, heatUV).r;
    }

    // In polar mode the data channel is laser-red (it is the reading laser), and
    // intensity is boosted since the rings read fainter than the raster grid.
    vec3 dataRGB = u_polarMode ? vec3(1.0, 0.08, 0.03) : u_dataColor.rgb;
    float gain = u_polarMode ? 2.0 : 1.0;

    float dataHeat = (u_showData && dataStamp != 0u) ? min(heatFromAge(dataStamp) * gain, 1.0) : 0.0;
    float audioHeat = (u_showAudio && audioStamp != 0u) ? min(heatFromAge(audioStamp) * gain, 1.0) : 0.0;
    float seekHeat = (u_showSeek && seekStamp != 0u) ? min(heatFromAge(seekStamp) * gain, 1.0) : 0.0;

    float totalHeat = dataHeat * u_dataColor.a + audioHeat * u_audioColor.a + seekHeat * u_seekColor.a;
    vec3 heatColor = dataHeat * dataRGB * u_dataColor.a
                   + audioHeat * u_audioColor.rgb * u_audioColor.a
                   + seekHeat * u_seekColor.rgb * u_seekColor.a;

    float blend = clamp(totalHeat, 0.0, 1.0);
    vec3 finalColor = mix(baseColor, heatColor / max(totalHeat, 0.001), blend);

    outColor = vec4(finalColor, 1.0);
}
)";

PCSX::Widgets::CDRomViewer::CDRomViewer(bool &show) : ZoomableImage(show) {
    m_editor.setText(s_defaultVertexShader, s_defaultPixelShader, "");
    m_cornerBR = {640.0f, 640.0f};
}

ImVec2 PCSX::Widgets::CDRomViewer::defaultViewSize() const { return {640.0f, 640.0f}; }

bool PCSX::Widgets::CDRomViewer::uvToLBA(ImVec2 uv, uint32_t &lba) const {
    const float side = float(CDRomLogger::c_side);
    if (!m_polarMode) {
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return false;
        uint32_t col = uint32_t(std::clamp(std::floor(uv.x * side), 0.0f, side - 1.0f));
        uint32_t row = uint32_t(std::clamp(std::floor(uv.y * side), 0.0f, side - 1.0f));
        lba = row * CDRomLogger::c_side + col;
        return true;
    }
    // Mirror the polar GLSL mapping (radius -> ring) for the hover readout. The
    // reported LBA is the first sector of the ring at the cursor's radius.
    const float h = c_innerHole;
    float px = uv.x - 0.5f, py = uv.y - 0.5f;
    float rho = std::sqrt(px * px + py * py) / 0.5f;
    if (rho < h || rho > 1.0f) return false;
    float fRadial = (rho * rho - h * h) / (1.0f - h * h);
    float nmax = side * side;
    lba = uint32_t(std::clamp(fRadial * nmax, 0.0f, nmax - 1.0f));
    return true;
}

void PCSX::Widgets::CDRomViewer::compileShader(GUI *gui) {
    auto status = m_editor.compile(gui, {"i_position", "i_texUV"});
    if (!status.isOk()) return;

    m_shaderProgram = m_editor.getProgram();

    m_locProjMtx = glGetUniformLocation(m_shaderProgram, "u_projMatrix");
    m_locVtxPos = glGetAttribLocation(m_shaderProgram, "i_position");
    m_locVtxUV = glGetAttribLocation(m_shaderProgram, "i_texUV");
    m_locDataHeatmap = glGetUniformLocation(m_shaderProgram, "u_dataHeatmap");
    m_locAudioHeatmap = glGetUniformLocation(m_shaderProgram, "u_audioHeatmap");
    m_locSeekHeatmap = glGetUniformLocation(m_shaderProgram, "u_seekHeatmap");
    m_locDataColor = glGetUniformLocation(m_shaderProgram, "u_dataColor");
    m_locAudioColor = glGetUniformLocation(m_shaderProgram, "u_audioColor");
    m_locSeekColor = glGetUniformLocation(m_shaderProgram, "u_seekColor");
    m_locShowData = glGetUniformLocation(m_shaderProgram, "u_showData");
    m_locShowAudio = glGetUniformLocation(m_shaderProgram, "u_showAudio");
    m_locShowSeek = glGetUniformLocation(m_shaderProgram, "u_showSeek");
    m_locCurrentCycle = glGetUniformLocation(m_shaderProgram, "u_currentCycle");
    m_locDecayHalfLife = glGetUniformLocation(m_shaderProgram, "u_decayHalfLife");
    m_locPolarMode = glGetUniformLocation(m_shaderProgram, "u_polarMode");
    m_locInnerHole = glGetUniformLocation(m_shaderProgram, "u_innerHole");
    m_locDiscSectors = glGetUniformLocation(m_shaderProgram, "u_discSectors");
    m_locSide = glGetUniformLocation(m_shaderProgram, "u_side");
    m_locDataRing = glGetUniformLocation(m_shaderProgram, "u_dataRing");
    m_locAudioRing = glGetUniformLocation(m_shaderProgram, "u_audioRing");
    m_locSeekRing = glGetUniformLocation(m_shaderProgram, "u_seekRing");
}

void PCSX::Widgets::CDRomViewer::imguiCB(const ImDrawList *parentList, const ImDrawCmd *cmd) {
    if (!m_shaderProgram) return;

    GLint imguiProgramID;
    glGetIntegerv(GL_CURRENT_PROGRAM, &imguiProgramID);

    GLint projMatrixLocation = glGetUniformLocation(imguiProgramID, "ProjMtx");
    GLfloat currentProjection[4][4];
    glGetUniformfv(imguiProgramID, projMatrixLocation, &currentProjection[0][0]);

    glUseProgram(m_shaderProgram);

    auto *logger = g_emulator->m_cdromLogger.get();
    uint32_t currentCycle = static_cast<uint32_t>(g_emulator->m_cpu->m_regs.cycle);
    uint32_t discSectors = logger->getDiscSectors();

    float innerHole = c_innerHole;

    glUniformMatrix4fv(m_locProjMtx, 1, GL_FALSE, &currentProjection[0][0]);
    glUniform1ui(m_locCurrentCycle, currentCycle);
    glUniform1f(m_locDecayHalfLife, logger->m_decayHalfLife);
    glUniform1i(m_locShowData, m_showData);
    glUniform1i(m_locShowAudio, m_showAudio);
    glUniform1i(m_locShowSeek, m_showSeek);
    glUniform4f(m_locDataColor, m_dataColor.x, m_dataColor.y, m_dataColor.z, m_dataColor.w);
    glUniform4f(m_locAudioColor, m_audioColor.x, m_audioColor.y, m_audioColor.z, m_audioColor.w);
    glUniform4f(m_locSeekColor, m_seekColor.x, m_seekColor.y, m_seekColor.z, m_seekColor.w);
    glUniform1i(m_locPolarMode, m_polarMode);
    glUniform1f(m_locInnerHole, innerHole);
    glUniform1ui(m_locDiscSectors, discSectors);
    glUniform1f(m_locSide, float(CDRomLogger::c_side));

    glUniform1i(m_locDataHeatmap, 0);
    glUniform1i(m_locAudioHeatmap, 1);
    glUniform1i(m_locSeekHeatmap, 2);
    glUniform1i(m_locDataRing, 3);
    glUniform1i(m_locAudioRing, 4);
    glUniform1i(m_locSeekRing, 5);

    glActiveTexture(GL_TEXTURE0);
    logger->bindDataHeatmap();
    glActiveTexture(GL_TEXTURE1);
    logger->bindAudioHeatmap();
    glActiveTexture(GL_TEXTURE2);
    logger->bindSeekHeatmap();
    glActiveTexture(GL_TEXTURE3);
    logger->bindDataRing();
    glActiveTexture(GL_TEXTURE4);
    logger->bindAudioRing();
    glActiveTexture(GL_TEXTURE5);
    logger->bindSeekRing();
    glActiveTexture(GL_TEXTURE0);

    glEnableVertexAttribArray(m_locVtxPos);
    glVertexAttribPointer(m_locVtxPos, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid *)offsetof(ImDrawVert, pos));
    glEnableVertexAttribArray(m_locVtxUV);
    glVertexAttribPointer(m_locVtxUV, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid *)offsetof(ImDrawVert, uv));
}

void PCSX::Widgets::CDRomViewer::drawDisc(GUI *gui) {
    if (!m_shaderProgram) {
        compileShader(gui);
    }

    m_resolution = ImGui::GetContentRegionAvail();
    m_origin = ImGui::GetCursorScreenPos();
    m_mousePos = ImGui::GetIO().MousePos;

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    // The OpenGL backend binds its own sampler object, which overrides the texture's filter parameters.
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    drawList->AddCallback(
        [](const ImDrawList *parentList, const ImDrawCmd *cmd) {
            CDRomViewer *that = reinterpret_cast<CDRomViewer *>(cmd->UserCallbackData);
            that->imguiCB(parentList, cmd);
        },
        this);

    ImVec2 dimensions = m_cornerBR - m_cornerTL;
    ImVec2 texTL = ImVec2(0.0f, 0.0f) - m_cornerTL / dimensions;
    ImVec2 texBR = ImVec2(1.0f, 1.0f) - (m_cornerBR - m_resolution) / dimensions;

    auto *logger = g_emulator->m_cdromLogger.get();
    GLuint texID = logger->getDataHeatmapID();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::ImageButton("cdrom", (ImTextureID)(intptr_t)texID, m_resolution, texTL, texBR);
    ImGui::PopStyleVar();

    bool hovered = m_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_None);

    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    // Without sampler objects, the backend's filter mode survives the reset.
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

    const auto &io = ImGui::GetIO();
    ImVec2 texSpan = texBR - texTL;
    if (hovered) {
        m_mouseUV = texTL + texSpan * (m_mousePos - m_origin) / m_resolution;
    }

    if (!hovered) return;
    handlePanZoom(io, dimensions);
}

void PCSX::Widgets::CDRomViewer::draw(GUI *gui) {
    bool openDataColorPicker = false;
    bool openAudioColorPicker = false;
    bool openSeekColorPicker = false;

    auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar;
    if (ImGui::Begin(_("CD-ROM Viewer"), &m_show, flags)) {
        m_DPI = ImGui::GetWindowDpiScale();
        if (!m_firstShown) {
            resetView();
            m_firstShown = true;
        }
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu(_("View"))) {
                if (ImGui::MenuItem(_("Reset view"))) resetView();
                ImGui::Separator();
                ImGui::MenuItem(_("Polar disc layout"), nullptr, &m_polarMode);
                ImGui::Separator();
                ImGui::MenuItem(_("Show data reads"), nullptr, &m_showData);
                ImGui::MenuItem(_("Show audio (CDDA)"), nullptr, &m_showAudio);
                ImGui::MenuItem(_("Show seeks"), nullptr, &m_showSeek);
                ImGui::Separator();
                ImGui::MenuItem(_("Show Shader Editor"), nullptr, &m_editor.m_show);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Configuration"))) {
                ImGui::MenuItem(_("Select data color"), nullptr, &openDataColorPicker);
                ImGui::MenuItem(_("Select audio color"), nullptr, &openAudioColorPicker);
                ImGui::MenuItem(_("Select seek color"), nullptr, &openSeekColorPicker);
                ImGui::Separator();
                auto *logger = g_emulator->m_cdromLogger.get();
                float halfLifeMs = logger->m_decayHalfLife / 33868.8f;
                if (ImGui::SliderFloat(_("Decay half-life (ms)"), &halfLifeMs, 100.0f, 30000.0f, "%.0f",
                                       ImGuiSliderFlags_Logarithmic)) {
                    logger->m_decayHalfLife = halfLifeMs * 33868.8f;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::Separator();

            // Cursor readout: LBA + MSF + disc extent.
            if (m_hovered) {
                uint32_t lba;
                if (uvToLBA(m_mouseUV, lba)) {
                    PCSX::IEC60908b::MSF msf(lba);
                    ImGui::Text("LBA %u  %02u:%02u:%02u", lba, msf.m, msf.s, msf.f);
                }
            }
            ImGui::EndMenuBar();
        }
        drawDisc(gui);
    }

    if (openDataColorPicker) ImGui::OpenPopup(_("Data Color Picker"));
    if (ImGui::BeginPopupModal(_("Data Color Picker"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker4(
            "##DataColorPicker", (float *)&m_dataColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
        if (ImGui::Button(_("OK"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openAudioColorPicker) ImGui::OpenPopup(_("Audio Color Picker"));
    if (ImGui::BeginPopupModal(_("Audio Color Picker"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker4(
            "##AudioColorPicker", (float *)&m_audioColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
        if (ImGui::Button(_("OK"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openSeekColorPicker) ImGui::OpenPopup(_("Seek Color Picker"));
    if (ImGui::BeginPopupModal(_("Seek Color Picker"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker4(
            "##SeekColorPicker", (float *)&m_seekColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
        if (ImGui::Button(_("OK"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();

    if (m_editor.m_show) {
        bool changed = m_editor.draw(gui, _("CD-ROM Shader Editor"));
        if (changed) compileShader(gui);
    }
}
