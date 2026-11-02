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

#include <algorithm>
#include <cstdint>

#include "core/debug.h"
#include "core/psxemulator.h"
#include "gpu/soft/async.h"
#include "gpu/soft/interface.h"
#include "gpu/soft/pixel-writer.h"
#include "gpu/soft/soft.h"
#include "gui/gui.h"
#include "imgui.h"
#include "support/imgui-helpers.h"
#include "tracy/Tracy.hpp"

// GP1 status-register bit definitions. Names are protocol-visible and
// must stay stable - logs and debugger expressions read them out.
static constexpr uint32_t GPUSTATUS_DMABITS = 0x60000000;
static constexpr uint32_t GPUSTATUS_READYFORCOMMANDS = 0x10000000;
static constexpr uint32_t GPUSTATUS_IDLE = 0x04000000;
static constexpr uint32_t GPUSTATUS_DISPLAYDISABLED = 0x00800000;
static constexpr uint32_t GPUSTATUS_INTERLACED = 0x00400000;
static constexpr uint32_t GPUSTATUS_RGB24 = 0x00200000;
static constexpr uint32_t GPUSTATUS_PAL = 0x00100000;
static constexpr uint32_t GPUSTATUS_DOUBLEHEIGHT = 0x00080000;
static constexpr uint32_t GPUSTATUS_WIDTHBITS = 0x00070000;

int32_t PCSX::SoftGPU::impl::initBackend(UI *ui) {
    m_ui = ui;
    m_doVSyncUpdate = true;
    initDisplay();

    // VRAM is always allocated at the full 2MB (1024x1024) size; the bank gate
    // and fitment decide addressing semantics, not physical size. The extra 1MB
    // is a security guard for soft drawing-func overruns.
    m_allocatedVRAM = new uint8_t[1024 * 1024 * 2 + (1024 * 1024)]();
    if (!m_allocatedVRAM) return -1;

    //!!! ATTENTION !!!
    m_vram = m_allocatedVRAM + 512 * 1024;  // security offset into double sized psx vram!
    m_vram16 = (uint16_t *)m_vram;

    // 1MB-fitment open-bus model: on a retail board the upper 1MB chip is
    // unpopulated, so an opened upper bank (GP1(09h).1 on 1MB) reads back the
    // floating bus - measured constant 0xFFFF, data-independent. Prefill the
    // upper half (Y >= 512) with that value so reads need NO per-access branch
    // on the hot path (scanout + texture sampling just read the buffer);
    // upper-bank writes are dropped by the vramOpenBus() guards, keeping the
    // region pristine. Under 2MB fitment the upper bank is real RAM (left
    // zeroed by the allocation above).
    if (!vram2MBFitted()) {
        for (size_t i = 512u * 1024u; i < 1024u * 1024u; i++) m_vram16[i] = kVramOpenBusValue;
    }

    // Set the renderer's effective drawable height from the current fitment/gate.
    vramConfigChanged();

    m_softDisplay.RGB24 = false;  // init some stuff
    m_softDisplay.Interlaced = false;
    m_softDisplay.DrawOffset.x = 0;
    m_softDisplay.DrawOffset.y = 0;
    m_softDisplay.DisplayMode.x = 320;
    m_softDisplay.DisplayMode.y = 240;
    m_previousDisplay.DisplayMode.x = 320;
    m_previousDisplay.DisplayMode.y = 240;
    m_softDisplay.Disabled = false;
    m_previousDisplay.Range.x0 = 0;
    m_previousDisplay.Range.y0 = 0;
    m_softDisplay.Range.x0 = 0;
    m_softDisplay.Range.x1 = 0;
    m_previousDisplay.DisplayModeNew.y = 0;
    m_softDisplay.Double = 1;

    // device initialised already !
    m_statusRet = 0x14802000;
    m_statusRet |= GPUSTATUS_IDLE;
    m_statusRet |= GPUSTATUS_READYFORCOMMANDS;

    setThreaded(g_emulator->settings.get<Emulator::SettingSoftGPUThreaded>());

    return 0;
}

void PCSX::SoftGPU::impl::setThreaded(bool threaded) {
    // Either way every op pushed so far runs; still, have the next drawing primitive forward its parity.
    m_pushedSkipParity = s_skipParityUnknown;
    if (threaded) {
        m_queue.start(*this);
    } else {
        m_queue.stop();
    }
}

int32_t PCSX::SoftGPU::impl::shutdown() {
    m_queue.stop();
    delete[] m_allocatedVRAM;
    return 0;
}

std::unique_ptr<PCSX::GPU> PCSX::GPU::getSoft() { return std::unique_ptr<PCSX::GPU>(new PCSX::SoftGPU::impl()); }

void PCSX::SoftGPU::impl::updateDisplay(bool fromGui) {
    GUI *gui = dynamic_cast<GUI *>(m_ui);
    if (!gui) return;
    if (m_softDisplay.Disabled) {
        glClearColor(1, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    doBufferSwap(fromGui);
}

////////////////////////////////////////////////////////////////////////
// roughly emulated screen centering bits... not complete !!!
////////////////////////////////////////////////////////////////////////

void PCSX::SoftGPU::impl::changeDispOffsetsX() {
    if (!m_softDisplay.Range.x1) return;

    int32_t l = m_previousDisplay.DisplayMode.x;

    l *= (int32_t)m_softDisplay.Range.x1;
    l /= 2560;
    int32_t lx = l;
    l &= 0xfffffff8;

    if (l == m_previousDisplay.Range.y1) return;  // abusing range.y1 for
    m_previousDisplay.Range.y1 = (int16_t)l;      // storing last x range and test

    if (lx >= m_previousDisplay.DisplayMode.x) {
        m_previousDisplay.Range.x1 = (int16_t)m_previousDisplay.DisplayMode.x;
        m_previousDisplay.Range.x0 = 0;
    } else {
        m_previousDisplay.Range.x1 = (int16_t)l;

        m_previousDisplay.Range.x0 = (m_softDisplay.Range.x0 - 500) / 8;

        if (m_previousDisplay.Range.x0 < 0) m_previousDisplay.Range.x0 = 0;

        if ((m_previousDisplay.Range.x0 + lx) > m_previousDisplay.DisplayMode.x) {
            m_previousDisplay.Range.x0 = (int16_t)(m_previousDisplay.DisplayMode.x - lx);
            m_previousDisplay.Range.x0 += 2;  //???

            m_previousDisplay.Range.x1 += (int16_t)(lx - l);
        }
        GUI *gui = dynamic_cast<GUI *>(m_ui);
        if (gui) {
            glClearColor(1, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }

    m_doVSyncUpdate = true;
}

void PCSX::SoftGPU::impl::changeDispOffsetsY() {
    int iT, iO = m_previousDisplay.Range.y0;
    int iOldYOffset = m_previousDisplay.DisplayModeNew.y;
    const int scanoutHeight = vram2MBGateOpen() ? VRAM_HEIGHT * 2 : VRAM_HEIGHT;

    if ((m_previousDisplay.DisplayModeNew.x + m_softDisplay.DisplayModeNew.y) > scanoutHeight) {
        int dy1 = scanoutHeight - m_previousDisplay.DisplayModeNew.x;
        int dy2 = (m_previousDisplay.DisplayModeNew.x + m_softDisplay.DisplayModeNew.y) - scanoutHeight;

        if (dy1 >= dy2) {
            m_previousDisplay.DisplayModeNew.y = -dy2;
        } else {
            m_softDisplay.DisplayPosition.y = 0;
            m_previousDisplay.DisplayModeNew.y = -dy1;
        }
    } else {
        m_previousDisplay.DisplayModeNew.y = 0;
    }

    if (m_previousDisplay.DisplayModeNew.y != iOldYOffset) {
        // if old offset!=new offset: recalc height
        m_softDisplay.Height = m_softDisplay.Range.y1 - m_softDisplay.Range.y0 + m_previousDisplay.DisplayModeNew.y;
        m_softDisplay.DisplayModeNew.y = m_softDisplay.Height * m_softDisplay.Double;
    }

    if (m_softDisplay.PAL) {
        iT = 48;
    } else {
        iT = 28;
    }

    if (m_softDisplay.Range.y0 >= iT) {
        m_previousDisplay.Range.y0 = (int16_t)((m_softDisplay.Range.y0 - iT - 4) * m_softDisplay.Double);
        if (m_previousDisplay.Range.y0 < 0) m_previousDisplay.Range.y0 = 0;
        m_softDisplay.DisplayModeNew.y += m_previousDisplay.Range.y0;
    } else {
        m_previousDisplay.Range.y0 = 0;
    }

    if (iO != m_previousDisplay.Range.y0) {
        GUI *gui = dynamic_cast<GUI *>(m_ui);
        if (gui) {
            glClearColor(1, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
}

////////////////////////////////////////////////////////////////////////
// check if update needed
////////////////////////////////////////////////////////////////////////

void PCSX::SoftGPU::impl::updateDisplayIfChanged() {
    if ((m_softDisplay.DisplayMode.y == m_softDisplay.DisplayModeNew.y) &&
        (m_softDisplay.DisplayMode.x == m_softDisplay.DisplayModeNew.x)) {
        if ((m_softDisplay.RGB24 == m_softDisplay.RGB24New) &&
            (m_softDisplay.Interlaced == m_softDisplay.InterlacedNew))
            return;
    }

    m_softDisplay.RGB24 = m_softDisplay.RGB24New;  // get new infos

    m_softDisplay.DisplayMode.y = m_softDisplay.DisplayModeNew.y;
    m_softDisplay.DisplayMode.x = m_softDisplay.DisplayModeNew.x;
    m_previousDisplay.DisplayMode.x =                // previous will hold
        std::min(640, m_softDisplay.DisplayMode.x);  // max 640x512... that's
    m_previousDisplay.DisplayMode.y =                // the size of my
        std::min(512, m_softDisplay.DisplayMode.y);  // back buffer surface
    m_softDisplay.Interlaced = m_softDisplay.InterlacedNew;

    m_softDisplay.DisplayEnd.x =  // calc end of display
        m_softDisplay.DisplayPosition.x + m_softDisplay.DisplayMode.x;
    m_softDisplay.DisplayEnd.y =
        m_softDisplay.DisplayPosition.y + m_softDisplay.DisplayMode.y + m_previousDisplay.DisplayModeNew.y;
    m_previousDisplay.DisplayEnd.x = m_previousDisplay.DisplayPosition.x + m_softDisplay.DisplayMode.x;
    m_previousDisplay.DisplayEnd.y =
        m_previousDisplay.DisplayPosition.y + m_softDisplay.DisplayMode.y + m_previousDisplay.DisplayModeNew.y;

    changeDispOffsetsX();
}

void PCSX::SoftGPU::impl::vblank(bool fromGui) {
    // Frame presentation reads VRAM.
    m_queue.sync();
    m_statusRet ^= 0x80000000;  // odd/even bit

    if (m_softDisplay.Interlaced) {
        // interlaced mode?
        if (m_doVSyncUpdate && m_softDisplay.DisplayMode.x > 0 && m_softDisplay.DisplayMode.y > 0) {
            updateDisplay(fromGui);
        }
    } else {
        // non-interlaced?
        // some primitives drawn?
        if (m_doVSyncUpdate) updateDisplay(fromGui);  // -> update display
    }

    m_doVSyncUpdate = false;  // vsync done
}

void PCSX::SoftGPU::impl::presentFromGui() {
    GUI *gui = dynamic_cast<GUI *>(m_ui);
    if (!gui) return;
    m_queue.sync();
    if (m_softDisplay.Disabled) {
        // Same opaque black the OpenGL backend shows for a disabled display.
        gui->setViewport();
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    doBufferSwap(true);
}

uint32_t PCSX::SoftGPU::impl::readStatusInternal() { return m_statusRet; }

void PCSX::SoftGPU::impl::restoreStatus(uint32_t status) { m_statusRet = status; }

bool PCSX::SoftGPU::impl::configure() {
    // The settings below are read by the renderer thread.
    m_queue.sync();
    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
    const char *ditherValues[] = {_("No dithering (fastest)"), _("Game-dependent dithering (slow)"),
                                  _("Always dither g-shaded polygons (slowest)")};

    if (ImGui::Begin(_("Soft GPU configuration"), &m_showCfg)) {
        if (ImGui::Combo(_("Dithering"), &m_useDither, ditherValues, 3)) {
            changed = true;
            g_emulator->settings.get<Emulator::SettingDither>() = m_useDither;
        }

        if (ImGui::Checkbox(_("Use cached dithering tables"),
                            &g_emulator->settings.get<Emulator::SettingCachedDithering>().value)) {
            changed = true;
            setCachedDithering(g_emulator->settings.get<Emulator::SettingCachedDithering>());
        }
        ImGuiHelpers::ShowHelpMarker(
            _("Dithering tables are cached in memory for faster processing. Dithering will be done much faster, at the "
              "cost of a 512MB cache."));

        if (ImGui::Checkbox(_("Use linear filtering"),
                            &g_emulator->settings.get<Emulator::SettingLinearFiltering>().value)) {
            changed = true;
            setLinearFiltering();
        }

        if (ImGui::Checkbox(_("Render on a separate thread"),
                            &g_emulator->settings.get<Emulator::SettingSoftGPUThreaded>().value)) {
            changed = true;
            setThreaded(g_emulator->settings.get<Emulator::SettingSoftGPUThreaded>());
        }
        ImGuiHelpers::ShowHelpMarker(
            _("Rasterize on a worker thread while the emulation keeps running. VRAM readbacks and frame presentation "
              "wait for the worker to catch up."));

        ImGui::Checkbox(_("Disable textures for polygons"), &m_disableTexturesInPolygons);
        ImGui::Checkbox(_("Disable textures for sprites"), &m_disableTexturesInRectangles);

        ImGui::End();
    }

    return changed;
}

void PCSX::SoftGPU::impl::debugVideoMode(uint32_t stat) {
    static constexpr const char *hresNames[] = {"256", "320", "512", "640", "368", "384"};
    const auto &info = m_display.info;
    const uint32_t mode = getStatusControl(8);
    ImGui::SeparatorText(_("Video mode"));
    ImGui::Text(_("GP1(08h): %06x"), mode & 0xffffff);
    ImGui::Text(_("Resolution: %s x %s, %s, %s"), hresNames[magic_enum::enum_integer(info.hres)],
                info.vres == CtrlDisplayMode::VR_480 ? "480" : "240",
                info.mode == CtrlDisplayMode::VM_PAL ? "PAL" : "NTSC",
                info.depth == CtrlDisplayMode::CD_24BITS ? _("24 bits") : _("15 bits"));
    ImGui::Text(_("Interlace: %s"), info.interlace ? _("on") : _("off"));
    ImGui::Text(_("Display (GPUSTAT.23): %s"), (stat & 0x800000) ? _("disabled") : _("enabled"));
}

void PCSX::SoftGPU::impl::debugDisplayArea() {
    ImGui::SeparatorText(_("Display area"));
    const uint32_t start = getStatusControl(5);
    ImGui::Text(_("Start (GP1(05h)): %u, %u"), start & 0x3ff, (start >> 10) & 0x3ff);
    ImGui::Text(_("Horizontal range (GP1(06h)): %d - %d"), m_display.x1, m_display.x2);
    ImGui::Text(_("Vertical range (GP1(07h)): %d - %d"), m_display.y1, m_display.y2);
    ImGui::Text(_("Shown rectangle: %d, %d, %d x %d"), m_display.start.x(), m_display.start.y(), m_display.size.x(),
                m_display.size.y());
}

void PCSX::SoftGPU::impl::debugDrawing(uint32_t stat) {
    ImGui::SeparatorText(_("Drawing"));
    ImGui::Text(_("Draw to display area (E1h.10): %s"), (stat & 0x400) ? _("on") : _("off"));
    ImGui::Text(_("Dither (E1h.9): %s"), (stat & 0x200) ? _("on") : _("off"));
    ImGui::Text(_("Mask: set %s, check %s"), (stat & 0x800) ? _("on") : _("off"), (stat & 0x1000) ? _("on") : _("off"));
    const uint32_t areaStart = getDrawingStartRaw();
    const uint32_t areaEnd = getDrawingEndRaw();
    const uint32_t offset = getDrawingOffsetRaw();
    ImGui::Text(_("Drawing area: %u, %u - %u, %u"), areaStart & 0x3ff, (areaStart >> 10) & 0x3ff, areaEnd & 0x3ff,
                (areaEnd >> 10) & 0x3ff);
    // GP0(E5h) offsets are 11-bit signed.
    const int offsetX = int32_t(offset << 21) >> 21;
    const int offsetY = int32_t((offset >> 11) << 21) >> 21;
    ImGui::Text(_("Drawing offset: %d, %d"), offsetX, offsetY);
}

void PCSX::SoftGPU::impl::debugRowSkip(uint32_t stat) {
    ImGui::SeparatorText(_("480i row skip"));
    ImGui::Text(_("GPUSTAT.31: %u, GPUSTAT.13: %u"), stat >> 31, (stat >> 13) & 1);
    if (m_pushedSkipParity == -1) {
        ImGui::TextUnformatted(_("Skip parity at the last draw: none, every row drawn"));
    } else if (m_pushedSkipParity == s_skipParityUnknown) {
        ImGui::TextUnformatted(_("Skip parity at the last draw: no draw since the last reset or threading change"));
    } else {
        ImGui::Text(_("Skip parity at the last draw: %s rows skipped"), m_pushedSkipParity ? _("odd") : _("even"));
    }
}

void PCSX::SoftGPU::impl::debug() {
    if (ImGui::Begin(_("Soft GPU debugger"), &m_showDebug)) {
        const uint32_t stat = readStatusFromBus();
        debugVideoMode(stat);
        debugDisplayArea();
        debugDrawing(stat);
        debugRowSkip(stat);
    }
    ImGui::End();
}

static constexpr inline uint16_t BGR24to16(uint32_t BGR) { return PCSX::SoftGPU::Channel555::fromCommandColor(BGR); }

static constexpr inline uint16_t BGR24to16(PCSX::GPU::EmptyColor) { return 0; }

void PCSX::SoftGPU::impl::write0(ClearCache *) {}

void PCSX::SoftGPU::impl::write0(FastFill *prim) { dispatchDraw(prim); }

void PCSX::SoftGPU::impl::exec(FastFill *prim) {
    int16_t sX = prim->x;
    int16_t sY = prim->y;
    int16_t sW = prim->w;
    int16_t sH = prim->h;

    sW = (sW + 15) & ~15;

    // Increase H & W if they are one int16_t of full values, because they never can be full values
    if (sH >= 1023) sH = 1024;
    if (sW >= 1023) sW = 1024;

    // x and y of end pos
    sW += sX;
    sH += sY;

    fillArea(sX, sY, sW, sH, BGR24to16(prim->color));

    m_doVSyncUpdate = true;
}

template <PCSX::GPU::Shading shading, PCSX::GPU::Shape shape, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend,
          PCSX::GPU::Modulation modulation>
void PCSX::SoftGPU::impl::exec(Poly<shading, shape, textured, blend, modulation> *prim) {
    int16_t x0 = prim->x[0];
    int16_t y0 = prim->y[0];
    int16_t x1 = prim->x[1];
    int16_t y1 = prim->y[1];
    int16_t x2 = prim->x[2];
    int16_t y2 = prim->y[2];
    int16_t x3 = 0;
    int16_t y3 = 0;
    if constexpr (shape == Shape::Quad) {
        x3 = prim->x[3];
        y3 = prim->y[3];
        applyOffset4(x0, y0, x1, y1, x2, y2, x3, y3);
        if (checkCoord4(x0, y0, x1, y1, x2, y2, x3, y3)) return;
    } else {
        applyOffset3(x0, y0, x1, y1, x2, y2);
        if (checkCoord3(x0, y0, x1, y1, x2, y2)) return;
    }

    const int16_t bx[4] = {x0, x1, x2, x3};
    const int16_t by[4] = {y0, y1, y2, y3};
    constexpr int n = shape == Shape::Quad ? 4 : 3;
    forEachPixelWrap(bx, by, n, [&](int16_t dx, int16_t dy) {
        x0 = bx[0] + dx;
        y0 = by[0] + dy;
        x1 = bx[1] + dx;
        y1 = by[1] + dy;
        x2 = bx[2] + dx;
        y2 = by[2] + dy;
        x3 = bx[3] + dx;
        y3 = by[3] + dy;

        m_drawSemiTrans = blend == Blend::Semi;

        if constexpr (modulation == Modulation::On) {
            m_m1 = (prim->colors[0] >> 0) & 0xff;
            m_m2 = (prim->colors[0] >> 8) & 0xff;
            m_m3 = (prim->colors[0] >> 16) & 0xff;
        } else {
            m_m1 = m_m2 = m_m3 = 128;
        }

        if constexpr (shading == Shading::Flat) {
            if ((textured == Textured::Yes) && !m_disableTexturesInPolygons) {
                if constexpr (textured == Textured::Yes) {
                    texturePage(&prim->tpage);
                    if constexpr (shape == Shape::Quad) {
                        // Decompose into two triangles using the (1,3,2)+(0,1,2)
                        // PSX vertex split that drawPolyFlat4 and drawPoly4TG
                        // already use. drawPoly3T carries the same pixel-centre
                        // bias as the retired 4-vertex flat-textured walker, so
                        // this split is bit-equivalent to the old sweep for
                        // QFD-class geometry.
                        switch (m_globalTextTP) {
                            case GPU::TexDepth::Tex4Bits:
                                drawPoly3T<TexMode::Clut4>(x1, y1, x3, y3, x2, y2, prim->u[1], prim->v[1], prim->u[3],
                                                           prim->v[3], prim->u[2], prim->v[2], prim->clutX(),
                                                           prim->clutY());
                                drawPoly3T<TexMode::Clut4>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0], prim->u[1],
                                                           prim->v[1], prim->u[2], prim->v[2], prim->clutX(),
                                                           prim->clutY());
                                break;
                            case GPU::TexDepth::Tex8Bits:
                                drawPoly3T<TexMode::Clut8>(x1, y1, x3, y3, x2, y2, prim->u[1], prim->v[1], prim->u[3],
                                                           prim->v[3], prim->u[2], prim->v[2], prim->clutX(),
                                                           prim->clutY());
                                drawPoly3T<TexMode::Clut8>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0], prim->u[1],
                                                           prim->v[1], prim->u[2], prim->v[2], prim->clutX(),
                                                           prim->clutY());
                                break;
                            case GPU::TexDepth::Tex16Bits:
                                drawPoly3T<TexMode::Direct15>(x1, y1, x3, y3, x2, y2, prim->u[1], prim->v[1],
                                                              prim->u[3], prim->v[3], prim->u[2], prim->v[2], 0, 0);
                                drawPoly3T<TexMode::Direct15>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0],
                                                              prim->u[1], prim->v[1], prim->u[2], prim->v[2], 0, 0);
                                break;
                        }
                    } else {
                        switch (m_globalTextTP) {
                            case GPU::TexDepth::Tex4Bits:
                                drawPoly3T<TexMode::Clut4>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0], prim->u[1],
                                                           prim->v[1], prim->u[2], prim->v[2], prim->clutX(),
                                                           prim->clutY());
                                break;
                            case GPU::TexDepth::Tex8Bits:
                                drawPoly3T<TexMode::Clut8>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0], prim->u[1],
                                                           prim->v[1], prim->u[2], prim->v[2], prim->clutX(),
                                                           prim->clutY());
                                break;
                            case GPU::TexDepth::Tex16Bits:
                                drawPoly3T<TexMode::Direct15>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0],
                                                              prim->u[1], prim->v[1], prim->u[2], prim->v[2], 0, 0);
                                break;
                        }
                    }
                }
            } else {
                if constexpr (shape == Shape::Quad) {
                    drawPoly3F(x1, y1, x3, y3, x2, y2, prim->colors[0]);
                    drawPoly3F(x0, y0, x1, y1, x2, y2, prim->colors[0]);
                } else {
                    drawPoly3F(x0, y0, x1, y1, x2, y2, prim->colors[0]);
                }
            }
        } else {
            if ((textured == Textured::Yes) && !m_disableTexturesInPolygons) {
                if constexpr (textured == Textured::Yes) {
                    texturePage(&prim->tpage);
                    if constexpr (shape == Shape::Quad) {
                        switch (m_globalTextTP) {
                            case GPU::TexDepth::Tex4Bits:
                                drawPoly4TG<TexMode::Clut4>(x0, y0, x1, y1, x3, y3, x2, y2, prim->u[0], prim->v[0],
                                                            prim->u[1], prim->v[1], prim->u[3], prim->v[3], prim->u[2],
                                                            prim->v[2], prim->clutX(), prim->clutY(), prim->colors[0],
                                                            prim->colors[1], prim->colors[2], prim->colors[3]);
                                break;
                            case GPU::TexDepth::Tex8Bits:
                                drawPoly4TG<TexMode::Clut8>(x0, y0, x1, y1, x3, y3, x2, y2, prim->u[0], prim->v[0],
                                                            prim->u[1], prim->v[1], prim->u[3], prim->v[3], prim->u[2],
                                                            prim->v[2], prim->clutX(), prim->clutY(), prim->colors[0],
                                                            prim->colors[1], prim->colors[2], prim->colors[3]);
                                break;
                            case GPU::TexDepth::Tex16Bits:
                                drawPoly4TG<TexMode::Direct15>(x0, y0, x1, y1, x3, y3, x2, y2, prim->u[0], prim->v[0],
                                                               prim->u[1], prim->v[1], prim->u[3], prim->v[3],
                                                               prim->u[2], prim->v[2], 0, 0, prim->colors[0],
                                                               prim->colors[1], prim->colors[2], prim->colors[3]);
                                break;
                        }
                    } else {
                        switch (m_globalTextTP) {
                            case GPU::TexDepth::Tex4Bits:
                                drawPoly3TG<TexMode::Clut4>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0], prim->u[1],
                                                            prim->v[1], prim->u[2], prim->v[2], prim->clutX(),
                                                            prim->clutY(), prim->colors[0], prim->colors[1],
                                                            prim->colors[2]);
                                break;
                            case GPU::TexDepth::Tex8Bits:
                                drawPoly3TG<TexMode::Clut8>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0], prim->u[1],
                                                            prim->v[1], prim->u[2], prim->v[2], prim->clutX(),
                                                            prim->clutY(), prim->colors[0], prim->colors[1],
                                                            prim->colors[2]);
                                break;
                            case GPU::TexDepth::Tex16Bits:
                                drawPoly3TG<TexMode::Direct15>(x0, y0, x1, y1, x2, y2, prim->u[0], prim->v[0],
                                                               prim->u[1], prim->v[1], prim->u[2], prim->v[2], 0, 0,
                                                               prim->colors[0], prim->colors[1], prim->colors[2]);
                                break;
                        }
                    }
                }
            } else {
                if constexpr (shape == Shape::Quad) {
                    drawPoly4G(x0, y0, x1, y1, x2, y2, x3, y3, prim->colors[0], prim->colors[1], prim->colors[2],
                               prim->colors[3]);
                } else {
                    drawPoly3G(x0, y0, x1, y1, x2, y2, prim->colors[0], prim->colors[1], prim->colors[2]);
                }
            }
        }
    });
    m_doVSyncUpdate = true;
}

static constexpr int CHKMAX_X = 1024;
static constexpr int CHKMAX_Y = 512;

// Hardware drops lines whose endpoint delta exceeds the same per-edge bounds
// that gate polygons: |dx| <= 1023, |dy| <= 511 (verified on SCPH-5501 via
// gpu-raster-phase14 ct_line_dx_1024_drop / ct_line_dx_2047_drop).
static constexpr inline bool CheckCoordL(int16_t slx0, int16_t sly0, int16_t slx1, int16_t sly1) {
    int dx = slx1 - slx0;
    int dy = sly1 - sly0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx >= CHKMAX_X || dy >= CHKMAX_Y;
}

template <PCSX::GPU::Shading shading, PCSX::GPU::LineType lineType, PCSX::GPU::Blend blend>
void PCSX::SoftGPU::impl::exec(Line<shading, lineType, blend> *prim) {
    auto count = prim->colors.size();

    m_drawSemiTrans = blend == Blend::Semi;

    for (unsigned i = 1; i < count; i++) {
        int16_t x0 = prim->x[i - 1];
        int16_t x1 = prim->x[i];
        int16_t y0 = prim->y[i - 1];
        int16_t y1 = prim->y[i];
        auto c0 = prim->colors[i - 1];
        auto c1 = prim->colors[i];

        applyOffset2(x0, y0, x1, y1);
        if (CheckCoordL(x0, y0, x1, y1)) continue;

        const int16_t bx[2] = {x0, x1};
        const int16_t by[2] = {y0, y1};
        forEachPixelWrap(bx, by, 2, [&](int16_t dx, int16_t dy) {
            drawLine<shading>(bx[0] + dx, by[0] + dy, bx[1] + dx, by[1] + dy, c0, c1);
        });
    }
    m_doVSyncUpdate = true;
}

template <PCSX::GPU::Size size, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend, PCSX::GPU::Modulation modulation>
void PCSX::SoftGPU::impl::exec(Rect<size, textured, blend, modulation> *prim) {
    int16_t w, h;

    int16_t x0 = prim->x;
    int16_t y0 = prim->y;
    int16_t x1, y1, x2, y2, x3, y3;

    if constexpr (size == Size::Variable) {
        // Hardware masks the variable-rect dimensions to 10 bits (width) and
        // 9 bits (height); the effective extents are `w & 0x3FF` / `h & 0x1FF`,
        // not the documented `((w-1) & mask) + 1` shape. A width of 1024 maps
        // to 0 and silently drops the primitive on real silicon (verified via
        // gpu-raster-phase14 ct_rect_w1024 / ct_rect_h512 against SCPH-5501),
        // while 1025/513 fall through as 1-pixel wide/tall.
        w = static_cast<int16_t>(prim->w & 0x3FF);
        h = static_cast<int16_t>(prim->h & 0x1FF);
        if (w == 0 || h == 0) return;
    } else if constexpr (size == Size::S1) {
        w = h = 1;
    } else if constexpr (size == Size::S8) {
        w = h = 8;
    } else if constexpr (size == Size::S16) {
        w = h = 16;
    }

    m_drawSemiTrans = blend == Blend::Semi;

    if constexpr (modulation == Modulation::On) {
        m_m1 = (prim->color >> 0) & 0xff;
        m_m2 = (prim->color >> 8) & 0xff;
        m_m3 = (prim->color >> 16) & 0xff;
    } else {
        m_m1 = m_m2 = m_m3 = 128;
    }

    x1 = x2 = x0 + w + m_softDisplay.DrawOffset.x;
    x0 = x3 = x0 + m_softDisplay.DrawOffset.x;
    y2 = y3 = y0 + h + m_softDisplay.DrawOffset.y;
    y0 = y1 = y0 + m_softDisplay.DrawOffset.y;

    const int16_t bx[1] = {x0};
    const int16_t by[1] = {y0};
    const int16_t bx2 = x2, by2 = y2;
    forEachPixelWrap(bx, by, 1, [&](int16_t dx, int16_t dy) {
        x0 = bx[0] + dx;
        y0 = by[0] + dy;
        x2 = bx2 + dx;
        y2 = by2 + dy;

        if ((textured == Textured::Yes) && !m_disableTexturesInRectangles) {
            if constexpr (textured == Textured::Yes) {
                switch (m_globalTextTP) {
                    case GPU::TexDepth::Tex4Bits:
                        drawSprite<TexMode::Clut4>(x0, y0, w, h, prim->u, prim->v, prim->clutX(), prim->clutY());
                        break;
                    case GPU::TexDepth::Tex8Bits:
                        drawSprite<TexMode::Clut8>(x0, y0, w, h, prim->u, prim->v, prim->clutX(), prim->clutY());
                        break;
                    case GPU::TexDepth::Tex16Bits:
                        drawSprite<TexMode::Direct15>(x0, y0, w, h, prim->u, prim->v, 0, 0);
                        break;
                }
            }
        } else {
            drawRect(x0, y0, x2, y2, BGR24to16(prim->color));
        }
    });

    m_doVSyncUpdate = true;
}

void PCSX::SoftGPU::impl::write0(BlitVramVram *prim) { dispatch(prim); }

void PCSX::SoftGPU::impl::exec(BlitVramVram *prim) {
    int16_t imageY0, imageX0, imageY1, imageX1, imageSX, imageSY, i, j;

    imageX0 = prim->sX;
    imageY0 = prim->sY;
    imageX1 = prim->dX;
    imageY1 = prim->dY;
    imageSX = prim->w;
    imageSY = prim->h;

    // An in-place copy still rewrites every pixel, so it is only a no-op without set-mask.
    if ((imageX0 == imageX1) && (imageY0 == imageY1) && !getSetMask16()) return;
    if (imageSX <= 0) return;
    if (imageSY <= 0) return;

    // The copy honours the GP0(E6h) mask settings, per halfword. The masked case takes
    // this per-pixel path, which walks in the same order as the fast paths below.
    if ((imageY0 + imageSY) > VRAM_HEIGHT || (imageX0 + imageSX) > 1024 || (imageY1 + imageSY) > VRAM_HEIGHT ||
        (imageX1 + imageSX) > 1024 || getCheckMask() || getSetMask16()) {
        int i, j;
        for (j = 0; j < imageSY; j++) {
            // Drop rows written to an opened-but-unpopulated upper bank (1MB
            // fitment): the destination floats and the write is lost. Source
            // reads from such a region just return the prefilled 0xFFFF.
            if (vramOpenBus(imageY1 + j)) continue;
            for (i = 0; i < imageSX; i++) {
                uint16_t &dst = m_vram16[(1024 * resolveVramY(imageY1 + j)) + ((imageX1 + i) & VRAM_X_MASK)];
                if (getCheckMask() && (dst & 0x8000)) continue;
                dst = m_vram16[(1024 * resolveVramY(imageY0 + j)) + ((imageX0 + i) & VRAM_X_MASK)] | getSetMask16();
            }
        }

        m_doVSyncUpdate = true;

        return;
    }

    if (imageSX & 1) {
        // not dword aligned? slower func
        uint16_t *SRCPtr, *DSTPtr;
        uint16_t LineOffset;

        SRCPtr = m_vram16 + (1024 * imageY0) + imageX0;
        DSTPtr = m_vram16 + (1024 * imageY1) + imageX1;

        LineOffset = 1024 - imageSX;

        for (j = 0; j < imageSY; j++) {
            for (i = 0; i < imageSX; i++) *DSTPtr++ = *SRCPtr++;
            SRCPtr += LineOffset;
            DSTPtr += LineOffset;
        }
    } else {
        // dword aligned
        uint32_t *SRCPtr, *DSTPtr;
        uint16_t LineOffset;
        int dx = imageSX >> 1;

        SRCPtr = (uint32_t *)(m_vram16 + (1024 * imageY0) + imageX0);
        DSTPtr = (uint32_t *)(m_vram16 + (1024 * imageY1) + imageX1);

        LineOffset = 512 - dx;

        for (j = 0; j < imageSY; j++) {
            for (i = 0; i < dx; i++) *DSTPtr++ = *SRCPtr++;
            SRCPtr += LineOffset;
            DSTPtr += LineOffset;
        }
    }

    imageSX += imageX1;
    imageSY += imageY1;

    m_doVSyncUpdate = true;
}

void PCSX::SoftGPU::impl::texturePageStatus(GPU::TPage &tpage) {
    if (m_useDither == 2) {
        m_statusDitherMode = true;
    } else {
        m_statusDitherMode = tpage.dither && (m_useDither != 0);
    }
    m_statusRet &= ~0x07ff;
    m_statusRet |= tpage.raw & 0x07ff;
}

void PCSX::SoftGPU::impl::polygonTexturePageStatus(const GPU::TPage &tpage) {
    m_statusRet &= ~0x01ff;
    m_statusRet |= tpage.raw & 0x01ff;
}

void PCSX::SoftGPU::impl::maskBitStatus(const GPU::MaskBit &mask) {
    m_statusRet &= ~0x1800;
    if (mask.set) m_statusRet |= 0x0800;
    if (mask.check) m_statusRet |= 0x1000;
}

void PCSX::SoftGPU::impl::write0(TPage *prim) {
    m_drawToDisplay = prim->drawToDisplay;
    texturePageStatus(*prim);
    dispatch(prim);
}
void PCSX::SoftGPU::impl::write0(TWindow *prim) { dispatch(prim); }
void PCSX::SoftGPU::impl::write0(DrawingAreaStart *prim) { dispatch(prim); }
void PCSX::SoftGPU::impl::write0(DrawingAreaEnd *prim) { dispatch(prim); }
void PCSX::SoftGPU::impl::write0(DrawingOffset *prim) { dispatch(prim); }
void PCSX::SoftGPU::impl::write0(MaskBit *prim) {
    maskBitStatus(*prim);
    dispatch(prim);
}

// A textured polygon that is not culled loads its texpage, which shows in GPUSTAT. The cull test and the
// dither forcing are pure functions of the primitive and of state this thread mirrors, so they run here,
// before the primitive is handed to the renderer.
template <PCSX::GPU::Shading shading, PCSX::GPU::Shape shape, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend,
          PCSX::GPU::Modulation modulation>
void PCSX::SoftGPU::impl::dispatchPoly(Poly<shading, shape, textured, blend, modulation> *prim) {
    if constexpr (textured == Textured::Yes) {
        if (!m_disableTexturesInPolygons) {
            int16_t x0 = prim->x[0], y0 = prim->y[0];
            int16_t x1 = prim->x[1], y1 = prim->y[1];
            int16_t x2 = prim->x[2], y2 = prim->y[2];
            bool culled;
            if constexpr (shape == Shape::Quad) {
                int16_t x3 = prim->x[3], y3 = prim->y[3];
                culled = checkCoord4(x0, y0, x1, y1, x2, y2, x3, y3);
            } else {
                culled = checkCoord3(x0, y0, x1, y1, x2, y2);
            }
            if (!culled) {
                if (m_statusDitherMode) {
                    prim->tpage.dither = true;
                    prim->tpage.raw |= 0x200;
                }
                polygonTexturePageStatus(prim->tpage);
            }
        }
    }
    dispatchDraw(prim);
}

void PCSX::SoftGPU::impl::updateSkipParity() {
    int8_t parity = -1;
    if (!m_drawToDisplay && ((getStatusControl(8) & 0x2c) == 0x24)) parity = readStatusFromBus() >> 31;
    if (parity == m_pushedSkipParity) return;
    m_pushedSkipParity = parity;
    if (m_queue.running()) {
        m_queue.push(std::make_unique<SkipParityOp>(parity));
    } else {
        m_skipParity = parity;
    }
}

PCSX::GPU::ScreenShot PCSX::SoftGPU::impl::takeScreenShot() {
    m_queue.sync();
    ScreenShot ss;
    // Same rectangle as doBufferSwap: the display range, not the mode's nominal width.
    int startX = static_cast<int>(m_display.start.x());
    int startY = static_cast<int>(m_display.start.y());
    int width = static_cast<int>(m_display.size.x());
    int height = std::min(static_cast<int>(m_display.size.y()), 1024 - startY);
    // Keep each row inside its VRAM line: 1024 halfwords, i.e. 2048 bytes in 24bpp.
    width = std::min(width, m_softDisplay.RGB24 ? (2048 - startX * 2) / 3 : 1024 - startX);
    ss.width = width;
    ss.height = height;
    unsigned factor = m_softDisplay.RGB24 ? 3 : 2;
    ss.bpp = m_softDisplay.RGB24 ? ScreenShot::BPP_24 : ScreenShot::BPP_16;
    unsigned size = width * height * factor;
    char *pixels = reinterpret_cast<char *>(malloc(size));
    ss.data.acquire(pixels, size);
    if (m_softDisplay.RGB24) {
        auto ptr = m_vram;
        ptr += startX * 2 + startY * 1024 * 2;
        for (int i = 0; i < height; i++) {
            std::memcpy(pixels, ptr, width * 3);
            ptr += 1024 * 2;
            pixels += width * 3;
        }
    } else {
        auto ptr = m_vram16;
        ptr += startY * 1024 + startX;
        for (int i = 0; i < height; i++) {
            std::memcpy(pixels, ptr, width * sizeof(uint16_t));
            ptr += 1024;
            pixels += width * 2;
        }
    }

    return ss;
}

void PCSX::SoftGPU::impl::write1(CtrlReset *) {
    m_queue.sync();
    // GP1(00h) clears GP0(E1h), DFE included, and GP1(08h).
    m_drawToDisplay = false;
    m_pushedSkipParity = s_skipParityUnknown;
    m_display.reset();
    m_statusRet = 0x14802000;
    m_softDisplay.Disabled = 1;
    m_softDisplay.DrawOffset.x = m_softDisplay.DrawOffset.y = 0;
    resetRenderer();
    acknowledgeIRQ1();
    m_softDisplay.RGB24 = false;
    m_softDisplay.Interlaced = false;
}

void PCSX::SoftGPU::impl::write1(CtrlClearFifo *) {}

void PCSX::SoftGPU::impl::write1(CtrlIrqAck *) { acknowledgeIRQ1(); }

void PCSX::SoftGPU::impl::write1(CtrlDisplayEnable *ctrl) {
    m_previousDisplay.Disabled = m_softDisplay.Disabled;
    m_softDisplay.Disabled = !ctrl->enable;

    if (m_softDisplay.Disabled) {
        m_statusRet |= GPUSTATUS_DISPLAYDISABLED;
    } else {
        m_statusRet &= ~GPUSTATUS_DISPLAYDISABLED;
    }
}

void PCSX::SoftGPU::impl::write1(CtrlDmaSetting *ctrl) {
    m_statusRet &= ~GPUSTATUS_DMABITS;
    m_statusRet |= magic_enum::enum_integer(ctrl->dma) << 29;
}

void PCSX::SoftGPU::impl::write1(CtrlDisplayStart *raw) {
    // The decoded Y keeps the upper-bank bit; with the gate closed it folds
    // into the lower 512 rows like any other VRAM access.
    CtrlDisplayStart resolved = *raw;
    resolved.y = resolveVramY(raw->y);
    auto ctrl = &resolved;
    m_display.set(ctrl);
    m_previousDisplay.DisplayPosition.x = m_softDisplay.DisplayPosition.x;
    m_previousDisplay.DisplayPosition.y = m_softDisplay.DisplayPosition.y;

    // new
    m_softDisplay.DisplayPosition.y = ctrl->y;

    // store the same val in some helper var, we need it on later compares
    m_previousDisplay.DisplayModeNew.x = m_softDisplay.DisplayPosition.y;

    const int scanoutHeight = vram2MBGateOpen() ? VRAM_HEIGHT * 2 : VRAM_HEIGHT;
    if ((m_softDisplay.DisplayPosition.y + m_softDisplay.DisplayMode.y) > scanoutHeight) {
        int dy1 = scanoutHeight - m_softDisplay.DisplayPosition.y;
        int dy2 = (m_softDisplay.DisplayPosition.y + m_softDisplay.DisplayMode.y) - scanoutHeight;

        if (dy1 >= dy2) {
            m_previousDisplay.DisplayModeNew.y = -dy2;
        } else {
            m_softDisplay.DisplayPosition.y = 0;
            m_previousDisplay.DisplayModeNew.y = -dy1;
        }
    } else {
        m_previousDisplay.DisplayModeNew.y = 0;
    }
    // eon

    m_softDisplay.DisplayPosition.x = ctrl->x;
    m_softDisplay.DisplayEnd.x = m_softDisplay.DisplayPosition.x + m_softDisplay.DisplayMode.x;
    m_softDisplay.DisplayEnd.y =
        m_softDisplay.DisplayPosition.y + m_softDisplay.DisplayMode.y + m_previousDisplay.DisplayModeNew.y;
    m_previousDisplay.DisplayEnd.x = m_previousDisplay.DisplayPosition.x + m_softDisplay.DisplayMode.x;
    m_previousDisplay.DisplayEnd.y =
        m_previousDisplay.DisplayPosition.y + m_softDisplay.DisplayMode.y + m_previousDisplay.DisplayModeNew.y;

    m_doVSyncUpdate = true;
}

void PCSX::SoftGPU::impl::write1(CtrlHorizontalDisplayRange *ctrl) {
    m_display.set(ctrl);
    m_softDisplay.Range.x0 = ctrl->x0;
    m_softDisplay.Range.x1 = ctrl->x1;
    m_softDisplay.Range.x1 -= m_softDisplay.Range.x0;
    changeDispOffsetsX();
}

void PCSX::SoftGPU::impl::write1(CtrlVerticalDisplayRange *ctrl) {
    m_display.set(ctrl);
    m_softDisplay.Range.y0 = ctrl->y0;
    m_softDisplay.Range.y1 = ctrl->y1;

    m_previousDisplay.Height = m_softDisplay.Height;

    m_softDisplay.Height = m_softDisplay.Range.y1 - m_softDisplay.Range.y0 + m_previousDisplay.DisplayModeNew.y;

    if (m_previousDisplay.Height != m_softDisplay.Height) {
        m_softDisplay.DisplayModeNew.y = m_softDisplay.Height * m_softDisplay.Double;

        changeDispOffsetsY();

        updateDisplayIfChanged();
    }
}

void PCSX::SoftGPU::impl::write1(CtrlDisplayMode *ctrl) {
    // Display::set also switches the PAL/NTSC setting, which a detached GPU must not touch.
    if (!isDetached()) m_display.set(ctrl);
    m_softDisplay.DisplayModeNew.x = s_displayWidths[magic_enum::enum_integer(ctrl->hres)];

    if (ctrl->vres == CtrlDisplayMode::VR_480) {
        m_softDisplay.Double = 2;
    } else {
        m_softDisplay.Double = 1;
    }

    m_softDisplay.DisplayModeNew.y = m_softDisplay.Height * m_softDisplay.Double;

    changeDispOffsetsY();

    m_softDisplay.PAL = ctrl->mode == CtrlDisplayMode::VM_PAL;
    m_softDisplay.RGB24New = ctrl->depth == CtrlDisplayMode::CD_24BITS;
    m_softDisplay.InterlacedNew = ctrl->interlace;

    if (!isDetached() && g_emulator->settings.get<PCSX::Emulator::SettingAutoVideo>()) {
        if (m_softDisplay.PAL) {
            g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_PAL;
        } else {
            g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_NTSC;
        }
    }

    m_statusRet &= ~GPUSTATUS_WIDTHBITS;
    m_statusRet |= ctrl->widthRaw << 16;

    if (m_softDisplay.InterlacedNew) {
        if (!m_softDisplay.Interlaced) {
            m_previousDisplay.DisplayPosition.x = m_softDisplay.DisplayPosition.x;
            m_previousDisplay.DisplayPosition.y = m_softDisplay.DisplayPosition.y;
        }
        m_statusRet |= GPUSTATUS_INTERLACED;
    } else {
        m_statusRet &= ~GPUSTATUS_INTERLACED;
    }

    if (m_softDisplay.PAL) {
        m_statusRet |= GPUSTATUS_PAL;
    } else {
        m_statusRet &= ~GPUSTATUS_PAL;
    }

    if (m_softDisplay.Double == 2) {
        m_statusRet |= GPUSTATUS_DOUBLEHEIGHT;
    } else {
        m_statusRet &= ~GPUSTATUS_DOUBLEHEIGHT;
    }

    if (m_softDisplay.RGB24New) {
        m_statusRet |= GPUSTATUS_RGB24;
    } else {
        m_statusRet &= ~GPUSTATUS_RGB24;
    }

    updateDisplayIfChanged();
}

template <typename Prim>
void PCSX::SoftGPU::PrimOp<Prim>::run(PCSX::SoftGPU::impl &r) {
    r.exec(&prim);
}

void PCSX::SoftGPU::VramWriteOp::run(PCSX::SoftGPU::impl &r) { r.vramWrite(x, y, w, h, pixels.data()); }

void PCSX::SoftGPU::SkipParityOp::run(PCSX::SoftGPU::impl &r) { r.m_skipParity = parity; }

// clang-format off
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchPoly(prim); }
void PCSX::SoftGPU::impl::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchPoly(prim); }

void PCSX::SoftGPU::impl::write0(Line<Shading::Flat, LineType::Simple, Blend::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Line<Shading::Flat, LineType::Simple, Blend::Semi> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Line<Shading::Flat, LineType::Poly, Blend::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Line<Shading::Flat, LineType::Poly, Blend::Semi> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Line<Shading::Gouraud, LineType::Simple, Blend::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Line<Shading::Gouraud, LineType::Simple, Blend::Semi> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Line<Shading::Gouraud, LineType::Poly, Blend::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Line<Shading::Gouraud, LineType::Poly, Blend::Semi> *prim) { dispatchDraw(prim); }

void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::On> *prim) { dispatchDraw(prim); }
void PCSX::SoftGPU::impl::write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::On> *prim) { dispatchDraw(prim); }
