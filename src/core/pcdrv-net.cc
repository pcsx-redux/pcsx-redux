/***************************************************************************
 *   Copyright (C) 2023 PCSX-Redux authors                                 *
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

// PCdrv network names (tcp://, probe:net). Desktop only: a browser has no
// sockets to hand a UvFifo, so pcdrv-net-wasm.cc refuses them instead.

#ifndef __EMSCRIPTEN__

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "support/uvfile.h"

namespace {

// How long PCopen("tcp://...") waits for the connection before giving up.
constexpr auto c_pcdrvConnectTimeout = std::chrono::seconds(5);

// Lets the PCdrv code queue work on the UV thread, so that a socket is only ever
// freed there, after its uv_close: a read callback can't then land on a dead object.
struct PCdrvUvAccess : public PCSX::UvFifo {
    using PCSX::UvThreadOp::request;
};

bool startsWith(std::string_view str, std::string_view prefix) { return str.substr(0, prefix.size()) == prefix; }

std::string_view trim(std::string_view str) {
    while (!str.empty() && isspace(static_cast<unsigned char>(str.front()))) str.remove_prefix(1);
    while (!str.empty() && isspace(static_cast<unsigned char>(str.back()))) str.remove_suffix(1);
    return str;
}

bool pcdrvNetAllowed(std::string_view allowList, std::string_view hostPort) {
    if (allowList.empty()) return true;
    while (!allowList.empty()) {
        auto comma = allowList.find(',');
        if (trim(allowList.substr(0, comma)) == hostPort) return true;
        if (comma == std::string_view::npos) break;
        allowList.remove_prefix(comma + 1);
    }
    return false;
}

bool isDigit(char c) { return (c >= '0') && (c <= '9'); }

// Splits "host:port", with port a decimal number in 1-65535.
bool parseHostPort(std::string_view hostPort, std::string& host, unsigned& port) {
    auto colon = hostPort.rfind(':');
    if ((colon == std::string_view::npos) || (colon == 0)) return false;
    auto portStr = hostPort.substr(colon + 1);
    if (portStr.empty() || (portStr.size() > 5)) return false;
    if (!std::all_of(portStr.begin(), portStr.end(), isDigit)) return false;
    port = 0;
    for (auto c : portStr) port = port * 10 + (c - '0');
    if ((port == 0) || (port > 65535)) return false;
    host = hostPort.substr(0, colon);
    return true;
}

// A name resolution running on the UV thread. Shared between the emulator thread and the
// uv_getaddrinfo callback, so a callback landing after the deadline touches nothing freed.
struct PCdrvResolve {
    enum { Pending, Done, Failed };
    std::atomic<int> state = Pending;
    char address[64] = {};
    // Only ever touched on the UV thread.
    uv_getaddrinfo_t req = {};
    bool active = false;
};

void pcdrvResolved(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
    auto holder = reinterpret_cast<std::shared_ptr<PCdrvResolve>*>(req->data);
    auto resolve = *holder;
    delete holder;
    resolve->active = false;
    bool ok = false;
    if ((status == 0) && res) {
        ok = uv_ip4_name(reinterpret_cast<const struct sockaddr_in*>(res->ai_addr), resolve->address,
                         sizeof(resolve->address)) == 0;
    }
    if (res) uv_freeaddrinfo(res);
    resolve->state.store(ok ? PCdrvResolve::Done : PCdrvResolve::Failed, std::memory_order_release);
}

// Runs on the UV thread.
void pcdrvStartResolve(uv_loop_t* loop, const std::shared_ptr<PCdrvResolve>& resolve, const std::string& host) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    // The request holds its own reference until its callback runs, cancelled or not.
    resolve->req.data = new std::shared_ptr<PCdrvResolve>(resolve);
    int r = uv_getaddrinfo(loop, &resolve->req, pcdrvResolved, host.c_str(), nullptr, &hints);
    if (r != 0) {
        delete reinterpret_cast<std::shared_ptr<PCdrvResolve>*>(resolve->req.data);
        resolve->state.store(PCdrvResolve::Failed, std::memory_order_release);
        return;
    }
    resolve->active = true;
}

// Resolves host to an IPv4 literal, since UvFifo only takes those. Empty on failure or past the deadline.
std::string pcdrvResolve(const std::string& host, std::chrono::steady_clock::time_point deadline) {
    auto resolve = std::make_shared<PCdrvResolve>();
    PCdrvUvAccess::request([resolve, host](uv_loop_t* loop) { pcdrvStartResolve(loop, resolve, host); });
    while ((resolve->state.load(std::memory_order_acquire) == PCdrvResolve::Pending) &&
           (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto state = resolve->state.load(std::memory_order_acquire);
    if (state == PCdrvResolve::Pending) {
        // Too slow. Cancelling may come too late; the shared state covers that.
        PCdrvUvAccess::request([resolve](uv_loop_t*) {
            if (resolve->active) uv_cancel(reinterpret_cast<uv_req_t*>(&resolve->req));
        });
        return {};
    }
    if (state != PCdrvResolve::Done) return {};
    return resolve->address;
}

PCSX::IO<PCSX::File> pcdrvConnectTo(const std::string& address, unsigned port,
                                    std::chrono::steady_clock::time_point deadline) {
    PCSX::IO<PCSX::UvFifo> fifo(new PCSX::UvFifo(address, port));
    auto connecting = [&fifo]() { return fifo->isConnecting() && !fifo->failed(); };
    while (connecting() && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (fifo->isConnecting() || fifo->failed()) {
        fifo->close();
        PCdrvUvAccess::request([fifo](uv_loop_t*) {});
        return {};
    }
    return fifo;
}

// Connects "host:port" and returns the stream, or an empty handle on any failure or after
// c_pcdrvConnectTimeout, which covers both the name resolution and the connection. Both
// happen on the UV thread; this thread only polls their state.
PCSX::IO<PCSX::File> pcdrvConnect(std::string_view hostPort) {
    auto deadline = std::chrono::steady_clock::now() + c_pcdrvConnectTimeout;
    std::string host;
    unsigned port;
    if (!parseHostPort(hostPort, host, port)) return {};
    auto address = pcdrvResolve(host, deadline);
    if (address.empty()) return {};
    return pcdrvConnectTo(address, port, deadline);
}

// The stream behind a network name: empty when networking is off, the name is unknown, or
// the host is not allowed. Sets probe for probe:net, which is an empty stream.
PCSX::IO<PCSX::File> pcdrvNetStream(std::string_view filename, bool& probe) {
    auto& debugSettings = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>();
    probe = false;
    if (!debugSettings.get<PCSX::Emulator::DebugSettings::PCdrvNet>()) return {};
    if (filename == "probe:net") {
        static uint8_t empty;
        probe = true;
        return PCSX::IO<PCSX::File>(new PCSX::BufferFile(&empty, 0));
    }
    if (!startsWith(filename, "tcp://")) return {};
    filename.remove_prefix(6);
    if (!pcdrvNetAllowed(debugSettings.get<PCSX::Emulator::DebugSettings::PCdrvNetAllow>().value, filename)) return {};
    return pcdrvConnect(filename);
}

}  // namespace

PCSX::R3000Acpu::PCdrvFile::~PCdrvFile() {
    if (m_kind != Kind::Socket) return;
    // Close now, but drop the last reference on the UV thread, once the uv_close is queued there.
    IO<File> keep(*static_cast<IO<File>*>(this));
    keep->close();
    PCdrvUvAccess::request([keep](uv_loop_t*) {});
}

void PCSX::R3000Acpu::pcdrvOpenNet(const std::string& filename) {
    auto& regs = m_regs.GPR.n;
    bool probe;
    IO<File> stream = pcdrvNetStream(filename, probe);
    if (!stream) {
        regs.v0 = -1;
        regs.v1 = -1;
        return;
    }
    uint16_t fd = m_availableFDs.front();
    m_availableFDs.pop_front();
    auto kind = probe ? PCdrvFile::Kind::Probe : PCdrvFile::Kind::Socket;
    auto file = m_pcdrvFiles.insert(fd, new PCdrvFile(std::move(stream), kind));
    file->m_relativeFilename = filename;
    regs.v0 = 0;
    regs.v1 = fd;
}

// Never blocks: whatever has arrived, up to a2 bytes; 0 if nothing yet. The probe reads nothing.
void PCSX::R3000Acpu::pcdrvReadNet(PCdrvFile& filei, IO<File>& memFile) {
    auto& regs = m_regs.GPR.n;
    regs.v0 = 0;
    regs.v1 = 0;
    if (filei.m_kind == PCdrvFile::Kind::Probe) return;
    IO<File> file = filei;
    if (file->failed() || file->eof()) {
        regs.v0 = -1;
        regs.v1 = -1;
        return;
    }
    size_t available = std::min(static_cast<size_t>(regs.a2), file->size());
    if (!available) return;
    auto slice = file->read(available);
    regs.v1 = slice.size();
    if (slice.size()) memFile->writeAt(std::move(slice), regs.a3);
}

#endif
