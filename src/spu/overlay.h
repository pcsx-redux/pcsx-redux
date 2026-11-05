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

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace PCSX {

namespace SPU {

// Replacement samples, keyed by the signature of the sound RAM sample they stand in for
// (SPUSample::signature). Built from a SoundFont whose sample names are signatures, which
// is what the SPU Samples export writes. A replacement plays on note 60 at SPU pitch 1000h,
// as the export defines it: its own rate, root key and pitch correction are folded into
// one playback ratio against that.
class Overlay {
  public:
    struct Entry {
        std::vector<float> pcm;
        double ratio = 1.0;  // source samples per output sample at pitch 1000h
        bool loops = false;
        double loopStart = 0;
        double loopEnd = 0;
    };

    // Samples whose name is not exactly 16 hex digits are skipped and counted in `skipped`.
    static std::shared_ptr<const Overlay> fromSF2(const uint8_t* data, size_t size, unsigned* skipped = nullptr);

    const Entry* find(uint64_t signature) const {
        auto it = m_entries.find(signature);
        return it == m_entries.end() ? nullptr : &it->second;
    }
    size_t size() const { return m_entries.size(); }

  private:
    std::unordered_map<uint64_t, Entry> m_entries;
};

// One voice's playhead into its replacement. It only produces the voice's audio: the ADPCM
// decoder keeps running underneath, so ENDX, loop flags, the repeat address, IRQ address
// hits, capture buffers and frequency modulation all still come from the original sample.
class OverlayVoice {
  public:
    void start(std::shared_ptr<const Overlay> pack, const Overlay::Entry* entry) {
        m_pack = std::move(pack);
        m_entry = entry;
        m_pos = 0;
        m_ended = false;
    }
    void reset() {
        m_pack.reset();
        m_entry = nullptr;
    }
    bool active() const { return m_entry != nullptr; }
    // The sample at the playhead, then advance by the voice's 16.16 pitch step. A one-shot
    // replacement that runs out goes silent while the original carries on.
    int32_t next(int32_t step);

  private:
    std::shared_ptr<const Overlay> m_pack;
    const Overlay::Entry* m_entry = nullptr;
    double m_pos = 0;
    bool m_ended = false;
};

}  // namespace SPU

}  // namespace PCSX
