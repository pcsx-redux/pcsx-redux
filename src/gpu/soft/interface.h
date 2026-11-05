/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#pragma once

#include <atomic>

#include "core/gpu.h"
#include "gpu/soft/async.h"
#include "gpu/soft/soft.h"
#include "support/opengl.h"

namespace PCSX {

class UI;

namespace SoftGPU {

class impl final : public GPU, private SoftRenderer {
    int32_t initBackend(UI *) override;
    int32_t shutdown() override;
    uint32_t readStatusInternal() override;
    void vblank(bool fromGui) override;
    void presentFromGui() override;
    bool configure() override;
    void debug() override;
    void debugVideoMode(uint32_t stat);
    void debugDisplayArea();
    void debugDrawing(uint32_t stat);
    void debugRowSkip(uint32_t stat);

    void setDither(int setting) override {
        m_queue.sync();
        m_useDither = setting;
    }
    void clearVRAM() override;
    void resetBackend() override {
        clearVRAM();
        m_display.reset();
    }
    void drainRenderer() override { m_queue.sync(); }
    void vramConfigChanged() override {
        // The upper bank is only drawable when a 2MB bank is physically fitted
        // and the gate is open; otherwise the renderer stays bounded to the
        // retail 512 rows (gate-closed mirrors and 1MB open-bus are handled by
        // resolveVramY / the open-bus prefill, not by drawing into row 512+).
        m_vramHeight = (vram2MBFitted() && vram2MBGateOpen()) ? (VRAM_HEIGHT * 2) : VRAM_HEIGHT;
    }
    // The RESOLVED texture, not the raw one. The VRAM viewer takes whatever
    // this returns from either backend, and the OpenGL backend hands it a
    // GL_RGBA8 - so a caller cannot know which format it is holding, and the
    // viewer's single sampler has to see the same thing from both. Since the
    // resolve reproduces the old packed decode exactly, mask bit included,
    // the viewer needs no change at all and the GL_R16UI stays private to
    // this backend.
    GLuint getVRAMTexture() override { return m_vramTextureResolved.handle(); }
    void setLinearFiltering() override;
    void setCachedDithering(bool value) override {
        m_queue.sync();
        if (value) {
            enableCachedDithering();
        } else {
            disableCachedDithering();
        }
    }

    void restoreStatus(uint32_t status) override;

    void updateDisplay(bool fromGui);
    void initDisplay();
    void doBufferSwap(bool fromGui);

    void changeDispOffsetsX();
    void changeDispOffsetsY();
    void updateDisplayIfChanged();

    Slice getVRAM(Ownership ownership) override {
        m_queue.sync();
        Slice ret;
        // A borrowed pointer would be read while the renderer thread keeps drawing, so the threaded
        // renderer always hands out a snapshot.
        if (ownership == Ownership::BORROW && !m_queue.running()) {
            ret.borrow(m_vram16, 1024 * 1024 * 2);
        } else {
            ret.copy(m_vram16, 1024 * 1024 * 2);
        }
        return ret;
    }

    void partialUpdateVRAM(int x, int y, int w, int h, const uint16_t *pixels, PartialUpdateVram) override {
        if (m_queue.running()) {
            m_queue.push(std::make_unique<VramWriteOp>(x, y, w, h, pixels));
        } else {
            vramWrite(x, y, w, h, pixels);
        }
    }

    friend struct VramWriteOp;
    void vramWrite(int x, int y, int w, int h, const uint16_t *pixels) {
        auto ptr = m_vram16;
        ptr += y * 1024 + x;
        for (int i = 0; i < h; i++) {
            std::memcpy(ptr, pixels, w * sizeof(uint16_t));
            ptr += 1024;
            pixels += w;
        }
    }

    virtual ScreenShot takeScreenShot() override;

    GLuint m_vramTexture16;
    GLuint m_vramTexture24;

    // m_vramTexture16 is a GL_R16UI integer texture holding untouched PS1 words,
    // and it never leaves this backend. resolveVRAM() unpacks it once per frame
    // into m_vramTextureResolved, which is what both the display path and
    // getVRAMTexture() hand out.
    void resolveVRAM();
    OpenGL::Texture m_vramTextureResolved;
    OpenGL::Framebuffer m_resolveFBO;
    OpenGL::Program m_resolveProgram;
    OpenGL::VertexArray m_resolveVAO;
    GLint m_resolveTexLoc = -1;

    UI *m_ui;

    std::atomic<bool> m_doVSyncUpdate = false;
    SoftDisplay m_previousDisplay;
    unsigned char *m_allocatedVRAM;
    static constexpr int16_t s_displayWidths[] = {256, 320, 512, 640, 368, 384};

    void write0(ClearCache *) override;
    void write0(FastFill *) override;

    // Threaded rendering. While the queue runs, the renderer thread owns VRAM and every piece of render state;
    // the emulation thread only pushes ops and calls m_queue.sync() before reading them. GPUSTAT stays on the
    // emulation thread, so the status side effects of a primitive are applied at push time.
    template <typename Prim>
    friend struct PrimOp;
    OpQueue m_queue;
    void setThreaded(bool threaded);
    template <typename Prim>
    void dispatch(Prim *prim) {
        if (m_queue.running()) {
            m_queue.push(std::make_unique<PrimOp<Prim>>(*prim));
        } else {
            exec(prim);
        }
    }
    template <Shading shading, Shape shape, Textured textured, Blend blend, Modulation modulation>
    void dispatchPoly(Poly<shading, shape, textured, blend, modulation> *);
    // NTSC 480i with GP0(E1h).10 (draw to display area) clear: drawing primitives and GP0(02h) do not
    // write the VRAM rows whose (y & 1) equals GPUSTAT.31 at the time they are drawn. updateSkipParity()
    // samples that on this thread before each drawing primitive is dispatched, and forwards it to the
    // renderer's m_skipParity when it changed. m_pushedSkipParity is this thread's view of the renderer
    // copy; s_skipParityUnknown matches nothing, so the next drawing primitive always forwards.
    static constexpr int8_t s_skipParityUnknown = -2;
    int8_t m_pushedSkipParity = s_skipParityUnknown;
    // GP0(E1h).10 as last written by GP0(E1h).
    bool m_drawToDisplay = false;
    friend struct SkipParityOp;
    void updateSkipParity();
    template <typename Prim>
    void dispatchDraw(Prim *prim) {
        updateSkipParity();
        dispatch(prim);
    }
    void texturePageStatus(GPU::TPage &tpage);
    // A textured polygon's texpage only loads GPUSTAT bits 0-8; bits 9 and 10 (dither, drawing to the
    // display area) keep their GP0(E1h) values, measured on v0 and v2 GPUs.
    void polygonTexturePageStatus(const GPU::TPage &tpage);
    void maskBitStatus(const GPU::MaskBit &mask);
    // The emulation thread's copy of SoftRenderer::m_ditherMode, which decides whether a textured polygon's
    // texpage gets its dither bit forced. It follows the same rule over the same texpage sequence.
    bool m_statusDitherMode = false;

    void exec(FastFill *);
    void exec(BlitVramVram *);
    void exec(TPage *prim) { texturePage(prim); }
    void exec(TWindow *prim) { twindow(prim); }
    void exec(DrawingAreaStart *prim) { drawingAreaStart(prim); }
    void exec(DrawingAreaEnd *prim) { drawingAreaEnd(prim); }
    void exec(DrawingOffset *prim) { drawingOffset(prim); }
    void exec(MaskBit *prim) { maskBit(prim); }
    template <Shading shading, Shape shape, Textured textured, Blend blend, Modulation modulation>
    void exec(Poly<shading, shape, textured, blend, modulation> *);
    template <Shading shading, LineType lineType, Blend blend>
    void exec(Line<shading, lineType, blend> *);
    template <Size size, Textured textured, Blend blend, Modulation modulation>
    void exec(Rect<size, textured, blend, modulation> *);

    void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *) override;

    void write0(Line<Shading::Flat, LineType::Simple, Blend::Off> *) override;
    void write0(Line<Shading::Flat, LineType::Simple, Blend::Semi> *) override;
    void write0(Line<Shading::Flat, LineType::Poly, Blend::Off> *) override;
    void write0(Line<Shading::Flat, LineType::Poly, Blend::Semi> *) override;
    void write0(Line<Shading::Gouraud, LineType::Simple, Blend::Off> *) override;
    void write0(Line<Shading::Gouraud, LineType::Simple, Blend::Semi> *) override;
    void write0(Line<Shading::Gouraud, LineType::Poly, Blend::Off> *) override;
    void write0(Line<Shading::Gouraud, LineType::Poly, Blend::Semi> *) override;

    void write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::Off> *) override;
    void write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::Off> *) override;
    void write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::On> *) override;
    void write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::On> *) override;
    void write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::On> *) override;
    void write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::On> *) override;
    void write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::On> *) override;
    void write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::On> *) override;

    void write0(BlitVramVram *) override;

    void write0(TPage *) override;
    void write0(TWindow *) override;
    void write0(DrawingAreaStart *) override;
    void write0(DrawingAreaEnd *) override;
    void write0(DrawingOffset *) override;
    void write0(MaskBit *) override;

    void write1(CtrlReset *) override;
    void write1(CtrlClearFifo *) override;
    void write1(CtrlIrqAck *) override;
    void write1(CtrlDisplayEnable *) override;
    void write1(CtrlDmaSetting *) override;
    void write1(CtrlDisplayStart *) override;
    void write1(CtrlHorizontalDisplayRange *) override;
    void write1(CtrlVerticalDisplayRange *) override;
    void write1(CtrlDisplayMode *) override;
};

}  // namespace SoftGPU

}  // namespace PCSX
