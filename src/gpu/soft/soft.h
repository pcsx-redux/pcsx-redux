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

#include <stdint.h>

#include "core/gpu.h"
#include "gpu/soft/raster-state.h"
#include "gpu/soft/walker.h"

namespace PCSX {

namespace SoftGPU {

struct SoftRenderer {
    SoftRenderer();
    ~SoftRenderer();

    void enableCachedDithering();
    void disableCachedDithering();
    bool m_usesDitherLUT = false;

    inline void resetRenderer() {
        m_globalTextAddrX = 0;
        m_globalTextAddrY = 0;
        m_globalTextTP = GPU::TexDepth::Tex4Bits;
        m_globalTextABR = GPU::BlendFunction::HalfBackAndHalfFront;
        m_drawX = m_drawY = 0;
        m_drawW = m_drawH = 0;
        m_checkMask = false;
        m_setMask16 = 0;
        m_setMask32 = 0;
    }

    void texturePage(GPU::TPage *prim);
    void twindow(GPU::TWindow *prim);
    void drawingAreaStart(GPU::DrawingAreaStart *prim);
    void drawingAreaEnd(GPU::DrawingAreaEnd *prim);
    void drawingOffset(GPU::DrawingOffset *prim);
    void maskBit(GPU::MaskBit *prim);
    // GP0(E6h) state, for the VRAM transfers that honour it outside the rasterizer.
    bool getCheckMask() const { return m_checkMask; }
    uint16_t getSetMask16() const { return m_setMask16; }

    bool checkCoord4(int16_t &x0, int16_t &y0, int16_t &x1, int16_t &y1, int16_t &x2, int16_t &y2, int16_t &x3,
                     int16_t &y3);
    bool checkCoord3(int16_t &x0, int16_t &y0, int16_t &x1, int16_t &y1, int16_t &x2, int16_t &y2);

    // The vertex-plus-offset sum is not wrapped: setup and the edge cull see
    // it at full width, so a vertex at 670 with offset 360 sits at 1030. The
    // wrap happens on the pixel address instead, modulo 2048 on both axes,
    // before the drawing-area clip: with offset -1000 a vertex at 1500 (-548
    // as an 11-bit vertex) lands at -1548 and its pixels show at 500. Verified
    // on SCPH-1001 and SCPH-5501 via gpu-raster-phase23 O3 and W0..W3.
    static int16_t offsetCoord(int16_t v, int16_t off) { return static_cast<int16_t>(int(v) + int(off)); }

    // Calls draw(dx, dy) once per copy of the primitive that the mod-2048
    // pixel wrap can bring into VRAM. Sums range over -2048..2046, so only
    // pixels at -2048..-1025 wrap into the 0..1023 window; everything else is
    // left to the drawing-area clip.
    template <typename Draw>
    static void forEachPixelWrap(const int16_t *xs, const int16_t *ys, int n, Draw &&draw) {
        int minX = xs[0], minY = ys[0];
        for (int i = 1; i < n; i++) {
            if (xs[i] < minX) minX = xs[i];
            if (ys[i] < minY) minY = ys[i];
        }
        const bool wrapX = minX < -1024;
        const bool wrapY = minY < -1024;
        draw(int16_t(0), int16_t(0));
        if (wrapX) draw(int16_t(2048), int16_t(0));
        if (wrapY) draw(int16_t(0), int16_t(2048));
        if (wrapX && wrapY) draw(int16_t(2048), int16_t(2048));
    }

    void applyOffset2(int16_t &x0, int16_t &y0, int16_t &x1, int16_t &y1) {
        x0 = offsetCoord(x0, m_softDisplay.DrawOffset.x);
        y0 = offsetCoord(y0, m_softDisplay.DrawOffset.y);
        x1 = offsetCoord(x1, m_softDisplay.DrawOffset.x);
        y1 = offsetCoord(y1, m_softDisplay.DrawOffset.y);
    }

    void applyOffset3(int16_t &x0, int16_t &y0, int16_t &x1, int16_t &y1, int16_t &x2, int16_t &y2) {
        x0 = offsetCoord(x0, m_softDisplay.DrawOffset.x);
        y0 = offsetCoord(y0, m_softDisplay.DrawOffset.y);
        x1 = offsetCoord(x1, m_softDisplay.DrawOffset.x);
        y1 = offsetCoord(y1, m_softDisplay.DrawOffset.y);
        x2 = offsetCoord(x2, m_softDisplay.DrawOffset.x);
        y2 = offsetCoord(y2, m_softDisplay.DrawOffset.y);
    }

    void applyOffset4(int16_t &x0, int16_t &y0, int16_t &x1, int16_t &y1, int16_t &x2, int16_t &y2, int16_t &x3,
                      int16_t &y3) {
        x0 = offsetCoord(x0, m_softDisplay.DrawOffset.x);
        y0 = offsetCoord(y0, m_softDisplay.DrawOffset.y);
        x1 = offsetCoord(x1, m_softDisplay.DrawOffset.x);
        y1 = offsetCoord(y1, m_softDisplay.DrawOffset.y);
        x2 = offsetCoord(x2, m_softDisplay.DrawOffset.x);
        y2 = offsetCoord(y2, m_softDisplay.DrawOffset.y);
        x3 = offsetCoord(x3, m_softDisplay.DrawOffset.x);
        y3 = offsetCoord(y3, m_softDisplay.DrawOffset.y);
    }

    void fillArea(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t col);

    void drawPoly3F(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t x3, int16_t y3, int32_t rgb);
    void drawPoly3G(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, int32_t rgb1, int32_t rgb2,
                    int32_t rgb3);
    template <TexMode Tex>
    void drawPoly3T(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t x3, int16_t y3, int16_t tx1, int16_t ty1,
                    int16_t tx2, int16_t ty2, int16_t tx3, int16_t ty3, int16_t clX, int16_t clY);
    template <TexMode Tex>
    void drawPoly3TG(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t x3, int16_t y3, int16_t tx1, int16_t ty1,
                     int16_t tx2, int16_t ty2, int16_t tx3, int16_t ty3, int16_t clX, int16_t clY, int32_t col1,
                     int32_t col2, int32_t col3);

    void drawPoly4G(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t x3, int16_t y3,
                    int32_t rgb1, int32_t rgb2, int32_t rgb3, int32_t rgb4);
    template <TexMode Tex>
    void drawPoly4TG(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t x3, int16_t y3, int16_t x4, int16_t y4,
                     int16_t tx1, int16_t ty1, int16_t tx2, int16_t ty2, int16_t tx3, int16_t ty3, int16_t tx4,
                     int16_t ty4, int16_t clX, int16_t clY, int32_t col1, int32_t col2, int32_t col3, int32_t col4);

    template <PCSX::GPU::Shading Shading>
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int32_t rgb0, int32_t rgb1 = 0);
    void drawRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t col);
    // Sprite/textured-rectangle rasterizer. Axis-aligned by definition
    // (GP0 0x64-0x67, 0x74-0x77 etc.); 1:1 UV-to-screen step, no fractional
    // edges. The dedicated implementation drops the edge-walker entirely
    // and walks an integer-aligned blit rectangle. Replaces the previous
    // drawPoly4T<Semi> dispatch from the textured-rect path.
    template <TexMode Tex>
    void drawSprite(int16_t x, int16_t y, int16_t w, int16_t h, int16_t u, int16_t v, int16_t clX, int16_t clY);

    struct Point {
        int32_t x;
        int32_t y;
    };

    struct ShortPoint {
        int16_t x;
        int16_t y;
    };

    struct SoftRect {
        int16_t x0;
        int16_t x1;
        int16_t y0;
        int16_t y1;
    };

    struct SoftDisplay {
        Point DisplayModeNew;
        Point DisplayMode;
        Point DisplayPosition;
        Point DisplayEnd;

        int32_t Double;
        int32_t Height;
        int32_t PAL;
        int32_t InterlacedNew;
        int32_t Interlaced;
        bool RGB24New;
        bool RGB24;
        ShortPoint DrawOffset;
        int32_t Disabled;
        SoftRect Range;
    };

    int m_useDither = 0;
    uint16_t *m_vram16;
    uint8_t *m_vram;
    // Effective drawable VRAM height. Equals the full 1024-row buffer only when
    // a 2MB bank is fitted AND the gate is open; otherwise the retail 512. Kept
    // in sync by impl::vramConfigChanged() and used as the rasterizer's vertical
    // bound so drawing/fills reach the upper bank only when it is genuinely
    // addressable. Public (like m_vram16) so impl can sync it across the
    // private SoftRenderer inheritance.
    int m_vramHeight = VRAM_HEIGHT;
    // Row mask for the texture page and CLUT bases of a textured primitive:
    // 0x1ff with the GP1(09h) gate closed, where every VRAM access wraps Y at
    // 9 bits, and 0x3ff with it open, where Y>=512 reaches the upper bank (or
    // the open-bus prefill on a 1MB fitment). Synced with m_vramHeight.
    int m_vramYMask = VRAM_Y_MASK;
    SoftDisplay m_softDisplay;
    int32_t m_statusRet;
    bool m_disableTexturesInPolygons = false;
    bool m_disableTexturesInRectangles = false;

    bool m_drawSemiTrans = false;
    bool m_ditherMode = false;
    // Row parity drawing primitives must not write (see RasterState::skipParity), or -1. Set from the
    // emulation thread, in primitive order, by impl::updateSkipParity().
    int8_t m_skipParity = -1;
    int16_t m_m1 = 255, m_m2 = 255, m_m3 = 255;
    GPU::TexDepth m_globalTextTP;

  private:
    SoftRect m_textureWindow;
    // Bit-substitution form of the GP0(E2) texture window, computed in
    // twindow() and consumed by Sampler<TexMode>. mask = mask_field * 8
    // (the bits of u/v the window overwrites); off = (off_field * 8) &
    // mask (pre-masked bits that get OR'd back in). See twindow() for the
    // hardware citation.
    uint8_t m_textureWindowMaskU = 0;
    uint8_t m_textureWindowMaskV = 0;
    uint8_t m_textureWindowOffU = 0;
    uint8_t m_textureWindowOffV = 0;
    int m_drawX, m_drawY, m_drawW, m_drawH;

    int32_t m_globalTextAddrX;
    int32_t m_globalTextAddrY;
    GPU::BlendFunction m_globalTextABR;

    bool m_checkMask = false;
    uint16_t m_setMask16 = 0;
    uint32_t m_setMask32 = 0;

    int16_t m_yMin;
    int16_t m_yMax;

    template <bool useCachedDither>
    void applyShadeDither(uint16_t *pdest, int32_t m1, int32_t m2, int32_t m3, bool semiTrans, uint16_t sM);
    template <bool useCachedDither>
    void getShadeTransColDither(uint16_t *pdest, int32_t m1, int32_t m2, int32_t m3);
    template <bool useCachedDither>
    void getTextureTransColShadeDither(uint16_t *pdest, uint16_t color, int32_t m1, int32_t m2, int32_t m3);

    template <bool HasUV, PCSX::GPU::Shading Shading, TexMode Tex, bool useCachedDither>
    void drawPoly3Raster(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t x3, int16_t y3, int16_t tx1,
                         int16_t ty1, int16_t tx2, int16_t ty2, int16_t tx3, int16_t ty3, int16_t clX, int16_t clY,
                         int32_t rgb1, int32_t rgb2, int32_t rgb3);

    template <TexMode Tex, bool useCachedDither>
    void drawPoly3TGi(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t x3, int16_t y3, int16_t tx1, int16_t ty1,
                      int16_t tx2, int16_t ty2, int16_t tx3, int16_t ty3, int16_t clX, int16_t clY, int32_t col1,
                      int32_t col2, int32_t col3);

    // Unified Bresenham line-octant rasterizers. One body handles all four
    // canonical post-dispatch octants per shading mode; the template
    // parameters select major axis, major-axis sign (Y-major only), minor
    // step direction, and the hardware-load-bearing initial-d bias. See
    // the Line namespace block above for the bias-vs-axis contract.
    template <Line::Axis MajorAxis, Line::MajorSign MaSign, Line::MinorSign MiSign, Line::Bias B,
              PCSX::GPU::Shading Shading>
    void drawLineOctant(int x0, int y0, int x1, int y1, uint32_t rgb0, uint32_t rgb1 = 0);
    // Axis-aligned line variants. Iter selects the iteration axis: X for
    // horizontal lines (y is fixed, x walks varStart..varEnd), Y for
    // vertical lines (x is fixed, y walks varStart..varEnd). Callers must
    // pass the range so that varStart <= varEnd; the dispatcher already
    // performs that swap on dispatch.
    template <Line::Axis Iter, PCSX::GPU::Shading Shading>
    void drawAxisLine(int constCoord, int varStart, int varEnd, uint32_t rgb0, uint32_t rgb1 = 0);

  private:
    // Primitive draw-area rejection. Returns true when the primitive should
    // be skipped: either every vertex sits past one edge of the draw rect
    // (so nothing inside the rect can be covered), or the rect itself is
    // degenerate. The caller passes parallel arrays of the primitive's
    // vertex x and y coordinates; N is the vertex count (3 for triangles,
    // 4 for quads). The comparisons use the renderer's m_drawX/Y/W/H rect
    // members directly - the per-primitive bodies that captured these into
    // locals are unaffected because the compiler folds the loads back.
    template <size_t N>
    inline bool primitiveOutsideDrawArea(const int16_t (&xs)[N], const int16_t (&ys)[N]) const {
        if (m_drawY >= m_drawH) return true;
        if (m_drawX >= m_drawW) return true;
        bool allR = true, allD = true, allL = true, allA = true;
        for (size_t i = 0; i < N; i++) {
            if (xs[i] <= m_drawW) allR = false;
            if (ys[i] <= m_drawH) allD = false;
            if (xs[i] >= m_drawX) allL = false;
            if (ys[i] >= m_drawY) allA = false;
        }
        return allR || allD || allL || allA;
    }

    // RasterState builders: capture the renderer's stable per-primitive state
    // into a value type so the inner loops read from a single struct instead
    // of repeatedly dereferencing renderer members.
    //
    // makeBaseRasterState() fills the fields every primitive needs: VRAM
    // pointers, ABR, the mask-write policy (checkMask + setMask16/32), and
    // the drawSemiTrans toggle. Untextured paths (fills, lines, flat
    // triangles, gouraud triangles without sampling) need nothing more.
    //
    // makeTexturedRasterState<Tex>(drawX, drawY, drawW, drawH, clX, clY)
    // adds the texture window, the texture page base,
    // the draw rect, the per-call modulation factors, and the CLUT pointer.
    // clutP is set to zero for Direct15 (the field is unused), and to
    // `(clY << 10) + clX` for the CLUT4 / CLUT8 modes.
    inline RasterState makeBaseRasterState() const {
        RasterState rs{};
        rs.vram = m_vram;
        rs.vram16 = m_vram16;
        rs.abr = m_globalTextABR;
        rs.checkMask = m_checkMask;
        rs.setMask16 = m_setMask16;
        rs.setMask32 = m_setMask32;
        rs.drawSemiTrans = m_drawSemiTrans;
        rs.skipParity = m_skipParity;
        return rs;
    }

    template <TexMode Tex>
    inline RasterState makeTexturedRasterState(int drawX, int drawY, int drawW, int drawH, int clX, int clY) const {
        RasterState rs{};
        rs.vram = m_vram;
        rs.vram16 = m_vram16;
        rs.texWindowX0 = m_textureWindowOffU;
        rs.texWindowY0 = m_textureWindowOffV;
        // Store the 8-bit complement as the keep-mask: the uint8_t truncation
        // yields 0xff & ~mask, folding the texture-window passthrough and the
        // mod-256 texel-coordinate wrap into a single per-texel AND.
        rs.texMaskX = (uint8_t)~m_textureWindowMaskU;
        rs.texMaskY = (uint8_t)~m_textureWindowMaskV;
        rs.texBaseX = m_globalTextAddrX;
        rs.texBaseY = m_globalTextAddrY & m_vramYMask;
        rs.abr = m_globalTextABR;
        rs.drawX = drawX;
        rs.drawY = drawY;
        rs.drawW = drawW;
        rs.drawH = drawH;
        rs.checkMask = m_checkMask;
        rs.setMask16 = m_setMask16;
        rs.setMask32 = m_setMask32;
        rs.drawSemiTrans = m_drawSemiTrans;
        rs.skipParity = m_skipParity;
        rs.m1 = m_m1;
        rs.m2 = m_m2;
        rs.m3 = m_m3;
        if constexpr (Tex == TexMode::Direct15) {
            rs.clutP = 0;
        } else {
            rs.clutP = ((clY & m_vramYMask) << 10) + clX;
        }
        return rs;
    }

    // Unified 3-vertex edge walkers. Four (HasUV, HasRGB) instantiations cover
    // the legacy Flat3 / Shade3 / FlatTextured3 / ShadeTextured3 family. The
    // m_deltaRight{U,V,R,G,B} members below are the X-direction span gradients
    // shared by those triangle walkers.
    template <bool HasUV, bool HasRGB>
    bool setupSections3(const TriInput &in);
    template <bool HasUV, bool HasRGB>
    int leftSection3();
    template <bool HasUV, bool HasRGB>
    int rightSection3();
    template <bool HasUV, bool HasRGB>
    bool nextRow3();

    struct SoftVertex {
        int x, y;
        int u, v;
        int32_t R, G, B;
    };

    SoftVertex m_vtx[4];
    SoftVertex *m_leftArray[4], *m_rightArray[4];
    int m_leftSection, m_rightSection;
    // Per-section row counters. m_*SectionHeight is the down-counter starting
    // at the section's full height; m_*SectionFullHeight is the constant
    // divisor for the per-row recompute formula below.
    int m_leftSectionHeight, m_leftSectionFullHeight;
    int m_rightSectionHeight, m_rightSectionFullHeight;
    // Per-row Y-axis edge state. The section-relative row index is
    // (m_*SectionFullHeight - m_*SectionHeight) after the down-counter has
    // been decremented in nextRow*. The current value is recomputed each row
    // as m_*Start + (int64_t)m_*Diff * row / m_*SectionFullHeight, which is
    // bit-exact against hardware (one truncation per row, no accumulator
    // drift) instead of the legacy +=quotient stepper.
    int m_leftX, m_leftStartX, m_leftDiffX;
    int m_leftU, m_leftStartU, m_leftDiffU;
    int m_leftV, m_leftStartV, m_leftDiffV;
    int m_leftR, m_leftStartR, m_leftDiffR;
    int m_leftG, m_leftStartG, m_leftDiffG;
    int m_leftB, m_leftStartB, m_leftDiffB;
    int m_rightX, m_rightStartX, m_rightDiffX;
    // 3-vert X-direction span gradients: set once by setupSections3 via
    // shl10idiv, read by drawPoly3{T,Gi,TG} as the per-pixel-X stride for
    // texture coordinates and gouraud channels.
    int m_deltaRightU, m_deltaRightV;
    int m_deltaRightR, m_deltaRightG, m_deltaRightB;

    static constexpr inline int shl10idiv(int x, int y) {
        int64_t bi = x;
        bi <<= 10;
        return bi / y;
    }
};

}  // namespace SoftGPU

}  // namespace PCSX
