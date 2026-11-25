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

#include <uv.h>

#include <chrono>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "loop.h"
#include "support/uvfile.h"

using namespace PCSX;

namespace {

// Ports picked high and odd to reduce the odds of colliding with anything real
// on a developer box or CI runner.
constexpr unsigned c_squattedPort = 47821;
constexpr unsigned c_freePort = 47823;
constexpr unsigned c_notifyPort = 47825;
constexpr unsigned c_deadPort = 47827;
constexpr unsigned c_flushPort = 47829;
constexpr unsigned c_dropPort = 47831;

// Pump the caller-side loop for a while, giving the uv worker thread time to
// service the queued request() and hand anything back through the async.
void pump(uv_loop_t* loop, int milliseconds = 250) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        uv_run(loop, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Takes a port and holds it, so UvFifoListener's bind is guaranteed to fail.
struct Squatter {
    explicit Squatter(uv_loop_t* loop, unsigned port) {
        uv_tcp_init(loop, &m_tcp);
        struct sockaddr_in addr;
        EXPECT_EQ(uv_ip4_addr("0.0.0.0", port, &addr), 0);
        EXPECT_EQ(uv_tcp_bind(&m_tcp, reinterpret_cast<const sockaddr*>(&addr), 0), 0);
        EXPECT_EQ(uv_listen(reinterpret_cast<uv_stream_t*>(&m_tcp), 16, [](uv_stream_t*, int) {}), 0);
    }
    void release(uv_loop_t* loop) {
        if (m_released) return;
        m_released = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&m_tcp), [](uv_handle_t*) {});
    }
    ~Squatter() {
        if (!m_released) uv_close(reinterpret_cast<uv_handle_t*>(&m_tcp), [](uv_handle_t*) {});
    }
    uv_tcp_t m_tcp = {};
    bool m_released = false;
};

}  // namespace

// Control: a listener that binds cleanly must start and stop without incident.
// If this one ever fails, the busy-port test below proves nothing.
TEST(UvFifoListener, StartStopOnFreePort) {
    UvThreadOp::UvThread uvThread;
    Loop loop;

    int nullptrCallbacks = 0;
    UvFifoListener listener;
    uv_async_t async = {};
    listener.start(c_freePort, &loop, &async, [&nullptrCallbacks](UvFifo* fifo) {
        if (!fifo) nullptrCallbacks++;
    });
    pump(&loop);

    EXPECT_EQ(listener.status(), UvFifoListener::Status::Listening);
    EXPECT_TRUE(listener.isListening());
    EXPECT_EQ(listener.lastErrorCode(), 0);
    EXPECT_EQ(nullptrCallbacks, 0);

    listener.stop();
    pump(&loop);

    EXPECT_EQ(listener.status(), UvFifoListener::Status::Stopped);
    EXPECT_EQ(nullptrCallbacks, 1);

    uv_close(reinterpret_cast<uv_handle_t*>(&async), [](uv_handle_t*) {});
    pump(&loop);
}

// The bug: start() on an occupied port hits the uv_tcp_bind failure path, which
// uv_close()es m_server and tells nobody. Nothing in UvFifoListener records that
// it is already closing, so a subsequent stop() - which is exactly what the GUI
// checkbox and the Quitting event both do, since SIO1Server/ATConsServer set
// SERVER_STARTED unconditionally - closes the same handle a second time.
TEST(UvFifoListener, StopAfterFailedBindIsSafe) {
    UvThreadOp::UvThread uvThread;
    Loop loop;

    Squatter squatter(&loop, c_squattedPort);

    int nullptrCallbacks = 0;
    UvFifoListener listener;
    uv_async_t async = {};
    listener.start(c_squattedPort, &loop, &async, [&nullptrCallbacks](UvFifo* fifo) {
        if (!fifo) nullptrCallbacks++;
    });
    pump(&loop);

    // The failure has to be visible, and it has to be the *right* failure -
    // an unreported bind error and a bind error reported as the wrong code are
    // both things a status bullet would render identically.
    EXPECT_EQ(listener.status(), UvFifoListener::Status::Failed);
    EXPECT_FALSE(listener.isListening());
    EXPECT_EQ(listener.lastErrorCode(), UV_EADDRINUSE);
    EXPECT_STREQ(listener.lastError(), uv_strerror(UV_EADDRINUSE));
    // Consumers learn about it through the same nullptr they get on shutdown,
    // so nobody has to grow a second teardown path.
    EXPECT_EQ(nullptrCallbacks, 1);

    // This is the line that used to abort inside uv_close.
    listener.stop();
    pump(&loop);

    EXPECT_EQ(listener.status(), UvFifoListener::Status::Failed);

    uv_close(reinterpret_cast<uv_handle_t*>(&async), [](uv_handle_t*) {});
    squatter.release(&loop);
    pump(&loop);
}

// UvFifo has no readable callback of its own - data lands in a lock-free queue
// on the worker thread and the consumer is expected to poll size(). That is
// what SIO1 and ATCons do off the counters, and it is why porting the GDB and
// web servers onto this transport needs a wake-up first: frame-granularity
// polling would put visible latency on every GDB packet.
//
// This test only passes if uv_async_send actually reached the caller's loop.
// Nothing else runs the callback, so a notifier that never fires fails it.
TEST(UvFifo, ReadableNotifierFires) {
    UvThreadOp::UvThread uvThread;
    Loop loop;

    UvFifo* accepted = nullptr;
    UvFifoListener listener;
    uv_async_t listenerAsync = {};
    listener.start(c_notifyPort, &loop, &listenerAsync, [&accepted](UvFifo* fifo) {
        if (fifo) accepted = fifo;
    });
    pump(&loop);
    ASSERT_EQ(listener.status(), UvFifoListener::Status::Listening);

    IO<File> client(new UvFifo("127.0.0.1", c_notifyPort));
    pump(&loop);
    ASSERT_NE(accepted, nullptr);
    IO<File> serverSide(accepted);

    int notifications = 0;
    uv_async_t notifyAsync = {};
    accepted->setNotifier(&loop, &notifyAsync, [&notifications]() { notifications++; });
    pump(&loop);
    const int baseline = notifications;

    static const char c_payload[] = "hello";
    constexpr size_t c_payloadSize = sizeof(c_payload) - 1;
    client->write(c_payload, c_payloadSize);
    pump(&loop, 500);

    EXPECT_GT(notifications, baseline);
    EXPECT_EQ(serverSide->size(), c_payloadSize);

    char buffer[16] = {};
    EXPECT_EQ(serverSide->read(buffer, c_payloadSize), static_cast<ssize_t>(c_payloadSize));
    EXPECT_STREQ(buffer, c_payload);

    // A peer hanging up has to wake the consumer too, or anything waiting on
    // the notifier instead of polling never learns the connection is gone.
    const int beforeClose = notifications;
    client.reset();
    pump(&loop, 500);
    EXPECT_GT(notifications, beforeClose);
    EXPECT_TRUE(serverSide->isClosed());

    accepted->clearNotifier();
    serverSide.reset();
    listener.stop();
    pump(&loop);
    uv_close(reinterpret_cast<uv_handle_t*>(&notifyAsync), [](uv_handle_t*) {});
    uv_close(reinterpret_cast<uv_handle_t*>(&listenerAsync), [](uv_handle_t*) {});
    pump(&loop);
}

// A failed outgoing connection has to end up in a state the UI can act on.
// Both halves of this matter and only one of them is about the error string:
// m_connecting was set in the constructor and cleared ONLY on the success
// path, so a fifo that failed to connect reported isConnecting() forever. The
// SIO1 "Reconnect" button - the single piece of network error UI in the whole
// emulator - is gated on `!connecting() && fifoError()`, so it could never
// become clickable after exactly the failure it exists to recover from.
TEST(UvFifo, FailedConnectIsActionable) {
    UvThreadOp::UvThread uvThread;

    // Nothing is listening here; c_deadPort is never bound by any test.
    IO<File> client(new UvFifo("127.0.0.1", c_deadPort));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto fifo = client.asA<UvFifo>();
    while (fifo->isConnecting() && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(fifo->failed());
    EXPECT_FALSE(fifo->isConnecting());
    EXPECT_EQ(fifo->connectErrorCode(), UV_ECONNREFUSED);
    EXPECT_STREQ(fifo->connectError(), uv_strerror(UV_ECONNREFUSED));
}

// Writes are queued to the worker thread, so "write() returned" and "the bytes
// left the machine" are different moments. uv_close cancels pending uv_writes,
// so closing between the two truncates the tail - for an HTTP response that is
// a silently short reply, and it is why the web server carried its own
// m_closeScheduled / m_requests.size() bookkeeping. close() is graceful now so
// consumers do not each have to reinvent that.
//
// The peer here deliberately does NOT read while the write is in flight. An
// earlier version of this test used two UvFifos, which share the worker thread,
// so the receiver drained as fast as the sender wrote and nothing was ever
// pending at close time - that version passed with the graceful close removed,
// i.e. it measured nothing. A silent peer fills the socket buffers and forces
// the writes to stay queued, which is the only state in which this is a test.
TEST(UvFifo, CloseFlushesPendingWrites) {
    UvThreadOp::UvThread uvThread;
    Loop loop;

    UvFifo* accepted = nullptr;
    UvFifoListener listener;
    uv_async_t listenerAsync = {};
    listener.start(c_flushPort, &loop, &listenerAsync, [&accepted](UvFifo* fifo) {
        if (fifo) accepted = fifo;
    });
    pump(&loop);
    ASSERT_EQ(listener.status(), UvFifoListener::Status::Listening);

    // A raw peer on the test loop, with no uv_read_start: it accepts the
    // connection and then stays silent.
    struct Peer {
        uv_tcp_t m_tcp = {};
        size_t m_received = 0;
        bool m_eof = false;
        bool m_reading = false;
    } peer;
    uv_tcp_init(&loop, &peer.m_tcp);
    peer.m_tcp.data = &peer;
    struct sockaddr_in target;
    ASSERT_EQ(uv_ip4_addr("127.0.0.1", c_flushPort, &target), 0);
    uv_connect_t connectReq;
    ASSERT_EQ(uv_tcp_connect(&connectReq, &peer.m_tcp, reinterpret_cast<const sockaddr*>(&target),
                             [](uv_connect_t*, int status) { EXPECT_EQ(status, 0); }),
              0);
    pump(&loop);
    ASSERT_NE(accepted, nullptr);
    IO<File> serverSide(accepted);

    // Big enough that it cannot possibly fit in the socket buffers, so the tail
    // is genuinely still queued when close() lands.
    constexpr size_t c_payloadSize = 64 * 1024 * 1024;
    std::string payload(c_payloadSize, 'x');
    serverSide->write(payload.data(), payload.size());
    ASSERT_GT(accepted->pendingWrites(), 0u);
    // No drain wait: close straight away, the way a handler that has finished
    // writing its response does.
    serverSide.reset();

    // Only now start reading.
    peer.m_reading = true;
    uv_read_start(
        reinterpret_cast<uv_stream_t*>(&peer.m_tcp),
        [](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
            buf->base = static_cast<char*>(malloc(suggested));
            buf->len = suggested;
        },
        [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
            auto self = static_cast<Peer*>(stream->data);
            if (nread > 0) {
                self->m_received += nread;
            } else if (nread < 0) {
                self->m_eof = true;
            }
            free(buf->base);
        });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!peer.m_eof && (std::chrono::steady_clock::now() < deadline)) {
        pump(&loop, 50);
    }

    EXPECT_TRUE(peer.m_eof);
    EXPECT_EQ(peer.m_received, c_payloadSize);

    uv_close(reinterpret_cast<uv_handle_t*>(&peer.m_tcp), [](uv_handle_t*) {});
    listener.stop();
    pump(&loop);
    uv_close(reinterpret_cast<uv_handle_t*>(&listenerAsync), [](uv_handle_t*) {});
    pump(&loop);
}

// The fifo is deleted as soon as its last reference goes, but the socket stays
// open until the queued writes drain, and the peer is free to keep talking in
// the meantime. Its bytes must not be read into the deleted fifo. Under ASan
// this is a heap-use-after-free without the fix; without ASan it may pass.
TEST(UvFifo, PeerTrafficAfterDropIsSafe) {
    UvThreadOp::UvThread uvThread;
    Loop loop;

    UvFifo* accepted = nullptr;
    UvFifoListener listener;
    uv_async_t listenerAsync = {};
    listener.start(c_dropPort, &loop, &listenerAsync, [&accepted](UvFifo* fifo) {
        if (fifo) accepted = fifo;
    });
    pump(&loop);
    ASSERT_EQ(listener.status(), UvFifoListener::Status::Listening);

    struct Peer {
        uv_tcp_t m_tcp = {};
        size_t m_received = 0;
        bool m_eof = false;
        ssize_t m_endCode = 0;
    } peer;
    uv_tcp_init(&loop, &peer.m_tcp);
    peer.m_tcp.data = &peer;
    struct sockaddr_in target;
    ASSERT_EQ(uv_ip4_addr("127.0.0.1", c_dropPort, &target), 0);
    uv_connect_t connectReq;
    ASSERT_EQ(uv_tcp_connect(&connectReq, &peer.m_tcp, reinterpret_cast<const sockaddr*>(&target),
                             [](uv_connect_t*, int status) { EXPECT_EQ(status, 0); }),
              0);
    pump(&loop);
    ASSERT_NE(accepted, nullptr);
    IO<File> serverSide(accepted);

    // Keep the socket open past the fifo's lifetime, as in the flush test.
    constexpr size_t c_payloadSize = 16 * 1024 * 1024;
    std::string payload(c_payloadSize, 'x');
    serverSide->write(payload.data(), payload.size());
    ASSERT_GT(accepted->pendingWrites(), 0u);
    serverSide.reset();

    // Now talk to the socket whose fifo is gone.
    static const char c_chatter[] = "still here";
    uv_buf_t chatter = uv_buf_init(const_cast<char*>(c_chatter), sizeof(c_chatter) - 1);
    uv_write_t writeReq;
    ASSERT_EQ(uv_write(&writeReq, reinterpret_cast<uv_stream_t*>(&peer.m_tcp), &chatter, 1,
                       [](uv_write_t*, int status) { EXPECT_EQ(status, 0); }),
              0);
    uv_read_start(
        reinterpret_cast<uv_stream_t*>(&peer.m_tcp),
        [](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
            buf->base = static_cast<char*>(malloc(suggested));
            buf->len = suggested;
        },
        [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
            auto self = static_cast<Peer*>(stream->data);
            if (nread > 0) {
                self->m_received += nread;
            } else if (nread < 0) {
                self->m_eof = true;
                self->m_endCode = nread;
            }
            free(buf->base);
        });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!peer.m_eof && (std::chrono::steady_clock::now() < deadline)) {
        pump(&loop, 50);
    }

    EXPECT_TRUE(peer.m_eof);
    // A reset rather than an orderly EOF means the socket was closed with
    // unread data on it, which also throws away what the peer had not read yet.
    EXPECT_STREQ(uv_err_name(static_cast<int>(peer.m_endCode)), "EOF");
    EXPECT_EQ(peer.m_received, c_payloadSize);

    uv_close(reinterpret_cast<uv_handle_t*>(&peer.m_tcp), [](uv_handle_t*) {});
    listener.stop();
    pump(&loop);
    uv_close(reinterpret_cast<uv_handle_t*>(&listenerAsync), [](uv_handle_t*) {});
    pump(&loop);
}
