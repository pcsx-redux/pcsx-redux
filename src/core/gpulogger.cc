/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include "core/gpulogger.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "core/gpu.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "imgui/imgui.h"

namespace {

// Separating axis test between a triangle and an axis aligned rectangle. Logged::getVertices hands
// out triangles for everything, including lines and palette rows, so this single test covers every
// command's footprint. Degenerate triangles project down to a segment or a point on the edge
// normals, which the interval overlap below still handles correctly.
bool triangleIntersectsRect(PCSX::OpenGL::ivec2 v1, PCSX::OpenGL::ivec2 v2, PCSX::OpenGL::ivec2 v3, int rx, int ry,
                            int rw, int rh) {
    const int rx2 = rx + rw;
    const int ry2 = ry + rh;
    const int tx[3] = {v1.x(), v2.x(), v3.x()};
    const int ty[3] = {v1.y(), v2.y(), v3.y()};

    if (std::max({tx[0], tx[1], tx[2]}) < rx || std::min({tx[0], tx[1], tx[2]}) > rx2) return false;
    if (std::max({ty[0], ty[1], ty[2]}) < ry || std::min({ty[0], ty[1], ty[2]}) > ry2) return false;

    for (unsigned i = 0; i < 3; i++) {
        const unsigned j = (i + 1) % 3;
        const int nx = ty[j] - ty[i];
        const int ny = tx[i] - tx[j];
        if ((nx == 0) && (ny == 0)) continue;
        int triMin = std::numeric_limits<int>::max();
        int triMax = std::numeric_limits<int>::min();
        for (unsigned k = 0; k < 3; k++) {
            const int p = nx * tx[k] + ny * ty[k];
            triMin = std::min(triMin, p);
            triMax = std::max(triMax, p);
        }
        int rectMin = std::numeric_limits<int>::max();
        int rectMax = std::numeric_limits<int>::min();
        for (unsigned k = 0; k < 4; k++) {
            const int p = nx * ((k & 1) ? rx2 : rx) + ny * ((k & 2) ? ry2 : ry);
            rectMin = std::min(rectMin, p);
            rectMax = std::max(rectMax, p);
        }
        if ((triMax < rectMin) || (rectMax < triMin)) return false;
    }

    return true;
}

}  // namespace

static const char* const c_vtx = R"(
#version 330 core

// inPos: The vertex position.

layout (location = 0) in ivec2 inPos;

// The PS1 tests coverage at the integer pixel coordinate, top and left edges inclusive. OpenGL samples at the
// pixel center, so shift the geometry by just under half a pixel: each pixel is then sampled a hair to the
// right of and below its integer corner, which keeps the inclusive edges and drops the right and bottom ones.
const vec2 vertexOffsets = vec2(0.5 - 1.0 / 64.0, 0.5 - 1.0 / 64.0);

void main() {
    // Normalize coords to [0, 2]
    float x = float(inPos.x);
    float y = float(inPos.y);
    float xx = (x + vertexOffsets.x) / 512.0;
    float yy = (y + vertexOffsets.y) / 256.0;

    // Normalize to [-1, 1]
    xx -= 1.0;
    yy -= 1.0;

    gl_Position = vec4(xx, yy, 1.0, 1.0);
}
)";

static const char* const c_frg = R"(
#version 330 core

out vec4 outColor;

void main() {
    outColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
}
)";

PCSX::GPULogger::GPULogger(bool followEmulator) : m_listener(g_system->m_eventBus), m_followEmulator(followEmulator) {
    if (!followEmulator) return;
    m_listener.listen<Events::GPU::VSync>([this](auto event) {
        setFrame(m_frameCounter + 1);
        const unsigned rate = g_emulator->settings.get<Emulator::SettingVideo>() == Emulator::PSX_TYPE_PAL ? 50 : 60;
        if (m_drawEnvFrameThisVSync) {
            m_drawEnvFrameThisVSync = false;
            m_drawEnvFrames++;
        }
        if (++m_fpsVSyncs >= rate) {
            const bool sameBuffer = m_displayStartChanges == 0 && m_interlaced480;
            const unsigned frames = sameBuffer ? m_drawEnvFrames : m_displayStartChanges;
            m_guestFPS = float(frames) * rate / m_fpsVSyncs;
            m_displayStartChanges = 0;
            m_drawEnvFrames = 0;
            m_fpsVSyncs = 0;
        }
        if (m_breakOnVSync) {
            g_system->pause();
        }
    });
}

void PCSX::GPULogger::enable() {
    GLint textureUnits;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textureUnits);
    if (textureUnits < 5) return;

    if (!m_vbo.exists()) {
        m_vbo.createFixedSize(sizeof(OpenGL::ivec2) * m_vertices.size(), GL_STREAM_DRAW);
    }
    m_vbo.bind();
    if (!m_vao.exists()) {
        m_vao.create();
    }
    m_vao.bind();
    m_vao.setAttributeInt<GLint>(0, 2, sizeof(OpenGL::ivec2), size_t(0));
    m_vao.enableAttribute(0);

    if (!m_program.exists()) {
        m_writtenHeatmapTex.create(1024, 512, GL_R32F);
        m_readHeatmapTex.create(1024, 512, GL_R32F);
        m_writtenHighlightTex.create(1024, 512, GL_R32F);
        m_readHighlightTex.create(1024, 512, GL_R32F);
        m_writtenAccumTex.create(1024, 512, GL_R32F);
        m_readAccumTex.create(1024, 512, GL_R32F);
        if (!m_writtenAccumTex.exists()) return;
        if (!m_readAccumTex.exists()) return;
        m_writtenAccumFB.createWithTexture(m_writtenAccumTex);
        m_readAccumFB.createWithTexture(m_readAccumTex);
        if (!m_writtenAccumFB.exists()) return;
        if (!m_readAccumFB.exists()) return;
        if (!m_writtenHeatmapTex.exists()) return;
        if (!m_readHeatmapTex.exists()) return;
        if (!m_writtenHighlightTex.exists()) return;
        if (!m_readHighlightTex.exists()) return;
        m_writtenHeatmapFB.createWithTexture(m_writtenHeatmapTex);
        m_readHeatmapFB.createWithTexture(m_readHeatmapTex);
        m_writtenHighlightFB.createWithTexture(m_writtenHighlightTex);
        m_readHighlightFB.createWithTexture(m_readHighlightTex);
        if (!m_writtenHeatmapFB.exists()) return;
        if (!m_readHeatmapFB.exists()) return;
        if (!m_writtenHighlightFB.exists()) return;
        if (!m_readHighlightFB.exists()) return;

        OpenGL::Shader vs, ps;
        auto status = vs.create(c_vtx, OpenGL::Vertex);
        if (!status.isOk()) return;
        status = ps.create(c_frg, OpenGL::Fragment);
        if (!status.isOk()) return;

        status = m_program.create({vs, ps});
        if (!status.isOk()) return;
    }

    m_hasFramebuffers = true;
}

void PCSX::GPULogger::disable() {
    m_hasFramebuffers = false;
    m_vram.reset();
}

void PCSX::GPULogger::addTri(OpenGL::ivec2& v1, OpenGL::ivec2& v2, OpenGL::ivec2& v3) {
    if ((m_verticesCount + 3) >= m_vertices.size()) flush();
    m_vertices[m_verticesCount++] = v1;
    m_vertices[m_verticesCount++] = v2;
    m_vertices[m_verticesCount++] = v3;
}

void PCSX::GPULogger::flush() {
    if (m_verticesCount == 0) return;
    m_vbo.bufferVertsSub(&m_vertices[0], m_verticesCount);
    OpenGL::draw(OpenGL::Triangles, m_verticesCount);
    m_verticesCount = 0;
}

void PCSX::GPULogger::checkVramBreakpoints(GPU::Logged* node) {
    bool watchingReads = false;
    bool watchingWrites = false;
    for (auto& bp : m_vramBreakpoints) {
        if (!bp.enabled) continue;
        watchingReads |= bp.onRead;
        watchingWrites |= bp.onWrite;
    }
    if (!watchingReads && !watchingWrites) return;

    unsigned hit = m_vramBreakpoints.size();
    auto scan = [this, node, &hit](GPU::Logged::PixelOp op) {
        node->getVertices(
            [this, &hit, op](auto v1, auto v2, auto v3) {
                if (hit != m_vramBreakpoints.size()) return;
                for (unsigned i = 0; i < m_vramBreakpoints.size(); i++) {
                    auto& bp = m_vramBreakpoints[i];
                    if (!bp.enabled) continue;
                    if (!(op == GPU::Logged::PixelOp::READ ? bp.onRead : bp.onWrite)) continue;
                    if (triangleIntersectsRect(v1, v2, v3, bp.area.x, bp.area.y, bp.area.w, bp.area.h)) {
                        hit = i;
                        return;
                    }
                }
            },
            op);
    };

    const char* what = _("write");
    if (watchingWrites) scan(GPU::Logged::PixelOp::WRITE);
    if ((hit == m_vramBreakpoints.size()) && watchingReads) {
        what = _("read");
        scan(GPU::Logged::PixelOp::READ);
    }
    if (hit == m_vramBreakpoints.size()) return;

    const auto& bp = m_vramBreakpoints[hit];
    const auto name = node->getName();
    node->highlight = true;
    g_system->log(LogClass::GPU,
                  _("VRAM breakpoint %i triggered: %.*s did a %s intersecting %ix%i at %i,%i - PC = 0x%08x\n"), hit,
                  int(name.size()), name.data(), what, bp.area.w, bp.area.h, bp.area.x, bp.area.y, node->pc);
    if (!m_followEmulator) return;
    g_system->m_eventBus->signal(Events::GUI::JumpToPC{node->pc});
    g_system->pause();
}

// The list shows the frame that just completed. Anything older goes when the next frame starts,
// so a frame that sent no commands shows as empty.
void PCSX::GPULogger::setFrame(uint64_t frame) {
    // Read the counts back at the end of the frame, not when the next frame starts drawing, so the
    // last frame before the GPU goes idle is still reported.
    if (m_overdrawStats && m_overdrawDirty && m_hasFramebuffers) {
        GLint oldReadFBO;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFBO);
        readOverdraw(m_writtenAccumFB, m_writtenOverdraw, m_writtenOverdrawPixels);
        readOverdraw(m_readAccumFB, m_readOverdraw, m_readOverdrawPixels);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFBO);
        m_hasOverdrawStats = true;
    } else if (m_overdrawStats && m_hasOverdrawStats) {
        // Nothing was drawn this frame.
        m_writtenOverdraw = {};
        m_readOverdraw = {};
        m_writtenOverdrawPixels.clear();
        m_readOverdrawPixels.clear();
    }
    m_overdrawDirty = false;
    m_frameCounter = frame;
    if (frame > 0) dropFramesBefore(frame - 1);
}

void PCSX::GPULogger::dropFramesBefore(uint64_t frame) {
    bool dropped = false;
    for (auto i = m_list.begin(); (i != m_list.end()) && (i->frame < frame); i = m_list.begin()) {
        delete &*i;
        dropped = true;
    }
    if (!dropped) return;
    // The next command starts a new frame for the heatmaps even though nothing is left to compare it to.
    m_pendingNewFrame = true;
    // An empty frame replays from the VRAM it started with, not from the snapshot of the dropped frame.
    if (m_list.empty()) startNewFrame();
}

void PCSX::GPULogger::addNodeInternal(GPU::Logged* node, GPU::Logged::Origin origin, uint32_t value, uint32_t length,
                                      bool clipped) {
    auto frame = m_frameCounter;

    bool gotNewFrame = std::exchange(m_pendingNewFrame, false);
    for (auto i = m_list.begin(); (i != m_list.end()) && (i->frame != frame); i = m_list.begin()) {
        delete &*i;
        gotNewFrame = true;
    }
    // An empty list is a cleared log, which has no snapshot of its own frame yet.
    if (gotNewFrame || m_list.empty()) startNewFrame();

    node->origin = origin;
    node->value = value;
    node->length = length;
    node->pc = m_followEmulator ? g_emulator->m_cpu->m_regs.pc : 0;
    node->frame = frame;
    node->generateStatsInfo();
    m_list.push_back(node);
    checkVramBreakpoints(node);

    if (!m_hasFramebuffers) return;
    m_overdrawDirty = true;

    const auto oldFBO = OpenGL::getDrawFramebuffer();

    m_vbo.bind();
    m_vao.bind();
    m_program.use();
    OpenGL::disableScissor();

    // The heatmaps count how many times each pixel is touched during the current frame.
    if (gotNewFrame) {
        GLint oldReadFBO;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFBO);
        m_writtenAccumFB.bind(OpenGL::ReadFramebuffer);
        m_writtenHeatmapFB.bind(OpenGL::DrawFramebuffer);
        glBlitFramebuffer(0, 0, 1024, 512, 0, 0, 1024, 512, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        m_readAccumFB.bind(OpenGL::ReadFramebuffer);
        m_readHeatmapFB.bind(OpenGL::DrawFramebuffer);
        glBlitFramebuffer(0, 0, 1024, 512, 0, 0, 1024, 512, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFBO);
        OpenGL::setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        m_writtenAccumFB.bind(OpenGL::DrawFramebuffer);
        OpenGL::clearColor();
        m_readAccumFB.bind(OpenGL::DrawFramebuffer);
        OpenGL::clearColor();
    }

    const bool blendWasEnabled = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);

    OpenGL::setViewport(m_writtenAccumTex.width(), m_writtenAccumTex.height());
    m_writtenAccumFB.bind(OpenGL::DrawFramebuffer);
    if (clipped) {
        int w = int(m_drawingAreaX1) - int(m_drawingAreaX0) + 1;
        int h = int(m_drawingAreaY1) - int(m_drawingAreaY0) + 1;
        OpenGL::enableScissor();
        OpenGL::setScissor(m_drawingAreaX0, m_drawingAreaY0, std::max(w, 0), std::max(h, 0));
    }
    node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::WRITE);
    flush();
    OpenGL::disableScissor();

    OpenGL::setViewport(m_readAccumTex.width(), m_readAccumTex.height());
    m_readAccumFB.bind(OpenGL::DrawFramebuffer);
    node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::READ);
    flush();

    if (!blendWasEnabled) glDisable(GL_BLEND);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldFBO);
    g_emulator->m_gpu->setOpenGLContext();
}

void PCSX::GPULogger::readOverdraw(OpenGL::Framebuffer& fb, OverdrawStats& stats, std::vector<float>& pixels) {
    pixels.resize(1024 * 512);
    fb.bind(OpenGL::ReadFramebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, 1024, 512, GL_RED, GL_FLOAT, pixels.data());
    stats = {};
    for (float f : pixels) {
        unsigned count = unsigned(f + 0.5f);
        if (count == 0) continue;
        stats.total += count;
        stats.pixels++;
        if (count > stats.max) stats.max = count;
        unsigned bucket = count == 1 ? 0 : count == 2 ? 1 : count <= 4 ? 2 : count <= 8 ? 3 : 4;
        stats.buckets[bucket]++;
    }
}

uint64_t PCSX::GPULogger::getOverdrawSum(bool read, int x, int y, int w, int h) const {
    auto& pixels = read ? m_readOverdrawPixels : m_writtenOverdrawPixels;
    if (pixels.size() != 1024 * 512) return 0;
    int x0 = std::max(x, 0), y0 = std::max(y, 0);
    int x1 = int(std::min<int64_t>(int64_t(x) + w, 1024)), y1 = int(std::min<int64_t>(int64_t(y) + h, 512));
    uint64_t sum = 0;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) sum += unsigned(pixels[py * 1024 + px] + 0.5f);
    }
    return sum;
}

PCSX::GPU* PCSX::GPULogger::gpu() const { return m_owner ? m_owner : g_emulator->m_gpu.get(); }

void PCSX::GPULogger::startNewFrame() { m_vram = gpu()->getVRAM(GPU::Ownership::ACQUIRE); }

void PCSX::GPULogger::replay(GPU* gpu) {
    if (m_vram.data()) gpu->partialUpdateVRAM(0, 0, 1024, 512, m_vram.data<uint16_t>());
    for (auto& node : m_list) {
        if (node.enabled) node.execute(gpu);
    }
    gpu->vblank(true);
}

void PCSX::GPULogger::highlight(GPU::Logged* node, bool only) {
    if (!m_hasFramebuffers) return;

    const auto oldFBO = OpenGL::getDrawFramebuffer();

    m_vbo.bind();
    m_vao.bind();
    m_program.use();
    OpenGL::disableScissor();

    OpenGL::setViewport(m_writtenHighlightTex.width(), m_writtenHighlightTex.height());
    m_writtenHighlightFB.bind(OpenGL::DrawFramebuffer);
    OpenGL::setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    OpenGL::clearColor();
    if (node) {
        node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::WRITE);
    }
    if (!only) {
        for (auto& node : m_list) {
            if (node.highlight) {
                node.getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); },
                                 GPU::Logged::PixelOp::WRITE);
            }
        }
    }
    flush();

    OpenGL::setViewport(m_readHighlightTex.width(), m_readHighlightTex.height());
    m_readHighlightFB.bind(OpenGL::DrawFramebuffer);
    OpenGL::setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    OpenGL::clearColor();
    if (node) {
        node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::READ);
    }
    if (!only) {
        for (auto& node : m_list) {
            if (node.highlight) {
                node.getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::READ);
            }
        }
    }
    flush();

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldFBO);
    g_emulator->m_gpu->setOpenGLContext();
}

PCSX::GPU::CtrlDisplayMode::CtrlDisplayMode(uint32_t value) {
    if ((value >> 6) & 1) {
        switch (value & 3) {
            case 0:
                hres = HR_368;
                break;
            case 1:
                hres = HR_384;
                break;
            case 2:
                hres = HR_512;
                break;
            case 3:
                hres = HR_640;
                break;
        }
    } else {
        hres = magic_enum::enum_cast<decltype(hres)>(value & 3).value();
    }
    vres = magic_enum::enum_cast<decltype(vres)>((value >> 2) & 1).value();
    mode = magic_enum::enum_cast<decltype(mode)>((value >> 3) & 1).value();
    depth = magic_enum::enum_cast<decltype(depth)>((value >> 4) & 1).value();
    interlace = (value >> 5) & 1;
    widthRaw = ((value >> 6) & 1) | ((value & 3) << 1);
}

void PCSX::GPU::ClearCache::drawLogNode(unsigned, const DrawLogSettings&) {}

void PCSX::GPU::FastFill::drawLogNode(unsigned itemIndex, const DrawLogSettings& settings) {
    drawColorBox(color, itemIndex, 0, settings);
    ImGui::Separator();
    ImGui::Text("  X0: %i, Y0: %i", x, y);
    ImGui::Text("  X1: %i, Y1: %i", x + w, y + h);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::BlitVramVram::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  From X: %i, Y: %i", sX, sY);
    ImGui::Text("  To X: %i, Y: %i", dX, dY);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::BlitRamVram::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X: %i, Y: %i", x, y);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::BlitVramRam::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X: %i, Y: %i", x, y);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::TPage::drawLogNodeCommon() {
    ImGui::Text(_("Texture Page X: %i, Texture Page Y: %i"), tx, ty);
    ImGui::TextUnformatted(_("Blending:"));
    ImGui::SameLine();
    switch (blendFunction) {
        case BlendFunction::HalfBackAndHalfFront:
            ImGui::TextUnformatted(_("50% Back + 50% Front"));
            break;
        case BlendFunction::FullBackAndFullFront:
            ImGui::TextUnformatted(_("100% Back + 100% Front"));
            break;
        case BlendFunction::FullBackSubFullFront:
            ImGui::TextUnformatted(_("100% Back - 100% Front"));
            break;
        case BlendFunction::FullBackAndQuarterFront:
            ImGui::TextUnformatted(_("100% Back + 25% Front"));
            break;
    }
    ImGui::TextUnformatted(_("Texture depth:"));
    ImGui::SameLine();
    switch (texDepth) {
        case TexDepth::Tex4Bits:
            ImGui::TextUnformatted(_("4 bits"));
            break;
        case TexDepth::Tex8Bits:
            ImGui::TextUnformatted(_("8 bits"));
            break;
        case TexDepth::Tex16Bits:
            ImGui::TextUnformatted(_("16 bits"));
            break;
    }
}

void PCSX::GPU::TPage::drawLogNode(unsigned, const DrawLogSettings&) {
    drawLogNodeCommon();
    ImGui::Text(_("Dithering: %s"), dither ? _("Yes") : _("No"));
}

void PCSX::GPU::TWindow::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X: %i, Y: %i", x, y);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::DrawingAreaStart::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }

void PCSX::GPU::DrawingAreaEnd::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }

void PCSX::GPU::DrawingOffset::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }

void PCSX::GPU::MaskBit::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  %s",
                fmt::format(f_("Set: {}, Check: {}"), set ? _("Yes") : _("No"), check ? _("Yes") : _("No")).c_str());
}

void PCSX::GPU::CtrlReset::drawLogNode(unsigned, const DrawLogSettings&) {}
void PCSX::GPU::CtrlClearFifo::drawLogNode(unsigned, const DrawLogSettings&) {}
void PCSX::GPU::CtrlIrqAck::drawLogNode(unsigned, const DrawLogSettings&) {}

void PCSX::GPU::CtrlDisplayEnable::drawLogNode(unsigned, const DrawLogSettings&) {
    if (enable) {
        ImGui::TextUnformatted(_("Display Enabled"));
    } else {
        ImGui::TextUnformatted(_("Display Disabled"));
    }
}

void PCSX::GPU::CtrlDmaSetting::drawLogNode(unsigned, const DrawLogSettings&) {
    switch (dma) {
        case Dma::Off:
            ImGui::TextUnformatted(_("DMA Off"));
            break;
        case Dma::FifoQuery:
            ImGui::TextUnformatted(_("FIFO Query"));
            break;
        case Dma::Read:
            ImGui::TextUnformatted(_("DMA Read"));
            break;
        case Dma::Write:
            ImGui::TextUnformatted(_("DMA Write"));
            break;
    }
}

void PCSX::GPU::CtrlDisplayStart::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }
void PCSX::GPU::CtrlHorizontalDisplayRange::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X0: %i, X1: %i", x0, x1);
}
void PCSX::GPU::CtrlVerticalDisplayRange::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  Y0: %i, Y1: %i", y0, y1);
}

void PCSX::GPU::CtrlDisplayMode::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::TextUnformatted(_("Horizontal resolution:"));
    ImGui::SameLine();
    switch (hres) {
        case HR_256:
            ImGui::TextUnformatted("256 pixels");
            break;
        case HR_320:
            ImGui::TextUnformatted("320 pixels");
            break;
        case HR_512:
            ImGui::TextUnformatted("512 pixels");
            break;
        case HR_640:
            ImGui::TextUnformatted("640 pixels");
            break;
        case HR_368:
            ImGui::TextUnformatted("368 pixels");
            break;
        case HR_384:
            ImGui::TextUnformatted("384 pixels");
            break;
    }
    ImGui::Text(_("Extended width mode: %s"), widthRaw & 1 ? _("Yes") : _("No"));
    ImGui::TextUnformatted(_("Vertical resolution:"));
    ImGui::SameLine();
    switch (vres) {
        case VR_240:
            ImGui::TextUnformatted("240 pixels");
            break;
        case VR_480:
            ImGui::TextUnformatted("480 pixels");
            break;
    }
    ImGui::Text(_("Output mode: %s"), mode == VM_NTSC ? "NTSC" : "PAL");
    ImGui::Text(_("Display depth: %s"), depth == CD_15BITS ? _("15 bits") : _("24 bits"));
    ImGui::Text(_("Interlaced: %s"), interlace ? _("Yes") : _("No"));
}

void PCSX::GPU::CtrlQuery::drawLogNode(unsigned, const DrawLogSettings&) {
    switch (type()) {
        case QueryType::TextureWindow:
            ImGui::TextUnformatted(_("Texture Window"));
            break;
        case QueryType::DrawAreaStart:
            ImGui::TextUnformatted(_("Draw Area Start"));
            break;
        case QueryType::DrawAreaEnd:
            ImGui::TextUnformatted(_("Draw Area End"));
            break;
        case QueryType::DrawOffset:
            ImGui::TextUnformatted(_("Draw Offset"));
            break;
        default:
            ImGui::TextUnformatted(_("Unknown"));
            break;
    }
}

void PCSX::GPU::FastFill::cumulateStats(GPUStats* stats) { stats->pixelWrites += h * w; }
void PCSX::GPU::BlitVramVram::cumulateStats(GPUStats* stats) {
    auto s = h * w;
    stats->pixelWrites += s;
    stats->pixelReads += s;
}
void PCSX::GPU::BlitRamVram::cumulateStats(GPUStats* stats) { stats->pixelWrites += h * w; }
void PCSX::GPU::BlitVramRam::cumulateStats(GPUStats* stats) { stats->pixelReads += h * w; }

void PCSX::GPU::FastFill::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::READ) return;
    add({int(x), int(y)}, {int(x + w), int(y)}, {int(x + w), int(y + h)});
    add({int(x + w), int(y + h)}, {int(x), int(y + h)}, {int(x), int(y)});
}
void PCSX::GPU::BlitVramVram::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::READ) {
        add({int(sX), int(sY)}, {int(sX + w), int(sY)}, {int(sX + w), int(sY + h)});
        add({int(sX + w), int(sY + h)}, {int(sX), int(sY + h)}, {int(sX), int(sY)});
    } else if (op == PixelOp::WRITE) {
        add({int(dX), int(dY)}, {int(dX + w), int(dY)}, {int(dX + w), int(dY + h)});
        add({int(dX + w), int(dY + h)}, {int(dX), int(dY + h)}, {int(dX), int(dY)});
    }
}
void PCSX::GPU::BlitRamVram::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::READ) return;
    add({int(x), int(y)}, {int(x + w), int(y)}, {int(x + w), int(y + h)});
    add({int(x + w), int(y + h)}, {int(x), int(y + h)}, {int(x), int(y)});
}
void PCSX::GPU::BlitVramRam::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::WRITE) return;
    add({int(x), int(y)}, {int(x + w), int(y)}, {int(x + w), int(y + h)});
    add({int(x + w), int(y + h)}, {int(x), int(y + h)}, {int(x), int(y)});
}

void PCSX::GPU::Logged::addLine(AddTri&& add, int x1, int y1, int x2, int y2) {
    const int32_t dx = x2 - x1;
    const int32_t dy = y2 - y1;

    const auto absDx = std::abs(dx);
    const auto absDy = std::abs(dy);

    // Both vertices coincide, render 1x1 rectangle with the colour and coords of v1
    if (dx == 0 && dy == 0) {
        add({x1, y1}, {x1 + 1, y1}, {x1 + 1, y1 + 1});
        add({x1 + 1, y1 + 1}, {x1, y1 + 1}, {x1, y1});
    } else {
        int xOffset, yOffset;
        if (absDx > absDy) {  // x-major line
            xOffset = 0;
            yOffset = 1;

            // Align line depending on whether dx is positive or not
            dx > 0 ? x2++ : x1++;
        } else {  // y-major line
            xOffset = 1;
            yOffset = 0;

            // Align line depending on whether dy is positive or not
            dy > 0 ? y2++ : y1++;
        }

        add({x1, y1}, {x2, y2}, {x2 + xOffset, y2 + yOffset});
        add({x2 + xOffset, y2 + yOffset}, {x1 + xOffset, y1 + yOffset}, {x1, y1});
    }
}
