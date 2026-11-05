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

#include "spu/overlay.h"

#include <algorithm>
#include <cmath>

#include "supportpsx/sf2.h"

std::shared_ptr<const PCSX::SPU::Overlay> PCSX::SPU::Overlay::fromSF2(const uint8_t* data, size_t size,
                                                                      unsigned* skipped) {
    auto samples = SF2::read(data, size);
    if (samples.empty()) return nullptr;
    auto overlay = std::make_shared<Overlay>();
    unsigned bad = 0;
    for (auto& s : samples) {
        uint64_t signature = 0;
        bool valid = s.name.size() == 16 && !s.pcm.empty() && s.sampleRate != 0;
        for (char c : s.name) {
            if (!valid) break;
            unsigned v;
            if (c >= '0' && c <= '9') {
                v = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                v = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                v = c - 'A' + 10;
            } else {
                valid = false;
                break;
            }
            signature = (signature << 4) | v;
        }
        if (!valid) {
            bad++;
            continue;
        }
        Entry& e = overlay->m_entries[signature];
        e.pcm.assign(s.pcm.begin(), s.pcm.end());
        e.ratio = double(s.sampleRate) / 44100.0 * std::exp2((60.0 - s.rootKey) / 12.0 + s.correction / 1200.0);
        e.loops = s.loops;
        e.loopStart = s.loopStart;
        e.loopEnd = s.loopEnd;
    }
    if (skipped) *skipped = bad;
    return overlay;
}

int32_t PCSX::SPU::OverlayVoice::next(int32_t step) {
    if (!m_entry || m_ended) return 0;
    const auto& pcm = m_entry->pcm;
    const size_t i = size_t(m_pos);
    const double frac = m_pos - double(i);
    const float a = i < pcm.size() ? pcm[i] : 0.0f;
    float b = i + 1 < pcm.size() ? pcm[i + 1] : 0.0f;
    if (m_entry->loops && double(i + 1) >= m_entry->loopEnd) b = pcm[size_t(m_entry->loopStart)];
    const double value = a + (b - a) * frac;

    m_pos += double(step) / 65536.0 * m_entry->ratio;
    if (m_entry->loops) {
        const double length = m_entry->loopEnd - m_entry->loopStart;
        while (m_pos >= m_entry->loopEnd) m_pos -= length;
    } else if (m_pos >= double(pcm.size())) {
        // Silence, not the original, for the rest of the note.
        m_ended = true;
    }
    return int32_t(std::clamp(std::lround(value), -32768l, 32767l));
}
