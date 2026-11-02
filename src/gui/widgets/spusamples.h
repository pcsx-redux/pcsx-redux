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

#include <map>
#include <string>
#include <vector>

#include "core/spulogger.h"
#include "gui/widgets/filedialog.h"
#include "supportpsx/spusample.h"

namespace PCSX {

namespace Widgets {

// Sample map of sound RAM, and a waveform view of the selected sample. Start addresses come
// from the SPU logger's key-on entries, which are the only ground truth for where a sample
// begins. Everything else is derived from sound RAM: each sample is walked a block at a time
// until a block carries the loop-end flag, noting the loop-start flag on the way. Each sample
// carries a content signature, and the map can be exported as a SoundFont.
class SPUSamples {
  public:
    SPUSamples(bool& show, std::vector<std::string>& favorites);
    void draw(PCSX::SPULogger* logger, const uint8_t* spuRam, const char* title);

    bool& m_show;

    struct Sample : SPUSample::Extent {
        uint64_t signature = 0;
        uint32_t keyOns = 0;
        uint32_t voiceMask = 0;  // voices seen keying it on
    };

    // Decodes a sample from its start, with the filter history cleared as key-on does.
    static void decode(const uint8_t* spuRam, const Sample& sample, std::vector<float>& out);
    // Every sample that ends in sound RAM, decoded, as one preset each. The SPU has no notion
    // of a root key, so each sample plays at its stored rate on MIDI note 60 (pitch 1000h,
    // 44100 Hz), and the bank comment says so.
    static std::vector<uint8_t> exportSF2(const uint8_t* spuRam, const std::map<uint32_t, Sample>& samples);

  private:
    void rebuild(PCSX::SPULogger* logger, const uint8_t* spuRam);

    std::map<uint32_t, Sample> m_samples;
    uint64_t m_lastSeen = ~uint64_t(0);
    uint64_t m_lastClears = 0;
    bool m_autoRefresh = true;
    bool m_selected = false;
    uint32_t m_selectedStart = 0;
    std::vector<float> m_waveform;
    FileDialog<FileDialogMode::Save> m_exportDialog;
    FileDialog<FileDialogMode::Open> m_overlayDialog;
    std::string m_exportStatus;
    std::string m_overlayStatus;
};

}  // namespace Widgets

}  // namespace PCSX
