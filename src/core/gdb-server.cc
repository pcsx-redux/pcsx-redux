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

// v1 wasm has no libuv, so no gdb server.
#ifndef __EMSCRIPTEN__

#include "core/gdb-server.h"

#include <assert.h>
#include <uv.h>

#include <magic_enum/magic_enum_all.hpp>

#include "core/cdrom.h"
#include "core/debug.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "fmt/format.h"
#include "support/strings-helpers.h"

const char PCSX::GdbClient::toHex[] = "0123456789ABCDEF";

PCSX::GdbServer::GdbServer() : Network::Server("GDB Server"), m_listener(g_system->m_eventBus) {
    m_listener.listen<Events::SettingsLoaded>([this](const auto& event) {
        auto& settings = g_emulator->settings.get<Emulator::SettingDebugSettings>();
        if (settings.get<Emulator::DebugSettings::GdbServer>() && !isRunning()) {
            start(g_system->getLoop(), settings.get<Emulator::DebugSettings::GdbServerPort>());
        }
    });
    m_listener.listen<Events::Quitting>([this](const auto& event) {
        if (isRunning()) stop();
    });
}

void PCSX::GdbServer::onConnection(IO<File> connection) {
    m_clients.push_back(new GdbClient(connection, g_system->getLoop()));
}

void PCSX::GdbServer::onStopped() {
    // Covers an orderly stop and a failed bind alike. Iterate defensively:
    // closing a client eventually deletes it, which unlinks it from this list.
    while (!m_clients.empty()) {
        auto client = m_clients.begin();
        client->close();
        if (!m_clients.empty() && (m_clients.begin() == client)) m_clients.erase(client);
    }
}

void PCSX::GdbServer::onFailed() {
    g_system->printf(_("GDB server: unable to listen on port %i: %s\n"), port(), lastError());
}

int PCSX::GdbServer::configuredPort() const {
    return g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::GdbServerPort>();
}

void PCSX::GdbClient::logOutgoing(const Slice& slice) {
    if (!g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::GdbServerTrace>()) {
        return;
    }
    std::string msg(static_cast<const char*>(slice.data()), slice.size());
    g_system->log(LogClass::GDB, "GDB <-- PCSX %s\n", msg.c_str());
}

void PCSX::GdbClient::sendRaw(Slice&& raw) {
    if (!m_connection || m_connection->isClosed()) return;
    logOutgoing(raw);
    m_connection->write(std::move(raw));
}

void PCSX::GdbClient::sendPacket(Slice&& payload) {
    if (!m_connection || m_connection->isClosed()) return;
    logOutgoing(payload);
    uint8_t chksum = 0;
    auto data = static_cast<const uint8_t*>(payload.data());
    for (size_t i = 0; i < payload.size(); i++) chksum += data[i];
    std::string framed;
    framed.reserve(payload.size() + 4);
    framed += '$';
    framed.append(static_cast<const char*>(payload.data()), payload.size());
    framed += '#';
    framed += toHex[chksum >> 4];
    framed += toHex[chksum & 0x0f];
    Slice out;
    out.acquire(std::move(framed));
    m_connection->write(std::move(out));
}

void PCSX::GdbClient::onReadable() {
    uint8_t buffer[BUFFER_SIZE];
    while (m_connection && (m_status == OPEN) && (m_connection->size() > 0)) {
        auto toRead = std::min(sizeof(buffer), m_connection->size());
        auto got = m_connection->read(buffer, toRead);
        if (got <= 0) break;
        Slice slice;
        slice.borrow(buffer, got);
        processData(slice);
    }
    // eof() is closed-and-drained, so a peer that hung up mid-packet still gets
    // everything it managed to send processed before we tear the client down.
    if (m_connection && m_connection->eof()) close();
}

void PCSX::GdbClient::close() {
    if (m_status != OPEN) return;
    m_status = CLOSING;
    if (m_connection) {
        m_connection.asA<UvFifo>()->clearNotifier();
        m_connection.reset();
    }
    // Deleting the client here would free the object the async callback is
    // running inside of. Hand that off to the async's own close callback, which
    // uv only runs once the handle is genuinely done.
    auto context = m_asyncContext;
    m_asyncContext = nullptr;
    if (!context) {
        delete this;
        return;
    }
    uv_close(reinterpret_cast<uv_handle_t*>(&context->m_async), [](uv_handle_t* handle) {
        auto context = reinterpret_cast<AsyncContext*>(handle);
        delete context->m_client;
        delete context;
    });
}

PCSX::GdbClient::~GdbClient() {
    if (m_reverse) {
        g_emulator->m_debug->stopReplay();
        g_emulator->suppressRewindCapture(false);
    }
    m_breakpoints.destroyAll();
}

void PCSX::GdbClient::startReverse(bool isContinue) {
    auto& emuSettings = g_emulator->settings;
    if (!emuSettings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
        g_system->printf(_("GDB server: reverse execution needs the debugger (use -debugger)\n"));
        write("E02");
        return;
    }
    if (g_emulator->m_cpu->isDynarec()) {
        g_system->printf(_("GDB server: reverse execution needs the interpreter (use -interpreter)\n"));
        write("E02");
        return;
    }
    if (g_system->running()) g_system->pause();
    const uint64_t now = g_emulator->m_cpu->m_regs.cycle;
    size_t snapshot = g_emulator->rewindSnapshotCount();
    while ((snapshot > 0) && (g_emulator->rewindSnapshotCycle(snapshot - 1) >= now)) snapshot--;
    if (snapshot == 0) {
        // Snapshots taken by hand (Lua's createRewindState()) are enough; with an empty ring that
        // isn't capturing, there will never be a past to go back to.
        if ((g_emulator->rewindSnapshotCount() == 0) &&
            (emuSettings.get<Emulator::SettingRewindInterval>().value == 0)) {
            g_system->printf(
                _("GDB server: reverse execution replays from the rewind ring, which is off (RewindInterval is 0)\n"));
            write("E01");
        } else {
            write("T05replaylog:begin;");
        }
        return;
    }
    snapshot--;
    m_reverse = ReverseJob{.isContinue = isContinue, .now = now, .snapshot = snapshot};
    g_emulator->suppressRewindCapture(true);
    replayFrom(snapshot, now);
}

void PCSX::GdbClient::replayFrom(size_t snapshot, uint64_t stopAt) {
    std::weak_ptr<GdbClient*> self = m_self;
    // The Pause that ends a replay is signalled from inside the emulation, so the restore has
    // to wait for the main loop.
    g_system->scheduleRestore([self, snapshot, stopAt]() {
        auto client = self.lock();
        if (!client) return;
        g_emulator->restoreRewindSnapshot(snapshot);
        if (g_emulator->m_debug->startReplay(stopAt)) {
            g_system->resume();
        } else {
            (*client)->onReplayPaused();
        }
    });
}

void PCSX::GdbClient::land(uint64_t target) {
    m_reverse->landing = true;
    m_reverse->target = target;
    replayFrom(m_reverse->snapshot, target);
}

void PCSX::GdbClient::onReplayPaused() {
    auto replay = g_emulator->m_debug->stopReplay();
    auto& job = *m_reverse;
    if (!replay || !replay->reached) {
        // Interrupted, or an exception paused it. Wherever it is, it is a point of the past.
        g_system->printf(_("GDB server: replay from snapshot %zu (cycle %llu) paused early at pc %08x\n"), job.snapshot,
                         static_cast<unsigned long long>(g_emulator->rewindSnapshotCycle(job.snapshot)),
                         static_cast<uint32_t>(g_emulator->m_cpu->m_regs.pc));
        finishReverse("T05");
        return;
    }
    const uint64_t reached = *replay->reached;
    // The first replay ends on the boundary the command was issued at, and a landing on a
    // boundary the scan saw. Anything else means the replay is not following the same path.
    const bool firstScan = !job.landing && !job.scanned;
    if ((job.landing && (reached != job.target)) || (firstScan && (reached != job.now))) {
        g_system->printf(_("GDB server: replay diverged, expected to stop at cycle %llu, stopped at %llu\n"),
                         static_cast<unsigned long long>(job.landing ? job.target : job.now),
                         static_cast<unsigned long long>(reached));
        finishReverse("E03");
        return;
    }
    if (job.landing) {
        finishReverse(job.atEdge ? "T05replaylog:begin;" : "T05");
        return;
    }
    job.scanned = true;
    if (!job.isContinue) {
        land(replay->lastBoundary);
        return;
    }
    if (replay->lastHit) {
        land(*replay->lastHit);
        return;
    }
    if (job.snapshot == 0) {
        job.atEdge = true;
        land(g_emulator->rewindSnapshotCycle(0));
        return;
    }
    job.snapshot--;
    replayFrom(job.snapshot, g_emulator->rewindSnapshotCycle(job.snapshot + 1));
}

void PCSX::GdbClient::finishReverse(const char* reply) {
    m_reverse.reset();
    g_emulator->suppressRewindCapture(false);
    // The snapshots past this point belong to a future that is going to be executed again.
    g_emulator->dropRewindSnapshotsAfter(g_emulator->m_cpu->m_regs.cycle);
    write(std::string(reply));
}

PCSX::GdbClient::GdbClient(IO<File> connection, uv_loop_t* loop) : m_listener(g_system->m_eventBus) {
    m_loop = loop;
    m_connection = connection;
    m_asyncContext = new AsyncContext{{}, this};
    m_connection.asA<UvFifo>()->setNotifier(loop, &m_asyncContext->m_async, [this]() { onReadable(); });
    m_listener.listen<Events::ExecutionFlow::Run>([this](const auto& event) {
        m_exception = false;
        // gdb is done writing what it wanted to the machine we stopped at the shell for.
        if (m_restartRewindOnResume && !m_reverse) {
            m_restartRewindOnResume = false;
            g_emulator->restartRewind(m_rewindWasInUse);
        }
    });
    m_listener.listen<Events::ExecutionFlow::Pause>([this](const auto& event) {
        m_exception = event.exception;
        if (m_reverse) {
            onReplayPaused();
            return;
        }
        if (m_waitingForShell) {
            // This is a bit of a problem. If there's any remaining
            // breakpoint, we just blow past them. I'm not sure
            // how or where to wipe all breakpoints. The gdb
            // protocol doesn't seem to have a command to list them.
            g_system->resume();
        }
        // we technically should specify here why we stopped, but we don't have
        // the architecture for this just yet. Maybe that'll be part of the pause
        // event later on.
        if (m_waitingForTrap) write("T05");
        m_waitingForTrap = false;
    });
    m_listener.listen<Events::ExecutionFlow::ShellReached>([this](const auto& event) {
        if (!m_waitingForShell) return;
        g_system->log(LogClass::GDB, "Shell reached in gdb-server, pausing execution now.\n");
        m_waitingForShell = false;
        // gdb is about to load a binary into the paused machine, which no replay from an earlier
        // snapshot can do. The ring starts again once it lets the emulation run.
        m_rewindWasInUse = g_emulator->clearRewindRing();
        m_restartRewindOnResume = true;
        g_system->pause();
        write("OK");
    });
    m_listener.listen<Events::LogMessage>([this](const auto& event) {
        if (!m_canReceiveLogs) return;
        auto& emuSettings = PCSX::g_emulator->settings;
        auto& debugSettings = emuSettings.get<Emulator::SettingDebugSettings>();
        auto gdbLog = debugSettings.get<Emulator::DebugSettings::GdbLogSetting>().value;
        if (gdbLog == Emulator::DebugSettings::GdbLog::None) return;
        if ((gdbLog == Emulator::DebugSettings::GdbLog::TTY) && (event.logClass != LogClass::MIPS)) return;
        if (event.logClass == LogClass::GDB) return;
        auto msg = fmt::format("PCSX::{}>{}", magic_enum::enum_name(event.logClass), event.message);
        writeEscaped(std::move(msg));
    });
}

static int fromHexChar(char c) {
    if ((c >= '0') && (c <= '9')) return c - '0';
    if ((c >= 'A') && (c <= 'F')) return c + 10 - 'A';
    if ((c >= 'a') && (c <= 'f')) return c + 10 - 'a';
    return -1;
}

void PCSX::GdbClient::processData(const Slice& slice) {
    const char* ptr = reinterpret_cast<const char*>(slice.data());
    auto size = slice.size();
    while (size) {
        if (m_passthrough) {  // passthrough
            Slice passthrough;
            passthrough.borrow(ptr, size);
            passthrough = passthroughData(passthrough);
            ptr = reinterpret_cast<const char*>(passthrough.data());
            size = passthrough.size();
            continue;
        }
        char c = *ptr++;
        size--;
        switch (m_state) {
            case WAIT_FOR_ACK:
                if (m_ackEnabled) {
                    if (c != '+') {
                        close();
                        break;
                    }
                }
            case WAIT_FOR_DOLLAR:
                if (c == '+') sendAck();
                if (c == 3) g_system->pause();
                if (c != '$') break;
                m_state = READING_COMMAND;
                break;
            case READING_COMMAND:
                if (c == '$') {
                    processCommand();
                    m_cmd.clear();
                    break;
                }
                if (c == '}') {
                    m_state = ESCAPE;
                    break;
                }
                if (c == '#') {
                    m_state = READING_CRC_FIRST_CHAR;
                    break;
                }
                m_cmd += c;
                break;
            case ESCAPE:
                m_cmd += c ^ ' ';
                m_state = READING_COMMAND;
                break;
            case READING_CRC_FIRST_CHAR:
                if (c == '$') {
                    processCommand();
                    m_cmd.clear();
                    m_state = READING_COMMAND;
                    break;
                } else {
                    m_state = READING_CRC_SECOND_CHAR;
                    m_crc = fromHexChar(c) << 4;
                }
                break;
            case READING_CRC_SECOND_CHAR:
                if (c == '$') {
                    m_state = READING_COMMAND;
                } else {
                    m_state = WAIT_FOR_DOLLAR;
                    m_crc |= fromHexChar(c);
                }
                processCommand();
                m_cmd.clear();
                break;
        }
    }
}

static std::pair<uint32_t, bool> parseHexNumber(const char* str) {
    uint64_t value = 0;
    char c;

    while ((c = *str++)) {
        int v = fromHexChar(c);
        if (v < 0) return std::pair<uint32_t, bool>(0, false);
        value <<= 4;
        value |= v;
        if (value > std::numeric_limits<uint32_t>::max()) return std::pair<uint32_t, bool>(0, false);
    }

    return std::pair<uint32_t, bool>(value, true);
}

static std::pair<uint32_t, bool> parseHexNumber(const std::string& str) { return parseHexNumber(str.c_str()); }

// P<n>=<value>: n in hex with no padding, value as 8 hex digits in target byte order
static bool parseRegisterWrite(const std::string& cmd, uint32_t& n, uint32_t& value) {
    auto equal = cmd.find('=');
    if ((equal == std::string::npos) || (equal < 2) || (cmd.length() != equal + 9)) return false;
    auto [parsedN, validN] = parseHexNumber(cmd.substr(1, equal - 1));
    auto [parsedValue, validValue] = parseHexNumber(cmd.substr(equal + 1));
    if (!validN || !validValue || (parsedN >= 72)) return false;
    n = parsedN;
    value = parsedValue;
    return true;
}

static const std::string memoryMap = R"(<?xml version="1.0"?>
<memory-map>
  <!-- Everything here is described as RAM, because we don't really
       have any better option. -->

  <!-- Main memory bloc: let's go with 8MB straight off the bat. -->
  <memory type="ram" start="0x0000000000000000" length="0x800000"/>
  <memory type="ram" start="0xffffffff80000000" length="0x800000"/>
  <memory type="ram" start="0xffffffffa0000000" length="0x800000"/>

  <!-- EXP1 can go up to 8MB too. -->
  <memory type="ram" start="0x000000001f000000" length="0x800000"/>
  <memory type="ram" start="0xffffffff9f000000" length="0x800000"/>
  <memory type="ram" start="0xffffffffbf000000" length="0x800000"/>

  <!-- Scratchpad -->
  <memory type="ram" start="0x000000001f800000" length="0x400"/>
  <memory type="ram" start="0xffffffff9f800000" length="0x400"/>

  <!-- Hardware registers -->
  <memory type="ram" start="0x000000001f801000" length="0x2000"/>
  <memory type="ram" start="0xffffffff9f801000" length="0x2000"/>
  <memory type="ram" start="0xffffffffbf801000" length="0x2000"/>

  <!-- DTL BIOS SRAM -->
  <memory type="ram" start="0x000000001fa00000" length="0x200000"/>
  <memory type="ram" start="0xffffffff9fa00000" length="0x200000"/>
  <memory type="ram" start="0xffffffffbfa00000" length="0x200000"/>

  <!-- BIOS -->
  <memory type="ram" start="0x000000001fc00000" length="0x80000"/>
  <memory type="ram" start="0xffffffff9fc00000" length="0x80000"/>
  <memory type="ram" start="0xffffffffbfc00000" length="0x80000"/>

  <!-- MSAN -->
  <memory type="ram" start="0x0000000020000000" length="0x60000000"/>

  <!-- This really is only for 0xfffe0130 -->
  <memory type="ram" start="0xfffffffffffe0000" length="0x200"/>
</memory-map>
)";

static const std::string targetXML = R"(<?xml version="1.0"?>
<!DOCTYPE feature SYSTEM "gdb-target.dtd">
<target version="1.0">

<!-- Helping GDB -->
<architecture>mips:3000</architecture>
<osabi>none</osabi>

<!-- Mapping ought to be flexible, but there seems to be some
     hardcoded parts in gdb, so let's use the same mapping. -->
<feature name="org.gnu.gdb.mips.cpu">
  <reg name="r0" bitsize="32" regnum="0"/>
  <reg name="r1" bitsize="32"/>
  <reg name="r2" bitsize="32"/>
  <reg name="r3" bitsize="32"/>
  <reg name="r4" bitsize="32"/>
  <reg name="r5" bitsize="32"/>
  <reg name="r6" bitsize="32"/>
  <reg name="r7" bitsize="32"/>
  <reg name="r8" bitsize="32"/>
  <reg name="r9" bitsize="32"/>
  <reg name="r10" bitsize="32"/>
  <reg name="r11" bitsize="32"/>
  <reg name="r12" bitsize="32"/>
  <reg name="r13" bitsize="32"/>
  <reg name="r14" bitsize="32"/>
  <reg name="r15" bitsize="32"/>
  <reg name="r16" bitsize="32"/>
  <reg name="r17" bitsize="32"/>
  <reg name="r18" bitsize="32"/>
  <reg name="r19" bitsize="32"/>
  <reg name="r20" bitsize="32"/>
  <reg name="r21" bitsize="32"/>
  <reg name="r22" bitsize="32"/>
  <reg name="r23" bitsize="32"/>
  <reg name="r24" bitsize="32"/>
  <reg name="r25" bitsize="32"/>
  <reg name="r26" bitsize="32"/>
  <reg name="r27" bitsize="32"/>
  <reg name="r28" bitsize="32"/>
  <reg name="r29" bitsize="32"/>
  <reg name="r30" bitsize="32"/>
  <reg name="r31" bitsize="32"/>

  <reg name="lo" bitsize="32" regnum="33"/>
  <reg name="hi" bitsize="32" regnum="34"/>
  <reg name="pc" bitsize="32" regnum="37"/>
</feature>
<feature name="org.gnu.gdb.mips.cp0">
  <reg name="status" bitsize="32" regnum="32"/>
  <reg name="badvaddr" bitsize="32" regnum="35"/>
  <reg name="cause" bitsize="32" regnum="36"/>
</feature>

<!-- We don't have an FPU, but gdb hardcodes one, and will choke
     if this section isn't present. -->
<feature name="org.gnu.gdb.mips.fpu">
  <reg name="f0" bitsize="32" type="ieee_single" regnum="38"/>
  <reg name="f1" bitsize="32" type="ieee_single"/>
  <reg name="f2" bitsize="32" type="ieee_single"/>
  <reg name="f3" bitsize="32" type="ieee_single"/>
  <reg name="f4" bitsize="32" type="ieee_single"/>
  <reg name="f5" bitsize="32" type="ieee_single"/>
  <reg name="f6" bitsize="32" type="ieee_single"/>
  <reg name="f7" bitsize="32" type="ieee_single"/>
  <reg name="f8" bitsize="32" type="ieee_single"/>
  <reg name="f9" bitsize="32" type="ieee_single"/>
  <reg name="f10" bitsize="32" type="ieee_single"/>
  <reg name="f11" bitsize="32" type="ieee_single"/>
  <reg name="f12" bitsize="32" type="ieee_single"/>
  <reg name="f13" bitsize="32" type="ieee_single"/>
  <reg name="f14" bitsize="32" type="ieee_single"/>
  <reg name="f15" bitsize="32" type="ieee_single"/>
  <reg name="f16" bitsize="32" type="ieee_single"/>
  <reg name="f17" bitsize="32" type="ieee_single"/>
  <reg name="f18" bitsize="32" type="ieee_single"/>
  <reg name="f19" bitsize="32" type="ieee_single"/>
  <reg name="f20" bitsize="32" type="ieee_single"/>
  <reg name="f21" bitsize="32" type="ieee_single"/>
  <reg name="f22" bitsize="32" type="ieee_single"/>
  <reg name="f23" bitsize="32" type="ieee_single"/>
  <reg name="f24" bitsize="32" type="ieee_single"/>
  <reg name="f25" bitsize="32" type="ieee_single"/>
  <reg name="f26" bitsize="32" type="ieee_single"/>
  <reg name="f27" bitsize="32" type="ieee_single"/>
  <reg name="f28" bitsize="32" type="ieee_single"/>
  <reg name="f29" bitsize="32" type="ieee_single"/>
  <reg name="f30" bitsize="32" type="ieee_single"/>
  <reg name="f31" bitsize="32" type="ieee_single"/>

  <reg name="fcsr" bitsize="32" group="float"/>
  <reg name="fir" bitsize="32" group="float"/>
</feature>
</target>
)";

std::pair<uint64_t, uint64_t> PCSX::GdbClient::parseCursor(const std::string& cursorStr) {
    auto cursorStrs = StringsHelpers::split(cursorStr, ",");
    uint64_t off = 0;
    uint64_t len = 0;
    if (cursorStrs.size() == 2) {
        auto [pOff, properOff] = parseHexNumber(cursorStrs[0]);
        auto [pLen, properLen] = parseHexNumber(cursorStrs[1]);
        if (properOff) off = pOff;
        if (properLen) len = pLen;
    }

    return std::make_pair(off, len);
}

void PCSX::GdbClient::writePaged(const std::string& out, const std::string& cursorStr) {
    auto [off, len] = parseCursor(cursorStr);
    if (len < (out.length() - off)) {
        writef("m%s", out.substr(off, len).c_str());
    } else if (off != 0) {
        writef("l%s", out.substr(off, len).c_str());
    } else {
        writef("l%s", out.c_str());
    }
}

void PCSX::GdbClient::writeEscaped(const std::string& out) {
    std::string escaped;
    escaped.reserve(out.length() * 2 + 1);
    escaped += 'O';
    for (auto& c : out) {
        escaped += toHex[(c >> 4) & 0x0f];
        escaped += toHex[c & 0x0f];
    }

    write(std::move(escaped));
}

std::string PCSX::GdbClient::dumpValue(uint32_t value) {
    std::string ret = "";

    for (int i = 0; i < 4; i++) {
        ret += toHex[(value & 0xf0) >> 4];
        ret += toHex[(value & 0x0f)];
        value >>= 8;
    }

    return ret;
}

std::string PCSX::GdbClient::dumpOneRegister(int n) {
    // All registers are transferred as thirty-two bit quantities in the order:
    // 32 general-purpose; sr; lo; hi; bad; cause; pc;
    auto& regs = g_emulator->m_cpu->m_regs;
    uint32_t value = 0;
    if (n < 32) {
        value = regs.GPR.r[n];
    } else if (n == 32) {
        value = regs.CP0.n.Status;
    } else if (n == 33) {
        value = regs.GPR.n.lo;
    } else if (n == 34) {
        value = regs.GPR.n.hi;
    } else if (n == 35) {
        value = regs.CP0.n.BadVAddr;
    } else if (n == 36) {
        value = regs.CP0.n.Cause;
    } else if (n == 37) {
        value = m_exception ? regs.CP0.n.EPC : regs.pc;
    }

    return dumpValue(value);
}

void PCSX::GdbClient::setOneRegister(int n, uint32_t value) {
    // All registers are transferred as thirty-two bit quantities in the order:
    // 32 general-purpose; sr; lo; hi; bad; cause; pc;
    value = ((value & 0xff000000) >> 24) | ((value & 0x00ff0000) >> 8) | ((value & 0x0000ff00) << 8) |
            ((value & 0x000000ff) << 24);
    auto& regs = g_emulator->m_cpu->m_regs;
    if (n < 32) {
        g_emulator->m_cpu->setGPR(n, value);
    } else if (n == 32) {
        regs.CP0.n.Status = value;
    } else if (n == 33) {
        regs.GPR.n.lo = value;
    } else if (n == 34) {
        regs.GPR.n.hi = value;
    } else if (n == 35) {
        regs.CP0.n.BadVAddr = value;
    } else if (n == 36) {
        regs.CP0.n.Cause = value;
    } else if (n == 37) {
        g_emulator->m_cpu->setPC(value);
    }
}

void PCSX::GdbClient::processCommand() {
    if (m_ackEnabled) sendAck();
    if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::GdbServerTrace>()) {
        g_system->log(LogClass::GDB, "GDB --> PCSX %s\n", m_cmd.c_str());
    }
    using namespace std::literals;

    static const auto qSupported = "qSupported:"sv;
    static const auto qXferFeatures = "qXfer:features:read:target.xml:"sv;
    static const auto qXferThreads = "qXfer:threads:read::"sv;
    static const auto qXferMemMap = "qXfer:memory-map:read::"sv;
    static const auto qSymbol = "qSymbol:"sv;
    if (m_cmd == "!") {
        // extended mode?
        write("OK");
    } else if (m_cmd == "?") {
        // query reason for stop
        if (g_system->running()) {
            write("S00");
        } else {  // we may need one for 02 ? SIGINT
            write("S05");
        }
    } else if ((m_cmd == "D") || StringsHelpers::startsWith(m_cmd, "D;")) {
        // detach, with a pid after the semicolon when multiprocess was negotiated
        write("OK");
        close();
    } else if (m_cmd == "qC") {
        // return current thread id - always 1, process-qualified when multiprocess was negotiated
        write(m_multiprocess ? "QCp1.1" : "QC1");
    } else if (m_cmd == "qAttached") {
        // query if attached to existing process - always true
        write("1");
    } else if (m_cmd == "g") {
        // read general register
        // replies with all registers
        std::string all = "";
        startStream();
        // the protocol really wants 72 registers:
        // 32 gpr + status + lo + hi + badv + cause + 32 fpr + 3 fpu registers
        for (int i = 0; i < 72; i++) stream(dumpOneRegister(i));
        stopStream();
    } else if (StringsHelpers::startsWith(m_cmd, "p")) {
        // read one register: p<n>, with n in hex and no padding, so p4 as well as p25
        auto [n, valid] = parseHexNumber(m_cmd.substr(1));
        if ((m_cmd.length() < 2) || !valid || (n >= 72)) {
            write("E00");
            return;
        }
        write(dumpOneRegister(n));
    } else if (StringsHelpers::startsWith(m_cmd, "P")) {
        // write one register
        uint32_t n, value;
        if (!parseRegisterWrite(m_cmd, n, value)) {
            write("E00");
            return;
        }
        setOneRegister(n, value);
        write("OK");
    } else if (m_cmd == "c") {
        // continue - this doesn't technically have a reply, only when the target stops later, using T05.
        m_canReceiveLogs = true;
        g_system->resume();
        m_waitingForTrap = true;
    } else if (m_cmd[0] == 'M') {
        // write memory
        auto elements = StringsHelpers::split(m_cmd, ":");
        auto [off, len] = parseCursor(elements[0].substr(1));
        if (((off == 0x8000f800) && (len == 0x800)) || ((off == 0x8000ffea) && (len == 22))) {
            // heuristic for our ps-exe.ld and cpe.ld
            write("OK");
            return;
        }
        size_t i = 0;
        IO<File> memFile = g_emulator->m_mem->getMemoryAsFile();
        memFile->wSeek(off);
        while (len--) {
            uint8_t c = fromHexChar(elements[1][i * 2 + 0]);
            c <<= 4;
            c |= fromHexChar(elements[1][i * 2 + 1]);
            memFile->write(c);
            i++;
        }
        write("OK");
    } else if (m_cmd[0] == 'm') {
        // read memory
        auto [off, len] = parseCursor(m_cmd.substr(1));
        startStream();
        IO<File> memFile = g_emulator->m_mem->getMemoryAsFile();
        memFile->rSeek(off);
        while (len--) {
            uint8_t v = memFile->read<uint8_t>();
            char s[3] = {0, 0, 0};
            s[0] = toHex[v >> 4];
            s[1] = toHex[(v & 0x0f)];
            stream(s);
        }
        stopStream();
    } else if ((m_cmd[0] == 'z') || (m_cmd[0] == 'Z')) {
        // insert or remove breakpoint
        enum class Action {
            ADD,
            REMOVE,
        } action = m_cmd[0] == 'z' ? Action::REMOVE : Action::ADD;
        if (m_cmd.find(';') != std::string::npos) {
            // we're not going to support advanced conditional breakpoints
            write("");
            return;
        }
        auto breakpointData = StringsHelpers::split(m_cmd.substr(1), ",");
        if (breakpointData.size() != 3) {
            // wrong number of arguments
            write("");
            return;
        }
        auto [type, vtype] = parseHexNumber(breakpointData[0]);
        auto [addr, vaddr] = parseHexNumber(breakpointData[1]);
        auto [kind, vkind] = parseHexNumber(breakpointData[2]);
        if (!vtype || !vaddr || !vkind) {
            // didn't manage to parse breapoint data properly.
            write("E00");
            close();
            return;
        }
        if ((action == Action::ADD) && (type <= 4) &&
            !g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
            // breakpoints are only checked when the debugger is enabled
            g_system->printf(_("GDB server: refusing breakpoint at %08x, the debugger is disabled (use -debugger)\n"),
                             addr);
            write("E02");
            return;
        }
        if ((action == Action::ADD) && (type >= 2) && (type <= 4)) {
            // watchpoint ranges: reject empty ones and ones wrapping around the address space
            uint32_t start = addr & ~0xe0000000;
            if ((kind == 0) || (start + kind - 1 < start)) {
                write("E01");
                return;
            }
        }
        const auto bpActionExec = [action, this](uint32_t addr, Debug::BreakpointType type, unsigned width) -> void {
            if (action == Action::ADD) {
                auto bp = g_emulator->m_debug->addBreakpoint(addr, type, width, _("GDB client"));
                m_breakpoints.push_back(bp);
            } else {
                addr &= ~0xe0000000;
                auto& tree = g_emulator->m_debug->getTree();
                auto bp = tree.find(addr, Debug::BreakpointTreeType::INTERVAL_SEARCH);
                while (bp != tree.end()) {
                    if (bp->type() == type && !m_breakpoints.isLinked(&*bp)) {
                        bp++;
                    } else {
                        g_emulator->m_debug->removeBreakpoint(&*bp);
                        bp = tree.find(addr, Debug::BreakpointTreeType::INTERVAL_SEARCH);
                    }
                }
            }
        };
        switch (type) {
            case 0:  // software breakpoint - meh, why?
            case 1:  // exec breakpoint
                // kind:
                //  2 = 16-bits MIPS16
                //  3 = 16-bits microMIPS
                //  4 = 32-bits MIPS
                //  5 = 32-bits microMIPS
                bpActionExec(addr, Debug::BreakpointType::Exec, 4);
                write("OK");
                break;
                // kind = number of bytes
            case 2:  // write breakpoint
                bpActionExec(addr, Debug::BreakpointType::Write, kind);
                write("OK");
                break;
            case 3:  // read breakpoint
                bpActionExec(addr, Debug::BreakpointType::Read, kind);
                write("OK");
                break;
            case 4:  // access breakpoint, aka both read and write
                bpActionExec(addr, Debug::BreakpointType::Read, kind);
                bpActionExec(addr, Debug::BreakpointType::Write, kind);
                write("OK");
                break;
            default:
                write("");
                return;
        }
    } else if (m_cmd == "s") {
        if (g_system->running()) g_system->pause();
        m_waitingForTrap = true;
        g_emulator->m_debug->stepIn();
    } else if (m_cmd == "bs") {
        startReverse(false);
    } else if (m_cmd == "bc") {
        startReverse(true);
    } else if (m_cmd == "Hc0") {
        // thread stuff
        write("OK");
    } else if (m_cmd == "Hc-1") {
        write("OK");
    } else if (m_cmd == "Hg0") {
        write("OK");
    } else if (StringsHelpers::startsWith(m_cmd, "vKill;")) {
        write("OK");
    } else if (StringsHelpers::startsWith(m_cmd, qSupported)) {
        auto elements = StringsHelpers::split(m_cmd.substr(qSupported.length()), ";");
        m_multiprocess = false;
        for (const auto& element : elements) {
            if (element == "multiprocess+") {
                m_multiprocess = true;
            }
        }
        std::string answer = "PacketSize=47ff;qXfer:threads:read+;QStartNoAckMode+;ReverseStep+;ReverseContinue+";
        if (m_multiprocess) {
            answer += ";multiprocess+";
        }
        if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::GdbManifest>()) {
            answer += ";qXfer:features:read+;qXfer:memory-map:read+";
        }
        write(std::move(answer));
    } else if (StringsHelpers::startsWith(m_cmd, "QStartNoAckMode")) {
        m_ackEnabled = false;
        write("OK");
    } else if (StringsHelpers::startsWith(m_cmd, "qRcmd,")) {
        // this is the "monitor" command
        size_t len = m_cmd.length() - 6;
        std::string monitor;
        monitor.reserve(len / 2);
        for (size_t i = 0; i < len; i += 2) {
            char c = fromHexChar(m_cmd[i + 6]);
            c <<= 4;
            c |= fromHexChar(m_cmd[i + 7]);
            monitor += c;
        }
        processMonitorCommand(monitor);
    } else if (StringsHelpers::startsWith(m_cmd, qXferMemMap) &&
               g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::GdbManifest>()) {
        writePaged(memoryMap, m_cmd.substr(qXferMemMap.length()));
    } else if (StringsHelpers::startsWith(m_cmd, qXferFeatures) &&
               g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::GdbManifest>()) {
        writePaged(targetXML, m_cmd.substr(qXferFeatures.length()));
    } else if (StringsHelpers::startsWith(m_cmd, qXferThreads)) {
        writePaged(m_multiprocess ? "<threads><thread id=\"p1.1\" core=\"0\" name=\"MainThread\"/></threads>"
                                  : "<threads><thread id=\"1\" core=\"0\" name=\"MainThread\"/></threads>",
                   m_cmd.substr(qXferThreads.length()));
    } else {
        g_system->log(LogClass::GDB, "Unknown GDB command: %s\n", m_cmd.c_str());
        write("");
    }
}

void PCSX::GdbClient::processMonitorCommand(const std::string& cmd) {
    auto words = StringsHelpers::split(cmd, " ");
    if (words[0] == "reset") {
        bool hard = false;
        bool halt = false;
        bool shellhalt = false;

        for (size_t i = 1; i < words.size(); i++) {
            if (words[i] == "hard") {
                hard = true;
            } else if (words[i] == "halt") {
                halt = true;
            } else if (words[i] == "shellhalt") {
                shellhalt = true;
            }
        }

        if (halt && shellhalt) {
            writeEscaped("Cannot use both halt and shellhalt\n");
            return;
        }

        if (hard) {
            writeEscaped("Emulation hard-reset\n");
            g_system->hardReset();
        } else {
            writeEscaped("Emulation reset\n");
            g_system->softReset();
        }

        if (halt) {
            writeEscaped("Emulation paused\n");
            g_system->pause();
        } else if (shellhalt) {
            writeEscaped("Emulation running until shell\n");
            m_waitingForShell = true;
            g_system->resume();
            // let's not reply to gdb just yet, until we've reached the shell
            // and are ready to load a binary.
            return;
        }
    } else if (words[0] == "mountcd") {
        if (words.size() != 2) {
            writeEscaped("Usage: mountcd <path>\n");
        } else {
            writeEscaped("Mounting CD image\n");
            auto pathCmd = cmd.substr(8);
            auto pathView = StringsHelpers::trim(pathCmd);
            g_emulator->m_cdrom->setIso(new CDRIso(pathView));
            g_emulator->m_cdrom->parseIso();
        }
    } else if (words[0] == "sharedmem") {
        if (words.size() != 2) {
            writeEscaped("Usage: sharedmem <type>");
        } else {
            if (words[1] == "wram") {
                writeEscaped(g_emulator->m_mem->m_wramShared.getSharedName());
            } else {
                writeEscaped("Unknown type. Valid types: wram");
            }
        }
    } else if (words[0] == "cache") {
        // Writing memory over gdb won't invalidate anything by itself: most of what goes through
        // there is data, and there's no d-cache to worry about. Patching code needs this after.
        if ((words.size() != 2) || (words[1] != "flush")) {
            writeEscaped("Usage: cache flush\n");
        } else {
            writeEscaped("Flushing i-cache\n");
            g_emulator->m_cpu->invalidateCache();
        }
    }
    write("OK");
}

PCSX::Slice PCSX::GdbClient::passthroughData(Slice slice) { return slice; }

#endif
