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

#define IMGUI_DEFINE_MATH_OPERATORS

#include "core/gpu.h"

#include <algorithm>
#include <cstdlib>
#include <magic_enum/magic_enum_all.hpp>

#include "core/debug.h"
#include "core/gpudump.h"
#include "core/gpulogger.h"
#include "core/pgxp_mem.h"
#include "core/psxcounters.h"
#include "core/psxdma.h"
#include "core/psxhw.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#define GPUSTATUS_READYFORVRAM 0x08000000
#define GPUSTATUS_IDLE 0x04000000  // CMD ready
#define GPUSTATUS_MODE 0x02000000  // Data request mode
#define GPUSTATUS_IRQ1 0x01000000  // GP0(1Fh) / GP1(02h)

namespace PCSX {

// clang-format off
// clang-format doesn't understand duff's device pattern...
template <GPU::Shading shading, GPU::Shape shape, GPU::Textured textured, GPU::Blend blend, GPU::Modulation modulation>
void GPU::Poly<shading, shape, textured, blend, modulation>::processWrite(Buffer & buf, Logged::Origin origin, uint32_t origvalue, uint32_t length) {
    uint32_t value = buf.get();
    switch (m_state) {
        for (/* m_count = 0 */; m_count < count; m_count++) {
            if (shading == Shading::Gouraud) {
                m_state = READ_COLOR;
                if (buf.isEmpty()) return;
                value = buf.get();
                [[fallthrough]];
        case READ_COLOR:
                if constexpr ((textured == Textured::Yes) && (modulation == Modulation::Off)) {
                    colors[m_count] = 0x808080;
                } else {
                    colors[m_count] = value & 0xffffff;
                }
            } else {
                colors[m_count] = colors[0];
            }
            m_state = READ_XY;
            if (buf.isEmpty()) return;
            value = buf.get();
            [[fallthrough]];
        case READ_XY:
            x[m_count] = GPU::signExtend<int, 11>(value & 0xffff);
            y[m_count] = GPU::signExtend<int, 11>(value >> 16);
            if (textured == Textured::Yes) {
                m_state = READ_UV;
                if (buf.isEmpty()) return;
                value = buf.get();
                [[fallthrough]];
        case READ_UV:
                if constexpr (textured == Textured::Yes) {
                    u[m_count] = value & 0xff;
                    v[m_count] = (value >> 8) & 0xff;
                    value >>= 16;
                    if (m_count == 0) {
                        clutraw = value;
                    } else if (m_count == 1) {
                        value &= 0b0000100111111111;
                        tpage = TPage(value);
                        uint32_t lastTPage = m_gpu->m_lastTPage.raw & ~0b0000100111111111;
                        m_gpu->m_lastTPage = TPage(lastTPage | value);
                    }
                }
            }
        }
    }
    m_count = 0;
    m_state = READ_COLOR;
    if constexpr (textured == Textured::Yes) {
        twindow = TWindow(m_gpu->m_lastTWindow.raw);
    }
    offset = m_gpu->m_lastOffset;
    m_gpu->m_defaultProcessor.setActive();
    m_gpu->m_logger->addNode(*this, origin, origvalue, length);
    m_gpu->write0(this);
}

template <GPU::Shading shading, GPU::LineType lineType, GPU::Blend blend>
void GPU::Line<shading, lineType, blend>::processWrite(Buffer & buf, Logged::Origin origin, uint32_t origvalue, uint32_t length) {
    uint32_t value = buf.get();
    if constexpr (lineType == LineType::Poly) {
        // The terminator is only recognized once two vertices are in; before that, the command word and the
        // second vertex's first word are always vertex data, even when they match the pattern.
        while ((x.size() < 2) || ((value & 0xf000f000) != 0x50005000)) {
            switch (m_state) {
                case READ_COLOR:
                    colors.push_back(value & 0xffffff);
                    m_state = READ_XY;
                    if (buf.isEmpty()) return;
                    value = buf.get();
                    [[fallthrough]];
                case READ_XY:
                    if constexpr (shading == Shading::Flat) {
                        if (x.size() != 0) colors.push_back(colors[0]);
                    } else {
                        m_state = READ_COLOR;
                    }
                    x.push_back(GPU::signExtend<int, 11>(value & 0xffff));
                    y.push_back(GPU::signExtend<int, 11>(value >> 16));
                    if (buf.isEmpty()) return;
                    value = buf.get();
            }
        }
    } else {
        switch (m_state) {
            for (/* m_count = 0 */; m_count < 2; m_count++) {
                if (shading == Shading::Gouraud) {
                    m_state = READ_COLOR;
                    if (buf.isEmpty()) return;
                    value = buf.get();
                    [[fallthrough]];
            case READ_COLOR:
                    colors[m_count] = value & 0xffffff;
                } else {
                    colors[m_count] = colors[0];
                }
                m_state = READ_XY;
                if (buf.isEmpty()) return;
                value = buf.get();
                [[fallthrough]];
            case READ_XY:
                x[m_count] = GPU::signExtend<int, 11>(value & 0xffff);
                y[m_count] = GPU::signExtend<int, 11>(value >> 16);
            }
        }
    }
    if constexpr (lineType == LineType::Simple) {
        m_count = 0;
    }
    m_state = READ_COLOR;
    offset = m_gpu->m_lastOffset;
    m_gpu->m_defaultProcessor.setActive();
    if ((colors.size() >= 2) && ((colors.size() == x.size()))) {
        m_gpu->m_logger->addNode(*this, origin, origvalue, length);
        m_gpu->write0(this);
    } else {
        g_system->log(LogClass::GPU, "Got an invalid line command...\n");
    }
    if constexpr (lineType == LineType::Poly) {
        colors.clear();
        x.clear();
        y.clear();
    }
}

template <GPU::Size size, GPU::Textured textured, GPU::Blend blend, GPU::Modulation modulation>
void GPU::Rect<size, textured, blend, modulation>::processWrite(Buffer & buf, Logged::Origin origin, uint32_t origvalue, uint32_t length) {
    uint32_t value = buf.get();
    switch (m_state) {
        case READ_COLOR:
            if constexpr ((textured == Textured::No) || (modulation == Modulation::On)) {
                color = value & 0xffffff;
            }
            m_state = READ_XY;
            if (buf.isEmpty()) return;
            value = buf.get();
            [[fallthrough]];
        case READ_XY:
            x = GPU::signExtend<int, 11>(value & 0xffff);
            y = GPU::signExtend<int, 11>(value >> 16);
            if (textured == Textured::Yes) {
                m_state = READ_UV;
                if (buf.isEmpty()) return;
                value = buf.get();
                [[fallthrough]];
        case READ_UV:
                if constexpr (textured == Textured::Yes) {
                    u = value & 0xff;
                    v = (value >> 8) & 0xff;
                    value >>= 16;
                    clutraw = value;
                }
            }
            if constexpr (size == Size::S1) {
                h = 1;
                w = 1;
            } else if constexpr (size == Size::S8) {
                h = 8;
                w = 8;
            } else if constexpr (size == Size::S16) {
                h = 16;
                w = 16;
            }

            if (size == Size::Variable) {
                m_state = READ_HW;
                if (buf.isEmpty()) return;
                value = buf.get();
                [[fallthrough]];
        case READ_HW:
                w = value & 0xffff;
                h = value >> 16;
            }
    }
    m_state = READ_COLOR;
    if constexpr (textured == Textured::Yes) {
        tpage = TPage(m_gpu->m_lastTPage.raw);
        twindow = TWindow(m_gpu->m_lastTWindow.raw);
    }
    offset = m_gpu->m_lastOffset;
    m_gpu->m_defaultProcessor.setActive();
    m_gpu->m_logger->addNode(*this, origin, origvalue, length);
    m_gpu->write0(this);
}
// clang-format on

namespace {

template <size_t Index>
auto *polyCommand() {
    // One instance per GPU: the commands hold a pointer back to the GPU that owns them.
    return new GPU::Poly < (Index & 0x10) ? GPU::Shading::Gouraud : GPU::Shading::Flat,
           (Index & 0x08) ? GPU::Shape::Quad : GPU::Shape::Tri, (Index & 0x04) ? GPU::Textured::Yes : GPU::Textured::No,
           (Index & 0x02) ? GPU::Blend::Semi : GPU::Blend::Off,
           (Index & 0x01) ? GPU::Modulation::Off : GPU::Modulation::On > ();
}

template <size_t Index>
auto *lineCommand() {
    // One instance per GPU: the commands hold a pointer back to the GPU that owns them.
    return new GPU::Line < (Index & 0x10) ? GPU::Shading::Gouraud : GPU::Shading::Flat,
           (Index & 0x08) ? GPU::LineType::Poly : GPU::LineType::Simple,
           (Index & 0x02) ? GPU::Blend::Semi : GPU::Blend::Off > ();
}

template <size_t Index>
consteval GPU::Size rectSize() {
    if constexpr (((Index >> 3) & 3) == 0) {
        return GPU::Size::Variable;
    } else if constexpr (((Index >> 3) & 3) == 1) {
        return GPU::Size::S1;
    } else if constexpr (((Index >> 3) & 3) == 2) {
        return GPU::Size::S8;
    } else {
        return GPU::Size::S16;
    }
}

template <size_t Index>
auto *rectCommand() {
    // One instance per GPU: the commands hold a pointer back to the GPU that owns them.
    return new GPU::Rect < rectSize<Index>(), (Index & 0x04) ? GPU::Textured::Yes : GPU::Textured::No,
           (Index & 0x02) ? GPU::Blend::Semi : GPU::Blend::Off,
           (Index & 0x01) ? GPU::Modulation::Off : GPU::Modulation::On > ();
}

}  // namespace

}  // namespace PCSX

PCSX::GPU::~GPU() {
    for (auto poly : m_polygons) delete poly;
    for (auto line : m_lines) delete line;
    for (auto rect : m_rects) delete rect;
}

PCSX::GPU::GPU() {
    for (unsigned i = 0; i < 256; i++) m_statusControl[i] = i << 24;
    for (unsigned i = 0; i < 7; i++) m_envRaw[i] = (0xe0 + i) << 24;
    [&]<size_t... Is>(std::index_sequence<Is...>) {
        ((m_polygons[Is] = polyCommand<Is>()), ...);
    }(std::make_index_sequence<32>{});
    [&]<size_t... Is>(std::index_sequence<Is...>) {
        ((m_lines[Is] = lineCommand<Is>()), ...);
    }(std::make_index_sequence<32>{});
    [&]<size_t... Is>(std::index_sequence<Is...>) {
        ((m_rects[Is] = rectCommand<Is>()), ...);
    }(std::make_index_sequence<32>{});
}

int PCSX::GPU::init(UI *ui) {
    if (!m_logger) m_logger = g_emulator->m_gpuLogger.get();
    for (auto poly : m_polygons) poly->setGPU(this);
    for (auto line : m_lines) line->setGPU(this);
    for (auto rect : m_rects) rect->setGPU(this);
    m_textureWindowRaw = 0;
    m_drawingStartRaw = 0;
    m_drawingEndRaw = 0;
    m_drawingOffsetRaw = 0;
    m_dataRet = 0x400;
    return initBackend(ui);
}

inline bool PCSX::GPU::CheckForEndlessLoop(uint32_t laddr) {
    if (laddr == s_usedAddr[1]) return true;
    if (laddr == s_usedAddr[2]) return true;

    if (laddr < s_usedAddr[0]) {
        s_usedAddr[1] = laddr;
    } else {
        s_usedAddr[2] = laddr;
    }

    s_usedAddr[0] = laddr;

    return false;
}

uint32_t PCSX::GPU::gpuDmaChainSize(uint32_t addr) {
    uint32_t size;
    uint32_t DMACommandCounter = 0;
    bool usingMsan = g_emulator->m_mem->msanInitialized();

    s_usedAddr[0] = s_usedAddr[1] = s_usedAddr[2] = 0xffffff;

    // initial linked list s_ptr (word)
    size = 1;

    do {
        uint32_t header;
        if (usingMsan && PCSX::Memory::inMsanRange(addr)) {
            addr &= 0xfffffffc;
            switch (g_emulator->m_mem->msanGetStatus<4>(addr)) {
                case PCSX::MsanStatus::UNINITIALIZED:
                    g_system->log(LogClass::GPU, _("GPU DMA went into usable but uninitialized msan memory: %8.8lx\n"),
                                  addr);
                    g_system->testQuit(PCSX::Memory::c_msanExitCode);
                    return size;
                case PCSX::MsanStatus::UNUSABLE:
                    g_system->log(LogClass::GPU, _("GPU DMA went into unusable msan memory: %8.8lx\n"), addr);
                    g_system->testQuit(PCSX::Memory::c_msanExitCode);
                    return size;
                case PCSX::MsanStatus::OK:
                    header = *(uint32_t *)(g_emulator->m_mem->m_msanRAM + (addr - PCSX::Memory::c_msanStart));
                    break;
            }
        } else {
            addr &= g_emulator->getRamMask<4>();
            header = SWAP_LEu32(*g_emulator->m_mem->getPointer<uint32_t>(addr));
        }

        if (DMACommandCounter++ > 2000000) break;
        if (CheckForEndlessLoop(addr)) break;

        // # 32-bit blocks to transfer
        size += (header >> 24) + 1;

        // next 32-bit pointer
        uint32_t nextAddr = header & 0xffffff;
        if (usingMsan && nextAddr == PCSX::Memory::c_msanChainMarker) {
            addr = g_emulator->m_mem->msanGetChainPtr(addr);
            continue;
        }
        addr = nextAddr;
    } while (!(addr & 0x800000));  // contrary to some documentation, the end-of-linked-list marker is not actually
    return size;  // 0xFF'FFFF any pointer with bit 23 set will do.
}

uint32_t PCSX::GPU::readStatus() {
    uint32_t ret = readStatusInternal();  // Get status from GPU core

// Gameshark Lite - wants to see VRAM busy
// - Must enable GPU 'Fake Busy States' hack
#if 0
    if ((ret & GPUSTATUS_IDLE) == 0) ret &= ~GPUSTATUS_READYFORVRAM;
#endif
    if (m_readFifo->size() != 0) ret |= GPUSTATUS_READYFORVRAM;
    // Bit 26 stays clear from a multi-word command's first word until its last one, and for
    // GP0(C0h) until the last word of the readout has been read.
    if (!isIdle() || (m_readFifo->size() != 0)) ret &= ~GPUSTATUS_IDLE;
    if (m_irq1Flag) ret |= GPUSTATUS_IRQ1;
    // Let's pretend our input fifo is always ready for more data.
    if ((ret & 0x60000000) == 0x20000000) ret |= 0x02000000;
    if (dmaBlockRequestLow()) ret &= ~0x12000000;
    return ret;
}

uint32_t PCSX::GPU::readStatusFromBus() {
    uint32_t ret = readStatus();
    // NTSC 480i: GP1(08h) bits 2 and 5 set, bit 3 clear. Timeline measured on a v2 GPU (SCPH-1001)
    // at 320 and 640 pixels wide, identical at both, counted in hblanks since the vblank that
    // opened the field. Bit 31 is 1 from line 23 of the 263-line field until the next vblank, and
    // 0 everywhere else, vblank included. Bit 13 is 1 from line 5 of the 262-line field until
    // line 6 of the following 263-line field. A v0 GPU (SCPH-1000) gives the same bit 31 and never
    // sets bit 13; there is no GPU revision setting, so this models v2. PAL is not measured.
    if ((m_statusControl[8] & 0x2c) == 0x24) {
        auto &counters = g_emulator->m_counters;
        uint32_t line = counters->linesSinceVBlank();
        bool longField = counters->longField();
        ret &= ~0x80002000;
        if (longField && (line >= 23)) ret |= 0x80000000;
        if (longField ? (line < 6) : (line >= 5)) ret |= 0x00002000;
    }
    return ret;
}

bool PCSX::GPU::dma(uint32_t madr, uint32_t bcr, uint32_t chcr) {  // GPU
    uint32_t *ptr;
    uint32_t size, bs;

    switch (chcr) {
        case 0x01000200:  // vram2mem
            PSXDMA_LOG("*** DMA2 GPU - vram2mem *** %lx addr = %lx size = %lx\n", chcr, madr, bcr);
            ptr = g_emulator->m_mem->getPointer<uint32_t>(madr);
            if (ptr == nullptr) {
                PSXDMA_LOG("*** DMA2 GPU - vram2mem *** NULL Pointer!!!\n");
                break;
            }
            // BA blocks * BS words (word = 32-bits)
            size = (bcr >> 16) * (bcr & 0xffff);
            directDMARead(ptr, size, madr);
            g_emulator->m_cpu->Clear(madr, size);
            g_emulator->m_mem->msanDmaWrite(madr, size * 4);
            if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                g_emulator->m_debug->checkDMAwrite(2, madr, size * 4);
            }
#if 1
            // already 32-bit word size ((size * 4) / 4)
            scheduleGPUDMAIRQ(size);
#else
            // Experimental burst dma transfer (0.333x max)
            scheduleGPUDMAIRQ(size / 3);
#endif
            return false;

        case 0x01000201:  // mem2vram
            bs = (bcr & 0xffff);
            size = (bcr >> 16) * bs;  // BA blocks * BS words (word = 32-bits)
            PSXDMA_LOG("*** DMA 2 - GPU mem2vram *** %lx addr = %lxh, BCR %lxh => size %d = BA(%d) * BS(%xh)\n", chcr,
                       madr, bcr, size, size / bs, size / (bcr >> 16));
            ptr = g_emulator->m_mem->getPointer<uint32_t>(madr);
            if (ptr == nullptr) {
                PSXDMA_LOG("*** DMA2 GPU - mem2vram *** NULL Pointer!!!\n");
                break;
            }
            if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                g_emulator->m_debug->checkDMAread(2, madr, size * 4);
            }
            pgxpMemory(PGXP_ConvertAddress(madr), PGXP_GetMem());
            // Each block waits for GPUSTAT.28. With GP1(04h)=2 it stays low while a polygon or line
            // command is partially received, and only DMA could deliver the rest, so a block boundary
            // inside one stops the channel for good: CHCR stays busy and no IRQ is raised.
            for (uint32_t done = 0; done < size; done += bs) {
                if (dmaBlockRequestLow()) {
                    auto &mem = g_emulator->m_mem;
                    mem->setMADR<2>(madr + done * 4);
                    mem->setBCR<2>((((size - done) / bs) << 16) | bs);
                    return true;
                }
                directDMAWrite(ptr + done, bs, madr + done * 4);
            }

#if 0
            // already 32-bit word size ((size * 4) / 4)
            scheduleGPUDMAIRQ(size);
#else
            // X-Files video interlace. Experimental delay depending of BS.
            scheduleGPUDMAIRQ((7 * size) / bs);
#endif
            return false;

        case 0x00000401:  // Vampire Hunter D: title screen linked list update (see psxhw.c)
        case 0x01000401:  // dma chain
            PSXDMA_LOG("*** DMA 2 - GPU dma chain *** %8.8lx addr = %lx size = %lx\n", chcr, madr, bcr);

            size = gpuDmaChainSize(madr);
            chainedDMAWrite((uint32_t *)PCSX::g_emulator->m_mem->m_wram, madr);

            // Tekken 3 = use 1.0 only (not 1.5x)

            // Einhander = parse linked list in pieces (todo)
            // Final Fantasy 4 = internal vram time (todo)
            // Rebel Assault 2 = parse linked list in pieces (todo)
            // Vampire Hunter D = allow edits to linked list (todo)
            scheduleGPUDMAIRQ(size);
            return false;

        default:
            PSXDMA_LOG("*** DMA 2 - GPU unknown *** %lx addr = %lx size = %lx\n", chcr, madr, bcr);
            break;
    }

    gpuInterrupt();
    return false;
}

bool PCSX::GPU::dmaBlockRequestLow() const {
    if (m_dmaDirection != 2) return false;
    for (auto *c : m_polygons) {
        if (m_processor == c) return true;
    }
    for (auto *c : m_lines) {
        if (m_processor == c) return true;
    }
    return false;
}

void PCSX::GPU::gpuInterrupt() {
    auto &mem = g_emulator->m_mem;
    mem->clearDMABusy<2>();
    mem->dmaInterrupt<2>();
}

bool PCSX::GPU::vram2MBFitted() const { return g_emulator->settings.get<Emulator::Setting2MBVRAM>(); }

unsigned PCSX::GPU::resolveVramY(unsigned y) const {
    // Gate closed: Y wraps within the 1MB region (9-bit), regardless of fitment.
    // This is retail-identical, and is the default for every existing game.
    if (!m_vram2MBGateOpen) return y & 0x1ff;
    // Gate open: full 10-bit Y honored.
    return y & 0x3ff;
}

bool PCSX::GPU::vramOpenBus(unsigned y) const {
    // Gate open but only 1MB fitted: an upper-half (Y>=512) access reads open
    // bus where the second chip would be. This is the real mechanism behind the
    // historical "texture disable" mislabel. Hardware-measured (retail 1MB,
    // SCPH-1001 + SCPH-5501): the bus floats high, returning a constant 0xFFFF,
    // independent of prior reads/writes; writes are dropped with no lower-bank
    // aliasing. See kVramOpenBusValue.
    return m_vram2MBGateOpen && !vram2MBFitted() && (y & 0x200);
}

void PCSX::GPU::writeStatus(uint32_t value) {
    uint32_t cmd = (value >> 24) & 0xff;
    bool gotUnknown = false;

    if (m_dumper) m_dumper->gp1(value);
    m_statusControl[cmd] = value;

    switch (cmd) {
        case 0: {
            m_readFifo->reset();
            m_processor->reset();
            m_defaultProcessor.setActive();
            m_dmaDirection = 0;
            // GP1(00h) also resets the display mode, which readStatusFromBus() keys on.
            m_statusControl[8] = 0x08000000;
            CtrlReset ctrl;
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
            m_textureWindowRaw = 0;
            m_drawingStartRaw = 0;
            m_drawingEndRaw = 0;
            m_drawingOffsetRaw = 0;
            for (unsigned i = 0; i < 7; i++) m_envRaw[i] = (0xe0 + i) << 24;
            m_dataRet = 0x400;
        } break;
        case 1: {
            CtrlClearFifo ctrl;
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 2: {
            CtrlIrqAck ctrl;
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 3: {
            CtrlDisplayEnable ctrl(value);
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 4: {
            m_dmaDirection = value & 3;
            CtrlDmaSetting ctrl(value);
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 5: {
            CtrlDisplayStart ctrl(value);
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 6: {
            CtrlHorizontalDisplayRange ctrl(value);
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 7: {
            CtrlVerticalDisplayRange ctrl(value);
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 8: {
            CtrlDisplayMode ctrl(value);
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        case 9: {
            // GP1(09h) - VRAM size / second-bank gate. Bit 0 = 1 opens the upper
            // 1MB bank for full 10-bit Y addressing; bit 0 = 0 is the retail
            // default (all Y wraps within 1MB, regardless of fitment). Always
            // present in the 208-pin GPU silicon; retail boards just leave the
            // second 1MB chip unpopulated.
            drainRenderer();
            m_vram2MBGateOpen = value & 1;
            vramConfigChanged();
        } break;
        case 16: {
            CtrlQuery ctrl(value);
            m_logger->addNode(ctrl, Logged::Origin::CTRLWRITE, value, 1);
            write1(&ctrl);
        } break;
        default: {
            gotUnknown = true;
        } break;
    }

    if (gotUnknown) {
        g_system->log(LogClass::GPU, "Got an unknown GPU control word: %08x\n", value);
    }
}

uint32_t PCSX::GPU::readData() {
    if (m_dumper) m_dumper->read(1);
    if (m_readFifo->size() == 0) {
        return m_dataRet;
    }
    return m_readFifo.asA<File>()->read<uint32_t>();
}

void PCSX::GPU::write1(CtrlQuery *ctrl) {
    switch (ctrl->type()) {
        case CtrlQuery::TextureWindow:
            m_dataRet = m_textureWindowRaw;
            return;
        case CtrlQuery::DrawAreaStart:
            m_dataRet = m_drawingStartRaw;
            return;
        case CtrlQuery::DrawAreaEnd:
            m_dataRet = m_drawingEndRaw;
            return;
        case CtrlQuery::DrawOffset:
            m_dataRet = m_drawingOffsetRaw;
            return;
    }
}

void PCSX::GPU::writeData(uint32_t value) {
    if (m_dumper) m_dumper->gp0(value);
    Buffer buf(value);
    m_processor->processWrite(buf, Logged::Origin::DATAWRITE, value, 1);
}

void PCSX::GPU::directDMAWrite(const uint32_t *feed, int transferSize, uint32_t hwAddr) {
    if (m_dumper) m_dumper->gp0LE(feed, transferSize);
    Buffer buf(feed, transferSize);
    while (!buf.isEmpty()) {
        m_processor->processWrite(buf, Logged::Origin::DIRECT_DMA, hwAddr, transferSize);
    }
}

void PCSX::GPU::directDMARead(uint32_t *dest, int transferSize, uint32_t hwAddr) {
    if (m_dumper) m_dumper->read(transferSize);
    auto size = m_readFifo->size();
    m_readFifo->read(dest, transferSize * 4);
    transferSize -= size / 4;
    dest += size / 4;
    while (transferSize != 0) {
        *dest++ = m_dataRet;
        transferSize--;
    }
}

void PCSX::GPU::chainedDMAWrite(const uint32_t *memory, uint32_t hwAddr) {
    uint32_t addr = hwAddr;
    uint32_t DMACommandCounter = 0;
    bool usingMsan = g_emulator->m_mem->msanInitialized();

    s_usedAddr[0] = s_usedAddr[1] = s_usedAddr[2] = 0xffffff;

    do {
        uint32_t header;
        const uint32_t *feed;
        if (usingMsan && PCSX::Memory::inMsanRange(addr)) {
            addr &= 0xfffffffc;
            const uint32_t *headerPtr = (uint32_t *)(g_emulator->m_mem->m_msanRAM + (addr - PCSX::Memory::c_msanStart));
            switch (g_emulator->m_mem->msanGetStatus<4>(addr)) {
                case PCSX::MsanStatus::UNINITIALIZED:
                    g_system->log(LogClass::GPU, _("GPU DMA went into usable but uninitialized msan memory: %8.8lx\n"),
                                  addr);
                    g_system->testQuit(PCSX::Memory::c_msanExitCode);
                    return;
                case PCSX::MsanStatus::UNUSABLE:
                    g_system->log(LogClass::GPU, _("GPU DMA went into unusable msan memory: %8.8lx\n"), addr);
                    g_system->testQuit(PCSX::Memory::c_msanExitCode);
                    return;
                case PCSX::MsanStatus::OK:
                    break;
            }
            header = *headerPtr;
            feed = headerPtr + 1;
        } else {
            addr &= g_emulator->getRamMask<4>();
            header = memory[addr / 4];
            feed = memory + addr / 4 + 1;
        }

        if (DMACommandCounter++ > 2000000) break;
        if (CheckForEndlessLoop(addr)) break;

        // # 32-bit blocks to transfer
        uint32_t transferWords = header >> 24;
        if (m_dumper && transferWords) m_dumper->gp0LE(feed, transferWords);
        Buffer buf(feed, transferWords);
        while (!buf.isEmpty()) {
            m_processor->processWrite(buf, Logged::Origin::CHAIN_DMA, addr, transferWords);
        }

        // next 32-bit pointer
        uint32_t nextAddr = header & 0xffffff;
        if (usingMsan && nextAddr == PCSX::Memory::c_msanChainMarker) {
            addr = g_emulator->m_mem->msanGetChainPtr(addr);
            continue;
        }
        addr = nextAddr;
    } while (!(addr & 0x800000));  // contrary to some documentation, the end-of-linked-list marker is not actually
}  // 0xFF'FFFF any pointer with bit 23 set will do.

void PCSX::GPU::Command::processWrite(Buffer &buf, Logged::Origin origin, uint32_t originValue, uint32_t length) {
    while (!buf.isEmpty()) {
        uint32_t value = buf.get();
        bool gotUnknown = false;
        const uint8_t cmdType = value >> 29;           // 3 topmost bits = command "type"
        const uint8_t command = (value >> 24) & 0x1f;  // 5 next bits = "command", which may be a bitfield

        const uint32_t packetInfo = value & c_PacketInfoMask;

        switch (cmdType) {
            case 0:  // GPU command
                switch (command) {
                    case 0x01: {  // clear cache
                        ClearCache prim;
                        m_gpu->write0(&prim);
                        m_gpu->m_logger->addNode(prim, origin, originValue, length);
                    } break;
                    case 0x02: {  // fast fill
                        buf.rewind();
                        m_gpu->m_fastFill.setActive();
                        m_gpu->m_fastFill.processWrite(buf, origin, originValue, length);
                    } break;
                    case 0x1f: {  // interrupt request (IRQ1)
                        m_gpu->requestIRQ1();
                    } break;
                    default: {
                        gotUnknown = true;
                    } break;
                }
                break;
            case 1: {  // Polygon primitive
                buf.rewind();
                m_gpu->m_polygons[command]->setActive();
                m_gpu->m_processor->processWrite(buf, origin, originValue, length);
            } break;
            case 2: {  // Line primitive
                buf.rewind();
                m_gpu->m_lines[command]->setActive();
                m_gpu->m_processor->processWrite(buf, origin, originValue, length);
            } break;
            case 3: {  // Rectangle primitive
                buf.rewind();
                m_gpu->m_rects[command]->setActive();
                m_gpu->m_processor->processWrite(buf, origin, originValue, length);
            } break;
            case 4: {  // Move data in VRAM
                m_gpu->m_blitVramVram.setActive();
                m_gpu->m_processor->processWrite(buf, origin, originValue, length);
            } break;
            case 5: {  // Write data to VRAM
                m_gpu->m_blitRamVram.setActive();
                m_gpu->m_processor->processWrite(buf, origin, originValue, length);
            } break;
            case 6: {  // Read data from VRAM
                m_gpu->m_blitVramRam.setActive();
                m_gpu->m_processor->processWrite(buf, origin, originValue, length);
            } break;
            case 7: {  // Environment command
                if (command >= 1 && command <= 6) m_gpu->m_envRaw[command] = value;
                switch (command) {
                    case 1: {  // tpage
                        TPage prim(packetInfo);
                        m_gpu->m_lastTPage = TPage(packetInfo);
                        m_gpu->m_logger->addNode(prim, origin, originValue, length);
                        m_gpu->write0(&prim);
                    } break;
                    case 2: {  // twindow
                        TWindow prim(packetInfo);
                        m_gpu->m_lastTWindow = TWindow(packetInfo);
                        m_gpu->m_logger->addNode(prim, origin, originValue, length);
                        m_gpu->write0(&prim);
                        m_gpu->m_textureWindowRaw = packetInfo & 0xfffff;
                    } break;
                    case 3: {  // drawing area top left
                        DrawingAreaStart prim(packetInfo);
                        m_gpu->m_logger->addNode(prim, origin, originValue, length);
                        m_gpu->write0(&prim);
                        m_gpu->m_drawingStartRaw = packetInfo & 0xfffff;
                    } break;
                    case 4: {  // drawing area bottom right
                        DrawingAreaEnd prim(packetInfo);
                        m_gpu->m_logger->addNode(prim, origin, originValue, length);
                        m_gpu->write0(&prim);
                        m_gpu->m_drawingEndRaw = packetInfo & 0xfffff;
                    } break;
                    case 5: {  // drawing offset
                        DrawingOffset prim(packetInfo);
                        m_gpu->m_lastOffset = DrawingOffset(packetInfo);
                        m_gpu->m_logger->addNode(prim, origin, originValue, length);
                        m_gpu->write0(&prim);
                        m_gpu->m_drawingOffsetRaw = packetInfo & 0x3fffff;
                    } break;
                    case 6: {  // mask bit
                        MaskBit prim(packetInfo);
                        m_gpu->m_logger->addNode(prim, origin, originValue, length);
                        m_gpu->write0(&prim);
                    } break;
                    default: {
                        gotUnknown = true;
                    } break;
                }
            } break;
        }
        if (gotUnknown && (value != 0)) {
            g_system->log(LogClass::GPU, "Got an unknown GPU data word: %08x (cmdType: %hhu, command: %hhu)\n", value,
                          cmdType, command);
        }
    }
}

void PCSX::GPU::FastFill::processWrite(Buffer &buf, Logged::Origin origin, uint32_t origvalue, uint32_t length) {
    uint32_t value = buf.get();
    switch (m_state) {
        case READ_COLOR:
            color = value & 0xffffff;
            m_state = READ_XY;
            if (buf.isEmpty()) return;
            value = buf.get();
            [[fallthrough]];
        case READ_XY:
            x = value & 0xffff;
            y = value >> 16;
            m_state = READ_WH;
            if (buf.isEmpty()) return;
            value = buf.get();
            [[fallthrough]];
        case READ_WH:
            w = value & 0xffff;
            h = value >> 16;
            raw.x = x;
            raw.y = y;
            raw.w = w;
            raw.h = h;
            // GP0(02h) parameter masking (psx-spx): Xpos ignores its low 4 bits,
            // Xsiz rounds UP to the next multiple of 16 within 0..400h, Ysiz is
            // masked to 9 bits. A fill whose effective Xsiz or Ysiz is 0 does
            // nothing at all - so a Ysiz that is a multiple of 512 (e.g. 512 -> 0)
            // is silently rejected, and Param=400h is treated as Xsiz=0. This is
            // pure command decode, independent of the bank gate / VRAM fitment;
            // the 2MB Y reach is resolved downstream by resolveVramY, never
            // masked away here.
            x &= 0x3f0;
            w = ((w & 0x3ff) + 0xf) & ~0xf;
            h = h & 0x1ff;
            m_state = READ_COLOR;
            m_gpu->m_defaultProcessor.setActive();
            m_gpu->m_logger->addNode(*this, origin, origvalue, length);
            if (w == 0 || h == 0) return;  // Xsiz=0 or Ysiz=0: no fill
            // Gate open: clip against the full 1024x1024 buffer so a fast-fill
            // can target the upper bank; the soft fillArea still bounds the
            // actual fill to the effective drawable height (512 unless 2MB).
            // Gate closed: retail, Y folds into the lower 512 rows.
            if (m_gpu->vram2MBGateOpen()) {
                clipped = GPU::clip<unsigned, 1024, 1024>(x, y, w, h);
            } else {
                clipped = GPU::clip(x, y, w, h);
            }
            m_gpu->write0(this);
            return;
    }
}

void PCSX::GPU::BlitVramVram::processWrite(Buffer &buf, Logged::Origin origin, uint32_t origvalue, uint32_t length) {
    uint32_t value;
    switch (m_state) {
        case READ_COMMAND:
            m_state = READ_SRC_XY;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_SRC_XY:
            value = buf.get();
            sX = signExtend<int, 11>(value & 0xffff);
            sY = signExtend<int, 11>(value >> 16);
            m_state = READ_DST_XY;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_DST_XY:
            value = buf.get();
            dX = signExtend<int, 11>(value & 0xffff);
            dY = signExtend<int, 11>(value >> 16);
            m_state = READ_HW;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_HW:
            value = buf.get();
            w = value & 0xffff;
            h = value >> 16;
            raw.sX = sX;
            raw.sY = sY;
            raw.dX = dX;
            raw.dY = dY;
            raw.w = w;
            raw.h = h;
            // psx-spx "Masking for COPY Commands": effective transfer size is
            // Xsiz_eff = ((Xsiz-1) AND 3FFh)+1, Ysiz_eff = ((Ysiz-1) AND 1FFh)+1.
            w = ((w - 1) & 0x3ff) + 1;
            h = ((h - 1) & 0x1ff) + 1;
            // Clip against the full 1024x1024 physical buffer, not the retail
            // 512 height: an upper-bank (Y>=512) source or destination must
            // survive decode so the bank-aware copy in write0 (which routes
            // every row through resolveVramY) can run. With the default
            // hMax=512 a copy touching the upper bank had its height clamped
            // to 0 and was silently dropped. Matches the upload/readback clip.
            // Gate open: both source and destination Y wrap mod-1024 (silicon
            // WRAP for copy, transfer-wrap-y 2026-06-15; psx-spx: src/dst may
            // straddle the Y=512 bank boundary cleanly). Clamp X only and let
            // write0 route every row through resolveVramY. Gate closed: retail.
            if (m_gpu->vram2MBGateOpen()) {
                clipped = GPU::clipX<unsigned, 1024>(sX, w);
                clipped |= GPU::clipX<unsigned, 1024>(dX, w);
            } else {
                clipped = GPU::clip<unsigned, 1024, 1024>(sX, sY, w, h);
                clipped |= GPU::clip<unsigned, 1024, 1024>(dX, dY, w, h);
            }
            m_state = READ_COMMAND;
            m_gpu->m_defaultProcessor.setActive();
            m_gpu->m_logger->addNode(*this, origin, origvalue, length);
            m_gpu->write0(this);
            return;
    }
}

void PCSX::GPU::BlitRamVram::processWrite(Buffer &buf, Logged::Origin origin, uint32_t origvalue, uint32_t length) {
    uint32_t value;
    size_t size = (w * h + 1) / 2;
    bool done = false;
    switch (m_state) {
        case READ_COMMAND:
            m_state = READ_XY;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_XY:
            value = buf.get();
            x = value & 0xffff;
            y = value >> 16;
            m_state = READ_HW;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_HW:
            value = buf.get();
            w = value & 0xffff;
            h = value >> 16;
            raw.x = x;
            raw.y = y;
            raw.w = w;
            raw.h = h;
            // psx-spx "Masking for COPY Commands": the GPU transfers
            // Xsiz_eff = ((Xsiz-1) AND 3FFh)+1 columns (1..1024) and
            // Ysiz_eff = ((Ysiz-1) AND 1FFh)+1 rows (1..512). Inputs outside
            // those ranges wrap, so the FIFO handshake (and the destination
            // row loop) must use the effective counts. Using the raw value
            // desyncs the transfer: too few words sent stalls the GPU forever
            // waiting for the rest, too many corrupts the command stream.
            w = ((w - 1) & 0x3ff) + 1;
            h = ((h - 1) & 0x1ff) + 1;
            size = (w * h + 1) / 2;
            m_data.clear();
            m_data.reserve(size * 4);
            m_state = READ_PIXELS;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_PIXELS:
            if ((buf.size() >= size) && (m_data.empty())) {
                data.borrow(buf.data(), size * 4);
                buf.consume(size);
                done = true;
            } else {
                size_t toConsume = std::min(buf.size(), size - m_data.size() / 4);
                m_data.append(reinterpret_cast<const char *>(buf.data()), toConsume * 4);
                done = m_data.size() == size * 4;
                buf.consume(toConsume);
                if (done) {
                    data.acquire(std::move(m_data));
                    m_data.clear();
                }
            }
            break;
    }
    if (done) {
        // raw.{x,y,w,h} captured at READ_HW before the COPY-size masking so
        // the logger keeps the values the game actually sent.
        // Gate open: Y wraps mod-1024 per row via resolveVramY (silicon-
        // measured WRAP, transfer-wrap-y 2026-06-15), so clamp X only and let
        // the per-row loop run the full height. Gate closed: retail clip.
        if (m_gpu->vram2MBGateOpen()) {
            clipped = GPU::clipX<unsigned, 1024>(x, w);
        } else {
            clipped = GPU::clip<unsigned, 1024, 1024>(x, y, w, h);
        }
        m_state = READ_COMMAND;
        m_gpu->m_defaultProcessor.setActive();
        m_gpu->m_logger->addNode(*this, origin, origvalue, length);
        {
            const uint16_t *src = data.data<uint16_t>();
            for (int row = 0; row < h; row++) {
                // Drop writes to an opened-but-unpopulated upper bank (1MB
                // fitment): they hit floating bus and are lost.
                if (m_gpu->vramOpenBus(y + row)) continue;
                m_gpu->partialUpdateVRAM(x, m_gpu->resolveVramY(y + row), w, 1, src + row * w,
                                         PartialUpdateVram::Synchronous);
            }
        }
    }
}

void PCSX::GPU::BlitRamVram::execute(GPU *gpu) {
    const uint16_t *src = data.data<uint16_t>();
    for (int row = 0; row < h; row++) {
        // Drop writes to an opened-but-unpopulated upper bank (1MB fitment).
        if (gpu->vramOpenBus(y + row)) continue;
        gpu->partialUpdateVRAM(x, gpu->resolveVramY(y + row), w, 1, src + row * w);
    }
}

void PCSX::GPU::BlitVramRam::processWrite(Buffer &buf, Logged::Origin origin, uint32_t origvalue, uint32_t length) {
    uint32_t value;
    switch (m_state) {
        case READ_COMMAND:
            m_gpu->m_readFifo->reset();
            m_state = READ_XY;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_XY:
            value = buf.get();
            x = value & 0xffff;
            y = value >> 16;
            m_state = READ_HW;
            if (buf.isEmpty()) return;
            [[fallthrough]];
        case READ_HW:
            value = buf.get();
            w = value & 0xffff;
            h = value >> 16;
            raw.x = x;
            raw.y = y;
            raw.w = w;
            raw.h = h;
            // psx-spx "Masking for COPY Commands": effective transfer size is
            // Xsiz_eff = ((Xsiz-1) AND 3FFh)+1, Ysiz_eff = ((Ysiz-1) AND 1FFh)+1.
            w = ((w - 1) & 0x3ff) + 1;
            h = ((h - 1) & 0x1ff) + 1;
            // Gate open: Y wraps mod-1024 per row via resolveVramY (silicon
            // WRAP, transfer-wrap-y 2026-06-15); clamp X only. Gate closed: retail.
            if (m_gpu->vram2MBGateOpen()) {
                clipped = GPU::clipX<unsigned, 1024>(x, w);
            } else {
                clipped = GPU::clip<unsigned, 1024, 1024>(x, y, w, h);
            }
            m_state = READ_COMMAND;
            m_gpu->m_defaultProcessor.setActive();
            m_gpu->m_logger->addNode(*this, origin, origvalue, length);
            m_gpu->m_vramReadSlice = m_gpu->getVRAM();
            for (auto l = y; l < y + h; l++) {
                Slice slice;
                if (m_gpu->vramOpenBus(l)) {
                    // Not every backend keeps the unpopulated bank prefilled, so
                    // produce the floating-bus words here.
                    static const std::array<uint16_t, 1024> openBus = [] {
                        std::array<uint16_t, 1024> a;
                        a.fill(kVramOpenBusValue);
                        return a;
                    }();
                    slice.borrow(openBus.data(), w * 2);
                    m_gpu->m_readFifo->pushSlice(std::move(slice));
                    continue;
                }
                slice.borrow(m_gpu->m_vramReadSlice, (m_gpu->resolveVramY(l) * 1024 + x) * 2, w * 2);
                m_gpu->m_readFifo->pushSlice(std::move(slice));
            }
            return;
    }
}

PCSX::GPU::TPage::TPage(uint32_t value) {
    raw = value;
    tx = value & 0x0f;
    blendFunction = magic_enum::enum_cast<BlendFunction>((value >> 5) & 3).value();
    auto depth = (value >> 7) & 3;
    texDepth = magic_enum::enum_cast<TexDepth>(depth == 3 ? 2 : depth).value();
    dither = (value >> 9) & 1;
    drawToDisplay = (value >> 10) & 1;
    xflip = (value >> 12) & 1;
    yflip = (value >> 13) & 1;
    // Texture-page Y base. Retail uses a single bit (bit 4 -> page row 0 or
    // 256). With a 2MB bank fitted, bit 11 - the historical, inert "texture
    // disable" bit - becomes the high bit of a 2-bit page selector, so the base
    // can be 0/256/512/768 and texture pages can live in the upper bank. On 1MB
    // that bit stays the (unused) texDisable flag and ty stays a single bit.
    if (g_emulator->settings.get<Emulator::Setting2MBVRAM>().value) {
        ty = ((value >> 4) & 1) | (((value >> 11) & 1) << 1);
        texDisable = false;
    } else {
        ty = (value >> 4) & 1;
        texDisable = (value >> 11) & 1;
    }
}

PCSX::GPU::TWindow::TWindow(uint32_t value) {
    x = value & c_TWindowFieldMask;
    y = (value >> 5) & c_TWindowFieldMask;
    w = (value >> 10) & c_TWindowFieldMask;
    h = (value >> 15) & c_TWindowFieldMask;

    raw = value;
}

PCSX::GPU::DrawingAreaStart::DrawingAreaStart(uint32_t value) {
    x = value & c_Coord10Mask;
    // Decode the full 10-bit Y the command carries (bit 19 is the upper-bank
    // bit). The gate/fitment semantics are applied downstream via resolveVramY,
    // not baked in here; on retail this bit is 0 so the value is unchanged.
    y = (value >> c_DrawingAreaYShift) & c_Coord10Mask;

    raw = value;
}

PCSX::GPU::DrawingAreaEnd::DrawingAreaEnd(uint32_t value) {
    x = value & c_Coord10Mask;
    y = (value >> c_DrawingAreaYShift) & c_Coord10Mask;

    raw = value;
}

PCSX::GPU::DrawingOffset::DrawingOffset(uint32_t value) {
    int ux = value & c_Coord11Mask;
    int uy = (value >> c_DrawingOffsetYShift) & c_Coord11Mask;
    x = signExtend<int, 11>(ux);
    y = signExtend<int, 11>(uy);

    raw = value;
}

PCSX::GPU::MaskBit::MaskBit(uint32_t value) {
    set = value & 1;
    check = (value >> 1) & 1;
}

void PCSX::GPU::write0(BlitVramVram *prim) {
    auto inSlice = getVRAM();

    auto sX = prim->sX;
    auto sY = prim->sY;
    auto dX = prim->dX;
    auto dY = prim->dY;
    auto w = prim->w;
    auto h = prim->h;

    if (m_vram2MBGateOpen) {
        // Bank-aware copy: source and destination Y each wrap mod-1024 per row
        // (resolveVramY), matching silicon (transfer-wrap-y verdict=WRAP for
        // copy, 2026-06-15). X was clamped at decode (clipX on both sX and dX)
        // and h was capped to <=512 by the COPY Ysiz mask, so no further
        // clamping is needed here. Snapshot the whole source first so an
        // overlapping src/dst copy stays correct, then write each destination
        // row, dropping rows that hit an unpopulated upper bank (open bus on a
        // 1MB fitment); source reads of an unpopulated upper bank land on the
        // 0xFFFF prefill, so no explicit source guard is needed.
        if ((w == 0) || (h == 0)) return;
        std::vector<uint16_t> rect;
        rect.resize((size_t)h * w);
        for (unsigned l = 0; l < h; l++) {
            Slice slice;
            slice.borrow(inSlice, ((size_t)resolveVramY(sY + l) * 1024 + sX) * sizeof(uint16_t), w * sizeof(uint16_t));
            memcpy(rect.data() + (size_t)l * w, slice.data(), slice.size());
        }
        for (unsigned l = 0; l < h; l++) {
            if (vramOpenBus(dY + l)) continue;
            partialUpdateVRAM(dX, resolveVramY(dY + l), w, 1, rect.data() + (size_t)l * w,
                              PartialUpdateVram::Synchronous);
        }
        return;
    }

    if (sX > 1024) {
        w -= sX - 1024;
        sX = 1024;
    }
    if (sY > 512) {
        h -= sY - 512;
        sY = 512;
    }
    if (dX > 1024) {
        w -= dX - 1024;
        dX = 1024;
    }
    if (dY > 512) {
        h -= dY - 512;
        dY = 512;
    }

    if ((w == 0) || (h == 0)) return;

    std::vector<uint16_t> rect;
    rect.resize(h * w);
    for (unsigned l = 0; l < h; l++) {
        Slice slice;
        slice.borrow(inSlice, ((l + sY) * 1024 + sX) * sizeof(uint16_t), w * sizeof(uint16_t));
        memcpy(rect.data() + l * w, slice.data(), slice.size());
    }
    partialUpdateVRAM(dX, dY, w, h, rect.data(), PartialUpdateVram::Synchronous);
}

// These technically belong to gpulogger.cc, but due to the template instanciation, they need to be here

void PCSX::GPU::Logged::drawColorBox(uint32_t color, unsigned itemIndex, unsigned colorIndex,
                                     const DrawLogSettings &settings) {
    auto R = (color >> 0) & 0xff;
    auto G = (color >> 8) & 0xff;
    auto B = (color >> 16) & 0xff;
    ImGui::ColorButton(fmt::format("##ColorBox%i%i", itemIndex, colorIndex).c_str(),
                       ImVec4{R / 255.0f, G / 255.0f, B / 255.0f, 1.0f}, ImGuiColorEditFlags_NoAlpha);

    switch (settings.colorFormat) {
        case DrawLogSettings::ColorFormat::None:
            break;
        case DrawLogSettings::ColorFormat::Expanded:
            ImGui::SameLine();
            ImGui::Text(" R: %i, G: %i, B: %i", R, G, B);
            break;
        case DrawLogSettings::ColorFormat::HTML: {
            ImGui::SameLine();
            ImGui::TextUnformatted(" ");
            ImGui::SameLine();
            std::string label = fmt::format("#{:02X}{:02X}{:02X}###ColorBox%i%i", R, G, B, itemIndex, colorIndex);
            if (ImGui::Button(label.c_str())) {
                ImGui::SetClipboardText(fmt::format("{:02X}{:02X}{:02X}", R, G, B).c_str());
            }
        } break;
    }
}

template <PCSX::GPU::Shading shading, PCSX::GPU::Shape shape, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend,
          PCSX::GPU::Modulation modulation>
void PCSX::GPU::Poly<shading, shape, textured, blend, modulation>::drawLogNode(unsigned itemIndex,
                                                                               const DrawLogSettings &settings) {
    if constexpr ((textured == Textured::No) || (modulation == Modulation::On)) {
        if constexpr (shading == Shading::Flat) {
            ImGui::TextUnformatted(_("Shading: Flat"));
            drawColorBox(colors[0], itemIndex, 0, settings);
        } else if constexpr (shading == Shading::Gouraud) {
            ImGui::TextUnformatted(_("Shading: Gouraud"));
        }
    }
    if constexpr (textured == Textured::Yes) {
        ImGui::TextUnformatted(_("Textured"));
    }
    if constexpr (blend == Blend::Semi) {
        ImGui::TextUnformatted(_("Semi-transparency blending"));
    }
    if constexpr (textured == Textured::Yes) {
        ImGui::Separator();
        tpage.drawLogNodeCommon();
        if (tpage.texDepth != TexDepth::Tex16Bits) {
            std::string label = fmt::format("  ClutX: {}, ClutY: {}", clutX(), clutY());
            if (ImGui::Button(label.c_str())) {
                g_system->m_eventBus->signal(Events::GUI::SelectClut{clutX(), clutY()});
            }
        }
    }
    int minX = 2048, minY = 1024, maxX = -1024, maxY = -512;
    int minU = 2048, minV = 1024, maxU = -1024, maxV = -512;
    for (unsigned i = 0; i < count; i++) {
        ImGui::PushID(i);
        ImGui::Separator();
        ImGui::Text(_("Vertex %i"), i);
        ImGui::Text("  X: %i + %i = %i, Y: %i + %i = %i", x[i], offset.x, x[i] + offset.x, y[i], offset.y,
                    y[i] + offset.y);
        minX = std::min(minX, x[i] + offset.x);
        minY = std::min(minY, y[i] + offset.y);
        maxX = std::max(maxX, x[i] + offset.x);
        maxY = std::max(maxY, y[i] + offset.y);
        if constexpr ((shading == Shading::Gouraud) && ((textured == Textured::No) || (modulation == Modulation::On))) {
            drawColorBox(colors[i], itemIndex, i, settings);
        }
        if constexpr (textured == Textured::Yes) {
            unsigned tx = tpage.tx * 64;
            unsigned ty = tpage.ty * 256;
            unsigned shift = tpage.texDepth == TexDepth::Tex4Bits ? 2 : tpage.texDepth == TexDepth::Tex8Bits ? 1 : 0;
            ImGui::Text("  U: %i >> %i + %i = %i, V: %i + %i = %i", u[i], shift, tx, (u[i] >> shift) + tx, v[i], ty,
                        v[i] + ty);
            minU = std::min(minU, int(u[i] >> shift));
            minV = std::min(minV, int(v[i]));
            maxU = std::max(maxU, int(u[i] >> shift));
            maxV = std::max(maxV, int(v[i]));
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    std::string label = fmt::format(f_("Go to primitive##{}"), itemIndex);
    if (ImGui::Button(label.c_str())) {
        g_system->m_eventBus->signal(Events::GUI::VRAMFocus{minX, minY, maxX, maxY});
    }
    if constexpr (textured == Textured::Yes) {
        unsigned tx = tpage.tx * 64;
        unsigned ty = tpage.ty * 256;
        ImGui::SameLine();
        std::string label = fmt::format(f_("Go to texture##{}"), itemIndex);
        if (ImGui::Button(label.c_str())) {
            const auto mode = tpage.texDepth == TexDepth::Tex16Bits  ? Events::GUI::VRAM_16BITS
                              : tpage.texDepth == TexDepth::Tex8Bits ? Events::GUI::VRAM_8BITS
                                                                     : Events::GUI::VRAM_4BITS;
            g_system->m_eventBus->signal(Events::GUI::SelectClut{clutX(), clutY()});
            g_system->m_eventBus->signal(
                Events::GUI::VRAMFocus{int(minU + tx), int(minV + ty), int(maxU + tx), int(maxV + ty), mode});
        }
        if (tpage.texDepth != TexDepth::Tex16Bits) {
            ImGui::SameLine();
            std::string label = fmt::format(f_("Go to CLUT##{}"), itemIndex);
            if (ImGui::Button(label.c_str())) {
                int depth = tpage.texDepth == TexDepth::Tex4Bits ? 16 : 256;
                g_system->m_eventBus->signal(
                    Events::GUI::VRAMFocus{int(clutX()), int(clutY()), int(clutX() + depth), int(clutY() + 1)});
            }
        }
    }
}

template <PCSX::GPU::Shading shading, PCSX::GPU::LineType lineType, PCSX::GPU::Blend blend>
void PCSX::GPU::Line<shading, lineType, blend>::drawLogNode(unsigned itemIndex, const DrawLogSettings &settings) {
    int minX = x[0], minY = y[0], maxX = x[0], maxY = y[0];
    if constexpr (shading == Shading::Flat) {
        ImGui::TextUnformatted(_("Shading: Flat"));
        drawColorBox(colors[0], itemIndex, 0, settings);
    } else if constexpr (shading == Shading::Gouraud) {
        ImGui::TextUnformatted(_("Shading: Gouraud"));
    }
    if constexpr (blend == Blend::Semi) {
        ImGui::TextUnformatted(_("Semi-transparency blending"));
    }
    for (unsigned i = 1; i < colors.size(); i++) {
        ImGui::PushID(i);
        ImGui::Separator();
        if constexpr (lineType == LineType::Poly) {
            ImGui::Text(_("Line %i"), i);
        }
        ImGui::Text("  X0: %i + %i = %i, Y0: %i + %i = %i", x[i - 1], offset.x, x[i - 1] + offset.x, y[i - 1], offset.y,
                    y[i - 1] + offset.y);
        if constexpr (shading == Shading::Gouraud) {
            drawColorBox(colors[i - 1], itemIndex, i * 2, settings);
        }
        ImGui::Text("  X1: %i + %i = %i, Y1: %i + %i = %i", x[i], offset.x, x[i] + offset.x, y[i], offset.y,
                    y[i] + offset.y);
        if constexpr (shading == Shading::Gouraud) {
            drawColorBox(colors[i], itemIndex, i * 2 + 1, settings);
        }
        minX = std::min(minX, x[i] + offset.x);
        minY = std::min(minY, y[i] + offset.y);
        maxX = std::max(maxX, x[i] + offset.x);
        maxY = std::max(maxY, y[i] + offset.y);
        ImGui::PopID();
    }
    ImGui::Separator();
    std::string label = fmt::format(f_("Go to primitive##{}"), itemIndex);
    if (ImGui::Button(label.c_str())) {
        g_system->m_eventBus->signal(Events::GUI::VRAMFocus{minX, minY, maxX, maxY});
    }
}

template <PCSX::GPU::Size size, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend, PCSX::GPU::Modulation modulation>
void PCSX::GPU::Rect<size, textured, blend, modulation>::drawLogNode(unsigned itemIndex,
                                                                     const DrawLogSettings &settings) {
    ImGui::Text("  X0: %i + %i = %i, Y0: %i + %i = %i", x, offset.x, x + offset.x, y, offset.y, y + offset.y);
    ImGui::Text("  X1: %i + %i = %i, Y1: %i + %i = %i", x + w, offset.x, x + w + offset.x, y + h, offset.y,
                y + h + offset.y);
    ImGui::Text("  W: %i, H: %i", w, h);
    if constexpr ((textured == Textured::No) || (modulation == Modulation::On)) {
        drawColorBox(color, itemIndex, 0, settings);
    }
    if constexpr (blend == Blend::Semi) {
        ImGui::TextUnformatted(_("Semi-transparency blending"));
    }
    if constexpr (textured == Textured::Yes) {
        unsigned tx = tpage.tx * 64;
        unsigned ty = tpage.ty * 256;
        unsigned shift = tpage.texDepth == TexDepth::Tex4Bits ? 2 : tpage.texDepth == TexDepth::Tex8Bits ? 1 : 0;
        ImGui::Text("  U: %i >> %i + %i = %i, V: %i + %i = %i", u, shift, tx, (u >> shift) + tx, v, ty, v + ty);
        std::string label = fmt::format("  ClutX: {}, ClutY: {}", clutX(), clutY());
        if (ImGui::Button(label.c_str())) {
            g_system->m_eventBus->signal(Events::GUI::SelectClut{clutX(), clutY()});
        }
    }
    ImGui::Separator();
    std::string label = fmt::format(f_("Go to primitive##{}"), itemIndex);
    if (ImGui::Button(label.c_str())) {
        g_system->m_eventBus->signal(
            Events::GUI::VRAMFocus{x + offset.x, y + offset.y, x + w + offset.x, y + h + offset.y});
    }
    if constexpr (textured == Textured::Yes) {
        unsigned tx = tpage.tx * 64;
        unsigned ty = tpage.ty * 256;
        unsigned shift = tpage.texDepth == TexDepth::Tex4Bits ? 2 : tpage.texDepth == TexDepth::Tex8Bits ? 1 : 0;
        ImGui::SameLine();
        std::string label = fmt::format(f_("Go to texture##{}"), itemIndex);
        if (ImGui::Button(label.c_str())) {
            const auto mode = tpage.texDepth == TexDepth::Tex16Bits  ? Events::GUI::VRAM_16BITS
                              : tpage.texDepth == TexDepth::Tex8Bits ? Events::GUI::VRAM_8BITS
                                                                     : Events::GUI::VRAM_4BITS;
            g_system->m_eventBus->signal(Events::GUI::SelectClut{clutX(), clutY()});
            g_system->m_eventBus->signal(Events::GUI::VRAMFocus{int((u >> shift) + tx), int(v + ty),
                                                                int(((u + w) >> shift) + tx), int(v + h + ty), mode});
        }
        if (tpage.texDepth != TexDepth::Tex16Bits) {
            ImGui::SameLine();
            std::string label = fmt::format(f_("Go to CLUT##{}"), itemIndex);
            if (ImGui::Button(label.c_str())) {
                g_system->m_eventBus->signal(
                    Events::GUI::VRAMFocus{int(clutX()), int(clutY()), int(clutX() + 256), int(clutY() + 1)});
            }
        }
    }
}

static float triangleArea(float x1, float x2, float x3, float y1, float y2, float y3) {
    float adx = x1 - x2;
    float ady = y1 - y2;
    float bdx = x1 - x3;
    float bdy = y1 - y3;
    float cdx = x2 - x3;
    float cdy = y2 - y3;
    float a = sqrtf(adx * adx + ady * ady);
    float b = sqrtf(bdx * bdx + bdy * bdy);
    float c = sqrtf(cdx * cdx + cdy * cdy);
    float s = (a + b + c) / 2;
    return sqrtf(s * (s - a) * (s - b) * (s - c));
}

template <PCSX::GPU::Shading shading, PCSX::GPU::Shape shape, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend,
          PCSX::GPU::Modulation modulation>
void PCSX::GPU::Poly<shading, shape, textured, blend, modulation>::generateStatsInfo() {
    float pixelArea = 0.0f;
    float textureArea = 0.0f;

    pixelArea = triangleArea(x[0], x[1], x[2], y[0], y[1], y[2]);
    if constexpr (shape == Shape::Quad) {
        pixelArea += triangleArea(x[1], x[2], x[3], y[1], y[2], y[3]);
    }
    if constexpr (textured == Textured::Yes) {
        unsigned shift = tpage.texDepth == TexDepth::Tex4Bits ? 2 : tpage.texDepth == TexDepth::Tex8Bits ? 1 : 0;
        textureArea += triangleArea(u[0] >> shift, u[1] >> shift, u[2] >> shift, v[0], v[1], v[2]);
        if constexpr (shape == Shape::Quad) {
            textureArea += triangleArea(u[1] >> shift, u[2] >> shift, u[3] >> shift, v[1], v[2], v[3]);
        }
    }

    if constexpr (textured == Textured::Yes) {
        stats.texturedTriangles = shape == Shape::Quad ? 2 : 1;
    } else {
        stats.triangles = shape == Shape::Quad ? 2 : 1;
    }
    stats.pixelWrites = pixelArea;
    if constexpr (blend == Blend::Semi) {
        stats.pixelReads = pixelArea;
    }
    stats.texelReads = textureArea;
}

template <PCSX::GPU::Shading shading, PCSX::GPU::LineType lineType, PCSX::GPU::Blend blend>
void PCSX::GPU::Line<shading, lineType, blend>::generateStatsInfo() {
    unsigned pixels = 0;
    for (unsigned i = 1; i < colors.size(); i++) {
        auto dx = std::abs(x[i] - x[i - 1]);
        auto dy = std::abs(y[i] - y[i - 1]);
        if (dx > dy) {
            pixels += dx;
        } else {
            pixels += dy;
        }
    }

    stats.pixelWrites += pixels;
    if constexpr (blend == Blend::Semi) {
        stats.pixelReads += pixels;
    }
}

template <PCSX::GPU::Shading shading, PCSX::GPU::Shape shape, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend,
          PCSX::GPU::Modulation modulation>
void PCSX::GPU::Poly<shading, shape, textured, blend, modulation>::cumulateStats(GPUStats *accum) {
    *accum += stats;
}

template <PCSX::GPU::Shading shading, PCSX::GPU::LineType lineType, PCSX::GPU::Blend blend>
void PCSX::GPU::Line<shading, lineType, blend>::cumulateStats(GPUStats *accum) {
    *accum += stats;
}

template <PCSX::GPU::Size size, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend, PCSX::GPU::Modulation modulation>
void PCSX::GPU::Rect<size, textured, blend, modulation>::cumulateStats(GPUStats *stats) {
    auto s = h * w;
    stats->pixelWrites += s;
    if constexpr (textured == Textured::Yes) {
        unsigned shift = tpage.texDepth == TexDepth::Tex4Bits ? 2 : tpage.texDepth == TexDepth::Tex8Bits ? 1 : 0;
        stats->texelReads += s >> shift;
        stats->sprites++;
    } else {
        stats->rectangles++;
    }
    if constexpr (blend == Blend::Semi) {
        stats->pixelReads += s;
    }
}

// The GPU does not wrap the vertex plus drawing offset sum. It wraps the pixel address modulo 2048 on both axes
// before the drawing-area clip (gpu-raster-phase23 W0..W5), so a primitive reaching below -1024 also covers the
// same pixels shifted by 2048. The logger emits that copy too, or its picture of what a primitive touched is not
// what was drawn.
static inline int wrapCoord(int v, int off) { return v + off; }

static void addWrapped(const PCSX::GPU::Logged::AddTri &add, PCSX::OpenGL::ivec2 a, PCSX::OpenGL::ivec2 b,
                       PCSX::OpenGL::ivec2 c) {
    const bool wrapX = std::min({a.x(), b.x(), c.x()}) < -1024;
    const bool wrapY = std::min({a.y(), b.y(), c.y()}) < -1024;
    auto shifted = [&](int dx, int dy) {
        add(PCSX::OpenGL::ivec2(a.x() + dx, a.y() + dy), PCSX::OpenGL::ivec2(b.x() + dx, b.y() + dy),
            PCSX::OpenGL::ivec2(c.x() + dx, c.y() + dy));
    };
    shifted(0, 0);
    if (wrapX) shifted(2048, 0);
    if (wrapY) shifted(0, 2048);
    if (wrapX && wrapY) shifted(2048, 2048);
}

template <PCSX::GPU::Shading shading, PCSX::GPU::Shape shape, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend,
          PCSX::GPU::Modulation modulation>
void PCSX::GPU::Poly<shading, shape, textured, blend, modulation>::getVertices(AddTri &&add, PixelOp op) {
    if (op == PixelOp::WRITE) {
        addWrapped(add, {wrapCoord(x[0], offset.x), wrapCoord(y[0], offset.y)},
                   {wrapCoord(x[1], offset.x), wrapCoord(y[1], offset.y)},
                   {wrapCoord(x[2], offset.x), wrapCoord(y[2], offset.y)});
        if constexpr (shape == Shape::Quad) {
            addWrapped(add, {wrapCoord(x[1], offset.x), wrapCoord(y[1], offset.y)},
                       {wrapCoord(x[2], offset.x), wrapCoord(y[2], offset.y)},
                       {wrapCoord(x[3], offset.x), wrapCoord(y[3], offset.y)});
        }
    } else if (op == PixelOp::READ) {
        if constexpr (blend == Blend::Semi) {
            addWrapped(add, {wrapCoord(x[0], offset.x), wrapCoord(y[0], offset.y)},
                       {wrapCoord(x[1], offset.x), wrapCoord(y[1], offset.y)},
                       {wrapCoord(x[2], offset.x), wrapCoord(y[2], offset.y)});
            if constexpr (shape == Shape::Quad) {
                addWrapped(add, {wrapCoord(x[1], offset.x), wrapCoord(y[1], offset.y)},
                           {wrapCoord(x[2], offset.x), wrapCoord(y[2], offset.y)},
                           {wrapCoord(x[3], offset.x), wrapCoord(y[3], offset.y)});
            }
        }
        if constexpr (textured == Textured::Yes) {
            unsigned tx = tpage.tx * 64;
            unsigned ty = tpage.ty * 256;
            unsigned shift = tpage.texDepth == TexDepth::Tex4Bits ? 2 : tpage.texDepth == TexDepth::Tex8Bits ? 1 : 0;
            add({int((u[0] >> shift) + tx), int(v[0] + ty)}, {int((u[1] >> shift) + tx), int(v[1] + ty)},
                {int((u[2] >> shift) + tx), int(v[2] + ty)});
            if constexpr (shape == Shape::Quad) {
                add({int((u[1] >> shift) + tx), int(v[1] + ty)}, {int((u[2] >> shift) + tx), int(v[2] + ty)},
                    {int((u[3] >> shift) + tx), int(v[3] + ty)});
            }
            if (tpage.texDepth == TexDepth::Tex4Bits) {
                addLine(std::move(add), clutX(), clutY(), clutX() + 16, clutY());
            } else if (tpage.texDepth == TexDepth::Tex8Bits) {
                addLine(std::move(add), clutX(), clutY(), clutX() + 256, clutY());
            }
        }
    }
}

template <PCSX::GPU::Shading shading, PCSX::GPU::LineType lineType, PCSX::GPU::Blend blend>
void PCSX::GPU::Line<shading, lineType, blend>::getVertices(AddTri &&add, PixelOp op) {
    if constexpr (blend == Blend::Off) {
        if (op == PixelOp::READ) return;
    }
    auto count = colors.size();
    for (unsigned i = 1; i < count; i++) {
        auto x0 = x[i - 1];
        auto x1 = x[i];
        auto y0 = y[i - 1];
        auto y1 = y[i];

        addLine([&add](auto v1, auto v2, auto v3) { addWrapped(add, v1, v2, v3); }, wrapCoord(x0, offset.x),
                wrapCoord(y0, offset.y), wrapCoord(x1, offset.x), wrapCoord(y1, offset.y));
    }
}

template <PCSX::GPU::Size size, PCSX::GPU::Textured textured, PCSX::GPU::Blend blend, PCSX::GPU::Modulation modulation>
void PCSX::GPU::Rect<size, textured, blend, modulation>::getVertices(AddTri &&add, PixelOp op) {
    if (op == PixelOp::WRITE) {
        const int rx = wrapCoord(x, offset.x), ry = wrapCoord(y, offset.y);
        addWrapped(add, {rx, ry}, {rx + w, ry}, {rx + w, ry + h});
        addWrapped(add, {rx + w, ry + h}, {rx, ry + h}, {rx, ry});
    } else if (op == PixelOp::READ) {
        if constexpr (blend == Blend::Semi) {
            const int rx = wrapCoord(x, offset.x), ry = wrapCoord(y, offset.y);
            addWrapped(add, {rx, ry}, {rx + w, ry}, {rx + w, ry + h});
            addWrapped(add, {rx + w, ry + h}, {rx, ry + h}, {rx, ry});
        }
        if constexpr (textured == Textured::Yes) {
            unsigned tx = tpage.tx * 64;
            unsigned ty = tpage.ty * 256;
            unsigned shift = tpage.texDepth == TexDepth::Tex4Bits ? 2 : tpage.texDepth == TexDepth::Tex8Bits ? 1 : 0;
            unsigned minU = u >> shift;
            unsigned minV = v;
            unsigned maxU = (u + w) >> shift;
            unsigned maxV = v + h;
            add({int(minU + tx), int(minV + ty)}, {int(maxU + tx), int(minV + ty)}, {int(maxU + tx), int(maxV + ty)});
            add({int(maxU + tx), int(maxV + ty)}, {int(minU + tx), int(maxV + ty)}, {int(minU + tx), int(minV + ty)});
        }
    }
}

// Polygons fill a span from the left edge inclusive to the right edge exclusive, and rows from the top
// inclusive to the bottom exclusive. That is the same as testing the point x + e, y + e * e for a tiny e
// strictly inside the triangle: when the pixel sits exactly on an edge, the edge's slope decides.
bool PCSX::GPU::Logged::isInsideTriangle(int x, int y, int x1, int y1, int x2, int y2, int x3, int y3) {
    auto side = [x, y](int ax, int ay, int bx, int by) {
        int e = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
        if (e == 0) e = ay - by;
        if (e == 0) e = bx - ax;
        return e;
    };
    int s1 = side(x1, y1, x2, y2);
    int s2 = side(x2, y2, x3, y3);
    int s3 = side(x3, y3, x1, y1);
    return (s1 > 0 && s2 > 0 && s3 > 0) || (s1 < 0 && s2 < 0 && s3 < 0);
}

// Walks the same pixels as the soft renderer's line rasterizer (gpu/soft/lines.cc), including the
// size cull that drops a segment with |dx| >= 1024 or |dy| >= 512.
bool PCSX::GPU::Logged::isInsideLine(int x, int y, int x1, int y1, int x2, int y2) {
    if (std::abs(x2 - x1) >= 1024 || std::abs(y2 - y1) >= 512) return false;
    if (x2 < x1) {
        std::swap(x1, x2);
        std::swap(y1, y2);
    }
    if ((x < x1) || (x > x2) || (y < std::min(y1, y2)) || (y > std::max(y1, y2))) return false;
    const int dx = x2 - x1;
    const int dy = y2 - y1;
    if ((dx == 0) || (dy == 0)) return true;
    const int ady = std::abs(dy);
    const int step = dy > 0 ? 1 : -1;
    int px = x1, py = y1;
    if (ady > dx) {
        // Y-major: midpoint bias, x nudges right.
        int d = 2 * dx - ady;
        while (true) {
            if ((px == x) && (py == y)) return true;
            if (py == y2) return false;
            if (d <= 0) {
                d += 2 * dx;
            } else {
                d += 2 * (dx - ady);
                px++;
            }
            py += step;
        }
    }
    // X-major: pixel-centre bias, y nudges toward y2.
    int d = 3 * ady - dx;
    while (true) {
        if ((px == x) && (py == y)) return true;
        if (px == x2) return false;
        if (d <= 0) {
            d += 2 * ady;
        } else {
            d += 2 * (ady - dx);
            py += step;
        }
        px++;
    }
}

void PCSX::GPU::getRestoreSequence(std::vector<uint32_t> &gp0, std::vector<uint32_t> &gp1) const {
    gp1.clear();
    gp1.push_back(0x00000000);
    for (unsigned cmd : {3, 8, 6, 7, 5, 4}) gp1.push_back(m_statusControl[cmd]);
    gp0.clear();
    for (unsigned cmd = 1; cmd <= 6; cmd++) gp0.push_back(m_envRaw[cmd]);
}
