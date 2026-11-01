/***************************************************************************
 *   Copyright (C) 2020 PCSX-Redux authors                                 *
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

#include <assert.h>
#include <uv.h>

#include <cstdarg>
#include <memory>
#include <optional>
#include <string>

#include "core/debug.h"
#include "core/psxemulator.h"
#include "support/eventbus.h"
#include "support/hashtable.h"
#include "support/list.h"
#include "support/network.h"
#include "support/slice.h"

namespace PCSX {

class GdbClient : public Intrusive::List<GdbClient>::Node {
  public:
    GdbClient(IO<File> connection, uv_loop_t* loop);
    ~GdbClient();
    typedef Intrusive::List<GdbClient> ListType;

    void close();

  private:
    void write(const Slice& slice) {
        Slice payload;
        payload.copy(slice.data(), slice.size());
        sendPacket(std::move(payload));
    }
    void write(const std::string& msg) {
        assert(msg.size() <= std::numeric_limits<uint32_t>::max());
        Slice payload;
        payload.copy(msg);
        sendPacket(std::move(payload));
    }
    void write(std::string&& msg) {
        assert(msg.size() <= std::numeric_limits<uint32_t>::max());
        Slice payload;
        payload.acquire(std::move(msg));
        sendPacket(std::move(payload));
    }
    template <size_t L>
    void write(const char (&str)[L]) {
        static_assert((L - 1) <= std::numeric_limits<uint32_t>::max());
        Slice payload;
        payload.borrow(str, L - 1);
        sendPacket(std::move(payload));
    }
    void writef(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        size_t len;
        char* msg;
#ifdef _WIN32
        len = _vscprintf(fmt, a);
        msg = (char*)malloc(len + 1);
        vsnprintf(msg, len + 1, fmt, a);
#else
        len = vasprintf(&msg, fmt, a);
#endif
        Slice payload;
        payload.acquire(msg, len);
        sendPacket(std::move(payload));
        va_end(a);
    }
    void writePaged(const std::string& out, const std::string& cursorStr);
    void writeEscaped(const std::string& out);
    void sendAck() {
        Slice raw;
        raw.copy("+", 1);
        sendRaw(std::move(raw));
    }

    void startStream() {
        m_crc = 0;
        Slice raw;
        raw.copy("$", 1);
        sendRaw(std::move(raw));
    }

    void stream(const std::string& data) {
        for (int i = 0; i < data.length(); i++) {
            m_crc += data[i];
        }
        Slice raw;
        raw.copy(data.data(), data.size());
        sendRaw(std::move(raw));
    }

    void stopStream() {
        char end[3] = {'#'};
        end[1] = toHex[m_crc >> 4];
        end[2] = toHex[m_crc & 0x0f];
        Slice raw;
        raw.copy(end, 3);
        sendRaw(std::move(raw));
    }

    static const char toHex[];

    // Framing and transport. Previously this was a WriteRequest intrusive hash
    // table doing 3-buffer scatter uv_writes, duplicated verbatim in the web
    // server. A File has no scatter/gather, so a packet is framed into one
    // buffer and handed over as a single Slice - GDB packets are small, and it
    // is one allocation per packet against a hash table insert plus erase.
    void sendPacket(Slice&& payload);  // wraps in $...#XX
    void sendRaw(Slice&& raw);         // as-is, for acks and streamed chunks
    void logOutgoing(const Slice& slice);

    static constexpr size_t BUFFER_SIZE = 256;
    // Woken by the fifo's notifier; drains whatever arrived into processData.
    void onReadable();

    // The async lives here rather than in the client so that the close callback
    // can find its way back to the client after uv is done with the handle -
    // UvFifo::setNotifier owns the handle's data pointer.
    struct AsyncContext {
        uv_async_t m_async;
        GdbClient* m_client;
    };
    AsyncContext* m_asyncContext = nullptr;

    void processData(const Slice& slice);
    void processCommand();
    void processMonitorCommand(const std::string&);
    Slice passthroughData(Slice slice);
    std::pair<uint64_t, uint64_t> parseCursor(const std::string& cursorStr);

    // Reverse execution, bs and bc. Both replay from the emulator's rewind ring: restore the
    // newest snapshot before the current boundary, replay up to it while recording, then
    // restore again and replay to the boundary to stop at. bs stops at the boundary before
    // this one. bc stops at the last breakpoint hit before it, and goes one snapshot further
    // back each time a segment has none, down to the oldest snapshot, the edge of the replay
    // log. The replays run as ordinary emulation, and every one of them ends in a Pause.
    struct ReverseJob {
        bool isContinue;
        // false while scanning a segment for the boundary to stop at, true while replaying to it
        bool landing = false;
        bool scanned = false;
        bool atEdge = false;
        uint64_t now;
        size_t snapshot;
        uint64_t target = 0;
    };
    void startReverse(bool isContinue);
    void replayFrom(size_t snapshot, uint64_t stopAt);
    void land(uint64_t target);
    void onReplayPaused();
    void finishReverse(const char* reply);
    std::optional<ReverseJob> m_reverse;
    // A restore queued for the main loop holds a weak reference to this, so it does nothing if
    // the client went away in between.
    std::shared_ptr<GdbClient*> m_self = std::make_shared<GdbClient*>(this);

    std::string dumpOneRegister(int n);
    void setOneRegister(int n, uint32_t value);
    static std::string dumpValue(uint32_t value);

    IO<File> m_connection;
    enum { OPEN, CLOSING } m_status = OPEN;

    enum {
        WAIT_FOR_ACK,
        WAIT_FOR_DOLLAR,
        READING_COMMAND,
        ESCAPE,
        READING_CRC_FIRST_CHAR,
        READING_CRC_SECOND_CHAR,
    } m_state = WAIT_FOR_DOLLAR;
    bool m_passthrough = false;
    bool m_ackEnabled = true;
    bool m_multiprocess = false;
    bool m_waitingForTrap = false;
    bool m_waitingForShell = false;
    bool m_restartRewindOnResume = false;
    bool m_rewindWasInUse = false;
    bool m_exception = false;
    // Not sure about the logic here; we need to keep an eye on when
    // gdb complains about invalid responses, and toggle this accordingly.
    bool m_canReceiveLogs = false;
    std::string m_cmd;
    uint8_t m_crc;
    EventBus::Listener m_listener;
    uv_loop_t* m_loop;
    Debug::BreakpointUserListType m_breakpoints;
};

class GdbServer : public Network::Server {
  public:
    GdbServer();

  protected:
    void onConnection(IO<File> connection) override;
    void onStopped() override;
    void onFailed() override;
    int configuredPort() const override;

  private:
    GdbClient::ListType m_clients;
    EventBus::Listener m_listener;
};

}  // namespace PCSX
