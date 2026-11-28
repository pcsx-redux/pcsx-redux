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

#include <cmath>

#include "GL/gl3w.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/ramlogger.h"
#include "core/system.h"
#include "gui/gui.h"
#include "gui/widgets/ram-viewer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "support/imgui-helpers.h"

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
precision highp sampler2D;
precision highp usampler2D;
#endif

uniform sampler2D u_ramTexture;
uniform usampler2D u_readHeatmap;
uniform usampler2D u_writeHeatmap;
uniform usampler2D u_execHeatmap;
uniform sampler2D u_fontAtlas;

uniform vec4 u_readColor;
uniform vec4 u_writeColor;
uniform vec4 u_execColor;

uniform bool u_showRead;
uniform bool u_showWrite;
uniform bool u_showExec;
uniform bool u_showGreyscale;

uniform float u_ramHeight;
uniform uint u_currentCycle;
uniform float u_decayHalfLife;
uniform vec2 u_resolution;
uniform vec2 u_origin;
uniform bool u_hovered;
uniform vec2 u_mousePos;
uniform vec2 u_mouseUV;
uniform vec2 u_cornerTL;
uniform vec2 u_cornerBR;
uniform vec2 u_pixelScale;
uniform bool u_drawGrid;
uniform vec4 u_gridColor;
uniform bool u_showHex;

// Glyph UV rects for hex digits 0-F: vec4(u0, v0, u1, v1)
uniform vec4 u_glyphUVs[16];
// Glyph aspect ratio (width / height)
uniform float u_glyphAspect;

in vec2 fragUV;
out vec4 outColor;

float cycleAge(uint timestamp) {
    uint age = u_currentCycle - timestamp;
    return float(age);
}

float heatFromAge(float age) {
    return exp2(-age / u_decayHalfLife);
}

float sampleGlyph(int nibble, vec2 cellPos) {
    vec4 gUV = u_glyphUVs[nibble];
    vec2 glyphUV = mix(gUV.xy, gUV.zw, cellPos);
    return texture(u_fontAtlas, glyphUV).a;
}

void main() {
    vec2 uv = fragUV;
    float normalizedHeight = u_ramHeight / 4096.0;
    vec2 ramUV = vec2(uv.x, uv.y * normalizedHeight);

    // Bounds check
    if (ramUV.x < 0.0 || ramUV.x > 1.0 || ramUV.y < 0.0 || ramUV.y > normalizedHeight) {
        outColor = vec4(0.1, 0.1, 0.1, 1.0);
        return;
    }

    // Sample raw RAM value
    float ramByte = texture(u_ramTexture, ramUV).r;
    int byteVal = int(ramByte * 255.0 + 0.5);
    vec3 baseColor = u_showGreyscale ? vec3(ramByte) : vec3(0.0);

    // Sample cycle timestamps
    uint readStamp = texture(u_readHeatmap, ramUV).r;
    uint writeStamp = texture(u_writeHeatmap, ramUV).r;
    uint execStamp = texture(u_execHeatmap, ramUV).r;

    // Compute heat intensities (0 timestamp = never accessed)
    float readHeat = (u_showRead && readStamp != 0u) ? heatFromAge(cycleAge(readStamp)) : 0.0;
    float writeHeat = (u_showWrite && writeStamp != 0u) ? heatFromAge(cycleAge(writeStamp)) : 0.0;
    float execHeat = (u_showExec && execStamp != 0u) ? heatFromAge(cycleAge(execStamp)) : 0.0;

    // Composite heatmap colors over base
    float totalHeat = readHeat * u_readColor.a + writeHeat * u_writeColor.a + execHeat * u_execColor.a;
    vec3 heatColor = readHeat * u_readColor.rgb * u_readColor.a
                   + writeHeat * u_writeColor.rgb * u_writeColor.a
                   + execHeat * u_execColor.rgb * u_execColor.a;

    float blend = clamp(totalHeat, 0.0, 1.0);
    vec3 finalColor = mix(baseColor, heatColor / max(totalHeat, 0.001), blend);

    vec2 pixelPos = vec2(2048.0, u_ramHeight) * uv;
    vec2 pixelFrac = fract(pixelPos);

    // Byte-level grid
    if (u_drawGrid) {
        if (pixelPos.x >= 0.0 && pixelPos.x <= 2048.0 && pixelPos.y >= 0.0 && pixelPos.y <= u_ramHeight) {
            vec2 pixelStep = 1.0 / u_pixelScale;
            float gridBlend = smoothstep(3.0, 5.0, u_pixelScale.x) * u_gridColor.a;
            float vertLine = 1.0 - step(0.5, smoothstep(0.0, pixelStep.x * 2.0, pixelFrac.x));
            float horzLine = 1.0 - step(0.5, smoothstep(0.0, pixelStep.y * 2.0, pixelFrac.y));
            vec3 gridRGB = u_gridColor.rgb;
            finalColor = mix(finalColor, gridRGB, vertLine * gridBlend);
            finalColor = mix(finalColor, gridRGB, horzLine * gridBlend);
        }
    }

    // Hex byte display when zoomed in enough
    if (u_showHex && u_pixelScale.x > 12.0) {
        float hexBlend = smoothstep(12.0, 18.0, u_pixelScale.x);

        int hiNibble = (byteVal >> 4) & 0xf;
        int loNibble = byteVal & 0xf;

        // Layout: two glyphs side by side within the cell, centered
        float glyphH = 0.5;  // 50% of cell height
        float glyphW = u_glyphAspect * glyphH;
        float gap = 0.02;
        float totalW = glyphW * 2.0 + gap;
        float startX = (1.0 - totalW) * 0.5;
        float startY = (1.0 - glyphH) * 0.5;  // vertically centered

        float glyphAlpha = 0.0;
        vec2 cellPos;

        // High nibble
        if (pixelFrac.x >= startX && pixelFrac.x < startX + glyphW &&
            pixelFrac.y >= startY && pixelFrac.y < startY + glyphH) {
            cellPos = vec2((pixelFrac.x - startX) / glyphW, (pixelFrac.y - startY) / glyphH);
            glyphAlpha = sampleGlyph(hiNibble, cellPos);
        }
        // Low nibble
        float loX = startX + glyphW + gap;
        if (pixelFrac.x >= loX && pixelFrac.x < loX + glyphW &&
            pixelFrac.y >= startY && pixelFrac.y < startY + glyphH) {
            cellPos = vec2((pixelFrac.x - loX) / glyphW, (pixelFrac.y - startY) / glyphH);
            glyphAlpha = sampleGlyph(loNibble, cellPos);
        }

        // Contrast: use white text on dark cells, black on light
        float lum = dot(finalColor, vec3(0.299, 0.587, 0.114));
        vec3 textColor = lum > 0.5 ? vec3(0.0) : vec3(1.0);
        finalColor = mix(finalColor, textColor, glyphAlpha * hexBlend);
    }

    outColor = vec4(finalColor, 1.0);
}
)";

PCSX::Widgets::RAMViewer::RAMViewer(bool &show) : ZoomableImage(show), m_listener(g_system->m_eventBus) {
    m_editor.setText(s_defaultVertexShader, s_defaultPixelShader, "");
    m_cornerBR = {2048.0f, 1024.0f};
    m_listener.listen<PCSX::Events::GUI::RAMFocus>([this](auto event) {
        m_show = true;
        focusOn(event.address, event.size);
    });
}

void PCSX::Widgets::RAMViewer::focusOn(uint32_t address, uint32_t size) {
    // Convert PS1 address to physical RAM offset
    if (g_emulator->settings.get<Emulator::Setting8MB>()) {
        address &= 0x7fffff;
    } else {
        address &= 0x1fffff;
    }

    // Convert to pixel coordinates in the 2048-wide layout
    uint32_t endAddr = address + size;
    float x1 = float(address % 2048);
    float y1 = float(address / 2048);
    float x2 = float(endAddr % 2048);
    float y2 = float(endAddr / 2048);

    // If the range spans multiple rows, show the full row width
    if (y2 > y1) {
        x1 = 0.0f;
        x2 = 2048.0f;
    }

    // Add some padding around the focus area
    float padX = (x2 - x1) * 0.2f + 4.0f;
    float padY = (y2 - y1) * 0.2f + 4.0f;
    x1 -= padX;
    y1 -= padY;
    x2 += padX;
    y2 += padY;

    // Set the view to show this region
    float viewW = x2 - x1;
    float viewH = y2 - y1;

    // Maintain aspect ratio with the window
    if (m_resolution.x > 0 && m_resolution.y > 0) {
        float windowAspect = m_resolution.x / m_resolution.y;
        float regionAspect = viewW / viewH;
        if (regionAspect > windowAspect) {
            viewH = viewW / windowAspect;
        } else {
            viewW = viewH * windowAspect;
        }
    }

    m_cornerTL = {x1 * m_DPI, y1 * m_DPI};
    m_cornerBR = {(x1 + viewW) * m_DPI, (y1 + viewH) * m_DPI};
}

ImVec2 PCSX::Widgets::RAMViewer::defaultViewSize() const {
    bool is8MB = g_emulator->settings.get<Emulator::Setting8MB>();
    return {2048.0f, is8MB ? 4096.0f : 1024.0f};
}

void PCSX::Widgets::RAMViewer::compileShader(GUI *gui) {
    auto status = m_editor.compile(gui, {"i_position", "i_texUV"});
    if (!status.isOk()) return;

    m_shaderProgram = m_editor.getProgram();

    m_locProjMtx = glGetUniformLocation(m_shaderProgram, "u_projMatrix");
    m_locVtxPos = glGetAttribLocation(m_shaderProgram, "i_position");
    m_locVtxUV = glGetAttribLocation(m_shaderProgram, "i_texUV");
    m_locRAMTexture = glGetUniformLocation(m_shaderProgram, "u_ramTexture");
    m_locReadHeatmap = glGetUniformLocation(m_shaderProgram, "u_readHeatmap");
    m_locWriteHeatmap = glGetUniformLocation(m_shaderProgram, "u_writeHeatmap");
    m_locExecHeatmap = glGetUniformLocation(m_shaderProgram, "u_execHeatmap");
    m_locReadColor = glGetUniformLocation(m_shaderProgram, "u_readColor");
    m_locWriteColor = glGetUniformLocation(m_shaderProgram, "u_writeColor");
    m_locExecColor = glGetUniformLocation(m_shaderProgram, "u_execColor");
    m_locCornerTL = glGetUniformLocation(m_shaderProgram, "u_cornerTL");
    m_locCornerBR = glGetUniformLocation(m_shaderProgram, "u_cornerBR");
    m_locResolution = glGetUniformLocation(m_shaderProgram, "u_resolution");
    m_locOrigin = glGetUniformLocation(m_shaderProgram, "u_origin");
    m_locMousePos = glGetUniformLocation(m_shaderProgram, "u_mousePos");
    m_locMouseUV = glGetUniformLocation(m_shaderProgram, "u_mouseUV");
    m_locHovered = glGetUniformLocation(m_shaderProgram, "u_hovered");
    m_locRAMHeight = glGetUniformLocation(m_shaderProgram, "u_ramHeight");
    m_locCurrentCycle = glGetUniformLocation(m_shaderProgram, "u_currentCycle");
    m_locDecayHalfLife = glGetUniformLocation(m_shaderProgram, "u_decayHalfLife");
    m_locShowRead = glGetUniformLocation(m_shaderProgram, "u_showRead");
    m_locShowWrite = glGetUniformLocation(m_shaderProgram, "u_showWrite");
    m_locShowExec = glGetUniformLocation(m_shaderProgram, "u_showExec");
    m_locPixelScale = glGetUniformLocation(m_shaderProgram, "u_pixelScale");
    m_locDrawGrid = glGetUniformLocation(m_shaderProgram, "u_drawGrid");
    m_locGridColor = glGetUniformLocation(m_shaderProgram, "u_gridColor");
    m_locFontAtlas = glGetUniformLocation(m_shaderProgram, "u_fontAtlas");
    m_locGlyphUVs = glGetUniformLocation(m_shaderProgram, "u_glyphUVs");
    m_locGlyphAspect = glGetUniformLocation(m_shaderProgram, "u_glyphAspect");
    m_locShowHex = glGetUniformLocation(m_shaderProgram, "u_showHex");
    m_locShowGreyscale = glGetUniformLocation(m_shaderProgram, "u_showGreyscale");
}

void PCSX::Widgets::RAMViewer::imguiCB(const ImDrawList *parentList, const ImDrawCmd *cmd) {
    if (!m_shaderProgram) return;

    GLint imguiProgramID;
    glGetIntegerv(GL_CURRENT_PROGRAM, &imguiProgramID);

    GLint projMatrixLocation = glGetUniformLocation(imguiProgramID, "ProjMtx");
    GLfloat currentProjection[4][4];
    glGetUniformfv(imguiProgramID, projMatrixLocation, &currentProjection[0][0]);

    glUseProgram(m_shaderProgram);

    bool is8MB = g_emulator->settings.get<Emulator::Setting8MB>();
    float ramHeight = is8MB ? 4096.0f : 1024.0f;
    uint32_t currentCycle = static_cast<uint32_t>(g_emulator->m_cpu->m_regs.cycle);

    auto *logger = g_emulator->m_ramLogger.get();

    glUniformMatrix4fv(m_locProjMtx, 1, GL_FALSE, &currentProjection[0][0]);
    glUniform2f(m_locCornerTL, m_cornerTL.x, m_cornerTL.y);
    glUniform2f(m_locCornerBR, m_cornerBR.x, m_cornerBR.y);
    glUniform2f(m_locResolution, m_resolution.x, m_resolution.y);
    glUniform2f(m_locOrigin, m_origin.x, m_origin.y);
    glUniform2f(m_locMousePos, m_mousePos.x, m_mousePos.y);
    glUniform2f(m_locMouseUV, m_mouseUV.x, m_mouseUV.y);
    glUniform1i(m_locHovered, m_hovered);
    glUniform1f(m_locRAMHeight, ramHeight);
    glUniform1ui(m_locCurrentCycle, currentCycle);
    glUniform1f(m_locDecayHalfLife, logger->m_decayHalfLife);
    glUniform1i(m_locShowRead, m_showRead);
    glUniform1i(m_locShowWrite, m_showWrite);
    glUniform1i(m_locShowExec, m_showExec);
    glUniform4f(m_locReadColor, m_readColor.x, m_readColor.y, m_readColor.z, m_readColor.w);
    glUniform4f(m_locWriteColor, m_writeColor.x, m_writeColor.y, m_writeColor.z, m_writeColor.w);
    glUniform4f(m_locExecColor, m_execColor.x, m_execColor.y, m_execColor.z, m_execColor.w);

    // Pixel scale: how many screen pixels per byte
    ImVec2 dimensions = (m_cornerBR - m_cornerTL) / m_DPI;
    ImVec2 pixelScale = dimensions / ImVec2(2048.0f, ramHeight);
    glUniform2f(m_locPixelScale, pixelScale.x, pixelScale.y);
    glUniform1i(m_locDrawGrid, m_drawGrid);
    glUniform4f(m_locGridColor, m_gridColor.x, m_gridColor.y, m_gridColor.z, m_gridColor.w);
    glUniform1i(m_locShowHex, m_showHex);
    glUniform1i(m_locShowGreyscale, m_showGreyscale);

    // Glyph UV rects + aspect are now pre-fetched in drawRAM() before the
    // ImGui render pass; consume the cached snapshot here so we don't grow
    // the font atlas mid-render and end up sampling with stale UVs.
    glUniform4fv(m_locGlyphUVs, 16, m_glyphUVs);
    glUniform1f(m_locGlyphAspect, m_glyphAspect);

    // Bind textures
    glUniform1i(m_locRAMTexture, 0);
    glUniform1i(m_locReadHeatmap, 1);
    glUniform1i(m_locWriteHeatmap, 2);
    glUniform1i(m_locExecHeatmap, 3);
    glUniform1i(m_locFontAtlas, 4);

    glActiveTexture(GL_TEXTURE0);
    logger->bindRAMTexture();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE1);
    logger->bindReadHeatmap();
    glActiveTexture(GL_TEXTURE2);
    logger->bindWriteHeatmap();
    glActiveTexture(GL_TEXTURE3);
    logger->bindExecHeatmap();
    glActiveTexture(GL_TEXTURE4);
    // Pre-fetched in drawRAM(); see m_fontTexID notes in ram-viewer.h.
    glBindTexture(GL_TEXTURE_2D, m_fontTexID);
    glActiveTexture(GL_TEXTURE0);

    // Set vertex attributes to match ImGui vertex layout
    glEnableVertexAttribArray(m_locVtxPos);
    glVertexAttribPointer(m_locVtxPos, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid *)offsetof(ImDrawVert, pos));
    glEnableVertexAttribArray(m_locVtxUV);
    glVertexAttribPointer(m_locVtxUV, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid *)offsetof(ImDrawVert, uv));
}

void PCSX::Widgets::RAMViewer::drawRAM(GUI *gui) {
    if (!m_shaderProgram) {
        compileShader(gui);
    }

    m_resolution = ImGui::GetContentRegionAvail();
    m_origin = ImGui::GetCursorScreenPos();
    m_mousePos = ImGui::GetIO().MousePos;

    // Pre-fetch hex-glyph UVs from ImFontBaked at the current font size BEFORE
    // ImGui renders. Doing this from inside the AddCallback render closure
    // could grow the atlas after the backend has already staged its texture
    // upload for the frame, leading the custom shader to sample with stale
    // UVs. The render callback only consumes the cached snapshot.
    {
        ImFont *font = ImGui::GetFont();
        ImFontBaked *baked = font ? font->GetFontBaked(ImGui::GetFontSize()) : nullptr;
        m_glyphAspect = 0.5f;  // default fallback if FindGlyph misses
        static const char hexChars[] = "0123456789ABCDEF";
        for (int i = 0; i < 16; i++) {
            const ImFontGlyph *g = baked ? baked->FindGlyph(hexChars[i]) : nullptr;
            if (g) {
                m_glyphUVs[i * 4 + 0] = g->U0;
                m_glyphUVs[i * 4 + 1] = g->V0;
                m_glyphUVs[i * 4 + 2] = g->U1;
                m_glyphUVs[i * 4 + 3] = g->V1;
                if (i == 0) {
                    float gw = g->X1 - g->X0;
                    float gh = g->Y1 - g->Y0;
                    if (gh > 0.0f) m_glyphAspect = gw / gh;
                }
            } else {
                m_glyphUVs[i * 4 + 0] = 0.0f;
                m_glyphUVs[i * 4 + 1] = 0.0f;
                m_glyphUVs[i * 4 + 2] = 0.0f;
                m_glyphUVs[i * 4 + 3] = 0.0f;
            }
        }
        m_fontTexID = (GLuint)(intptr_t)ImGui::GetIO().Fonts->TexRef.GetTexID();
    }

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    // The OpenGL backend binds its own sampler object, which overrides the texture's filter parameters.
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    drawList->AddCallback(
        [](const ImDrawList *parentList, const ImDrawCmd *cmd) {
            RAMViewer *that = reinterpret_cast<RAMViewer *>(cmd->UserCallbackData);
            that->imguiCB(parentList, cmd);
        },
        this);

    // Compute texture coordinates from corner positions
    ImVec2 dimensions = m_cornerBR - m_cornerTL;
    ImVec2 texTL = ImVec2(0.0f, 0.0f) - m_cornerTL / dimensions;
    ImVec2 texBR = ImVec2(1.0f, 1.0f) - (m_cornerBR - m_resolution) / dimensions;

    auto *logger = g_emulator->m_ramLogger.get();
    GLuint ramTexID = logger->getRAMTextureID();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::ImageButton("ram", (ImTextureID)(intptr_t)ramTexID, m_resolution, texTL, texBR);
    ImGui::PopStyleVar();

    bool hovered = m_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_None);
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    // Without sampler objects, the backend's filter mode survives the reset.
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

    const auto &io = ImGui::GetIO();

    ImVec2 texSpan = texBR - texTL;
    if (hovered) {
        m_mouseUV = texTL + texSpan * (m_mousePos - m_origin) / m_resolution;
    }

    if (!hovered) return;

    // Click: jump hex editor to this address
    if (clicked && !io.MouseDown[1]) {
        bool is8MB = g_emulator->settings.get<Emulator::Setting8MB>();
        float height = is8MB ? 4096.0f : 1024.0f;
        float pixelX = m_mouseUV.x * 2048.0f;
        float pixelY = m_mouseUV.y * height;
        uint32_t addr = uint32_t(pixelY) * 2048 + uint32_t(pixelX);
        uint32_t ramSize = is8MB ? 0x800000 : 0x200000;
        if (addr < ramSize) {
            g_system->m_eventBus->signal(PCSX::Events::GUI::JumpToMemory{0x80000000 + addr, 1, 0});
        }
    }

    handlePanZoom(io, dimensions);
}

void PCSX::Widgets::RAMViewer::draw(GUI *gui) {
    bool openReadColorPicker = false;
    bool openWriteColorPicker = false;
    bool openExecColorPicker = false;
    bool openGridColorPicker = false;

    auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar;
    if (ImGui::Begin(_("RAM Viewer"), &m_show, flags)) {
        m_DPI = ImGui::GetWindowDpiScale();
        if (!m_firstShown) {
            resetView();
            m_firstShown = true;
        }
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu(_("View"))) {
                if (ImGui::MenuItem(_("Reset view"))) resetView();
                ImGui::Separator();
                ImGui::MenuItem(_("Show reads"), nullptr, &m_showRead);
                ImGui::MenuItem(_("Show writes"), nullptr, &m_showWrite);
                ImGui::MenuItem(_("Show execution"), nullptr, &m_showExec);
                ImGui::MenuItem(_("Show greyscale"), nullptr, &m_showGreyscale);
                ImGui::Separator();
                ImGui::MenuItem(_("Show grid"), nullptr, &m_drawGrid);
                ImGui::MenuItem(_("Show hex values"), nullptr, &m_showHex);
                ImGui::MenuItem(_("Select grid color"), nullptr, &openGridColorPicker);
                ImGui::Separator();
                ImGui::MenuItem(_("Show Shader Editor"), nullptr, &m_editor.m_show);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Configuration"))) {
                ImGui::MenuItem(_("Select read color"), nullptr, &openReadColorPicker);
                ImGui::MenuItem(_("Select write color"), nullptr, &openWriteColorPicker);
                ImGui::MenuItem(_("Select exec color"), nullptr, &openExecColorPicker);
                ImGui::Separator();
                auto *logger = g_emulator->m_ramLogger.get();
                float halfLifeMs = logger->m_decayHalfLife / 33868.8f;
                if (ImGui::SliderFloat(_("Decay half-life (ms)"), &halfLifeMs, 10.0f, 10000.0f, "%.0f",
                                       ImGuiSliderFlags_Logarithmic)) {
                    logger->m_decayHalfLife = halfLifeMs * 33868.8f;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::Separator();

            // Show cursor info
            if (m_hovered) {
                bool is8MB = g_emulator->settings.get<Emulator::Setting8MB>();
                float height = is8MB ? 4096.0f : 1024.0f;
                float pixelX = m_mouseUV.x * 2048.0f;
                float pixelY = m_mouseUV.y * height;
                uint32_t addr = uint32_t(pixelY) * 2048 + uint32_t(pixelX);
                uint32_t ramSize = is8MB ? 0x800000 : 0x200000;
                if (addr < ramSize) {
                    uint8_t value = g_emulator->m_mem->m_wram[addr];
                    ImGui::Text("0x%08x: 0x%02x", 0x80000000 + addr, value);
                }
            }
            ImGui::EndMenuBar();
        }
        drawRAM(gui);
    }

    // Color pickers
    if (openReadColorPicker) ImGui::OpenPopup(_("Read Color Picker"));
    if (ImGui::BeginPopupModal(_("Read Color Picker"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker4(
            "##ReadColorPicker", (float *)&m_readColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
        if (ImGui::Button(_("OK"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openWriteColorPicker) ImGui::OpenPopup(_("Write Color Picker"));
    if (ImGui::BeginPopupModal(_("Write Color Picker"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker4(
            "##WriteColorPicker", (float *)&m_writeColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
        if (ImGui::Button(_("OK"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openExecColorPicker) ImGui::OpenPopup(_("Exec Color Picker"));
    if (ImGui::BeginPopupModal(_("Exec Color Picker"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker4(
            "##ExecColorPicker", (float *)&m_execColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
        if (ImGui::Button(_("OK"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openGridColorPicker) ImGui::OpenPopup(_("Grid Color Picker"));
    if (ImGui::BeginPopupModal(_("Grid Color Picker"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker4(
            "##GridColorPicker", (float *)&m_gridColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
        if (ImGui::Button(_("OK"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();

    if (m_editor.m_show) {
        bool changed = m_editor.draw(gui, _("RAM Shader Editor"));
        if (changed) compileShader(gui);
    }
}
