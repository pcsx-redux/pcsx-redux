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

#include "core/spulogger.h"

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/spu.h"

void PCSX::SPULogger::logInternal(Kind kind, uint32_t address, uint32_t length, uint32_t value, uint8_t voice) {
    // Allocated on first use rather than at construction, so a session that never turns the
    // logger on never pays for the ring.
    if (m_events.size() != c_capacity) m_events.resize(c_capacity);

    Event ev;
    const auto &regs = g_emulator->m_cpu->m_regs;
    ev.cycle = regs.cycle;
    ev.pc = regs.pc;
    ev.address = address;
    ev.length = length;
    ev.value = value;
    ev.kind = kind;
    ev.voice = voice;

    if (m_count == c_capacity) {
        m_events[m_head] = ev;
        m_head = (m_head + 1) % c_capacity;
    } else {
        m_events[(m_head + m_count) % c_capacity] = ev;
        m_count++;
    }
    m_seen++;
}

void PCSX::SPULogger::clear() {
    m_head = 0;
    m_count = 0;
    m_seen = 0;
    m_clears++;
    m_baselineValid = false;
    m_gap = false;
}

void PCSX::SPULogger::setEnabled(bool enabled) { m_enabled = enabled; }

void PCSX::SPULogger::captureBaseline(const uint16_t *registers) {
    std::copy(registers, registers + m_baseline.size(), m_baseline.begin());
    m_baselineValid = true;
}

std::string PCSX::SPULogger::exportSPD(std::vector<uint8_t> &out, SPD::Options options) {
    if (!m_baselineValid) return "no register writes captured since the last clear";
    if (m_seen != m_count) return "the log has dropped its oldest events; clear it and capture a shorter span";
    if (m_gap) return "the logger was paused during the capture; clear it and capture again";

    std::vector<SPD::RegWrite> writes;
    for (size_t i = 0; i < m_count; i++) {
        const Event &ev = (*this)[i];
        if (ev.kind != Kind::RegWrite) continue;
        writes.push_back({ev.cycle, uint16_t(ev.address), uint16_t(ev.value)});
    }
    if (options.tickRate == 0) {
        bool pal = g_emulator->settings.get<Emulator::SettingVideo>() == Emulator::PSX_TYPE_PAL;
        options.tickRate = ((pal ? SPD::c_palHblanks : SPD::c_ntscHblanks) << 16) / 16;
    }
    options.clock = Emulator::m_psxClockSpeed;
    // Sound RAM is written from the mixer thread too; take a consistent copy.
    std::vector<uint8_t> ram(SPD::c_spuRamSize);
    g_emulator->m_spu->lockSPURAM();
    std::copy_n(g_emulator->m_spu->getSpuRam(), ram.size(), ram.begin());
    g_emulator->m_spu->unlockSPURAM();
    auto result = SPD::writeCapture(m_baseline, writes, ram.data(), options);
    if (!result.error.empty()) return result.error;
    out = std::move(result.data);
    return {};
}

const char *PCSX::SPULogger::kindName(Kind kind) {
    switch (kind) {
        case Kind::DMAWrite:
            return "DMA write";
        case Kind::DMARead:
            return "DMA read";
        case Kind::PIOWrite:
            return "PIO write";
        case Kind::TransferAddr:
            return "Transfer addr";
        case Kind::VoiceStart:
            return "Voice start";
        case Kind::VoiceLoop:
            return "Voice loop";
        case Kind::KeyOn:
            return "Key on";
        case Kind::KeyOff:
            return "Key off";
        case Kind::ReverbAddr:
            return "Reverb addr";
        case Kind::RegWrite:
            return "Register write";
    }
    return "?";
}

bool PCSX::SPULogger::isVoiceScoped(Kind kind) {
    switch (kind) {
        case Kind::VoiceStart:
        case Kind::VoiceLoop:
            return true;
        default:
            return false;
    }
}
