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

#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace PCSX {

namespace SPU {

// Replacement samples, keyed by the signature of the sound RAM sample they stand in for
// (SPUSample::signature). Built from a SoundFont whose sample names are signatures, which
// is what the SPU Samples export writes. A replacement's rate, root key and pitch correction
// say what note it sounds, and it is played so that it sounds the same note as the original
// at every pitch. The original's note at pitch 1000h is measured from its own data
// (SPU::sampleFundamental, RootKey::fromFundamental), the same way the export measures it, so
// an unedited export plays back unchanged. An original with no fundamental counts as note 60.
class Overlay {
  public:
    struct Entry {
        std::vector<float> pcm;
        double ratio = 1.0;  // source samples per output sample at pitch 1000h, for an original on note 60
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
    // `originalNote` is the note the original sounds at pitch 1000h, fractional.
    void start(std::shared_ptr<const Overlay> pack, const Overlay::Entry* entry, double originalNote = 60.0) {
        m_pack = std::move(pack);
        m_entry = entry;
        m_ratio = entry ? entry->ratio * std::exp2((originalNote - 60.0) / 12.0) : 1.0;
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
    double m_ratio = 1.0;
    double m_pos = 0;
    bool m_ended = false;
};

}  // namespace SPU

}  // namespace PCSX
