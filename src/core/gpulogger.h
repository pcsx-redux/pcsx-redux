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

#include <array>
#include <type_traits>
#include <vector>

#include "core/gpu.h"
#include "support/eventbus.h"
#include "support/opengl.h"
#include "support/slice.h"

namespace PCSX {

namespace Widgets {
class GPULogger;
}

class GPULogger {
  public:
    // A rectangle of VRAM to watch. Any logged command whose read or write footprint intersects an
    // enabled breakpoint pauses the emulator. The footprints come straight from Logged::getVertices,
    // so this covers primitives, blits, and the palette rows of paletted textures alike.
    struct VramBreakpoint {
        union {
            int raw[4];
            struct {
                int x, y, w, h;
            };
        } area = {0, 0, 64, 64};
        bool onRead = false;
        bool onWrite = true;
        bool enabled = true;
    };

    // Per-pixel touch counts of one complete frame, reduced from the heatmap accumulation buffers.
    // Buckets are pixels touched 1, 2, 3-4, 5-8 and 9+ times.
    struct OverdrawStats {
        uint64_t total = 0;
        unsigned pixels = 0;
        unsigned max = 0;
        std::array<unsigned, 5> buckets{};
        float mean() const { return pixels ? float(total) / float(pixels) : 0.0f; }
    };

    // The emulator's logger follows the emulator: its frames are the emulator's vsyncs and it records
    // the guest pc. A logger owned by a detached GPU (the dump player's) is told which frame it is on
    // with setFrame() instead, and snapshots and replays that GPU.
    explicit GPULogger(bool followEmulator = true);
    void setOwner(GPU* gpu) { m_owner = gpu; }
    GPU* gpu() const;
    bool followsEmulator() const { return m_followEmulator; }
    void setFrame(uint64_t frame);
    // Logging without the VRAM heatmaps, which need an OpenGL context; enable() sets those up.
    void setLogging(bool enabled) { m_enabled = enabled; }
    size_t loggedCount() const { return m_list.size(); }
    GPU::LoggedList& frameLog() { return m_list; }
    void clearFrameLog() {
        m_list.destroyAll();
        // The next command starts a new frame for the heatmaps, so nothing drawn before the clear is counted
        // or published by the next setFrame().
        m_pendingNewFrame = true;
        m_overdrawDirty = false;
    }
    template <typename T>
    void addNode(const T& data, GPU::Logged::Origin origin, uint32_t value, uint32_t length) {
        if constexpr (std::is_same_v<T, GPU::CtrlDisplayStart>) {
            // A game flips buffers by moving the display start, so counting the moves gives its framerate.
            // 480i games that keep displaying the same buffer are counted from the draw environment below.
            uint32_t start = data.x | (data.y << 16);
            if (start != m_lastDisplayStart) {
                m_lastDisplayStart = start;
                m_displayStartChanges++;
            }
        }
        // A 480i game can keep displaying the buffer it draws into, so its display start never moves. It still
        // sets up its draw environment at the start of each frame, so a drawing area or offset write that follows
        // some primitives marks a finished frame. At most one is counted per vsync, so a game switching draw
        // environments several times in a frame counts once, and a field that redraws the same frame without
        // touching the draw environment does not count.
        if constexpr (std::is_same_v<T, GPU::CtrlDisplayMode>) {
            m_interlaced480 = data.interlace && data.vres == GPU::CtrlDisplayMode::VR_480;
        } else if constexpr (std::is_same_v<T, GPU::DrawingAreaStart> || std::is_same_v<T, GPU::DrawingAreaEnd> ||
                             std::is_same_v<T, GPU::DrawingOffset>) {
            if (m_primitivesSinceDrawEnv) {
                m_primitivesSinceDrawEnv = false;
                m_drawEnvFrameThisVSync = true;
            }
        } else if constexpr (requires { data.offset; }) {
            m_primitivesSinceDrawEnv = true;
        }
        // Track the drawing area even while disabled, so the heatmap clips correctly as soon as it is enabled.
        if constexpr (std::is_same_v<T, GPU::DrawingAreaStart>) {
            m_drawingAreaX0 = data.x;
            m_drawingAreaY0 = data.y;
        } else if constexpr (std::is_same_v<T, GPU::DrawingAreaEnd>) {
            m_drawingAreaX1 = data.x;
            m_drawingAreaY1 = data.y;
        } else if constexpr (std::is_same_v<T, GPU::CtrlReset>) {
            m_drawingAreaX0 = m_drawingAreaY0 = m_drawingAreaX1 = m_drawingAreaY1 = 0;
            m_interlaced480 = false;
            m_primitivesSinceDrawEnv = false;
        }
        if (m_enabled) {
            // Only primitives are clipped to the drawing area; fills and blits ignore it.
            constexpr bool clipped = requires { data.offset; };
            addNodeInternal(new T(data), origin, value, length, clipped);
        }
    }
    float getGuestFPS() const { return m_guestFPS; }
    const OverdrawStats& getWrittenOverdraw() const { return m_writtenOverdraw; }
    const OverdrawStats& getReadOverdraw() const { return m_readOverdraw; }
    bool hasOverdrawStats() const { return m_hasOverdrawStats; }
    // Sum of the touch counts over a VRAM rectangle, from the same frame as the stats above.
    uint64_t getOverdrawSum(bool read, int x, int y, int w, int h) const;
    // Reading the counts back stalls the GL pipeline once per frame, so it only runs when asked for.
    bool m_overdrawStats = false;
    void replay(GPU*);
    void highlight(GPU::Logged* node, bool only = false);
    void enable();
    void disable();
    void setEnabled(bool enabled) {
        if (enabled == m_enabled) return;
        m_enabled = enabled;
        if (enabled) {
            enable();
        } else {
            disable();
        }
    }
    void bindWrittenHeatmap() { m_writtenHeatmapTex.bind(); }
    void bindReadHeatmap() { m_readHeatmapTex.bind(); }
    void bindWrittenHighlight() { m_writtenHighlightTex.bind(); }
    void bindReadHighlight() { m_readHighlightTex.bind(); }

  private:
    void startNewFrame();
    void dropFramesBefore(uint64_t frame);
    void addNodeInternal(GPU::Logged* node, GPU::Logged::Origin, uint32_t value, uint32_t length, bool clipped);
    void checkVramBreakpoints(GPU::Logged* node);
    void readOverdraw(OpenGL::Framebuffer& fb, OverdrawStats& stats, std::vector<float>& pixels);

    EventBus::Listener m_listener;
    unsigned m_drawingAreaX0 = 0, m_drawingAreaY0 = 0, m_drawingAreaX1 = 0, m_drawingAreaY1 = 0;
    const bool m_followEmulator;
    GPU* m_owner = nullptr;
    bool m_enabled = false;
    bool m_breakOnVSync = false;
    std::vector<VramBreakpoint> m_vramBreakpoints;
    bool m_hasFramebuffers = false;
    uint64_t m_frameCounter = 0;
    bool m_pendingNewFrame = false;
    uint32_t m_lastDisplayStart = 0xffffffff;
    unsigned m_displayStartChanges = 0;
    bool m_interlaced480 = false;
    bool m_primitivesSinceDrawEnv = false;
    bool m_drawEnvFrameThisVSync = false;
    unsigned m_drawEnvFrames = 0;
    unsigned m_fpsVSyncs = 0;
    float m_guestFPS = 0.0f;
    OverdrawStats m_writtenOverdraw, m_readOverdraw;
    bool m_hasOverdrawStats = false;
    bool m_overdrawDirty = false;
    std::vector<float> m_writtenOverdrawPixels, m_readOverdrawPixels;
    GPU::LoggedList m_list;
    Slice m_vram;
    float m_impact = 1.0f / 256.0f;
    float m_decayRate = 1.0f / 1024.0f;

    std::array<OpenGL::ivec2, 3 * 0x10000> m_vertices;
    unsigned m_verticesCount = 0;

    OpenGL::Framebuffer m_writtenHeatmapFB, m_readHeatmapFB, m_writtenHighlightFB, m_readHighlightFB;
    OpenGL::Texture m_writtenHeatmapTex, m_readHeatmapTex, m_writtenHighlightTex, m_readHighlightTex;
    // The heatmaps accumulate into these during a frame, and are copied to the displayed ones when it ends.
    OpenGL::Framebuffer m_writtenAccumFB, m_readAccumFB;
    OpenGL::Texture m_writtenAccumTex, m_readAccumTex;
    OpenGL::VertexArray m_vao;
    OpenGL::VertexBuffer m_vbo;
    OpenGL::Program m_program;

    void addTri(OpenGL::ivec2& v1, OpenGL::ivec2& v2, OpenGL::ivec2& v3);
    void flush();

    friend class Widgets::GPULogger;
};

}  // namespace PCSX
