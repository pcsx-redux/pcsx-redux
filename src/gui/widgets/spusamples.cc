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

#include "gui/widgets/spusamples.h"

#include <algorithm>

#include "core/spu.h"
#include "core/system.h"
#include "imgui.h"
#include "implot/implot.h"
#include "spu/adpcm.h"
#include "spu/overlay.h"
#include "support/file.h"
#include "support/imgui-helpers.h"
#include "supportpsx/sf2.h"

namespace {

using PCSX::SPUSample::c_blockSize;
using PCSX::SPUSample::c_samplesPerBlock;

}  // namespace

PCSX::Widgets::SPUSamples::SPUSamples(bool& show, std::vector<std::string>& favorites)
    : m_show(show),
      m_exportDialog(l_("Export SoundFont"), favorites),
      m_overlayDialog(l_("Load replacement SoundFont"), favorites) {}

void PCSX::Widgets::SPUSamples::decode(const uint8_t* spuRam, const Sample& sample, std::vector<float>& out) {
    out.clear();
    // decodeBlock reads the block in place without writing it, but takes a mutable pointer.
    uint8_t* ram = const_cast<uint8_t*>(spuRam);
    SPU::AdpcmDecoder decoder;
    decoder.reset();
    for (uint32_t addr = sample.start; addr + c_blockSize <= sample.end; addr += c_blockSize) {
        decoder.decodeBlock(ram + addr);
        while (!decoder.bufferExhausted()) out.push_back(float(decoder.takeSample()));
    }
}

std::vector<uint8_t> PCSX::Widgets::SPUSamples::exportSF2(const uint8_t* spuRam,
                                                          const std::map<uint32_t, Sample>& samples) {
    std::vector<SF2::Sample> out;
    std::vector<float> pcm;
    for (auto& [start, cached] : samples) {
        // Sound RAM may have changed since the map was last rebuilt.
        Sample s = cached;
        static_cast<SPUSample::Extent&>(s) = SPUSample::walk(spuRam, start);
        if (s.truncated) continue;
        s.signature = SPUSample::signature(spuRam, s);
        decode(spuRam, s, pcm);
        SF2::Sample& o = out.emplace_back();
        o.name = fmt::format("{:016x}", s.signature);
        o.pcm.reserve(pcm.size());
        for (float v : pcm) o.pcm.push_back(int16_t(std::clamp(v, -32768.0f, 32767.0f)));
        o.sampleRate = 44100;
        o.rootKey = 60;
        if (s.repeats && s.hasLoop) {
            o.loops = true;
            o.loopStart = (s.loop - s.start) / c_blockSize * c_samplesPerBlock;
            o.loopEnd = uint32_t(o.pcm.size());
        }
    }
    if (out.empty()) return {};
    return SF2::write(out, "PCSX-Redux SPU samples",
                      "Sample names are the SPU sample signatures. Root keys are unknown: every sample plays at "
                      "its stored rate on note 60, which is SPU pitch 1000h (44100 Hz). Note 69 is 440 Hz (A4). "
                      "Loops come from the ADPCM loop flags only; a loop address the game wrote to the voice "
                      "register is not exported.");
}

void PCSX::Widgets::SPUSamples::rebuild(PCSX::SPULogger* logger, const uint8_t* spuRam) {
    const uint64_t seen = logger->seen();
    const size_t count = logger->size();
    // A clear restarts the count; anything else is new entries at the tail of the ring.
    if (m_lastSeen == ~uint64_t(0) || logger->clears() != m_lastClears) {
        m_samples.clear();
        m_lastSeen = 0;
        m_lastClears = logger->clears();
    }
    const size_t fresh = size_t(std::min<uint64_t>(seen - m_lastSeen, count));
    for (size_t i = count - fresh; i < count; i++) {
        const auto& ev = (*logger)[i];
        if (ev.kind != PCSX::SPULogger::Kind::KeyOn) continue;
        auto& s = m_samples[ev.address];
        s.keyOns++;
        if (ev.voice < 32) s.voiceMask |= 1u << ev.voice;
    }
    m_lastSeen = seen;

    // Sound RAM may have been rewritten under any of them, so every sample is walked again.
    for (auto& [start, s] : m_samples) {
        static_cast<SPUSample::Extent&>(s) = SPUSample::walk(spuRam, start);
        s.signature = s.truncated ? 0 : SPUSample::signature(spuRam, s);
    }

    if (m_selected) {
        auto it = m_samples.find(m_selectedStart);
        if (it == m_samples.end()) {
            m_selected = false;
            m_waveform.clear();
        } else {
            decode(spuRam, it->second, m_waveform);
        }
    }
}

void PCSX::Widgets::SPUSamples::draw(PCSX::SPULogger* logger, const uint8_t* spuRam, const char* title) {
    ImGui::SetNextWindowSize(ImVec2(640, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    if (!spuRam) {
        ImGui::TextUnformatted(_("The SPU is not initialized."));
        ImGui::End();
        return;
    }

    bool refresh = ImGui::Button(_("Refresh"));
    ImGui::SameLine();
    ImGui::Checkbox(_("Auto refresh"), &m_autoRefresh);
    if (refresh || (m_autoRefresh && (logger->seen() != m_lastSeen || logger->clears() != m_lastClears))) {
        rebuild(logger, spuRam);
    }

    if (!logger->enabled()) {
        ImGui::TextWrapped("%s", _("Sample starts come from key on events. Enable the SPU logger to collect them."));
    }
    ImGui::TextUnformatted(fmt::format(f_("{} samples"), m_samples.size()).c_str());
    ImGui::SameLine();
    if (ImGui::Button(_("Export SoundFont..."))) m_exportDialog.openDialog();
    ImGui::TextWrapped("%s", _("Root keys are unknown. The export plays every sample at its stored rate on note 60 "
                               "(pitch 1000h, 44100 Hz)."));
    if (!m_exportStatus.empty()) ImGui::TextUnformatted(m_exportStatus.c_str());

    auto overlay = g_emulator->m_spu->getOverlay();
    if (ImGui::Button(_("Load replacements..."))) m_overlayDialog.openDialog();
    ImGui::SameLine();
    ImGui::BeginDisabled(!overlay);
    if (ImGui::Button(_("Clear replacements"))) {
        g_emulator->m_spu->setOverlay(nullptr);
        overlay.reset();
        m_overlayStatus.clear();
    }
    ImGui::EndDisabled();
    ImGuiHelpers::ShowHelpMarker(
        _("Load a SoundFont whose sample names are signatures, such as an edited export. Each key on of a "
          "sample with a matching signature plays the replacement instead; the original keeps running "
          "underneath for everything but the sound. A replacement is tuned so that its root key plays at "
          "pitch 1000h."));
    if (!m_overlayStatus.empty()) ImGui::TextUnformatted(m_overlayStatus.c_str());

    const float tableHeight = ImGui::GetContentRegionAvail().y * 0.4f;
    if (ImGui::BeginTable(
            "SPUSamples", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, tableHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(_("Start"));
        ImGui::TableSetupColumn(_("End"));
        ImGui::TableSetupColumn(_("Samples"));
        ImGui::TableSetupColumn(_("Loop"));
        ImGui::TableSetupColumn(_("Key ons"));
        ImGui::TableSetupColumn(_("Voices"));
        ImGui::TableSetupColumn(_("Signature"));
        ImGui::TableSetupColumn(_("Replaced"));
        ImGui::TableHeadersRow();

        for (auto& [start, s] : m_samples) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool isSelected = m_selected && m_selectedStart == start;
            if (ImGui::Selectable(fmt::format("{:05x}", start).c_str(), isSelected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selected = true;
                m_selectedStart = start;
                decode(spuRam, s, m_waveform);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(fmt::format("{:05x}{}", s.end, s.truncated ? "+" : "").c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(fmt::format("{}", (s.end - start) / c_blockSize * 28).c_str());
            ImGui::TableSetColumnIndex(3);
            if (s.repeats && s.hasLoop) {
                ImGui::TextUnformatted(fmt::format("{:05x}", s.loop).c_str());
            } else if (s.repeats) {
                ImGui::TextUnformatted(_("repeat"));
            } else {
                ImGui::TextUnformatted(_("one shot"));
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(fmt::format("{}", s.keyOns).c_str());
            ImGui::TableSetColumnIndex(5);
            std::string voices;
            for (unsigned v = 0; v < 24; v++) {
                if (!(s.voiceMask & (1u << v))) continue;
                if (!voices.empty()) voices += ' ';
                voices += fmt::format("{}", v);
            }
            ImGui::TextUnformatted(voices.c_str());
            ImGui::TableSetColumnIndex(6);
            if (s.truncated) {
                ImGui::TextUnformatted(_("no end"));
            } else {
                ImGui::TextUnformatted(fmt::format("{:016x}", s.signature).c_str());
            }
            ImGui::TableSetColumnIndex(7);
            if (overlay && !s.truncated && overlay->find(s.signature)) ImGui::TextUnformatted(_("yes"));
        }
        ImGui::EndTable();
    }

    if (m_selected && !m_waveform.empty()) {
        auto it = m_samples.find(m_selectedStart);
        if (ImPlot::BeginPlot("##SPUWaveform", ImVec2(-1, -1))) {
            ImPlot::SetupAxes(_("Sample"), nullptr);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -32768, 32767);
            ImPlot::PlotLine(fmt::format("{:05x}", m_selectedStart).c_str(), m_waveform.data(), int(m_waveform.size()));
            if (it != m_samples.end() && it->second.repeats && it->second.hasLoop) {
                const double loopAt = double((it->second.loop - it->second.start) / c_blockSize * 28);
                ImPlot::PlotInfLines(_("Loop"), &loopAt, 1);
            }
            ImPlot::EndPlot();
        }
    }

    ImGui::End();

    if (m_overlayDialog.draw()) {
        auto& selected = m_overlayDialog.selected();
        if (!selected.empty()) {
            IO<File> file = new PosixFile(selected[0]);
            std::vector<uint8_t> data;
            if (!file->failed()) {
                data.resize(file->size());
                if (file->read(data.data(), data.size()) != ssize_t(data.size())) data.clear();
            }
            unsigned skipped = 0;
            auto loaded = data.empty() ? nullptr : SPU::Overlay::fromSF2(data.data(), data.size(), &skipped);
            if (!loaded) {
                m_overlayStatus = _("Not a SoundFont file.");
            } else {
                m_overlayStatus =
                    fmt::format(f_("{} replacements loaded, {} samples without a signature name skipped."),
                                loaded->size(), skipped);
                g_emulator->m_spu->setOverlay(std::move(loaded));
            }
        }
    }

    if (m_exportDialog.draw()) {
        auto& selected = m_exportDialog.selected();
        if (!selected.empty()) {
            auto data = exportSF2(spuRam, m_samples);
            if (data.empty()) {
                m_exportStatus = _("No complete samples to export.");
            } else {
                IO<File> file = new PosixFile(selected[0], FileOps::TRUNCATE);
                if (file->failed()) {
                    m_exportStatus = _("Unable to open the file for writing.");
                } else if (file->write(data.data(), data.size()) != ssize_t(data.size())) {
                    m_exportStatus = _("Writing the file failed.");
                } else {
                    m_exportStatus = fmt::format(f_("Exported {} bytes."), data.size());
                }
            }
        }
    }
}
