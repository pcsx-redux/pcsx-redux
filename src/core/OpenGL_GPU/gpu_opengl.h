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

#pragma once

#include <array>
#include <vector>

#include "core/gpu.h"
#include "gui/widgets/shader-editor.h"
#include "support/opengl.h"

namespace PCSX {
class OpenGL_GPU final : public GPU {
    // Interface functions
    int initBackend(UI *) override;
    int shutdown() override;
    uint32_t readStatusInternal() override;
    void setOpenGLContext() override;
    void vblank(bool fromGui) override;
    // This backend's vblank() touches no emulated state (no GPUSTAT toggle): it flushes the
    // pending batch and presents, so it is safe to reuse here.
    void presentFromGui() override { vblank(true); }
    bool configure() override;
    void debug() override;

    void setDither(int setting) override { m_useDither = setting; }
    void setCachedDithering(bool cached) override {}
    void clearVRAM() override;
    void resetBackend() override;
    GLuint getVRAMTexture() override;
    void setLinearFiltering() override;
    Slice getVRAM(Ownership) override;
    void partialUpdateVRAM(int x, int y, int w, int h, const uint16_t *pixels, PartialUpdateVram) override;
    void restoreStatus(uint32_t status) { m_gpustat = status; }

  private:
    // Actual emulation stuff
    struct Vertex {
        OpenGL::ivec2 positions;
        uint32_t colour;
        uint16_t texpage;
        uint16_t clut;
        OpenGL::Vector<uint16_t, 2> uv;

        // We use bit 15 of the texpage attribute (normally unused) to indicate an untextured prim.
        static constexpr uint16_t c_untexturedPrimitiveTexpage = 0x8000;

        Vertex() : Vertex(0, 0, 0) {}

        Vertex(int x, int y, uint32_t col) {
            positions.x() = x;
            positions.y() = y;
            colour = col;
            texpage = c_untexturedPrimitiveTexpage;
        }

        Vertex(int x, int y, uint32_t col, uint16_t clut, uint16_t texpage, unsigned u, unsigned v)
            : colour(col), clut(clut), texpage(texpage) {
            positions.x() = x;
            positions.y() = y;

            uv.u() = u;
            uv.v() = v;
        }
    };

    enum class TransferMode { CommandTransfer, VRAMTransfer };

    uint32_t m_gpustat = 0x14802000;
    GUI *m_gui = nullptr;
    int m_useDither = 0;

    static constexpr int vramWidth = 1024;
    // The VRAM texture is always a full 1024x1024, whatever the fitment: the
    // bank semantics live in the contents (open-bus fill) and in the sampling
    // mask below, so no texture has to be resized when the fitment or the
    // GP1(09h) gate changes.
    static constexpr int vramHeight = 1024;
    static constexpr int vertexBufferSize = 0x100000;

    OpenGL::Program m_program;
    OpenGL::VertexArray m_vao;
    OpenGL::VertexBuffer m_vbo;
    OpenGL::Framebuffer m_fbo;
    OpenGL::Texture m_vramTexture;
    OpenGL::Texture m_blankTexture;  // Black texture to display when the display is off

    // We need non-MSAA copies of our texture & FBO when using multisampling
    OpenGL::Texture m_vramTextureNoMSAA;
    OpenGL::Framebuffer m_fboNoMSAA;
    Widgets::ShaderEditor m_shaderEditor = {"hw-renderer"};

    // For CPU->VRAM texture transfers
    OpenGL::Texture m_sampleTexture;

    // For the 16-bits to 24-bits conversion
    OpenGL::Framebuffer m_fbo24;
    OpenGL::Texture m_vramTexture24;
    Widgets::ShaderEditor m_shaderEditor24 = {"16-to-24"};

    std::vector<Vertex> m_vertices;
    OpenGL::Rect m_scissorBox;
    int m_drawAreaLeft, m_drawAreaRight, m_drawAreaTop, m_drawAreaBottom;

    OpenGL::ivec2 m_drawingOffset;
    // Clear colour used in the debugger
    OpenGL::vec3 m_clearColour = OpenGL::vec3(0.f, 0.f, 0.f);
    // Specifies how and whether to fill renderer polygons
    OpenGL::FillMode m_polygonMode = OpenGL::FillPoly;
    // x: The factor to multiply the destination (framebuffer) colour with
    // y: The factor to multiply the source colour with
    OpenGL::vec2 m_blendFactors;

    bool m_multisampled = false;
    int m_polygonModeIndex = 0;

    GLint m_drawingOffsetLoc;
    GLint m_texWindowLoc;
    GLint m_blendFactorsLoc;
    GLint m_blendFactorsIfOpaqueLoc;
    GLint m_vramMaskYLoc;
    GLint m_texpage2MBLoc;
    // Set when the fitment or the GP1(09h) bank gate moves; the new sampling
    // mask is pushed from renderBatch(), which always runs with our context
    // current, rather than from the GP1 handler itself.
    bool m_vramConfigDirty = true;

    int m_vertexCount = 0;
    bool m_updateDrawOffset = false;
    bool m_syncVRAM = true;
    uint32_t m_rectTexpage = 0;  // Rects have their own texpage settings
    // NTSC 480i with GP0(E1h).10 (draw to display area) clear: drawing primitives and GP0(02h) do not
    // write the VRAM rows whose (y & 1) equals GPUSTAT.31 at the time they are drawn. updateSkipParity()
    // samples that before each drawing primitive and pushes it to the u_skipParity uniform, flushing the
    // batch first, when it changed. -1 skips nothing; s_skipParityUnknown matches nothing, so the next
    // drawing primitive always pushes.
    static constexpr int s_skipParityUnknown = -2;
    int m_skipParity = s_skipParityUnknown;
    GLint m_skipParityLoc;
    // GP0(E1h).10 as last written by GP0(E1h). A polygon's texpage cannot change it.
    bool m_drawToDisplay = false;
    void updateSkipParity();
    uint32_t m_lastTexwindowSetting = 0;
    uint32_t m_lastDrawOffsetSetting = 0;
    uint32_t m_drawMode;

    template <int count>
    void maybeRenderBatch() {
        if ((m_vertexCount + count) >= vertexBufferSize) renderBatch();
    }
    void renderBatch();
    void vramConfigChanged() override { m_vramConfigDirty = true; }
    void pushVRAMConfig();
    void clearVRAM(float r, float g, float b, float a = 1.0);
    void updateDrawArea();
    void setScissorArea();
    void setDrawOffset(uint32_t cmd);
    void setTexWindow(uint32_t cmd);
    void setTexWindowUnchecked(uint32_t cmd);
    void setDisplayEnable(bool setting);

    // For untextured primitives, there's flat and gouraud shading.
    // For textured primitives, RawTexture and RawTextureGouraud work the same way, except the latter has unused colour
    // parameters RawTextureGouraud is used a lot by some games, like Castlevania TextureBlendFlat is texture blending
    // with a flat colour, TextureBlendGouraud is texture blending with a gouraud shaded colour
    enum class GLShading { Flat, Gouraud, RawTexture, RawTextureGouraud, TextureBlendFlat, TextureBlendGouraud };

    enum class Transparency { Opaque, Transparent };

    // 0: Back / 2 + Front / 2
    // 1: Back + Front
    // 2: Back - Front
    // 3: Back + Front / 4
    // -1: Transparency was previously disabled
    int m_lastBlendingMode = -1;
    Transparency m_lastTransparency;

    // We can emulate raw texture primitives as primitives with texture blending enabled
    // And 0x808080 as the blend colour
    static constexpr uint32_t c_rawTextureBlendColour = 0x808080;

    void drawTri(int *x, int *y, uint32_t *colors);
    void drawTriTextured(int *x, int *y, uint32_t *colors, uint16_t clut, uint16_t texpage, unsigned *u, unsigned *v);
    void drawRect(int x, int y, int w, int h, uint32_t color);
    void drawRectTextured(int x, int y, int w, int h, uint32_t color, uint16_t clut, unsigned u, unsigned v);
    void drawLine(int x1, int y1, uint32_t color1, int x2, int y2, uint32_t color2);

    template <Transparency setting>
    void setTransparency();

    void setBlendingModeFromTexpage(uint32_t texpage);
    void setBlendFactors(float sourceFactor, float destFactor);

    void write0(ClearCache *) override;
    void write0(FastFill *) override;

    template <Shading shading, Shape shape, Textured textured, Blend blend, Modulation modulation>
    void polyExec(Poly<shading, shape, textured, blend, modulation> *);
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

    template <Shading shading, LineType lineType, Blend blend>
    void lineExec(Line<shading, lineType, blend> *);
    void write0(Line<Shading::Flat, LineType::Simple, Blend::Off> *) override;
    void write0(Line<Shading::Flat, LineType::Simple, Blend::Semi> *) override;
    void write0(Line<Shading::Flat, LineType::Poly, Blend::Off> *) override;
    void write0(Line<Shading::Flat, LineType::Poly, Blend::Semi> *) override;
    void write0(Line<Shading::Gouraud, LineType::Simple, Blend::Off> *) override;
    void write0(Line<Shading::Gouraud, LineType::Simple, Blend::Semi> *) override;
    void write0(Line<Shading::Gouraud, LineType::Poly, Blend::Off> *) override;
    void write0(Line<Shading::Gouraud, LineType::Poly, Blend::Semi> *) override;

    template <Size size, Textured textured, Blend blend, Modulation modulation>
    void rectExec(Rect<size, textured, blend, modulation> *);
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

}  // namespace PCSX
