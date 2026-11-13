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

#include "gpu_opengl.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

#include "core/debug.h"
#include "core/psxemulator.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "support/imgui-helpers.h"
#include "tracy/Tracy.hpp"

std::unique_ptr<PCSX::GPU> PCSX::GPU::getOpenGL() { return std::unique_ptr<PCSX::GPU>(new PCSX::OpenGL_GPU()); }

void PCSX::OpenGL_GPU::resetBackend() {
    m_gpustat = 0x14802000;
    m_setMask = false;
    m_checkMask = false;
    m_maskDirty = true;
    m_batchBoundsValid = false;

    m_lastTransparency = Transparency::Opaque;
    m_lastBlendingMode = -1;
    m_drawMode = 0;
    m_rectTexpage = 0;
    m_vertexCount = 0;
    m_syncVRAM = true;
    m_display.reset();

    m_drawAreaLeft = m_drawAreaTop = 0;
    m_drawAreaBottom = vramHeight;
    m_drawAreaRight = vramWidth;
    updateDrawArea();

    m_drawingOffset = OpenGL::ivec2(0, 0);

    m_vramConfigDirty = true;

    m_program.use();
    setDrawOffset(0x00000000);
    setTexWindowUnchecked(0x00000000);
    // Semi-transparency is blended in the shader, so GL blending only ever passes the fragment through.
    setBlendFactors(1.0, 0.0);
    setDisplayEnable(false);

    clearVRAM();
}

void PCSX::OpenGL_GPU::clearVRAM(float r, float g, float b, float a) {
    const auto oldFBO = OpenGL::getDrawFramebuffer();
    const auto oldScissor = OpenGL::scissorEnabled();

    m_fbo.bind(OpenGL::DrawFramebuffer);
    OpenGL::disableScissor();
    OpenGL::setClearColor(r, g, b, a);
    OpenGL::clearColor();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldFBO);

    if (oldScissor) OpenGL::enableScissor();
}

void PCSX::OpenGL_GPU::clearVRAM() { clearVRAM(0.f, 0.f, 0.f, 0.f); }

// Do not forget to call this with an active OpenGL context.
int PCSX::OpenGL_GPU::initBackend(UI *ui) {
    m_gui = dynamic_cast<GUI *>(ui);
    // Reserve some size for vertices & vram transfers to avoid dynamic allocations later.
    m_vertices.resize(vertexBufferSize);

    m_vbo.createFixedSize(sizeof(Vertex) * vertexBufferSize, GL_STREAM_DRAW);
    m_vbo.bind();
    m_vao.create();
    m_vao.bind();

    // Position (x and y coord) attribute. Signed 11-bit numbers
    m_vao.setAttributeInt<GLint>(0, 2, sizeof(Vertex), offsetof(Vertex, positions));
    m_vao.enableAttribute(0);
    // Colour attribute
    m_vao.setAttributeInt<GLuint>(1, 1, sizeof(Vertex), offsetof(Vertex, colour));
    m_vao.enableAttribute(1);
    // CLUT attribute
    m_vao.setAttributeInt<GLushort>(2, 1, sizeof(Vertex), offsetof(Vertex, clut));
    m_vao.enableAttribute(2);
    // Texpage attribute
    m_vao.setAttributeInt<GLushort>(3, 1, sizeof(Vertex), offsetof(Vertex, texpage));
    m_vao.enableAttribute(3);
    // UV attribute
    m_vao.setAttributeFloat<GLushort>(4, 2, sizeof(Vertex), offsetof(Vertex, uv));
    m_vao.enableAttribute(4);
    // Affine UV walk setup, as six ivec4
    for (unsigned i = 0; i < c_affineSetupSize / 4; i++) {
        m_vao.setAttributeInt<GLint>(5 + i, 4, sizeof(Vertex), offsetof(Vertex, affine) + i * 4 * sizeof(int32_t));
        m_vao.enableAttribute(5 + i);
    }

    // Make VRAM texture and attach it to draw frambuffer
    const int msaaSampleCount = g_emulator->settings.get<Emulator::SettingMSAA>();
    if (msaaSampleCount > 1 && glTexStorage2DMultisample != nullptr) {
        m_vramTexture.createMSAA(vramWidth, vramHeight, GL_RGBA8, msaaSampleCount);
        m_fbo.createWithTextureMSAA(m_vramTexture);

        m_vramTextureNoMSAA.create(vramWidth, vramHeight, GL_RGBA8);
        m_fboNoMSAA.createWithTexture(m_vramTextureNoMSAA);
        m_multisampled = true;
    } else {
        m_vramTexture.create(vramWidth, vramHeight, GL_RGBA8);
        m_fbo.createWithTexture(m_vramTexture);
        m_multisampled = false;
    }

    m_maskTexture.create(vramWidth, vramHeight, GL_RGBA8);
    m_sampleTexture.create(vramWidth, vramHeight, GL_RGBA8);

    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Non-complete framebuffer");
    }

    // Create a plain black texture for when the display is disabled
    // Since GL can automatically repeat textures for us, we can create a tiny texture for this
    OpenGL::Framebuffer dummyFBO;
    m_blankTexture.create(8, 8, GL_RGBA8);
    dummyFBO.createWithDrawTexture(m_blankTexture);  // Create FBO and bind our texture to it
    dummyFBO.bind(OpenGL::DrawFramebuffer);

    // Clear the texture and remove FBO
    OpenGL::setViewport(8, 8);
    OpenGL::setClearColor(0.0, 0.0, 0.0, 1.0);
    OpenGL::clearColor();
    OpenGL::bindScreenFramebuffer();

    // Without manually setting alignment, texture uploads in eg the BIOS will break if the width/x coord are odd
    // TODO: Find efficient method to handle this
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);

    static const char *vertSource = R"(
        #version 330 core

        // inPos: The vertex position.
        // inColor: The colour in BGR888. Top 8 bits are garbage and are trimmed by the vertex shader to conserve CPU time
        // inClut: The CLUT (palette) for textured primitives
        // inTexpage: The texpage. We use bit 15 for indicating an untextured primitive (1 = untextured). This
        // lets us batch untextured and textured primitives together. Bit 15 is unused by hardware, so this is a possible optimization
        // inUV: The UVs (texture coordinates) for textured primitives

        layout (location = 0) in ivec2 inPos;
        layout (location = 1) in uint inColor;
        layout (location = 2) in int inClut;
        layout (location = 3) in int inTexpage;
        layout (location = 4) in vec2 inUV;
        // inAffine: the triangle's UV walk setup, see computeAffineSetup
        layout (location = 5) in ivec4 inAffine0;
        layout (location = 6) in ivec4 inAffine1;
        layout (location = 7) in ivec4 inAffine2;
        layout (location = 8) in ivec4 inAffine3;
        layout (location = 9) in ivec4 inAffine4;
        layout (location = 10) in ivec4 inAffine5;

        out vec4 vertexColor;
        out vec2 texCoords;
        flat out ivec2 clutBase;
        flat out ivec2 texpageBase;
        flat out int texMode;
        flat out int dither;
        flat out ivec4 affine0;
        flat out ivec4 affine1;
        flat out ivec4 affine2;
        flat out ivec4 affine3;
        flat out ivec4 affine4;
        flat out ivec4 affine5;
        flat out ivec2 drawOffset;

        // We always apply a 0.5 offset in addition to the drawing offsets, to cover up OpenGL inaccuracies
        uniform vec2 u_vertexOffsets = vec2(+0.5, +0.5);

        // 1 when 2MB of VRAM is fitted, in which case texpage bit 11 is the
        // high bit of the texture page Y instead of the (unused) texture
        // disable bit.
        uniform int u_texpage2MB = 0;

        void main() {
           // Normalize coords to [0, 2]. The VRAM is 1024x1024, so the halves
           // are 512 and 512.
           float x = float(inPos.x);
           float y = float(inPos.y);
           float xx = (x + u_vertexOffsets.x) / 512.0;
           float yy = (y + u_vertexOffsets.y) / 512.0;

           // Normalize to [-1, 1]
           xx -= 1.0;
           yy -= 1.0;

           float red = float(inColor & 0xffu);
           float green = float((inColor >> 8u) & 0xffu);
           float blue = float((inColor >> 16u) & 0xffu);
           vec3 color = vec3(red, green, blue);

           gl_Position = vec4(xx, yy, 1.0, 1.0);
           vertexColor = vec4(color / 255.0, 1.0);
           dither = (inTexpage >> 14) & 1;
           affine0 = inAffine0;
           affine1 = inAffine1;
           affine2 = inAffine2;
           affine3 = inAffine3;
           affine4 = inAffine4;
           affine5 = inAffine5;
           drawOffset = ivec2(floor(u_vertexOffsets));

           if ((inTexpage & 0x8000) != 0) { // Untextured primitive
               texMode = 4;
           } else {
               texMode = (inTexpage >> 7) & 3;
               texCoords = inUV;
               int texpageY = ((inTexpage >> 4) & 0x1) | (((inTexpage >> 11) & u_texpage2MB) << 1);
               texpageBase = ivec2((inTexpage & 0xf) * 64, texpageY * 256);
               clutBase = ivec2((inClut & 0x3f) * 16, inClut >> 6);
           }
        }
    )";

    static const char *fragSource = R"(
        #version 330 core
        in vec4 vertexColor;
        in vec2 texCoords;
        flat in ivec2 clutBase;
        flat in ivec2 texpageBase;
        flat in int texMode;
        flat in int dither;
        flat in ivec4 affine0;
        flat in ivec4 affine1;
        flat in ivec4 affine2;
        flat in ivec4 affine3;
        flat in ivec4 affine4;
        flat in ivec4 affine5;
        flat in ivec2 drawOffset;

        // floor(a * d / 65536) for a in [0, 65535], without 64-bit integers.
        int mulShr16(int a, int d) {
            return a * (d >> 16) + int((uint(a) * (uint(d) & 0xffffu)) >> 16u);
        }

        // The texel coordinates of this pixel, walked in 16.16 the way the GPU does (computeAffineSetup
        // holds the per-triangle half). Unsigned arithmetic wraps like the GPU's 32-bit accumulators.
        ivec2 affineUV() {
            ivec2 p = ivec2(gl_FragCoord.xy) - drawOffset;
            bool second = affine2.w > 0 && p.y >= affine2.z;
            int yStart = second ? affine2.z : affine0.x;
            int height = second ? affine2.w : affine0.y;
            int startX = second ? affine3.x : affine0.z;
            int quotientX = second ? affine3.y : affine0.w;
            int remainderX = second ? affine3.z : affine1.x;
            int signX = second ? affine3.w : affine1.y;
            ivec4 uv = second ? affine4 : ivec4(affine1.zw, affine2.xy);
            int row = p.y - yStart;
            int leftX = startX + signX * (quotientX * row + (remainderX * row) / height);
            int xmin = (leftX + 0xffff) >> 16;
            int fraction = (xmin << 16) - leftX;
            uint u = uint(uv.x) + uint(row) * uint(uv.y);
            uint v = uint(uv.z) + uint(row) * uint(uv.w);
            u += uint(mulShr16(fraction, affine5.x)) + 0x8000u + uint(p.x - xmin) * uint(affine5.x);
            v += uint(mulShr16(fraction, affine5.y)) + 0x8000u + uint(p.x - xmin) * uint(affine5.y);
            return ivec2(int(u >> 16), int(v >> 16)) & ivec2(0xff);
        }

        // We use dual-source blending in order to emulate the fact that the GPU can enable blending per-pixel
        // FragColor: The colour of the pixel before alpha blending comes into play
        // BlendColor: Contains blending coefficients
        layout(location = 0, index = 0) out vec4 FragColor;
        layout(location = 0, index = 1) out vec4 BlendColor;

        // Tex window uniform format
        // x, y components: masks to & coords with
        // z, w components: masks to | coords with
        uniform ivec4 u_texWindow;
        uniform sampler2D u_vramTex;
        uniform vec4 u_blendFactors;
        uniform vec4 u_blendFactorsIfOpaque = vec4(1.0, 1.0, 1.0, 0.0);
        // 511 when only the lower bank is addressable (1MB, or 2MB with the
        // GP1(09h) gate closed - the upper rows then mirror onto the lower
        // ones), 1023 when the full 2MB is reachable.
        uniform int u_vramMaskY = 511;
        // NTSC 480i with GP0(E1h).10 clear: the VRAM row parity not to write, or -1.
        uniform int u_skipParity = -1;
        // GP0(E6h). The mask bit is stored in alpha. With check-mask on, u_maskTex holds the
        // destination as it was before the batch.
        uniform float u_setMask = 0.0;
        uniform int u_checkMask = 0;
        uniform sampler2D u_maskTex;

        int floatToU5(float f) {
            return int(floor(f * 31.0 + 0.5));
        }

        vec4 sampleVRAM(ivec2 coords) {
            coords &= ivec2(1023, u_vramMaskY); // Out-of-bounds VRAM accesses wrap
            return texelFetch(u_vramTex, coords, 0);
        }

        int sample16(ivec2 coords) {
            vec4 colour = sampleVRAM(coords);
            int r = floatToU5(colour.r);
            int g = floatToU5(colour.g);
            int b = floatToU5(colour.b);
            int msb = int(ceil(colour.a)) << 15;
            return r | (g << 5) | (b << 10) | msb;
        }

        // 1 when the primitive is semi-transparent.
        uniform int u_blending = 0;
        // GP0(E1h) bits 5-6 of a semi-transparent primitive.
        uniform int u_blendMode = 0;

        // The 8-bit vertex colour as an integer.
        vec3 colour8(vec3 c) {
            return floor(c * 255.0 + 0.5);
        }

        // 4x4 dither offsets, added to the 8-bit colour before it drops to 5 bits, indexed by the
        // VRAM coordinates of the pixel.
        const int ditherTable[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

        // The GPU writes 5 bits per channel, dropping the low 3 bits of an 8-bit colour, after the
        // dither offset if the primitive is dithered. Writing k / 31 keeps VRAM 5-bit exact through
        // the RGBA8 framebuffer.
        vec3 to5(ivec3 c8) {
            if (dither != 0) {
                ivec2 p = ivec2(gl_FragCoord.xy) & 3;
                c8 += ditherTable[p.y * 4 + p.x];
            }
            return vec3(clamp(c8, ivec3(0), ivec3(255)) >> 3) / 31.0;
        }

        // Semi-transparency is computed here rather than by GL blending: u_maskTex then holds the
        // destination as it was before the batch, which overlap flushes keep current. The math is the
        // GPU's in 5 bits per channel, with front = F and front / 4 = Fq, saturated to 31. A dithered
        // primitive blends in 8 bits instead, the destination shifted up and front = F8, and is
        // dithered before dropping to 5 bits.
        vec3 semiBlend(ivec3 F, ivec3 Fq, ivec3 F8) {
            vec4 dest = texelFetch(u_maskTex, ivec2(gl_FragCoord.xy), 0);
            ivec3 B = ivec3(floor(dest.rgb * 31.0 + 0.5));
            if (dither != 0) {
                ivec3 B8 = B << 3;
                ivec3 result8;
                if (u_blendMode == 0) {
                    result8 = (B8 + F8) >> 1;
                } else if (u_blendMode == 1) {
                    result8 = B8 + F8;
                } else if (u_blendMode == 2) {
                    result8 = max(B8 - F8, ivec3(0));
                } else {
                    result8 = B8 + (F8 >> 2);
                }
                return to5(min(result8, ivec3(255)));
            }
            ivec3 result;
            if (u_blendMode == 0) {
                result = (B + F) >> 1;
            } else if (u_blendMode == 1) {
                result = B + F;
            } else if (u_blendMode == 2) {
                result = max(B - F, ivec3(0));
            } else {
                result = B + Fq;
            }
            return vec3(min(result, ivec3(31))) / 31.0;
        }

        vec3 untexturedColour(vec3 c, bool semi) {
            ivec3 c8 = ivec3(colour8(c));
            if (semi) return semiBlend(c8 >> 3, c8 >> 5, c8);
            return to5(c8);
        }

        // Apply texture blending: (texel * colour) / 128 per channel, with a 5-bit texel and an 8-bit
        // colour, truncated and saturated to 5 bits. The 8-bit intermediate is (texel * colour) / 16.
        vec4 texBlend(vec4 colour1, vec4 colour2, bool semi) {
            vec4 ret;
            ivec3 texel5 = ivec3(floor(colour1.rgb * 31.0 + 0.5));
            ivec3 c8 = ivec3(colour8(colour2.rgb));
            if (semi) {
                ret.rgb = semiBlend((texel5 * c8) >> 7, ((texel5 >> 2) * c8) >> 7, (texel5 * c8) >> 4);
            } else {
                ret.rgb = to5((texel5 * c8) >> 4);
            }
            // The texel's mask bit is written through, ORed with set-mask.
            ret.a = max(step(0.5, colour1.a), u_setMask);
            return ret;
        }

        void main() {
           if ((int(gl_FragCoord.y) & 1) == u_skipParity) discard;
           if (u_checkMask != 0 && texelFetch(u_maskTex, ivec2(gl_FragCoord.xy), 0).a >= 0.5) discard;
           if (texMode == 4) { // Untextured primitive
               FragColor = vec4(untexturedColour(vertexColor.rgb, u_blending != 0), u_setMask);
               BlendColor = u_blendFactors;
               return;
           }

           // Fix up UVs and apply texture window
           // Triangles walk their UVs like the GPU; rectangles map texels to pixels one to one.
           ivec2 UV = affine0.y > 0 ? affineUV() : ivec2(floor(texCoords + vec2(0.0001, 0.0001))) & ivec2(0xff);
           UV = (UV & u_texWindow.xy) | u_texWindow.zw;

           if (texMode == 0) { // 4bpp texture
               ivec2 texelCoord = ivec2(UV.x >> 2, UV.y) + texpageBase;

               int sample = sample16(texelCoord);
               int shift = (UV.x & 3) << 2;
               int clutIndex = (sample >> shift) & 0xf;

               ivec2 sampleCoords = ivec2(clutBase.x + clutIndex, clutBase.y);
               FragColor = sampleVRAM(sampleCoords);

               // Only 0x0000 is transparent: black with bit 15 set (0x8000) is drawn.
               if (FragColor.rgb == vec3(0.0, 0.0, 0.0) && FragColor.a < 0.5) discard;
               bool semi = u_blending != 0 && FragColor.a >= 0.5;
               BlendColor = FragColor.a >= 0.5 ? u_blendFactors : u_blendFactorsIfOpaque;
               FragColor = texBlend(FragColor, vertexColor, semi);
           } else if (texMode == 1) { // 8bpp texture
               ivec2 texelCoord = ivec2(UV.x >> 1, UV.y) + texpageBase;

               int sample = sample16(texelCoord);
               int shift = (UV.x & 1) << 3;
               int clutIndex = (sample >> shift) & 0xff;

               ivec2 sampleCoords = ivec2(clutBase.x + clutIndex, clutBase.y);
               FragColor = sampleVRAM(sampleCoords);

               if (FragColor.rgb == vec3(0.0, 0.0, 0.0) && FragColor.a < 0.5) discard;
               bool semi = u_blending != 0 && FragColor.a >= 0.5;
               BlendColor = FragColor.a >= 0.5 ? u_blendFactors : u_blendFactorsIfOpaque;
               FragColor = texBlend(FragColor, vertexColor, semi);
           } else { // Texture depth 2 and 3 both indicate 16bpp textures
               ivec2 texelCoord = UV + texpageBase;
               FragColor = sampleVRAM(texelCoord);

               if (FragColor.rgb == vec3(0.0, 0.0, 0.0) && FragColor.a < 0.5) discard;
               // As with CLUT entries, only texels with bit 15 set are semi-transparent.
               bool semi = u_blending != 0 && FragColor.a >= 0.5;
               BlendColor = FragColor.a >= 0.5 ? u_blendFactors : u_blendFactorsIfOpaque;
               FragColor = texBlend(FragColor, vertexColor, semi);
           }
        }
    )";

    m_shaderEditor.init();
    m_shaderEditor.reset(m_gui);
    m_shaderEditor.setText(vertSource, fragSource, "");
    auto status = m_shaderEditor.compile(m_gui);
    if (!status.isOk()) return -1;
    m_program.setProgram(m_shaderEditor.getProgram());

    m_program.use();
    m_drawingOffsetLoc = OpenGL::uniformLocation(m_program, "u_vertexOffsets");
    m_texWindowLoc = OpenGL::uniformLocation(m_program, "u_texWindow");
    m_blendFactorsLoc = OpenGL::uniformLocation(m_program, "u_blendFactors");
    m_blendFactorsIfOpaqueLoc = OpenGL::uniformLocation(m_program, "u_blendFactorsIfOpaque");
    m_vramMaskYLoc = OpenGL::uniformLocation(m_program, "u_vramMaskY");
    m_texpage2MBLoc = OpenGL::uniformLocation(m_program, "u_texpage2MB");
    m_skipParityLoc = OpenGL::uniformLocation(m_program, "u_skipParity");
    m_skipParity = s_skipParityUnknown;
    m_vramConfigDirty = true;
    pushVRAMConfig();

    const auto vramSamplerLoc = OpenGL::uniformLocation(m_program, "u_vramTex");
    glUniform1i(vramSamplerLoc, 0);  // Make the fragment shader read from currently binded texture
    m_setMaskLoc = OpenGL::uniformLocation(m_program, "u_setMask");
    m_checkMaskLoc = OpenGL::uniformLocation(m_program, "u_checkMask");
    m_blendingLoc = OpenGL::uniformLocation(m_program, "u_blending");
    m_blendModeLoc = OpenGL::uniformLocation(m_program, "u_blendMode");
    glUniform1i(OpenGL::uniformLocation(m_program, "u_maskTex"), 1);
    m_maskDirty = true;

    m_vramTexture24.create(vramWidth, vramHeight, GL_RGBA8);
    m_fbo24.createWithDrawTexture(m_vramTexture24);
    m_shaderEditor24.init();
    m_shaderEditor24.reset(m_gui);
    m_shaderEditor24.setDefaults();
    m_shaderEditor24.setText(R"(#version 330 core

precision highp float;
layout (location = 0) in vec2 Position;
layout (location = 1) in vec2 UV;
layout (location = 2) in vec4 Color;
out vec2 Frag_UV;
out vec4 Frag_Color;
void main() {
    Frag_UV = UV;
    Frag_Color = Color;
    gl_Position = vec4(Position.xy, 0, 1);
}
)",
                             R"(#version 330 core

precision highp float;
uniform sampler2D u_vramTexture;
in vec2 Frag_UV;
in vec4 Frag_Color;
layout (location = 0) out vec4 Out_Color;

uniform int u_24shift = 0;

int texelToRaw(in vec4 t) {
    int c = (int(t.r * 31.0f + 0.5f) <<  0) |
            (int(t.g * 31.0f + 0.5f) <<  5) |
            (int(t.b * 31.0f + 0.5f) << 10) |
            (int(t.a) << 15);
    return c;
}

vec4 readTexture(in vec2 pos) {
    vec2 apos = vec2(1024.0f, 1024.0f) * pos;
    vec2 fpos = fract(apos);
    ivec2 ipos = ivec2(apos);
    vec4 ret = vec4(0.0f);
    if (pos.x > 1.0f) return ret;
    if (pos.y > 1.0f) return ret;
    if (pos.x < 0.0f) return ret;
    if (pos.y < 0.0f) return ret;

    float scale = 0.0f;
    int p = 0;
    vec4 t = texture(u_vramTexture, pos);
    int c = texelToRaw(t);

    ret.a = 1.0f;
    vec4 tb = texture(u_vramTexture, pos - vec2(1.0 / 1024.0f, 0.0f));
    vec4 ta = texture(u_vramTexture, pos + vec2(1.0 / 1024.0f, 0.0f));
    int cb = texelToRaw(tb);
    int ca = texelToRaw(ta);
    switch ((ipos.x + u_24shift) % 3) {
        case 0:
            ret.r = float((c >> 0) & 0xff) / 255.0f;
            ret.g = float((c >> 8) & 0xff) / 255.0f;
            ret.b = float((ca >> 0) & 0xff) / 255.0f;
            break;
        case 1:
            if (fpos.x < 0.5f) {
                ret.r = float((cb >> 0) & 0xff) / 255.0f;
                ret.g = float((cb >> 8) & 0xff) / 255.0f;
                ret.b = float((c >> 0) & 0xff) / 255.0f;
            } else {
                ret.r = float((c >> 8) & 0xff) / 255.0f;
                ret.g = float((ca >> 0) & 0xff) / 255.0f;
                ret.b = float((ca >> 8) & 0xff) / 255.0f;
            }
            break;
        case 2:
            ret.r = float((cb >> 8) & 0xff) / 255.0f;
            ret.g = float((c >> 0) & 0xff) / 255.0f;
            ret.b = float((c >> 8) & 0xff) / 255.0f;
            break;
    }

    return ret;
}

void main() {
    Out_Color = readTexture(Frag_UV.st);
    Out_Color.a = 1.0;
}
)",
                             R"(
local loc24shift = gl.glGetUniformLocation(shaderProgramID, 'u_24shift')

function BindAttributes(textureID, shaderProgramID, srcLocX, srcLocY, srcSizeX, srcSizeY, dstSizeX, dstSizeY, shift)
    gl.glUniform1i(loc24shift, shift)
end
)");
    status = m_shaderEditor24.compile(m_gui);
    if (!status.isOk()) return -1;

    reset();
    setOpenGLContext();
    return 0;
}

void PCSX::OpenGL_GPU::setLinearFiltering() {
    auto setting = g_emulator->settings.get<Emulator::SettingLinearFiltering>().value;
    const auto filter = setting ? GL_LINEAR : GL_NEAREST;
    const auto tex = getVRAMTexture();

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    // This function is used for texture initialization so might as well define our wrapping rules too
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_display.setLinearFiltering();
}

int PCSX::OpenGL_GPU::shutdown() { return 0; }

uint32_t PCSX::OpenGL_GPU::readStatusInternal() {
    return 0b01011110100000000000000000000000;
    // return m_gpustat;
}

bool PCSX::OpenGL_GPU::configure() {
    bool changed = false;

    if (m_shaderEditor.m_show && m_shaderEditor.draw(m_gui, "Hardware renderer shader editor")) {
        const auto status = m_shaderEditor.compile(m_gui);
        if (status.isOk()) {
            m_program.setProgram(m_shaderEditor.getProgram());
            const auto lastProgram = OpenGL::getProgram();

            m_program.use();
            m_drawingOffsetLoc = OpenGL::uniformLocation(m_program, "u_vertexOffsets");
            m_texWindowLoc = OpenGL::uniformLocation(m_program, "u_texWindow");
            m_blendFactorsLoc = OpenGL::uniformLocation(m_program, "u_blendFactors");
            m_blendFactorsIfOpaqueLoc = OpenGL::uniformLocation(m_program, "u_blendFactorsIfOpaque");
            m_vramMaskYLoc = OpenGL::uniformLocation(m_program, "u_vramMaskY");
            m_texpage2MBLoc = OpenGL::uniformLocation(m_program, "u_texpage2MB");
            m_skipParityLoc = OpenGL::uniformLocation(m_program, "u_skipParity");
            m_skipParity = s_skipParityUnknown;
            m_vramConfigDirty = true;

            const auto vramSamplerLoc = OpenGL::uniformLocation(m_program, "u_vramTex");
            glUniform1i(vramSamplerLoc, 0);  // Make the fragment shader read from currently bound texture
            m_setMaskLoc = OpenGL::uniformLocation(m_program, "u_setMask");
            m_checkMaskLoc = OpenGL::uniformLocation(m_program, "u_checkMask");
            m_blendingLoc = OpenGL::uniformLocation(m_program, "u_blending");
            m_blendModeLoc = OpenGL::uniformLocation(m_program, "u_blendMode");
            glUniform1i(OpenGL::uniformLocation(m_program, "u_maskTex"), 1);
            m_maskDirty = true;
            glUniform4f(m_blendFactorsIfOpaqueLoc, 1.0, 1.0, 1.0, 0.0);
            glUniform4f(m_blendFactorsLoc, m_blendFactors.x(), m_blendFactors.x(), m_blendFactors.x(),
                        m_blendFactors.y());

            setDrawOffset(m_lastDrawOffsetSetting);
            setTexWindowUnchecked(m_lastTexwindowSetting);
            glUseProgram(lastProgram);
        }
    }

    if (ImGui::Begin(_("OpenGL GPU configuration"), &m_showCfg)) {
        static std::function<const char *()> const c_polygonModeNames[] = {l_("Fill polygons"), l_("Wireframe"),
                                                                           l_("Vertices only")};
        constexpr OpenGL::FillMode polygonModes[] = {OpenGL::FillPoly, OpenGL::DrawWire, OpenGL::DrawPoints};

        if (ImGui::BeginCombo(_("Polygon rendering mode"), c_polygonModeNames[m_polygonModeIndex]())) {
            for (auto i = 0; i < 3; i++) {
                if (ImGui::Selectable(c_polygonModeNames[i]())) {
                    m_polygonMode = polygonModes[i];
                    m_polygonModeIndex = i;
                }
            }
            ImGui::EndCombo();
        }

        int msaaSampleCount = g_emulator->settings.get<Emulator::SettingMSAA>();
        const auto msaaString = msaaSampleCount == 1 ? _("No MSAA") : fmt::format(f_("{}x MSAA"), msaaSampleCount);

        if (ImGui::BeginCombo("MSAA", msaaString.c_str())) {
            const int maxMSAA = OpenGL::maxSamples();

            if (ImGui::Selectable(_("No MSAA"))) {
                g_emulator->settings.get<Emulator::SettingMSAA>() = 1;
                changed = true;
            }

            for (int i = 2; i <= maxMSAA; i *= 2) {
                auto str = std::to_string(i) + "x MSAA";
                if (ImGui::Selectable(str.c_str())) {
                    g_emulator->settings.get<Emulator::SettingMSAA>() = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::Checkbox(_("Use linear filtering"),
                            &g_emulator->settings.get<Emulator::SettingLinearFiltering>().value)) {
            changed = true;
            setLinearFiltering();
        }
        if (ImGui::Checkbox(_("Deinterlace"), &g_emulator->settings.get<Emulator::SettingDeinterlace>().value)) {
            changed = true;
        }
        ImGuiHelpers::ShowHelpMarker(
            _("In 480i, draw every row. With drawing to the displayed area off, the console skips the rows of the "
              "field being displayed, which makes moving images comb. Timing is unchanged."));

        ImGui::Checkbox(_("Edit OpenGL GPU shaders"), &m_shaderEditor.m_show);
        ImGui::End();
    }

    return changed;
}

void PCSX::OpenGL_GPU::debug() {
    if (ImGui::Begin(_("OpenGL GPU Debugger"), &m_showDebug)) {
        const auto width = m_display.size.x();
        const auto height = m_display.size.y();
        const auto startX = m_display.start.x();
        const auto startY = m_display.start.y();

        ImGui::Text(_("Display horizontal range: %d-%d"), startX, startX + width);
        ImGui::Text(_("Display vertical range: %d-%d"), startY, startY + height);
        ImGui::Text(_("Drawing area offset: (%d, %d)"), m_drawingOffset.x(), m_drawingOffset.y());
        ImGui::Text(_("Resolution: %dx%d"), width, height);

        ImGui::ColorEdit3(_("Clear colour"), &m_clearColour[0]);
        if (ImGui::Button(_("Clear VRAM"))) {
            clearVRAM(m_clearColour.r(), m_clearColour.g(), m_clearColour.b());
        }
    }

    ImGui::End();
}

// Called at the start of a UI frame to restore context
void PCSX::OpenGL_GPU::setOpenGLContext() {
    m_vbo.bind();
    m_vao.bind();
    m_fbo.bind(OpenGL::DrawAndReadFramebuffer);
    m_sampleTexture.bind();
    OpenGL::setViewport(m_vramTexture.width(), m_vramTexture.height());
    OpenGL::enableScissor();

    if (m_polygonMode != OpenGL::FillPoly) {
        OpenGL::setFillMode(m_polygonMode);
    }

    m_program.use();
}

void PCSX::OpenGL_GPU::updateDrawArea() {
    renderBatch();
    const int left = m_drawAreaLeft;
    const int width = std::max<int>(m_drawAreaRight - left + 1, 0);
    const int top = m_drawAreaTop;
    const int height = std::max<int>(m_drawAreaBottom - m_drawAreaTop + 1, 0);

    m_scissorBox.x = left;
    m_scissorBox.y = top;
    m_scissorBox.width = width;
    m_scissorBox.height = height;
    setScissorArea();
}

// Set the OpenGL scissor based on our PS1's drawing area.
void PCSX::OpenGL_GPU::setScissorArea() {
    OpenGL::setScissor(m_scissorBox.x, m_scissorBox.y, m_scissorBox.width, m_scissorBox.height);
}

GLuint PCSX::OpenGL_GPU::getVRAMTexture() {
    if (!m_multisampled) {
        return m_vramTexture.handle();
    } else {
        return m_vramTextureNoMSAA.handle();
    }
}

// Called at the end of a frame
void PCSX::OpenGL_GPU::vblank(bool fromGui) {
    renderBatch();

    // Set the fill mode to fill before passing the OpenGL context to the GUI
    if (m_polygonMode != OpenGL::FillPoly) {
        OpenGL::setFillMode(OpenGL::FillPoly);
    }

    // Disable scissor before passing the GPU to the frontend. This is also necessary as scissor testing affects
    // glBlitFramebuffer
    OpenGL::disableScissor();
    if (m_lastTransparency == Transparency::Transparent) {
        m_lastTransparency = Transparency::Opaque;
        m_lastBlendingMode = -1;
        OpenGL::disableBlend();
    }

    // We can't draw the MSAA texture directly, so resolve it into the non-MSAA texture. This also runs with the
    // display disabled, since the VRAM viewer reads the resolved texture.
    if (m_multisampled) {
        m_fbo.bind(OpenGL::ReadFramebuffer);
        m_fboNoMSAA.bind(OpenGL::DrawFramebuffer);
        glBlitFramebuffer(0, 0, vramWidth, vramHeight, 0, 0, vramWidth, vramHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    GLuint texture;
    if (!m_display.enabled) {
        texture = m_blankTexture.handle();
    } else {
        texture = m_multisampled ? m_vramTextureNoMSAA.handle() : m_vramTexture.handle();
    }

    float startX = m_display.startNormalized.x();
    float startY = m_display.startNormalized.y();
    float width = m_display.sizeNormalized.x();
    float height = m_display.sizeNormalized.y();

    if (m_display.info.depth == CtrlDisplayMode::CD_24BITS) {
        m_fbo24.bind(OpenGL::DrawFramebuffer);
        OpenGL::setViewport(vramWidth, vramHeight);
        m_shaderEditor24.render(m_gui, texture, {0, 0}, {1, 1}, {vramWidth, vramHeight},
                                {lua_Number(m_display.start.x() * 2)});
        texture = m_vramTexture24.handle();
        width *= 1.5f;
    }

    m_gui->setViewport();
    if (!fromGui) m_gui->flip();  // Set up offscreen framebuffer before rendering

    m_gui->m_offscreenShaderEditor.render(m_gui, texture, {startX, startY}, {width, height}, m_gui->getRenderSize());
}

// Push the VRAM fitment / bank gate state down to the shaders. Called from
// renderBatch() rather than from the GP1(09h) handler because only the former
// is guaranteed to run with our OpenGL context current and our program bound.
void PCSX::OpenGL_GPU::pushVRAMConfig() {
    if (!m_vramConfigDirty) return;
    m_vramConfigDirty = false;

    const bool fitted = vram2MBFitted();
    m_program.use();
    // The upper bank is only addressable with 2MB fitted and the gate open.
    // Otherwise Y folds back into the lower 512 rows, which is what silicon
    // does and what resolveVramY() applies on the transfer paths.
    glUniform1i(m_vramMaskYLoc, (fitted && vram2MBGateOpen()) ? 1023 : 511);
    glUniform1i(m_texpage2MBLoc, fitted ? 1 : 0);
}

void PCSX::OpenGL_GPU::pushMaskConfig() {
    if (!m_maskDirty) return;
    m_maskDirty = false;
    m_program.use();
    glUniform1f(m_setMaskLoc, m_setMask ? 1.0f : 0.0f);
    glUniform1i(m_checkMaskLoc, m_checkMask ? 1 : 0);
}

// GP0(E6h) applies to the primitives that follow it, so the pending batch is drawn with the old bits.
void PCSX::OpenGL_GPU::setMaskBits(bool set, bool check) {
    if (set == m_setMask && check == m_checkMask) return;
    renderBatch();
    m_setMask = set;
    m_checkMask = check;
    m_maskDirty = true;
}

// Records the VRAM bounding box of a primitive drawn under check-mask, after the draw offset and the
// drawing area clip. The pixels a polygon, line or rectangle writes lie inside the box of its
// vertices; with MSAA, also one pixel to the right and below, for the samples taken past the right and
// bottom edges. If the box overlaps the pending batch, the batch is drawn first.
void PCSX::OpenGL_GPU::addCheckMaskBounds(const int *xs, const int *ys, int n) {
    if (!readsDestination()) return;
    // A primitive adds at most 6 vertices. Flush now if they would not fit, so the primitive and its
    // bounds land in the same batch.
    maybeRenderBatch<6>();
    const int offsetX = int32_t(m_lastDrawOffsetSetting << 21) >> 21;
    const int offsetY = int32_t(m_lastDrawOffsetSetting << 10) >> 21;
    int minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
    for (int i = 1; i < n; i++) {
        minX = std::min(minX, xs[i]);
        maxX = std::max(maxX, xs[i]);
        minY = std::min(minY, ys[i]);
        maxY = std::max(maxY, ys[i]);
    }
    const int clipLeft = int(m_scissorBox.x), clipTop = int(m_scissorBox.y);
    const int fringe = m_multisampled ? 1 : 0;
    minX = std::max(minX + offsetX, clipLeft);
    maxX = std::min(maxX + offsetX + fringe, clipLeft + int(m_scissorBox.width) - 1);
    minY = std::max(minY + offsetY, clipTop);
    maxY = std::min(maxY + offsetY + fringe, clipTop + int(m_scissorBox.height) - 1);
    if (minX > maxX || minY > maxY) return;

    if (m_batchBoundsValid) {
        if (minX <= m_batchMaxX && maxX >= m_batchMinX && minY <= m_batchMaxY && maxY >= m_batchMinY) {
            renderBatch();
        }
    }
    if (!m_batchBoundsValid) {
        m_batchBoundsValid = true;
        m_batchMinX = minX;
        m_batchMaxX = maxX;
        m_batchMinY = minY;
        m_batchMaxY = maxY;
        return;
    }
    m_batchMinX = std::min(m_batchMinX, minX);
    m_batchMaxX = std::max(m_batchMaxX, maxX);
    m_batchMinY = std::min(m_batchMinY, minY);
    m_batchMaxY = std::max(m_batchMaxY, maxY);
}

// Reads a rectangle of VRAM that does not wrap, as 16-bit pixels.
void PCSX::OpenGL_GPU::readVRAMRect(int x, int y, int w, int h, uint16_t *pixels) {
    syncVRAMForRead();
    if (m_multisampled) resolveMSAA(x, y, w, h);
    GLint readFB;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    if (m_multisampled) {
        m_fboNoMSAA.bind(OpenGL::ReadFramebuffer);
    } else {
        m_fbo.bind(OpenGL::ReadFramebuffer);
    }
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
}

void PCSX::OpenGL_GPU::renderBatch() {
    if (m_vertexCount > 0) {
        pushVRAMConfig();
        pushMaskConfig();
        glUniform1i(m_blendingLoc, m_lastTransparency == Transparency::Transparent ? 1 : 0);
        glUniform1i(m_blendModeLoc, std::max(m_lastBlendingMode, 0));
        if (readsDestination()) {
            // Snapshot the destination the batch reads for check-mask and semi-transparency. Without
            // bounds (a primitive split across a full vertex buffer), take all of VRAM.
            int x = 0, y = 0, w = vramWidth, h = vramHeight;
            if (m_batchBoundsValid) {
                x = m_batchMinX;
                y = m_batchMinY;
                w = m_batchMaxX - m_batchMinX + 1;
                h = m_batchMaxY - m_batchMinY + 1;
            }
            glActiveTexture(GL_TEXTURE1);
            m_maskTexture.bind();
            if (m_multisampled) {
                resolveMSAA(x, y, w, h);
                m_fboNoMSAA.bind(OpenGL::ReadFramebuffer);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, x, y, w, h);
                m_fbo.bind(OpenGL::ReadFramebuffer);
            } else {
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, x, y, w, h);
            }
            glActiveTexture(GL_TEXTURE0);
        }
        m_batchBoundsValid = false;
        if (m_syncVRAM) {
            m_syncVRAM = false;
            if (m_multisampled) {
                // glCopyTexSubImage2D cannot read a multisampled framebuffer, so copy from the resolved one.
                resolveMSAA(0, 0, vramWidth, vramHeight);
                m_fboNoMSAA.bind(OpenGL::ReadFramebuffer);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vramWidth, vramHeight);
                m_fbo.bind(OpenGL::ReadFramebuffer);
            } else {
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vramWidth, vramHeight);
            }
        }

        if (m_updateDrawOffset) {
            m_updateDrawOffset = false;
            setDrawOffset(m_lastDrawOffsetSetting);
        }
        m_vbo.bufferVertsSub(&m_vertices[0], m_vertexCount);
        OpenGL::draw(OpenGL::Triangles, m_vertexCount);
        m_vertexCount = 0;
    }
}

void PCSX::OpenGL_GPU::setDisplayEnable(bool enabled) { m_display.enabled = enabled; }

// Resolves a rectangle of the multisampled VRAM into m_vramTextureNoMSAA. Framebuffer bindings and the
// scissor test are put back afterwards, since this runs from both the draw path and getVRAM().
void PCSX::OpenGL_GPU::resolveMSAA(GLint x, GLint y, GLint w, GLint h) {
    GLint drawFB, readFB;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    const bool scissor = OpenGL::scissorEnabled();
    OpenGL::disableScissor();  // Scissor testing affects glBlitFramebuffer
    m_fbo.bind(OpenGL::ReadFramebuffer);
    m_fboNoMSAA.bind(OpenGL::DrawFramebuffer);
    glBlitFramebuffer(x, y, x + w, y + h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
    if (scissor) OpenGL::enableScissor();
}

// Copies a rectangle of m_vramTextureNoMSAA into the multisampled VRAM. A blit from a single-sample
// framebuffer writes each pixel to every sample. Leaves m_fbo bound for drawing and reading.
void PCSX::OpenGL_GPU::blitFromResolved(GLint srcX, GLint srcY, GLint dstX, GLint dstY, GLint w, GLint h) {
    const bool scissor = OpenGL::scissorEnabled();
    OpenGL::disableScissor();
    m_fboNoMSAA.bind(OpenGL::ReadFramebuffer);
    m_fbo.bind(OpenGL::DrawFramebuffer);
    glBlitFramebuffer(srcX, srcY, srcX + w, srcY + h, dstX, dstY, dstX + w, dstY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    m_fbo.bind(OpenGL::DrawAndReadFramebuffer);
    if (scissor) OpenGL::enableScissor();
}

// Draws the pending batch so getVRAM() sees it. getVRAM() is called from the GP0(C0h) path with our
// state bound, but also from the GUI, save states, Lua, the web server and the GPU dumper, which may
// hold other bindings, so our state is bound for the draw and the caller's is put back afterwards.
void PCSX::OpenGL_GPU::syncVRAMForRead() {
    if (m_vertexCount == 0) return;

    GLint program, vao, arrayBuffer, drawFB, readFB, activeTexture, tex2D;
    GLint viewport[4], scissorBox[4];
    // Core profiles may return a single value for GL_POLYGON_MODE.
    GLint polygonMode[2] = {GL_FILL, -1};
    GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha, blendEqRGB, blendEqAlpha;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    glGetIntegerv(GL_POLYGON_MODE, polygonMode);
    if (polygonMode[1] == -1) polygonMode[1] = polygonMode[0];
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
    const bool scissor = OpenGL::isEnabled(GL_SCISSOR_TEST);
    const bool blend = OpenGL::isEnabled(GL_BLEND);
    // The GPU never touches these and draws assuming their defaults.
    const GLenum defaultedCaps[] = {GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST};
    bool capsEnabled[3];
    for (unsigned i = 0; i < 3; i++) {
        capsEnabled[i] = OpenGL::isEnabled(defaultedCaps[i]);
        glDisable(defaultedCaps[i]);
    }
    GLboolean colorMask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // renderBatch() binds the check-mask snapshot on unit 1.
    GLint tex2DUnit1;
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2DUnit1);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2D);

    setOpenGLContext();
    OpenGL::setFillMode(m_polygonMode);
    setScissorArea();
    if (m_lastTransparency == Transparency::Transparent) {
        OpenGL::enableBlend();
    } else {
        OpenGL::disableBlend();
    }
    // Same blend state setBlendingModeFromTexpage() leaves.
    if (m_lastBlendingMode >= 0) {
        OpenGL::setBlendFactor(GL_SRC1_COLOR, GL_SRC1_ALPHA, GL_ONE, GL_ZERO);
        OpenGL::setBlendEquation(OpenGL::BlendEquation::Add);
    }
    renderBatch();

    glUseProgram(program);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
    glBindTexture(GL_TEXTURE_2D, tex2D);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex2DUnit1);
    glActiveTexture(activeTexture);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    if (polygonMode[0] == polygonMode[1]) {
        glPolygonMode(GL_FRONT_AND_BACK, polygonMode[0]);
    } else {
        glPolygonMode(GL_FRONT, polygonMode[0]);
        glPolygonMode(GL_BACK, polygonMode[1]);
    }
    glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
    glBlendEquationSeparate(blendEqRGB, blendEqAlpha);
    if (scissor) {
        OpenGL::enableScissor();
    } else {
        OpenGL::disableScissor();
    }
    if (blend) {
        OpenGL::enableBlend();
    } else {
        OpenGL::disableBlend();
    }
    for (unsigned i = 0; i < 3; i++) {
        if (capsEnabled[i]) glEnable(defaultedCaps[i]);
    }
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
}

PCSX::Slice PCSX::OpenGL_GPU::getVRAM(Ownership) {
    static constexpr uint32_t texSize = vramWidth * vramHeight * sizeof(uint16_t);
    uint16_t *pixels = (uint16_t *)malloc(texSize);
    syncVRAMForRead();
    if (m_multisampled) resolveMSAA(0, 0, vramWidth, vramHeight);
    glFlush();
    const auto oldTex = OpenGL::getTex2D();
    glBindTexture(GL_TEXTURE_2D, getVRAMTexture());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);
    glBindTexture(GL_TEXTURE_2D, oldTex);

    Slice slice;
    slice.acquire(pixels, texSize);
    return slice;
}

void PCSX::OpenGL_GPU::partialUpdateVRAM(int x, int y, int w, int h, const uint16_t *pixels,
                                         PartialUpdateVram updateType) {
    renderBatch();

    if (m_multisampled) {
        // A multisampled texture takes no texel uploads: upload into the resolved copy, then blit the
        // rectangle into the multisampled VRAM.
        const auto oldTex = OpenGL::getTex2D();
        m_vramTextureNoMSAA.bind();
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);
        glBindTexture(GL_TEXTURE_2D, oldTex);
        blitFromResolved(x, y, x, y, w, h);
        m_syncVRAM = true;
        return;
    }

    OpenGL::bindScreenFramebuffer();
    const auto oldTex = updateType == PartialUpdateVram::Asynchronous ? OpenGL::getTex2D() : GLint(-1);
    m_vramTexture.bind();

    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);

    if (updateType == PartialUpdateVram::Asynchronous) glBindTexture(GL_TEXTURE_2D, oldTex);
    m_fbo.bind(OpenGL::DrawAndReadFramebuffer);

    m_syncVRAM = true;
}

void PCSX::OpenGL_GPU::uploadVRAM(int x, int y, int w, int h, const uint16_t *pixels, PartialUpdateVram updateType) {
    if (!m_setMask && !m_checkMask) {
        partialUpdateVRAM(x, y, w, h, pixels, updateType);
        return;
    }
    const size_t count = size_t(w) * h;
    std::vector<uint16_t> masked(pixels, pixels + count);
    if (m_checkMask) {
        // Destination pixels with bit 15 set keep their value.
        std::vector<uint16_t> dest(count);
        readVRAMRect(x, y, w, h, dest.data());
        for (size_t i = 0; i < count; i++) {
            if (dest[i] & 0x8000) masked[i] = dest[i];
        }
    }
    const uint16_t setBit = m_setMask ? 0x8000 : 0;
    for (auto &pixel : masked) pixel |= setBit;
    partialUpdateVRAM(x, y, w, h, masked.data(), updateType);
}

template <PCSX::OpenGL_GPU::Transparency setting>
void PCSX::OpenGL_GPU::setTransparency() {
    // Check if we had transparency previously disabled and it just got enabled or vice versa
    if (m_lastTransparency != setting) {
        renderBatch();
        if constexpr (setting == Transparency::Opaque) {
            m_lastBlendingMode = -1;
            OpenGL::disableBlend();
        } else {
            OpenGL::enableBlend();
        }

        m_lastTransparency = setting;
    }
}

void PCSX::OpenGL_GPU::setBlendingModeFromTexpage(uint32_t texpage) {
    const auto newBlendingMode = (texpage >> 5) & 3;

    if (m_lastBlendingMode != newBlendingMode) {
        renderBatch();  // This must be executed before we set the new blend mode
        // The shader does the blending (semiBlend), from u_blendMode, pushed in renderBatch(). GL blending
        // stays set to write the fragment as is: u_blendFactors is (1, 1, 1, 0).
        m_lastBlendingMode = newBlendingMode;
        OpenGL::setBlendFactor(GL_SRC1_COLOR, GL_SRC1_ALPHA, GL_ONE, GL_ZERO);
        OpenGL::setBlendEquation(OpenGL::BlendEquation::Add);
    }
}

void PCSX::OpenGL_GPU::setBlendFactors(float sourceFactor, float destFactor) {
    if (m_blendFactors.x() != sourceFactor || m_blendFactors.y() != destFactor) {
        m_blendFactors.x() = sourceFactor;
        m_blendFactors.y() = destFactor;

        glUniform4f(m_blendFactorsLoc, sourceFactor, sourceFactor, sourceFactor, destFactor);
    }
}

void PCSX::OpenGL_GPU::drawTri(int *x, int *y, uint32_t *colors, uint16_t flags) {
    maybeRenderBatch<3>();

    m_vertices[m_vertexCount++] = Vertex(x[0], y[0], colors[0], flags);
    m_vertices[m_vertexCount++] = Vertex(x[1], y[1], colors[1], flags);
    m_vertices[m_vertexCount++] = Vertex(x[2], y[2], colors[2], flags);
}

// The texture coordinates of a triangle are walked in 16.16 fixed point, the same way the soft renderer
// does it in setupSections3, nextRow3 and seedSpan: the vertices are sorted by Y, the left edge is split
// in one or two sections whose U and V advance by a truncated per-row step, and every row starts at the
// first pixel right of its fractional left edge, from where U and V advance by per-pixel steps derived
// from the longest span, with a +0x8000 bias. This computes the per-triangle part; the fragment shader
// does the per-pixel part. Layout: two left-edge sections of {first row, height, X at the first row,
// |X step| quotient and remainder against the height, X step sign, U, U step, V, V step}, then the
// per-pixel U and V steps. A height of 0 marks an absent section.
static void computeAffineSetup(const int *xs, const int *ys, const unsigned *us, const unsigned *vs,
                               const int (&order)[3], int32_t *out) {
    struct WalkVertex {
        int32_t x, y, u, v;
    };
    WalkVertex vertices[3];
    for (int i = 0; i < 3; i++) {
        const int j = order[i];
        vertices[i] = {int32_t(xs[j]) << 16, ys[j], int32_t(us[j]) << 16, int32_t(vs[j]) << 16};
    }
    WalkVertex *v1 = &vertices[0], *v2 = &vertices[1], *v3 = &vertices[2];
    if (v1->y > v2->y) std::swap(v1, v2);
    if (v1->y > v3->y) std::swap(v1, v3);
    if (v2->y > v3->y) std::swap(v2, v3);

    const int32_t height = v3->y - v1->y;
    if (height == 0) return;
    const int32_t temp = ((v2->y - v1->y) << 16) / height;
    int32_t longest = int32_t(uint32_t(temp) * uint32_t((v3->x - v1->x) >> 16) + uint32_t(v1->x - v2->x));
    if (longest == 0) return;

    auto section = [](const WalkVertex *a, const WalkVertex *b, int32_t *s) {
        const int32_t h = b->y - a->y;
        if (h == 0) return false;
        const int32_t dx = b->x - a->x;
        const uint32_t absDx = dx < 0 ? uint32_t(-int64_t(dx)) : uint32_t(dx);
        s[0] = a->y;
        s[1] = h;
        s[2] = a->x;
        s[3] = int32_t(absDx / uint32_t(h));
        s[4] = int32_t(absDx % uint32_t(h));
        s[5] = dx < 0 ? -1 : 1;
        s[6] = a->u;
        s[7] = (b->u - a->u) / h;
        s[8] = a->v;
        s[9] = (b->v - a->v) / h;
        return true;
    };

    if (longest < 0) {
        section(v1, v3, out);
        if (longest > -0x1000) longest = -0x1000;
    } else {
        if (section(v1, v2, out)) {
            section(v2, v3, out + 10);
        } else {
            section(v2, v3, out);
        }
        if (longest < 0x1000) longest = 0x1000;
    }

    auto spanStep = [&](int32_t c1, int32_t c2, int32_t c3) {
        const int32_t numerator = int32_t(uint32_t(temp) * uint32_t((c3 - c1) >> 10) + (uint32_t(c1 - c2) << 6));
        return int32_t((int64_t(numerator) << 10) / longest);
    };
    out[20] = spanStep(v1->u, v2->u, v3->u);
    out[21] = spanStep(v1->v, v2->v, v3->v);
}

void PCSX::OpenGL_GPU::drawTriTextured(int *x, int *y, uint32_t *colors, uint16_t clut, uint16_t texpage, unsigned *u,
                                       unsigned *v, const int (&walkOrder)[3]) {
    maybeRenderBatch<3>();

    std::array<int32_t, c_affineSetupSize> affine{};
    computeAffineSetup(x, y, u, v, walkOrder, affine.data());
    for (int i = 0; i < 3; i++) {
        auto &vertex = m_vertices[m_vertexCount++];
        vertex = Vertex(x[i], y[i], colors[i], clut, texpage, u[i], v[i]);
        vertex.affine = affine;
    }
}

void PCSX::OpenGL_GPU::drawRect(int x, int y, int w, int h, uint32_t color) {
    maybeRenderBatch<6>();
    m_vertices[m_vertexCount++] = Vertex(x, y, color);
    m_vertices[m_vertexCount++] = Vertex(x + w, y, color);
    m_vertices[m_vertexCount++] = Vertex(x + w, y + h, color);
    m_vertices[m_vertexCount++] = Vertex(x + w, y + h, color);
    m_vertices[m_vertexCount++] = Vertex(x, y + h, color);
    m_vertices[m_vertexCount++] = Vertex(x, y, color);
}

void PCSX::OpenGL_GPU::drawRectTextured(int x, int y, int w, int h, uint32_t color, uint16_t clut, unsigned u,
                                        unsigned v) {
    maybeRenderBatch<6>();
    // Bits 14 and 15 of the vertex texpage are our own flags: rectangles are textured and never dithered.
    const uint32_t texpage = m_rectTexpage & ~(Vertex::c_untexturedPrimitiveTexpage | Vertex::c_ditherTexpage);
    m_vertices[m_vertexCount++] = Vertex(x, y, color, clut, texpage, u, v);
    m_vertices[m_vertexCount++] = Vertex(x + w, y, color, clut, texpage, u + w, v);
    m_vertices[m_vertexCount++] = Vertex(x + w, y + h, color, clut, texpage, u + w, v + h);
    m_vertices[m_vertexCount++] = Vertex(x + w, y + h, color, clut, texpage, u + w, v + h);
    m_vertices[m_vertexCount++] = Vertex(x, y + h, color, clut, texpage, u, v + h);
    m_vertices[m_vertexCount++] = Vertex(x, y, color, clut, texpage, u, v);
}

void PCSX::OpenGL_GPU::drawPixel(int x, int y, uint32_t color) {
    maybeRenderBatch<6>();
    m_vertices[m_vertexCount++] = Vertex(x, y, color);
    m_vertices[m_vertexCount++] = Vertex(x + 1, y, color);
    m_vertices[m_vertexCount++] = Vertex(x + 1, y + 1, color);
    m_vertices[m_vertexCount++] = Vertex(x + 1, y + 1, color);
    m_vertices[m_vertexCount++] = Vertex(x, y + 1, color);
    m_vertices[m_vertexCount++] = Vertex(x, y, color);
}

// Lines are stepped here, pixel by pixel, the same way as the soft renderer: a Bresenham walk whose
// initial error term is 3 * dy - dx for x-major lines and 2 * dx - dy for y-major ones, and a colour
// interpolated per step along the major axis. Each pixel is emitted as a 1x1 quad of constant colour.
// As in the soft renderer, the first colour goes to the left end (or the top end of a vertical line),
// whichever vertex that is.
void PCSX::OpenGL_GPU::drawLine(int x0, int y0, uint32_t color0, int x1, int y1, uint32_t color1) {
    if (x0 == x1 && y0 == y1) {
        drawPixel(x0, y0, color0);
        return;
    }

    auto lerp = [color0, color1](int step, int steps) -> uint32_t {
        uint32_t result = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            const int c0 = (color0 >> shift) & 0xff;
            const int c1 = (color1 >> shift) & 0xff;
            const int64_t delta = (int64_t(c1 - c0) << 16) * step / steps;
            result |= uint32_t(c0 + int(delta >> 16)) << shift;
        }
        return result;
    };

    if (x0 == x1 || y0 == y1) {
        const bool vertical = x0 == x1;
        const int start = vertical ? std::min(y0, y1) : std::min(x0, x1);
        const int end = vertical ? std::max(y0, y1) : std::max(x0, x1);
        const int steps = end - start;
        for (int v = start; v <= end; v++) {
            const uint32_t color = lerp(v - start, steps);
            if (vertical) {
                drawPixel(x0, v, color);
            } else {
                drawPixel(v, y0, color);
            }
        }
        return;
    }

    if (x1 < x0) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int ady = std::abs(dy);
    const int minorStep = dy < 0 ? -1 : 1;
    const bool xMajor = ady <= dx;

    int d, incMajor, incDiag, steps;
    if (xMajor) {
        d = 3 * ady - dx;
        incMajor = 2 * ady;
        incDiag = 2 * (ady - dx);
        steps = dx;
    } else {
        d = 2 * dx - ady;
        incMajor = 2 * dx;
        incDiag = 2 * (dx - ady);
        steps = ady;
    }

    int x = x0;
    int y = y0;
    drawPixel(x, y, color0);
    for (int step = 1; step <= steps; step++) {
        if (d <= 0) {
            d += incMajor;
        } else {
            d += incDiag;
            if (xMajor) {
                y += minorStep;
            } else {
                x++;
            }
        }
        if (xMajor) {
            x++;
        } else {
            y += minorStep;
        }
        drawPixel(x, y, lerp(step, steps));
    }
}

void PCSX::OpenGL_GPU::write0(ClearCache *) {
    renderBatch();
    m_syncVRAM = true;
}

void PCSX::OpenGL_GPU::write0(TPage *prim) {
    m_rectTexpage = prim->raw;
    m_drawToDisplay = prim->drawToDisplay;
}

void PCSX::OpenGL_GPU::updateSkipParity() {
    int parity = -1;
    if (!m_drawToDisplay && ((getStatusControl(8) & 0x2c) == 0x24) && !deinterlace()) {
        parity = readStatusFromBus() >> 31;
    }
    if (parity == m_skipParity) return;
    renderBatch();
    m_skipParity = parity;
    glUniform1i(m_skipParityLoc, parity);
}

// Set texture window, regardless of whether the window config changed
void PCSX::OpenGL_GPU::setTexWindowUnchecked(uint32_t cmd) {
    renderBatch();
    m_lastTexwindowSetting = cmd & 0xfffff;  // Only keep bottom 20 bits

    const uint32_t maskX = (cmd & 0x1f) * 8;          // Window mask x in 8 pixel steps
    const uint32_t maskY = ((cmd >> 5) & 0x1f) * 8;   // Window mask y in 8 pixel steps
    const uint32_t offsX = ((cmd >> 10) & 0x1f) * 8;  // Window offset x in 8 pixel steps
    const uint32_t offsY = ((cmd >> 15) & 0x1f) * 8;  // Window offset y in 8 pixel steps

    // Upload data to GPU
    glUniform4i(m_texWindowLoc, ~maskX, ~maskY, offsX & maskX, offsY & maskY);
}

// Set texture window, provided the window config actually changed
void PCSX::OpenGL_GPU::setTexWindow(uint32_t cmd) {
    cmd &= 0xfffff;  // Only keep bottom 20 bits
    if (m_lastTexwindowSetting != cmd) {
        setTexWindowUnchecked(cmd);
    }
}

void PCSX::OpenGL_GPU::write0(TWindow *prim) { setTexWindow(prim->raw); }

void PCSX::OpenGL_GPU::write0(MaskBit *prim) { setMaskBits(prim->set, prim->check); }

void PCSX::OpenGL_GPU::write0(DrawingAreaStart *prim) {
    m_drawAreaLeft = prim->x;
    m_drawAreaTop = prim->y;
    updateDrawArea();
}

void PCSX::OpenGL_GPU::write0(DrawingAreaEnd *prim) {
    m_drawAreaRight = prim->x;
    m_drawAreaBottom = prim->y;
    updateDrawArea();
}

void PCSX::OpenGL_GPU::setDrawOffset(uint32_t cmd) {
    m_updateDrawOffset = false;
    m_lastDrawOffsetSetting = cmd & 0x3fffff;  // Discard the bits we don't care about

    // Offset is a signed number in [-1024, 1023]
    const auto offsetX = (int32_t)cmd << 21 >> 21;
    const auto offsetY = (int32_t)cmd << 10 >> 21;

    m_drawingOffset.x() = offsetX;
    m_drawingOffset.y() = offsetY;

    // The 0.5 offsets help fix some holes in rendering, in places like the PS logo
    // TODO: This might not work when upscaling?
    float adjustedOffsets[2] = {static_cast<float>(offsetX) + 0.5f, static_cast<float>(offsetY) + 0.5f};
    glUniform2fv(m_drawingOffsetLoc, 1, adjustedOffsets);
}

void PCSX::OpenGL_GPU::write0(DrawingOffset *prim) {
    renderBatch();
    const uint32_t word = prim->raw & 0x3fffff;

    // Queue a draw offset update if it changed
    if (word != m_lastDrawOffsetSetting) {
        m_updateDrawOffset = true;
        m_lastDrawOffsetSetting = word;
    }
}

void PCSX::OpenGL_GPU::write0(FastFill *prim) {
    updateSkipParity();
    renderBatch();
    const auto colour = prim->color;
    // Written as k / 31 so VRAM keeps the 5-bit value the GPU stores (the low 3 bits dropped).
    const float r = float((colour & 0xff) >> 3) / 31.f;
    const float g = float(((colour >> 8) & 0xff) >> 3) / 31.f;
    const float b = float(((colour >> 16) & 0xff) >> 3) / 31.f;

    // GP0(02h) ignores GP0(E6h) and writes bit 15 as 0.
    OpenGL::setClearColor(r, g, b, 0.f);
    if (m_skipParity < 0) {
        OpenGL::setScissor(prim->x, prim->y, prim->w, prim->h);
        OpenGL::clearColor();
    } else {
        // GP0(02h) is not exempt from the 480i row skip: clear the rows of the other parity one by one.
        for (unsigned y = prim->y; y < prim->y + prim->h; y++) {
            if ((y & 1) == unsigned(m_skipParity)) continue;
            OpenGL::setScissor(prim->x, y, prim->w, 1);
            OpenGL::clearColor();
        }
    }
    setScissorArea();
}

void PCSX::OpenGL_GPU::write0(BlitVramVram *prim) {
    renderBatch();
    // The decoder has already masked the size; a zero here means the copy was clipped away.
    if (prim->w == 0 || prim->h == 0) return;
    OpenGL::disableScissor();  // We disable scissor testing because it affects glBlitFramebuffer

    // TODO: Sanitize this
    const auto srcX = prim->sX;
    const auto srcY = prim->sY;
    const auto destX = prim->dX;
    const auto destY = prim->dY;

    uint32_t width = prim->w;
    uint32_t height = prim->h;

    width = ((width - 1) & 0x3ff) + 1;
    height = ((height - 1) & 0x1ff) + 1;

    // Source and destination Y each wrap per row through resolveVramY(), and rows landing on an
    // unpopulated upper bank (open bus on a 1MB fitment) are dropped. A single blit only covers a copy
    // that is contiguous on both ends.
    const unsigned srcY0 = resolveVramY(srcY);
    const unsigned destY0 = resolveVramY(destY);
    bool contiguous = true;
    for (uint32_t j = 0; contiguous && (j < height); j++) {
        contiguous = !vramOpenBus(destY + j) && (resolveVramY(srcY + j) == srcY0 + j) &&
                     (resolveVramY(destY + j) == destY0 + j);
    }

    if (m_setMask || m_checkMask || !contiguous) {
        // The copy runs per pixel on a CPU copy of VRAM, in the same row-major order as the software
        // renderer, so it honours GP0(E6h), and an overlapping wrapped copy reads what the rows before
        // it wrote.
        OpenGL::enableScissor();
        auto slice = getVRAM(Ownership::BORROW);
        uint16_t *vram = slice.mutableData<uint16_t>();
        const uint16_t setBit = m_setMask ? 0x8000 : 0;
        for (uint32_t j = 0; j < height; j++) {
            if (vramOpenBus(destY + j)) continue;
            uint16_t *dstRow = vram + vramWidth * resolveVramY(destY + j);
            const uint16_t *srcRow = vram + vramWidth * resolveVramY(srcY + j);
            for (uint32_t i = 0; i < width; i++) {
                uint16_t &dst = dstRow[(destX + i) & 0x3ff];
                if (m_checkMask && (dst & 0x8000)) continue;
                dst = srcRow[(srcX + i) & 0x3ff] | setBit;
            }
        }
        partialUpdateVRAM(0, 0, vramWidth, vramHeight, vram, PartialUpdateVram::Asynchronous);
        return;
    }

    if (m_multisampled) {
        // A blit between two multisampled framebuffers needs identical source and destination rectangles,
        // so go through the resolved copy.
        resolveMSAA(srcX, srcY0, width, height);
        blitFromResolved(srcX, srcY0, destX, destY0, width, height);
    } else {
        glBlitFramebuffer(srcX, srcY0, srcX + width, srcY0 + height, destX, destY0, destX + width, destY0 + height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    OpenGL::enableScissor();
}

// Hardware drops a polygon or line with an edge spanning more than 1023 pixels
// horizontally or 511 vertically. Same test as the software renderer's
// checkCoord3/checkCoord4/CheckCoordL. The draw offset is added unwrapped, so
// it cancels out of the deltas and the raw vertices can be compared directly.
static bool edgeOverLimit(int x0, int y0, int x1, int y1) {
    return std::abs(x1 - x0) >= 1024 || std::abs(y1 - y0) >= 512;
}

// The vertex plus draw offset sum is not wrapped, but the pixel address wraps
// modulo 2048 on both axes before the drawing-area clip, as in the software
// renderer's forEachPixelWrap. Sums range over -2048..2046, so only a
// primitive reaching below -1024 has a copy that lands in VRAM, 2048 further
// along. Calls draw(dx, dy) once per copy, with the shift to add to the raw
// vertices; the shader adds the offset itself.
template <typename Draw>
static void forEachPixelWrap(uint32_t offsetWord, const int *xs, const int *ys, int n, Draw &&draw) {
    const int offsetX = int32_t(offsetWord << 21) >> 21;
    const int offsetY = int32_t(offsetWord << 10) >> 21;
    int minX = xs[0], minY = ys[0];
    for (int i = 1; i < n; i++) {
        minX = std::min(minX, xs[i]);
        minY = std::min(minY, ys[i]);
    }
    const bool wrapX = minX + offsetX < -1024;
    const bool wrapY = minY + offsetY < -1024;
    draw(0, 0);
    if (wrapX) draw(2048, 0);
    if (wrapY) draw(0, 2048);
    if (wrapX && wrapY) draw(2048, 2048);
}

template <PCSX::GPU::Shading shading, PCSX::GPU::Shape shape, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend,
          PCSX::GPU::Modulation modulation>
void PCSX::OpenGL_GPU::polyExec(Poly<shading, shape, textured, blend, modulation> *prim) {
    const auto &x = prim->x;
    const auto &y = prim->y;
    // A quad renders as (v0, v1, v2) and (v1, v2, v3), so its five triangle
    // edges are checked; v0-v3 belongs to neither triangle.
    if (edgeOverLimit(x[0], y[0], x[1], y[1]) || edgeOverLimit(x[1], y[1], x[2], y[2]) ||
        edgeOverLimit(x[2], y[2], x[0], y[0])) {
        return;
    }
    if constexpr (shape == Shape::Quad) {
        if (edgeOverLimit(x[1], y[1], x[3], y[3]) || edgeOverLimit(x[3], y[3], x[2], y[2])) return;
    }

    updateSkipParity();
    if constexpr (blend == Blend::Off) {
        setTransparency<Transparency::Opaque>();
    } else if constexpr (blend == Blend::Semi) {
        setTransparency<Transparency::Transparent>();
    }
    if constexpr (textured == Textured::No) {
        if constexpr (blend == Blend::Semi) {
            setBlendingModeFromTexpage(m_rectTexpage);
        }
    } else if constexpr (textured == Textured::Yes) {
        // A textured polygon's texpage attribute replaces the global texture page and blend mode
        // that later rectangles and lines use, leaving the dither and draw-to-display bits.
        m_rectTexpage = (m_rectTexpage & ~0b0000100111111111u) | (prim->tpage.raw & 0b0000100111111111u);
        if constexpr (blend == Blend::Semi) {
            setBlendingModeFromTexpage(prim->tpage.raw);
        }
    }

    // As in the soft renderer, only gouraud-shaded polygons are dithered, following GP0(E1h) bit 9
    // and the dithering setting (0 = never, 1 = as the game asks, 2 = always).
    uint16_t ditherFlag = 0;
    if constexpr (shading == Shading::Gouraud) {
        if (m_useDither == 2 || (m_useDither != 0 && (m_rectTexpage & 0x200))) ditherFlag = Vertex::c_ditherTexpage;
    }

    constexpr int n = shape == Shape::Quad ? 4 : 3;
    forEachPixelWrap(m_lastDrawOffsetSetting, x, y, n, [&](int dx, int dy) {
        int sx[n], sy[n];
        for (int i = 0; i < n; i++) {
            sx[i] = x[i] + dx;
            sy[i] = y[i] + dy;
        }
        addCheckMaskBounds(sx, sy, n);
        if constexpr (textured == Textured::No) {
            drawTri(&sx[0], &sy[0], &prim->colors[0], ditherFlag);
            if constexpr (shape == Shape::Quad) {
                drawTri(&sx[1], &sy[1], &prim->colors[1], ditherFlag);
            }
        } else if constexpr (textured == Textured::Yes) {
            const uint16_t texpage = prim->tpage.raw | ditherFlag;
            drawTriTextured(&sx[0], &sy[0], &prim->colors[0], prim->clutraw, texpage, &prim->u[0], &prim->v[0]);
            if constexpr (shape == Shape::Quad) {
                // The GPU walks the second triangle of a quad as (v1, v3, v2).
                drawTriTextured(&sx[1], &sy[1], &prim->colors[1], prim->clutraw, texpage, &prim->u[1], &prim->v[1],
                                {0, 2, 1});
            }
        }
    });
}

template <PCSX::GPU::Shading shading, PCSX::GPU::LineType lineType, PCSX::GPU::Blend blend>
void PCSX::OpenGL_GPU::lineExec(Line<shading, lineType, blend> *prim) {
    updateSkipParity();
    auto count = prim->colors.size();

    if constexpr (blend == Blend::Off) {
        setTransparency<Transparency::Opaque>();
    } else if constexpr (blend == Blend::Semi) {
        setTransparency<Transparency::Transparent>();
        setBlendingModeFromTexpage(m_rectTexpage);
    }

    for (unsigned i = 1; i < count; i++) {
        auto x0 = prim->x[i - 1];
        auto x1 = prim->x[i];
        auto y0 = prim->y[i - 1];
        auto y1 = prim->y[i];
        auto c0 = prim->colors[i - 1];
        auto c1 = prim->colors[i];

        if (edgeOverLimit(x0, y0, x1, y1)) continue;
        const int bx[2] = {x0, x1};
        const int by[2] = {y0, y1};
        forEachPixelWrap(m_lastDrawOffsetSetting, bx, by, 2,
                         [&](int dx, int dy) {
                             const int lx[2] = {x0 + dx, x1 + dx};
                             const int ly[2] = {y0 + dy, y1 + dy};
                             addCheckMaskBounds(lx, ly, 2);
                             drawLine(x0 + dx, y0 + dy, c0, x1 + dx, y1 + dy, c1);
                         });
    }
}

template <PCSX::GPU::Size size, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend, PCSX::GPU::Modulation modulation>
void PCSX::OpenGL_GPU::rectExec(Rect<size, textured, blend, modulation> *prim) {
    int w = prim->w;
    int h = prim->h;
    if constexpr (size == Size::Variable) {
        // The variable-rect dimensions are masked to 10 bits of width and 9
        // bits of height, with no size-0-means-max case: 1024 and 2048 wide
        // draw nothing, 1025 draws one column. Same as the software renderer.
        w &= 0x3ff;
        h &= 0x1ff;
        if (w == 0 || h == 0) return;
    }

    updateSkipParity();
    if constexpr (blend == Blend::Off) {
        setTransparency<Transparency::Opaque>();
    } else if constexpr (blend == Blend::Semi) {
        setTransparency<Transparency::Transparent>();
        setBlendingModeFromTexpage(m_rectTexpage);
    }
    const int bx[1] = {prim->x};
    const int by[1] = {prim->y};
    forEachPixelWrap(m_lastDrawOffsetSetting, bx, by, 1, [&](int dx, int dy) {
        const int rx[2] = {prim->x + dx, prim->x + dx + w - 1};
        const int ry[2] = {prim->y + dy, prim->y + dy + h - 1};
        addCheckMaskBounds(rx, ry, 2);
        if constexpr (textured == Textured::No) {
            drawRect(prim->x + dx, prim->y + dy, w, h, prim->color);
        } else if constexpr (textured == Textured::Yes) {
            uint32_t color = 0x808080;
            if constexpr (modulation == Modulation::On) {
                color = prim->color;
            }
            drawRectTextured(prim->x + dx, prim->y + dy, w, h, color, prim->clutraw, prim->u, prim->v);
        }
    });
}

void PCSX::OpenGL_GPU::write1(CtrlReset *) {
    // TODO: This should perform some more operations
    // GP1(00h) clears GP0(E1h), DFE included.
    m_drawToDisplay = false;
    setMaskBits(false, false);
    m_display.reset();
    setDisplayEnable(false);
    acknowledgeIRQ1();
}

void PCSX::OpenGL_GPU::write1(CtrlClearFifo *) {}

void PCSX::OpenGL_GPU::write1(CtrlIrqAck *) { acknowledgeIRQ1(); }

void PCSX::OpenGL_GPU::write1(CtrlDisplayEnable *ctrl) { setDisplayEnable(ctrl->enable); }

void PCSX::OpenGL_GPU::write1(CtrlDmaSetting *) {}

void PCSX::OpenGL_GPU::write1(CtrlDisplayStart *ctrl) {
    // The decoded Y keeps the upper-bank bit; with the gate closed it folds
    // into the lower 512 rows like any other VRAM access.
    CtrlDisplayStart resolved = *ctrl;
    resolved.y = resolveVramY(ctrl->y);
    m_display.set(&resolved);
}
void PCSX::OpenGL_GPU::write1(CtrlHorizontalDisplayRange *ctrl) { m_display.set(ctrl); }
void PCSX::OpenGL_GPU::write1(CtrlVerticalDisplayRange *ctrl) { m_display.set(ctrl); }
void PCSX::OpenGL_GPU::write1(CtrlDisplayMode *ctrl) { m_display.set(ctrl); }

// clang-format off
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { polyExec(prim); }
void PCSX::OpenGL_GPU::write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *prim) { polyExec(prim); }

void PCSX::OpenGL_GPU::write0(Line<Shading::Flat, LineType::Simple, Blend::Off> *prim) { lineExec(prim); }
void PCSX::OpenGL_GPU::write0(Line<Shading::Flat, LineType::Simple, Blend::Semi> *prim) { lineExec(prim); }
void PCSX::OpenGL_GPU::write0(Line<Shading::Flat, LineType::Poly, Blend::Off> *prim) { lineExec(prim); }
void PCSX::OpenGL_GPU::write0(Line<Shading::Flat, LineType::Poly, Blend::Semi> *prim) { lineExec(prim); }
void PCSX::OpenGL_GPU::write0(Line<Shading::Gouraud, LineType::Simple, Blend::Off> *prim) { lineExec(prim); }
void PCSX::OpenGL_GPU::write0(Line<Shading::Gouraud, LineType::Simple, Blend::Semi> *prim) { lineExec(prim); }
void PCSX::OpenGL_GPU::write0(Line<Shading::Gouraud, LineType::Poly, Blend::Off> *prim) { lineExec(prim); }
void PCSX::OpenGL_GPU::write0(Line<Shading::Gouraud, LineType::Poly, Blend::Semi> *prim) { lineExec(prim); }

void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::Off> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::On> *prim) { rectExec(prim); }
void PCSX::OpenGL_GPU::write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::On> *prim) { rectExec(prim); }
