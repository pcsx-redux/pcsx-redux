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

#include <stdint.h>

#include <functional>
#include <magic_enum/magic_enum_all.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "support/eventbus.h"
#include "support/file.h"
#include "support/list.h"
#include "support/opengl.h"
#include "support/polyfills.h"
#include "support/slice.h"

namespace PCSX {
class UI;
class GPUDumper;
class GPULogger;
struct SaveStateWrapper;

// GP0 / GP1 packet-layout constants. Each constant names the bit-field
// width that the PSX protocol assigns to a particular packet payload
// slot. The packet constructors and the GP0 command dispatcher decode
// these slots with `value & mask` / `(value >> shift) & mask` shapes;
// the constants below give those expressions hardware-anchored names
// without changing the constructor logic. The raw env-packet preserve
// sites (m_textureWindowRaw, m_drawingStartRaw, m_drawingEndRaw,
// m_drawingOffsetRaw) keep their inline literals because they are
// protocol-visible logging fields rather than decoded math.

// GP0 command words carry the command byte in bits 24..28 and the
// payload in bits 0..23. The 24-bit payload mask is the wire shape
// shared by every env-class packet constructor.
static constexpr uint32_t c_PacketInfoMask = 0x00ffffff;

// Coordinate field widths. PSX vertex coordinates pack as 11-bit
// signed values (sign-extended after extraction); draw-area positions
// pack as 10-bit X / 9-bit Y; horizontal display range as 12-bit X
// pairs; CLUT selectors as 6-bit X / 9-bit Y; texture-window fields
// as 5-bit selectors. With a second VRAM bank the drawing-area,
// display-start and CLUT Y fields carry a 10th bit (the upper-bank bit);
// they are decoded full-width and the gate/fitment is resolved downstream
// via resolveVramY rather than masked away at decode.
static constexpr uint32_t c_Coord9Mask = 0x1ff;
static constexpr uint32_t c_Coord10Mask = 0x3ff;
static constexpr uint32_t c_Coord11Mask = 0x7ff;
static constexpr uint32_t c_Coord12Mask = 0xfff;
static constexpr uint32_t c_ClutXFieldMask = 0x3f;
static constexpr uint32_t c_ClutYFieldMask = c_Coord10Mask;
static constexpr uint32_t c_TWindowFieldMask = 0x1f;

// Field shifts. The CLUT half-word packs X at bits 0..5 and Y at bits
// 6..14. The DrawingOffset packs X at bits 0..10 and Y at bits 11..21.
// The DrawingArea positions pack X at bits 0..9 and Y at bits 10..18.
// The horizontal display range packs x0 at bits 0..11 and x1 at bits
// 12..23.
static constexpr int c_ClutYShift = 6;
static constexpr int c_DrawingOffsetYShift = 11;
static constexpr int c_DrawingAreaYShift = 10;
static constexpr int c_HorizDisplayRangeX1Shift = 12;

class GPU {
  public:
    template <typename T, unsigned bits>
    static constexpr T signExtend(const T value) {
        struct {
            T value : bits;
        } t;
        return t.value = value;
    }

    uint32_t readStatus();
    // GPUSTAT as the CPU reads it from 1F801814h: readStatus() plus the bits that follow the
    // beam position.
    uint32_t readStatusFromBus();
    // Returns true when the transfer stopped part way, with MADR and BCR already set.
    bool dma(uint32_t madr, uint32_t bcr, uint32_t chcr);
    static void gpuInterrupt();

    // 2MB VRAM addressing. Two independent axes decide how an access resolves:
    // the GP1(09h).0 bank gate (runtime, below) and the emulated VRAM fitment
    // (1MB retail vs 2MB, a setting). resolveVramY() is the single chokepoint
    // every VRAM Y-addressing site should route through.
    bool vram2MBGateOpen() const { return m_vram2MBGateOpen; }
    bool vram2MBFitted() const;
    unsigned resolveVramY(unsigned y) const;
    bool vramOpenBus(unsigned y) const;
    // Value the GPU data bus floats to for an open-bus (unpopulated upper bank)
    // read. Hardware-measured on retail 1MB boards (SCPH-1001, SCPH-5501).
    static constexpr uint16_t kVramOpenBusValue = 0xFFFF;

    // Called whenever the bank gate or fitment changes, so a backend can resync
    // any derived state (e.g. the soft renderer's effective drawable height).
    virtual void vramConfigChanged() {}
    // Called before the bank gate changes. A backend rasterizing off-thread must finish the
    // primitives already queued, which were issued under the old gate.
    virtual void drainRenderer() {}

    // These set and clear the IRQ1 flag, which readStatus mirrors into GPUSTAT.24.
    void requestIRQ1() {
        m_irq1Flag = true;
        if (!m_detached) g_emulator->m_mem->setIRQ(2);
    }
    void acknowledgeIRQ1() {
        m_irq1Flag = false;
        if (!m_detached) g_emulator->m_mem->clearIRQ(2);
    }

    // A detached GPU is an extra instance that is not wired to the emulated machine, such as the one
    // used to play back GPU dumps. It raises no interrupts, touches no emulator settings, and logs
    // into its own GPULogger instead of the global one.
    void detach(GPULogger *logger) {
        m_detached = true;
        m_logger = logger;
    }
    bool isDetached() const { return m_detached; }
    void setDumper(GPUDumper *dumper) { m_dumper = dumper; }
    // True when no GP0 command is partially received, so a dump can start cleanly.
    bool isIdle() const { return m_processor == &m_defaultProcessor; }
    // True while VRAM-to-CPU words are queued for readData(). Not part of a save state.
    bool hasPendingReadback() { return m_readFifo->size() != 0; }
    // The last word written for a given GP1 command.
    uint32_t getStatusControl(uint8_t cmd) const { return m_statusControl[cmd]; }
    // The last GP0(E3h), GP0(E4h) and GP0(E5h) words, low 24 bits.
    uint32_t getDrawingStartRaw() const { return m_drawingStartRaw; }
    uint32_t getDrawingEndRaw() const { return m_drawingEndRaw; }
    uint32_t getDrawingOffsetRaw() const { return m_drawingOffsetRaw; }
    // Words that bring a freshly reset GPU to the current state, VRAM excluded.
    void getRestoreSequence(std::vector<uint32_t> &gp0, std::vector<uint32_t> &gp1) const;

    bool m_showCfg = false;
    bool m_showDebug = false;
    virtual bool configure() = 0;
    virtual void debug() = 0;
    virtual ~GPU();

    void serialize(SaveStateWrapper *);
    void deserialize(const SaveStateWrapper *);
    void restoreEnvironment(const uint8_t *env, uint32_t status);

    // The GPU half of a save state, kept in memory: what serialize() writes, without the protobuf.
    struct Snapshot {
        uint32_t status;
        uint32_t control[256];
        uint8_t env[24];
        std::vector<uint16_t> vram;
    };
    void snapshot(Snapshot &);
    void restore(const Snapshot &);

  private:
    void restoreState(uint32_t status, const uint16_t *vram, unsigned vramRows, const uint8_t *env);
    uint32_t s_usedAddr[3];
    bool CheckForEndlessLoop(uint32_t laddr);
    uint32_t gpuDmaChainSize(uint32_t addr);
    virtual void resetBackend() = 0;

  public:
    GPU();
    int init(UI *);
    virtual int initBackend(UI *) = 0;
    virtual int shutdown() = 0;
    uint32_t readData();
    virtual uint32_t readStatusInternal() = 0;
    void writeData(uint32_t gdata);
    void directDMAWrite(const uint32_t *feed, int transferSize, uint32_t hwAddr);
    void directDMARead(uint32_t *dest, int transferSize, uint32_t hwAddr);
    void chainedDMAWrite(const uint32_t *memory, uint32_t hwAddr);
    void writeStatus(uint32_t gdata);
    virtual void setOpenGLContext() {}

    virtual void restoreStatus(uint32_t status) = 0;

    virtual void vblank(bool fromGui = false) = 0;
    // Draws the current display into the GUI's offscreen target without stepping any
    // emulated state, unlike vblank(). The GUI flips afterwards.
    virtual void presentFromGui() = 0;
    virtual void addVertex(short sx, short sy, int64_t fx, int64_t fy, int64_t fz) {
        throw std::runtime_error("Not yet implemented");
    }
    virtual void pgxpMemory(unsigned int addr, unsigned char *pVRAM) {}
    virtual void pgxpCacheVertex(short sx, short sy, const unsigned char *_pVertex) {
        throw std::runtime_error("Not yet implemented");
    }

    virtual void setDither(int setting) = 0;
    void reset() {
        resetBackend();
        m_irq1Flag = false;
        drainRenderer();
        m_vram2MBGateOpen = false;
        m_dmaDirection = 0;
        vramConfigChanged();
        m_dataRet = 0;
        m_readFifo->reset();
        m_processor->reset();
        m_defaultProcessor.setActive();
    }
    virtual void clearVRAM() = 0;
    virtual GLuint getVRAMTexture() = 0;
    virtual void setLinearFiltering() = 0;
    virtual void setCachedDithering(bool value) = 0;

    static std::unique_ptr<GPU> getSoft();
    static std::unique_ptr<GPU> getOpenGL();

    enum class Ownership { BORROW, ACQUIRE };
    virtual Slice getVRAM(Ownership = Ownership::BORROW) = 0;
    enum class PartialUpdateVram : bool { Synchronous, Asynchronous };
    virtual void partialUpdateVRAM(int x, int y, int w, int h, const uint16_t *pixels,
                                   PartialUpdateVram = PartialUpdateVram::Asynchronous) = 0;

    struct ScreenShot {
        Slice data;
        uint16_t width, height;
        enum { BPP_16, BPP_24 } bpp;
    };
    virtual ScreenShot takeScreenShot() { throw std::runtime_error("Not yet implemented"); }

    struct GPUStats {
        unsigned triangles = 0;
        unsigned texturedTriangles = 0;
        unsigned rectangles = 0;
        unsigned sprites = 0;
        unsigned pixelWrites = 0;
        unsigned pixelReads = 0;
        unsigned texelReads = 0;
        GPUStats &operator+=(const GPUStats &stats) {
            triangles += stats.triangles;
            texturedTriangles += stats.texturedTriangles;
            rectangles += stats.rectangles;
            sprites += stats.sprites;
            pixelWrites += stats.pixelWrites;
            pixelReads += stats.pixelReads;
            texelReads += stats.texelReads;
            return *this;
        }
    };

    struct Logged;
    typedef Intrusive::List<Logged> LoggedList;
    struct Logged : public LoggedList::Node {
        enum class Origin { DATAWRITE, CTRLWRITE, DIRECT_DMA, CHAIN_DMA, REPLAY };
        enum class PixelOp { READ, WRITE };
        struct DrawLogSettings {
            enum class ColorFormat { Expanded, HTML, None };
            ColorFormat colorFormat = ColorFormat::Expanded;
        };

        typedef std::function<void(OpenGL::ivec2, OpenGL::ivec2, OpenGL::ivec2)> AddTri;

        virtual ~Logged() {}
        virtual std::string_view getName() = 0;
        virtual void drawLogNode(unsigned itemIndex, const DrawLogSettings &) = 0;
        void drawColorBox(uint32_t color, unsigned itemIndex, unsigned colorIndex, const DrawLogSettings &settings);
        virtual void execute(GPU *) = 0;
        virtual void generateStatsInfo() = 0;
        virtual void cumulateStats(GPUStats *) = 0;
        virtual void getVertices(AddTri &&, PixelOp) = 0;
        virtual bool isInside(unsigned x, unsigned y) { return false; }
        void addLine(AddTri &&, int x1, int y1, int x2, int y2);

        uint64_t frame;
        uint32_t value, length;
        uint32_t pc;

        Origin origin;

        bool enabled = true;
        bool highlight = false;

      protected:
        static bool isInsideTriangle(int x, int y, int x1, int y1, int x2, int y2, int x3, int y3);
        static bool isInsideLine(int x, int y, int x1, int y1, int x2, int y2);
        // The pixel address wraps modulo 2048 on both axes, and vertex plus offset sums range over
        // -2048..2046, so VRAM pixel x, y is also reached by a primitive covering x - 2048 or y - 2048.
        template <typename Test>
        static bool isInsideWrapped(unsigned x, unsigned y, Test &&test) {
            const int ix = int(x), iy = int(y);
            return test(ix, iy) || test(ix - 2048, iy) || test(ix, iy - 2048) || test(ix - 2048, iy - 2048);
        }
    };

  private:
    uint32_t m_statusControl[256];
    bool m_irq1Flag = false;
    uint32_t m_envRaw[7];
    bool m_detached = false;
    GPULogger *m_logger = nullptr;
    GPUDumper *m_dumper = nullptr;

    class Buffer {
      public:
        Buffer(uint32_t value) : m_value(SWAP_LE32(value)) {
            m_data = &m_value;
            m_size = 1;
        }
        Buffer(const uint32_t *ptr, size_t size) : m_size(size), m_data(ptr) {}
        Buffer(const Buffer &other) = delete;
        Buffer(Buffer &&other) = delete;
        Buffer &operator=(const Buffer &other) = delete;
        Buffer &operator=(Buffer &&other) = delete;
        bool isEmpty() const { return m_size == 0; }
        void rewind() {
            m_data--;
            m_size++;
        }
        void consume(size_t size) {
            m_data += size;
            m_size -= size;
        }
        uint32_t get() {
            if (isEmpty()) return 0;
            m_size--;
            return SWAP_LE32(*m_data++);
        }
        size_t size() { return m_size; }
        const uint32_t *data() { return m_data; }

      private:
        uint32_t m_value;
        size_t m_size;
        const uint32_t *m_data;
    };

    class Command {
      public:
        Command() : m_gpu(nullptr) {}
        Command(GPU *parent) : m_gpu(parent) {}
        virtual ~Command() {}
        virtual void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length);
        virtual void reset() {}
        void setActive() { m_gpu->m_processor = this; }

      protected:
        GPU *m_gpu;

      private:
        void setGPU(GPU *gpu) { m_gpu = gpu; }

        friend class GPU;
    };

  public:
    // X-only clip against the 1024-wide buffer. Used by the VRAM transfer
    // paths when the bank gate is open: there the per-row Y counter WRAPS at
    // the 10-bit boundary (resolveVramY -> y & 0x3ff) instead of clipping, so
    // Y/h must be left untouched and only the X extent clamped. Hardware-
    // measured on 573 silicon (transfer-wrap-y, 2026-06-15): a transfer that
    // runs past Y=1023 wraps to Y=0 rather than dropping the overflow rows
    // (verdict=WRAP for both upload and copy). psx-spx Wrapping note: Copy/Fill
    // wrap to the opposite edge at the addressable VRAM size.
    template <typename T, T wMax = 1024>
    static bool clipX(T &x, T &w) {
        bool clipped = false;
        if (x >= wMax) {
            x = wMax;
            if (w != 0) {
                w = 0;
                return true;
            }
            return false;
        }
        if (x + w > wMax) {
            clipped = true;
            w = wMax - x;
        }
        return clipped;
    }

    template <typename T, T wMax = 1024, T hMax = 512>
    static bool clip(T &x, T &y, T &w, T &h) {
        bool clipped = false;
        x %= wMax;
        y %= hMax;

        if (x + w > wMax) {
            x = 0;
            clipped = true;
        }

        if (y + h > hMax) {
            y = 0;
            clipped = true;
        }

        return clipped;
    }

    enum class Shading { Flat, Gouraud };
    enum class Shape { Tri, Quad };
    enum class Textured { No, Yes };
    enum class Blend { Off, Semi };
    enum class Modulation { On, Off };
    enum class LineType { Simple, Poly };
    enum class Size { Variable, S1, S8, S16 };

    enum class BlendFunction {
        HalfBackAndHalfFront,
        FullBackAndFullFront,
        FullBackSubFullFront,
        FullBackAndQuarterFront
    };
    enum class TexDepth { Tex4Bits, Tex8Bits, Tex16Bits };

    struct EmptyColor {};

    struct ClearCache final : public Logged {
        std::string_view getName() override { return "Clear Cache"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
    };

    struct FastFill final : public Command, public Logged {
        std::string_view getName() override { return "Fast Fill"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override;
        void getVertices(AddTri &&, PixelOp) override;
        FastFill(GPU *parent) : Command(parent) {}
        void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length) override;
        void reset() override { m_state = READ_COLOR; }
        bool isInside(unsigned x, unsigned y) override {
            return (x >= this->x) && (y >= this->y) && (x < this->x + this->w) && (y < this->y + this->h);
        }

        uint32_t color;
        unsigned x, y, w, h;

        struct {
            unsigned x, y, w, h;
        } raw;

        bool clipped = false;

      private:
        enum { READ_COLOR, READ_XY, READ_WH } m_state = READ_COLOR;
    };

    struct BlitVramVram final : public Command, public Logged {
        std::string_view getName() override { return "VRAM to VRAM blit"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override;
        void getVertices(AddTri &&, PixelOp) override;
        BlitVramVram(GPU *parent) : Command(parent) {}
        void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length) override;
        void reset() override { m_state = READ_COMMAND; }
        bool isInside(unsigned x, unsigned y) override {
            return (x >= this->dX) && (y >= this->dY) && (x < this->dX + this->w) && (y < this->dY + this->h);
        }

        unsigned sX, sY, dX, dY, w, h;

        struct {
            unsigned sX, sY, dX, dY, w, h;
        } raw;

        bool clipped = false;

      private:
        enum { READ_COMMAND, READ_SRC_XY, READ_DST_XY, READ_HW } m_state = READ_COMMAND;
    };

    struct BlitRamVram final : public Command, public Logged {
        std::string_view getName() override { return "RAM to VRAM blit"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override;
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override;
        void getVertices(AddTri &&, PixelOp) override;
        BlitRamVram() {}
        BlitRamVram(GPU *parent) : Command(parent) {}
        BlitRamVram(const BlitRamVram &other) {
            x = other.x;
            y = other.y;
            w = other.w;
            h = other.h;
            data.copy(other.data);
        }
        BlitRamVram(BlitRamVram &&) = delete;
        void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length) override;
        void reset() override {
            m_state = READ_COMMAND;
            m_data.clear();
        }
        bool isInside(unsigned x, unsigned y) override {
            return (x >= this->x) && (y >= this->y) && (x < this->x + this->w) && (y < this->y + this->h);
        }

        unsigned x, y, w, h;
        Slice data;

        struct {
            unsigned x, y, w, h;
        } raw;

        bool clipped = false;

      private:
        enum { READ_COMMAND, READ_XY, READ_HW, READ_PIXELS } m_state = READ_COMMAND;
        std::string m_data;
    };

    struct BlitVramRam final : public Command, public Logged {
        std::string_view getName() override { return "VRAM to RAM blit"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override {}
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override;
        void getVertices(AddTri &&, PixelOp) override;
        BlitVramRam(GPU *parent) : Command(parent) {}
        void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length) override;

        unsigned x, y, w, h;

        struct {
            unsigned x, y, w, h;
        } raw;

        bool clipped = false;

      private:
        enum { READ_COMMAND, READ_XY, READ_HW } m_state = READ_COMMAND;
    };

    struct TPage : public Logged {
        std::string_view getName() override { return "Texture Page"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void drawLogNodeCommon();
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        TPage() {}
        TPage(uint32_t value);
        TPage(const TPage &other) = default;
        TPage(TPage &&other) = default;
        TPage &operator=(const TPage &other) = default;
        uint32_t raw;
        unsigned tx, ty;
        BlendFunction blendFunction;
        TexDepth texDepth;
        bool dither;
        bool drawToDisplay;
        bool texDisable;
        bool xflip;
        bool yflip;
    };

    struct TWindow : public Logged {
        std::string_view getName() override { return "Texture Window"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        TWindow() {}
        TWindow(uint32_t value);
        TWindow(const TWindow &other) = default;
        TWindow(TWindow &&other) = default;
        TWindow &operator=(const TWindow &other) = default;
        unsigned x, y, w, h;
        uint32_t raw;
    };

    struct DrawingAreaStart : public Logged {
        std::string_view getName() override { return "Drawing Area Start"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        DrawingAreaStart(uint32_t value);
        DrawingAreaStart(const DrawingAreaStart &other) = default;
        DrawingAreaStart(DrawingAreaStart &&other) = default;
        DrawingAreaStart &operator=(const DrawingAreaStart &other) = default;
        unsigned x, y;
        uint32_t raw;
    };

    struct DrawingAreaEnd : public Logged {
        std::string_view getName() override { return "Drawing Area End"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        DrawingAreaEnd(uint32_t value);
        DrawingAreaEnd(const DrawingAreaEnd &other) = default;
        DrawingAreaEnd(DrawingAreaEnd &&other) = default;
        DrawingAreaEnd &operator=(const DrawingAreaEnd &other) = default;
        unsigned x, y;
        uint32_t raw;
    };

    struct DrawingOffset : public Logged {
        std::string_view getName() override { return "Drawing Offset"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        DrawingOffset() {}
        DrawingOffset(uint32_t value);
        DrawingOffset(const DrawingOffset &other) = default;
        DrawingOffset(DrawingOffset &&other) = default;
        DrawingOffset &operator=(const DrawingOffset &other) = default;
        int x, y;
        uint32_t raw;
    };

    struct MaskBit : public Logged {
        std::string_view getName() override { return "Mask Bit"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        MaskBit(uint32_t value);
        MaskBit(const MaskBit &other) = default;
        MaskBit(MaskBit &&other) = default;
        MaskBit &operator=(const MaskBit &other) = default;
        bool set, check;
    };

    template <Shading shading, Shape shape, Textured textured, Blend blend, Modulation modulation>
    struct Poly final : public Command, public Logged {
        static constexpr unsigned count = shape == Shape::Tri ? 3 : 4;

        std::string_view getName() override { return "Polygon"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override;
        void cumulateStats(GPUStats *) override;
        void getVertices(AddTri &&, PixelOp) override;
        Poly() {}
        void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length) override;
        void reset() override {
            m_state = READ_COLOR;
            m_count = 0;
        }
        bool isInside(unsigned x, unsigned y) override {
            return isInsideWrapped(x, y, [this](int x, int y) {
                if (isInsideTriangle(x, y, this->x[0] + offset.x, this->y[0] + offset.y, this->x[1] + offset.x,
                                     this->y[1] + offset.y, this->x[2] + offset.x, this->y[2] + offset.y)) {
                    return true;
                }
                if constexpr (shape == Shape::Quad) {
                    return isInsideTriangle(x, y, this->x[1] + offset.x, this->y[1] + offset.y, this->x[2] + offset.x,
                                            this->y[2] + offset.y, this->x[3] + offset.x, this->y[3] + offset.y);
                }
                return false;
            });
        }
        uint32_t colors[count];
        int x[count], y[count];
        struct Empty {};
        typedef typename std::conditional<textured == Textured::Yes, unsigned, Empty>::type TextureUnitType;
        struct EmptyU {};
        typedef typename std::conditional<textured == Textured::Yes, unsigned, EmptyU>::type TextureUnitTypeU;
        struct EmptyV {};
        typedef typename std::conditional<textured == Textured::Yes, unsigned, EmptyV>::type TextureUnitTypeV;
        POLYFILL_NO_UNIQUE_ADDRESS TextureUnitTypeU u[count];
        POLYFILL_NO_UNIQUE_ADDRESS TextureUnitTypeV v[count];
        struct EmptyTPage {};
        POLYFILL_NO_UNIQUE_ADDRESS typename std::conditional<textured == Textured::Yes, TPage, EmptyTPage>::type tpage;
        struct EmptyTWindow {};
        POLYFILL_NO_UNIQUE_ADDRESS
        typename std::conditional<textured == Textured::Yes, TWindow, EmptyTWindow>::type twindow;
        struct EmptyClutRAW {};
        POLYFILL_NO_UNIQUE_ADDRESS
        typename std::conditional<textured == Textured::Yes, uint16_t, EmptyClutRAW>::type clutraw;
        DrawingOffset offset;
        TextureUnitType clutX() {
            if constexpr (textured == Textured::Yes) {
                return (clutraw & c_ClutXFieldMask) * 16;
            } else {
                return {};
            }
        }
        TextureUnitType clutY() {
            if constexpr (textured == Textured::Yes) {
                return (clutraw >> c_ClutYShift) & c_ClutYFieldMask;
            } else {
                return {};
            }
        }

      private:
        GPUStats stats;
        unsigned m_count = 0;
        enum { READ_COLOR, READ_XY, READ_UV } m_state = READ_COLOR;
    };

    template <Shading shading, LineType lineType, Blend blend>
    struct Line final : public Command, public Logged {
        std::string_view getName() override { return "Line"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override;
        void cumulateStats(GPUStats *) override;
        void getVertices(AddTri &&, PixelOp) override;
        Line() {
            if constexpr (lineType == LineType::Simple) m_count = 0;
        }
        void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length) override;
        void reset() override {
            m_state = READ_COLOR;
            if constexpr (lineType == LineType::Simple) {
                m_count = 0;
            } else if constexpr (lineType == LineType::Poly) {
                x.clear();
                y.clear();
                colors.clear();
            }
        }
        bool isInside(unsigned x, unsigned y) override {
            return isInsideWrapped(x, y, [this](int x, int y) {
                for (unsigned i = 1; i < this->x.size(); i++) {
                    if (isInsideLine(x, y, this->x[i - 1] + offset.x, this->y[i - 1] + offset.y,
                                     this->x[i] + offset.x, this->y[i] + offset.y)) {
                        return true;
                    }
                }
                return false;
            });
        }

        template <typename T>
        using Storage = typename std::conditional<lineType == LineType::Poly, std::vector<T>, std::array<T, 2>>::type;

        Storage<int> x, y;
        Storage<uint32_t> colors;
        DrawingOffset offset;

      private:
        GPUStats stats;
        struct Empty {};
        POLYFILL_NO_UNIQUE_ADDRESS
        typename std::conditional<lineType == LineType::Simple, unsigned, Empty>::type m_count;
        enum { READ_COLOR, READ_XY } m_state = READ_COLOR;
    };

    template <Size size, Textured textured, Blend blend, Modulation modulation>
    struct Rect final : public Command, public Logged {
        std::string_view getName() override { return "Rectangle"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write0(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override;
        void getVertices(AddTri &&, PixelOp) override;
        Rect() {}
        void processWrite(Buffer &, Logged::Origin, uint32_t value, uint32_t length) override;
        void reset() override { m_state = READ_COLOR; }
        bool isInside(unsigned x, unsigned y) override {
            return isInsideWrapped(x, y, [this](int x, int y) {
                const int left = this->x + offset.x, top = this->y + offset.y;
                return (x >= left) && (y >= top) && (x < left + this->w) && (y < top + this->h);
            });
        }

        int x, y, w, h;
        struct Empty {};
        typedef typename std::conditional<textured == Textured::Yes, unsigned, Empty>::type TextureUnitType;
        struct EmptyU {};
        typedef typename std::conditional<textured == Textured::Yes, unsigned, EmptyU>::type TextureUnitTypeU;
        struct EmptyV {};
        typedef typename std::conditional<textured == Textured::Yes, unsigned, EmptyV>::type TextureUnitTypeV;
        POLYFILL_NO_UNIQUE_ADDRESS
        typename std::conditional<(textured == Textured::No) || (modulation == Modulation::On), uint32_t,
                                  EmptyColor>::type color;
        POLYFILL_NO_UNIQUE_ADDRESS TextureUnitTypeU u;
        POLYFILL_NO_UNIQUE_ADDRESS TextureUnitTypeV v;
        struct EmptyTPage {};
        POLYFILL_NO_UNIQUE_ADDRESS typename std::conditional<textured == Textured::Yes, TPage, EmptyTPage>::type tpage;
        struct EmptyTWindow {};
        POLYFILL_NO_UNIQUE_ADDRESS
        typename std::conditional<textured == Textured::Yes, TWindow, EmptyTWindow>::type twindow;
        struct EmptyClutRAW {};
        POLYFILL_NO_UNIQUE_ADDRESS
        typename std::conditional<textured == Textured::Yes, uint16_t, EmptyClutRAW>::type clutraw;
        DrawingOffset offset;
        TextureUnitType clutX() {
            if constexpr (textured == Textured::Yes) {
                return (clutraw & c_ClutXFieldMask) * 16;
            } else {
                return {};
            }
        }
        TextureUnitType clutY() {
            if constexpr (textured == Textured::Yes) {
                return (clutraw >> c_ClutYShift) & c_ClutYFieldMask;
            } else {
                return {};
            }
        }

      private:
        enum { READ_COLOR, READ_XY, READ_UV, READ_HW } m_state = READ_COLOR;
    };

    struct CtrlReset : public Logged {
        std::string_view getName() override { return "Reset"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
    };
    struct CtrlClearFifo : public Logged {
        std::string_view getName() override { return "Clear Fifo"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
    };
    struct CtrlIrqAck : public Logged {
        std::string_view getName() override { return "IRQ Ack"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override {}
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
    };
    struct CtrlDisplayEnable : public Logged {
        std::string_view getName() override { return "Display Enable"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        CtrlDisplayEnable(uint32_t value) : enable((value & 1) == 0) {}
        CtrlDisplayEnable(const CtrlDisplayEnable &other) = default;
        CtrlDisplayEnable(CtrlDisplayEnable &&other) = default;
        CtrlDisplayEnable &operator=(const CtrlDisplayEnable &other) = default;
        bool enable;
    };
    struct CtrlDmaSetting : public Logged {
        std::string_view getName() override { return "DMA Setting"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        CtrlDmaSetting(uint32_t value) : dma(magic_enum::enum_cast<Dma>(value & 3).value()) {}
        CtrlDmaSetting(const CtrlDmaSetting &other) = default;
        CtrlDmaSetting(CtrlDmaSetting &&other) = default;
        CtrlDmaSetting &operator=(const CtrlDmaSetting &other) = default;
        enum class Dma { Off, FifoQuery, Write, Read } dma;
    };
    struct CtrlDisplayStart : public Logged {
        std::string_view getName() override { return "Display Start"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        CtrlDisplayStart(uint32_t value)
            : x(value & c_Coord10Mask), y((value >> c_DrawingAreaYShift) & c_Coord10Mask) {}
        CtrlDisplayStart(const CtrlDisplayStart &other) = default;
        CtrlDisplayStart(CtrlDisplayStart &&other) = default;
        CtrlDisplayStart &operator=(const CtrlDisplayStart &other) = default;
        unsigned x, y;
    };
    struct CtrlHorizontalDisplayRange : public Logged {
        std::string_view getName() override { return "Horizontal Display Range"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        CtrlHorizontalDisplayRange(uint32_t value)
            : x0(value & c_Coord12Mask), x1((value >> c_HorizDisplayRangeX1Shift) & c_Coord12Mask) {}
        CtrlHorizontalDisplayRange(const CtrlHorizontalDisplayRange &other) = default;
        CtrlHorizontalDisplayRange(CtrlHorizontalDisplayRange &&other) = default;
        CtrlHorizontalDisplayRange &operator=(const CtrlHorizontalDisplayRange &other) = default;
        unsigned x0, x1;
    };
    struct CtrlVerticalDisplayRange : public Logged {
        std::string_view getName() override { return "Vertical Display Range"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        CtrlVerticalDisplayRange(uint32_t value)
            : y0(value & c_Coord10Mask), y1((value >> c_DrawingAreaYShift) & c_Coord10Mask) {}
        CtrlVerticalDisplayRange(const CtrlVerticalDisplayRange &other) = default;
        CtrlVerticalDisplayRange(CtrlVerticalDisplayRange &&other) = default;
        CtrlVerticalDisplayRange &operator=(const CtrlVerticalDisplayRange &other) = default;
        unsigned y0, y1;
    };
    struct CtrlDisplayMode : public Logged {
        std::string_view getName() override { return "Display Mode"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        CtrlDisplayMode() : CtrlDisplayMode(0) {}
        CtrlDisplayMode(uint32_t value);
        CtrlDisplayMode(const CtrlDisplayMode &other) = default;
        CtrlDisplayMode(CtrlDisplayMode &&other) = default;
        CtrlDisplayMode &operator=(const CtrlDisplayMode &other) = default;
        bool equals(const CtrlDisplayMode &other) const {
            return (hres == other.hres) && (vres == other.vres) && (mode == other.mode) && (depth == other.depth) &&
                   (interlace == other.interlace);
        }
        uint32_t widthRaw;
        enum { HR_256, HR_320, HR_512, HR_640, HR_368, HR_384 } hres;
        enum { VR_240, VR_480 } vres;
        enum { VM_NTSC, VM_PAL } mode;
        enum { CD_15BITS, CD_24BITS } depth;
        bool interlace;
    };
    struct CtrlQuery : public Logged {
        std::string_view getName() override { return "Query"; }
        void drawLogNode(unsigned itemIndex, const DrawLogSettings &) override;
        void execute(GPU *gpu) override { gpu->write1(this); }
        void generateStatsInfo() override {}
        void cumulateStats(GPUStats *) override {}
        void getVertices(AddTri &&, PixelOp) override {}
        CtrlQuery(uint32_t value) : query(value & 0x07) {}
        CtrlQuery(const CtrlQuery &other) = default;
        CtrlQuery(CtrlQuery &&other) = default;
        CtrlQuery &operator=(const CtrlQuery &other) = default;
        enum QueryType { TextureWindow, DrawAreaStart, DrawAreaEnd, DrawOffset, Unknown };
        QueryType type() {
            if ((query <= 1) || (query >= 6)) {
                return Unknown;
            }
            return magic_enum::enum_cast<QueryType>((query - 2) & 3).value();
        }
        uint8_t query;
    };
    struct Display {
        using ivec2 = OpenGL::ivec2;
        using vec2 = OpenGL::vec2;

        ivec2 start;           // Starting coords of the display area
        ivec2 size;            // Width and height of the display area
        vec2 startNormalized;  // Starting coords of the display area normalized in the [0, 1] range
        vec2 sizeNormalized;   // Width and height of the display area normalized in the [0, 1] range

        CtrlDisplayMode info;
        bool enabled;

        int x1, x2, y1, y2;  // Display area range variables

        void reset();
        void set(CtrlDisplayStart *);
        void set(CtrlHorizontalDisplayRange *);
        void set(CtrlVerticalDisplayRange *);
        void set(CtrlDisplayMode *);
        void setLinearFiltering();
        void updateDispArea();
    };
    Display m_display;

  private:
    Command m_defaultProcessor = {this};

    FastFill m_fastFill = {this};
    Command *m_polygons[32];
    Command *m_lines[32];
    Command *m_rects[32];
    BlitVramVram m_blitVramVram = {this};
    BlitRamVram m_blitRamVram = {this};
    BlitVramRam m_blitVramRam = {this};

    Command *m_processor = &m_defaultProcessor;

    IO<Fifo> m_readFifo = new Fifo();
    Slice m_vramReadSlice;

    TPage m_lastTPage;
    TWindow m_lastTWindow;
    DrawingOffset m_lastOffset;

    uint32_t m_dataRet = 0;
    uint32_t m_textureWindowRaw = 0;
    uint32_t m_drawingStartRaw = 0;
    uint32_t m_drawingEndRaw = 0;
    uint32_t m_drawingOffsetRaw = 0;
    bool m_vram2MBGateOpen = false;  // GP1(09h).0: 1 opens the upper 1MB bank
    uint32_t m_dmaDirection = 0;     // GP1(04h).0-1

    // GPUSTAT.28 with GP1(04h)=2: low while a polygon or line command is partially received.
    bool dmaBlockRequestLow() const;

    virtual void write0(ClearCache *) = 0;
    virtual void write0(FastFill *) = 0;

    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Flat, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Tri, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Poly<Shading::Gouraud, Shape::Quad, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;

    virtual void write0(Line<Shading::Flat, LineType::Simple, Blend::Off> *) = 0;
    virtual void write0(Line<Shading::Flat, LineType::Simple, Blend::Semi> *) = 0;
    virtual void write0(Line<Shading::Flat, LineType::Poly, Blend::Off> *) = 0;
    virtual void write0(Line<Shading::Flat, LineType::Poly, Blend::Semi> *) = 0;
    virtual void write0(Line<Shading::Gouraud, LineType::Simple, Blend::Off> *) = 0;
    virtual void write0(Line<Shading::Gouraud, LineType::Simple, Blend::Semi> *) = 0;
    virtual void write0(Line<Shading::Gouraud, LineType::Poly, Blend::Off> *) = 0;
    virtual void write0(Line<Shading::Gouraud, LineType::Poly, Blend::Semi> *) = 0;

    virtual void write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::Off> *) = 0;
    virtual void write0(Rect<Size::Variable, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::Variable, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::Variable, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::Variable, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S1, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S8, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::No, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::No, Blend::Semi, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::Yes, Blend::Off, Modulation::On> *) = 0;
    virtual void write0(Rect<Size::S16, Textured::Yes, Blend::Semi, Modulation::On> *) = 0;

    virtual void write0(BlitVramVram *);

    virtual void write0(TPage *) = 0;
    virtual void write0(TWindow *) = 0;
    virtual void write0(DrawingAreaStart *) = 0;
    virtual void write0(DrawingAreaEnd *) = 0;
    virtual void write0(DrawingOffset *) = 0;
    virtual void write0(MaskBit *) = 0;

    virtual void write1(CtrlReset *) = 0;
    virtual void write1(CtrlClearFifo *) = 0;
    virtual void write1(CtrlIrqAck *) = 0;
    virtual void write1(CtrlDisplayEnable *) = 0;
    virtual void write1(CtrlDmaSetting *) = 0;
    virtual void write1(CtrlDisplayStart *) = 0;
    virtual void write1(CtrlHorizontalDisplayRange *) = 0;
    virtual void write1(CtrlVerticalDisplayRange *) = 0;
    virtual void write1(CtrlDisplayMode *) = 0;
    virtual void write1(CtrlQuery *);
};

}  // namespace PCSX
