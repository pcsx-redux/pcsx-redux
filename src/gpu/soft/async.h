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

#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace PCSX {

namespace SoftGPU {

class impl;

// A self-contained, replayable soft-GPU operation.
//
// The core command processor decodes each GP0 primitive into a singleton node
// and reuses it for the next primitive of the same type, so the renderer
// thread cannot be handed a pointer into it. An op owns a copy of everything
// it needs (including polyline vectors and upload pixels) and replays it
// against the renderer later, on the renderer thread.
struct SoftOp {
    virtual ~SoftOp() = default;
    virtual void run(impl &r) = 0;
};

// One op type for every GP0 primitive the renderer consumes. run() calls the
// impl::exec overload matching Prim, so the compile-time dispatch over the
// templated primitive types is preserved: no runtime tag, no table.
template <typename Prim>
struct PrimOp final : SoftOp {
    Prim prim;
    explicit PrimOp(const Prim &p) : prim(p) {}
    void run(impl &r) override;  // defined in gpu.cc where impl is complete
};

// A CPU-side VRAM write (DMA upload, savestate restore, debugger poke).
struct VramWriteOp final : SoftOp {
    int x, y, w, h;
    std::vector<uint16_t> pixels;
    bool masked;
    VramWriteOp(int x_, int y_, int w_, int h_, const uint16_t *src, bool masked_ = false)
        : x(x_), y(y_), w(w_), h(h_), pixels(src, src + w_ * h_), masked(masked_) {}
    void run(impl &r) override;
};

// The row parity drawing primitives skip (SoftRenderer::m_skipParity). It is a
// function of the beam position, so the emulation thread samples it and hands
// it to the renderer in primitive order.
struct SkipParityOp final : SoftOp {
    int8_t parity;
    explicit SkipParityOp(int8_t p) : parity(p) {}
    void run(impl &r) override;
};

// Single producer (the emulation thread), single consumer (the renderer
// thread). The renderer is the only thread touching VRAM and render state
// while it runs; the producer calls sync() before anything that reads them.
class OpQueue {
  public:
    ~OpQueue() { stop(); }
    bool running() const { return m_thread.joinable(); }
    void start(impl &r) {
        if (running()) return;
        m_quit = false;
        m_thread = std::thread([this, &r]() { loop(r); });
    }
    void stop() {
        if (!running()) return;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_quit = true;
        }
        m_workCV.notify_one();
        m_thread.join();
    }
    void push(std::unique_ptr<SoftOp> &&op) {
        bool wake;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            wake = m_pending.empty();
            m_pending.push_back(std::move(op));
            m_pushed++;
        }
        if (wake) m_workCV.notify_one();
    }
    // Blocks until every op pushed so far has run.
    void sync() {
        if (!running()) return;
        std::unique_lock<std::mutex> l(m_mutex);
        m_doneCV.wait(l, [this]() { return m_done == m_pushed; });
    }

  private:
    void loop(impl &r) {
        std::vector<std::unique_ptr<SoftOp>> batch;
        while (true) {
            {
                std::unique_lock<std::mutex> l(m_mutex);
                m_workCV.wait(l, [this]() { return m_quit || !m_pending.empty(); });
                if (m_pending.empty()) return;
                batch.swap(m_pending);
            }
            for (auto &op : batch) op->run(r);
            auto count = batch.size();
            batch.clear();
            {
                std::lock_guard<std::mutex> l(m_mutex);
                m_done += count;
            }
            m_doneCV.notify_all();
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_workCV;
    std::condition_variable m_doneCV;
    std::vector<std::unique_ptr<SoftOp>> m_pending;
    uint64_t m_pushed = 0;
    uint64_t m_done = 0;
    bool m_quit = false;
};

}  // namespace SoftGPU

}  // namespace PCSX
