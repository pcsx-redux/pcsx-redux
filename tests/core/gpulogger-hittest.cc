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

// The GPU logger's "filter by pixel" keeps a primitive when Logged::isInside() says the pixel
// is in it. Each case here sends one primitive's GP0 words to the soft GPU with the logger on,
// and compares isInside() over all of VRAM against the pixels the soft renderer changed.

#include <stdint.h>

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/gpu.h"
#include "core/gpulogger.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "gtest/gtest.h"

namespace {

class TestSystem final : public PCSX::System {
  public:
    void softReset() override {}
    void hardReset() override {}
    void biosPutc(int) override {}
    const PCSX::Arguments& getArgs() const override { throw std::logic_error("unused"); }
    void printf(std::string&&) override {}
    void log(PCSX::LogClass, std::string&&) override {}
    void message(std::string&&) override {}
    void luaMessage(const std::string&, bool) override {}
    void update(bool) override {}
    void close() override {}
    void purgeAllEvents() override {}
    void testQuit(int) override {}
};

constexpr uint32_t xy(int x, int y) { return (uint32_t(y & 0xffff) << 16) | uint32_t(x & 0xffff); }
constexpr uint32_t c_white = 0xffffff;

struct Coverage {
    std::vector<bool> drawn, hit;
    unsigned misses = 0, extras = 0, drawnCount = 0;
    std::string firstMiss, firstExtra;
};

class GPULoggerHitTest : public ::testing::Test {
  protected:
    void SetUp() override {
        m_oldSystem = PCSX::g_system;
        m_oldEmulator = PCSX::g_emulator;
        PCSX::g_system = &m_system;
        m_emulator = std::make_unique<PCSX::Emulator>();
        PCSX::g_emulator = m_emulator.get();
        m_emulator->m_cpu = PCSX::Cpus::Interpreted();
        ASSERT_NE(m_emulator->m_cpu, nullptr);
        m_emulator->m_gpu = PCSX::GPU::getSoft();
        ASSERT_EQ(gpu()->init(nullptr), 0);
        gp0({0xe3000000, 0xe4000000 | (511 << 10) | 1023, 0xe5000000});
        logger()->setLogging(true);
    }
    void TearDown() override {
        logger()->setLogging(false);
        logger()->clearFrameLog();
        gpu()->shutdown();
        m_emulator.reset();
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }

    PCSX::GPU* gpu() { return m_emulator->m_gpu.get(); }
    PCSX::GPULogger* logger() { return m_emulator->m_gpuLogger.get(); }
    void gp0(std::initializer_list<uint32_t> words) {
        for (auto w : words) gpu()->writeData(w);
    }
    std::vector<uint16_t> vram() {
        gpu()->drainRenderer();
        auto slice = gpu()->getVRAM(PCSX::GPU::Ownership::ACQUIRE);
        auto p = slice.data<uint16_t>();
        return std::vector<uint16_t>(p, p + 1024 * 512);
    }
    void setOffset(int x, int y) { gp0({0xe5000000 | ((uint32_t(y) & 0x7ff) << 11) | (uint32_t(x) & 0x7ff)}); }
    // Fast fill ignores the drawing offset and area. Two halves, because a height of 512 does not fit.
    void clearVram() { gp0({0x02000000, xy(0, 0), xy(1023, 256), 0x02000000, xy(0, 256), xy(1023, 256)}); }

    // Sends one primitive and returns how its isInside() compares to the pixels it changed.
    Coverage measure(std::initializer_list<uint32_t> words) { return measure(std::vector<uint32_t>(words)); }
    Coverage measure(const std::vector<uint32_t>& words) {
        logger()->clearFrameLog();
        auto before = vram();
        for (auto w : words) gpu()->writeData(w);
        auto after = vram();
        Coverage c;
        c.drawn.resize(1024 * 512);
        c.hit.resize(1024 * 512);
        auto& list = logger()->frameLog();
        PCSX::GPU::Logged* node = nullptr;
        for (auto& n : list) {
            EXPECT_EQ(node, nullptr) << "more than one primitive logged";
            node = &n;
        }
        if (!node) {
            ADD_FAILURE() << "nothing logged";
            return c;
        }
        for (unsigned y = 0; y < 512; y++) {
            for (unsigned x = 0; x < 1024; x++) {
                unsigned i = y * 1024 + x;
                bool d = before[i] != after[i];
                bool h = node->isInside(x, y);
                c.drawn[i] = d;
                c.hit[i] = h;
                if (d) c.drawnCount++;
                if (d && !h) {
                    if (!c.misses++) c.firstMiss = std::to_string(x) + "," + std::to_string(y);
                }
                if (h && !d) {
                    if (!c.extras++) c.firstExtra = std::to_string(x) + "," + std::to_string(y);
                }
            }
        }
        return c;
    }

    // Every pixel the renderer wrote is listed by the filter.
    static void expectCovers(const Coverage& c) {
        EXPECT_GT(c.drawnCount, 0u) << "the primitive drew nothing, so the comparison proves nothing";
        EXPECT_EQ(c.misses, 0u) << "first drawn pixel the filter misses: " << c.firstMiss;
    }
    // ... and nothing else is.
    static void expectExact(const Coverage& c) {
        expectCovers(c);
        EXPECT_EQ(c.extras, 0u) << "first undrawn pixel the filter lists: " << c.firstExtra;
    }

    TestSystem m_system;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
};

}  // namespace

TEST_F(GPULoggerHitTest, FlatQuad) {
    auto c = measure({0x28000000 | c_white, xy(0, 0), xy(100, 0), xy(0, 100), xy(100, 100)});
    EXPECT_TRUE(c.drawn[50 * 1024 + 90]);
    EXPECT_TRUE(c.hit[50 * 1024 + 90]);
    expectCovers(c);
}

TEST_F(GPULoggerHitTest, FlatQuadSkewed) {
    expectCovers(measure({0x28000000 | c_white, xy(20, 10), xy(120, 30), xy(10, 110), xy(130, 140)}));
}

TEST_F(GPULoggerHitTest, FlatQuadWithOffset) {
    setOffset(200, 50);
    expectCovers(measure({0x28000000 | c_white, xy(0, 0), xy(100, 0), xy(0, 100), xy(100, 100)}));
}

TEST_F(GPULoggerHitTest, GouraudQuad) {
    expectCovers(
        measure({0x38000000 | c_white, xy(0, 0), c_white, xy(100, 0), c_white, xy(0, 100), c_white, xy(100, 100)}));
}

TEST_F(GPULoggerHitTest, FlatTriangle) {
    expectCovers(measure({0x20000000 | c_white, xy(10, 10), xy(90, 20), xy(40, 80)}));
}

// The renderer leaves out the right and bottom edges of a polygon.
TEST_F(GPULoggerHitTest, FlatQuadEdges) {
    expectExact(measure({0x28000000 | c_white, xy(0, 0), xy(100, 0), xy(0, 100), xy(100, 100)}));
}

TEST_F(GPULoggerHitTest, FlatTriangleEdges) {
    expectExact(measure({0x20000000 | c_white, xy(10, 10), xy(90, 20), xy(40, 80)}));
}

// The pixel address wraps modulo 2048: -1000 + -500 is -1500, which the renderer draws at 548.
TEST_F(GPULoggerHitTest, FlatQuadOffsetWraps) {
    setOffset(-500, 0);
    expectExact(measure({0x28000000 | c_white, xy(-1000, 10), xy(-900, 10), xy(-1000, 60), xy(-900, 60)}));
}

TEST_F(GPULoggerHitTest, LineOffsetWraps) {
    setOffset(-600, -1000);
    expectExact(measure({0x40000000 | c_white, xy(-900, -900), xy(-700, -860)}));
}

TEST_F(GPULoggerHitTest, RectOffsetWraps) {
    setOffset(-1000, 0);
    expectExact(measure({0x60000000 | c_white, xy(-300, 40), xy(50, 20)}));
}

// A line covers its Bresenham steps between its two ends, and nothing past them.
TEST_F(GPULoggerHitTest, LineDiagonal) { expectExact(measure({0x40000000 | c_white, xy(10, 10), xy(80, 40)})); }

TEST_F(GPULoggerHitTest, LineVertical) { expectExact(measure({0x40000000 | c_white, xy(5, 100), xy(5, 150)})); }

// A polyline is open: nothing joins the last vertex back to the first.
TEST_F(GPULoggerHitTest, PolyLineOpen) {
    auto c = measure({0x48000000 | c_white, xy(10, 10), xy(60, 10), xy(60, 60), 0x55555555});
    // On the closing segment, and on neither drawn segment nor their extensions.
    EXPECT_FALSE(c.hit[35 * 1024 + 35]);
    expectExact(c);
}

// Seeded random shapes in every orientation, some placed so the offset sum wraps.
class Lcg {
  public:
    explicit Lcg(uint32_t seed) : m_state(seed) {}
    int range(int lo, int hi) {
        m_state = m_state * 1664525u + 1013904223u;
        return lo + int((m_state >> 8) % uint32_t(hi - lo + 1));
    }

  private:
    uint32_t m_state;
};

TEST_F(GPULoggerHitTest, RandomTriangles) {
    Lcg rng(1);
    unsigned drawn = 0, wrapped = 0;
    for (unsigned i = 0; i < 48; i++) {
        bool wrap = (i % 4) == 3;
        setOffset(wrap ? -1000 : 0, wrap ? -1000 : 0);
        int bx = rng.range(wrap ? -600 : 0, wrap ? -300 : 800), by = rng.range(wrap ? -1000 : 0, wrap ? -900 : 380);
        uint32_t v[3];
        for (auto& w : v) w = xy(bx + rng.range(0, 200), by + rng.range(0, 120));
        clearVram();
        auto c = measure({0x20000000 | c_white, v[0], v[1], v[2]});
        if (c.drawnCount == 0) continue;
        drawn++;
        if (wrap) wrapped++;
        SCOPED_TRACE("triangle " + std::to_string(i));
        expectExact(c);
    }
    // A random triangle can be degenerate, but most are not, and the wrapped ones must draw too.
    EXPECT_GE(drawn, 40u);
    EXPECT_GE(wrapped, 8u);
}

TEST_F(GPULoggerHitTest, RandomQuads) {
    Lcg rng(2);
    unsigned drawn = 0, wrapped = 0;
    for (unsigned i = 0; i < 32; i++) {
        bool wrap = (i % 4) == 3;
        setOffset(wrap ? -1000 : 0, wrap ? -1000 : 0);
        int bx = rng.range(wrap ? -600 : 0, wrap ? -300 : 800), by = rng.range(wrap ? -1000 : 0, wrap ? -900 : 380);
        uint32_t v[4];
        for (auto& w : v) w = xy(bx + rng.range(0, 200), by + rng.range(0, 120));
        clearVram();
        auto c = measure({0x28000000 | c_white, v[0], v[1], v[2], v[3]});
        if (c.drawnCount == 0) continue;
        drawn++;
        if (wrap) wrapped++;
        SCOPED_TRACE("quad " + std::to_string(i));
        expectExact(c);
    }
    EXPECT_GE(drawn, 28u);
    EXPECT_GE(wrapped, 6u);
}

TEST_F(GPULoggerHitTest, RandomLines) {
    Lcg rng(3);
    for (unsigned i = 0; i < 64; i++) {
        bool wrap = (i % 4) == 3;
        setOffset(wrap ? -1000 : 0, wrap ? -1000 : 0);
        int bx = rng.range(wrap ? -600 : 0, wrap ? -300 : 800), by = rng.range(wrap ? -1000 : 0, wrap ? -900 : 380);
        clearVram();
        auto c = measure({0x40000000 | c_white, xy(bx + rng.range(0, 200), by + rng.range(0, 120)),
                          xy(bx + rng.range(0, 200), by + rng.range(0, 120))});
        SCOPED_TRACE("line " + std::to_string(i));
        expectExact(c);
    }
}

TEST_F(GPULoggerHitTest, RectVariable) { expectExact(measure({0x60000000 | c_white, xy(30, 40), xy(50, 20)})); }

TEST_F(GPULoggerHitTest, RectVariableWithOffset) {
    setOffset(300, 100);
    expectExact(measure({0x60000000 | c_white, xy(30, 40), xy(50, 20)}));
}

TEST_F(GPULoggerHitTest, Rect1) { expectExact(measure({0x68000000 | c_white, xy(100, 100)})); }

TEST_F(GPULoggerHitTest, Rect8) { expectExact(measure({0x70000000 | c_white, xy(100, 100)})); }

TEST_F(GPULoggerHitTest, Rect16) { expectExact(measure({0x78000000 | c_white, xy(100, 100)})); }

TEST_F(GPULoggerHitTest, FastFill) { expectExact(measure({0x02000000 | c_white, xy(32, 16), xy(64, 32)})); }

TEST_F(GPULoggerHitTest, BlitRamVram) {
    std::vector<uint32_t> words = {0xa0000000, xy(50, 60), xy(20, 10)};
    for (unsigned i = 0; i < 20 * 10 / 2; i++) words.push_back(0x7fff7fff);
    expectExact(measure(words));
}

TEST_F(GPULoggerHitTest, BlitVramVram) {
    gp0({0x02000000 | c_white, xy(0, 300), xy(48, 30)});
    expectExact(measure({0x80000000, xy(0, 300), xy(200, 200), xy(40, 30)}));
}
