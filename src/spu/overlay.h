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
#include <unordered_set>
#include <vector>

namespace PCSX {

namespace SPU {

// Replacement samples, keyed by the signature of the sound RAM sample they stand in for
// (SPUSample::signature). Built from a SoundFont whose sample names are signatures, which
// is what the SPU Samples export writes. A replacement's rate, root key and pitch correction
// say what note it sounds, and it is played so that it sounds the same note as the original
// at every pitch. The original's note at pitch 1000h is the one the pack records for it, which
// is what the export measured, so an unedited export plays back unchanged.
class Overlay {
  public:
    struct Entry {
        std::vector<float> pcm;
        double ratio = 1.0;  // source samples per output sample at pitch 1000h, for an original on note 60
        bool loops = false;
        double loopStart = 0;
        double loopEnd = 0;
        // The original's note at pitch 1000h as the pack's writer measured it, from its record.
        double originalNote = 60.0;
        // Bytes of sound RAM the original's signature covers, from its record.
        uint32_t bytes = 0;
    };

    // Samples whose name is not exactly 16 hex digits, or with no record in the bank's comment,
    // are skipped and counted in `skipped`.
    static std::shared_ptr<const Overlay> fromSF2(const uint8_t* data, size_t size, unsigned* skipped = nullptr);

    // The replacement for a sample with this signature over `bytes` of sound RAM. Both have to
    // match the record, so skipping mayReplace never changes the answer.
    const Entry* find(uint64_t signature, uint32_t bytes) const {
        auto it = m_entries.find(signature);
        return it == m_entries.end() || it->second.bytes != bytes ? nullptr : &it->second;
    }
    size_t size() const { return m_entries.size(); }
    // Whether a sample whose signature covers `bytes` of sound RAM can have a replacement here.
    // When it cannot, the key-on can skip computing the signature.
    bool mayReplace(uint32_t bytes) const { return m_sizes.contains(bytes); }

  private:
    std::unordered_map<uint64_t, Entry> m_entries;
    std::unordered_set<uint32_t> m_sizes;
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
