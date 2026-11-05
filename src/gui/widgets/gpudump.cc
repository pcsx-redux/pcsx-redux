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

#include "gui/widgets/gpudump.h"

#include "core/gpu.h"
#include "core/gpudump.h"
#include "core/psxemulator.h"
#include "core/system.h"
#include "imgui.h"
#include "support/file.h"

PCSX::Widgets::GPUDump::GPUDump(bool& show, std::vector<std::string>& favorites)
    : m_show(show),
      m_saveDialog([]() { return _("Save GPU dump"); }, favorites),
      m_openDialog([]() { return _("Open GPU dump"); }, favorites) {}

PCSX::Widgets::GPUDump::~GPUDump() {
    if (m_texture) glDeleteTextures(1, &m_texture);
}

void PCSX::Widgets::GPUDump::draw(const char* title) {
    ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }
    drawRecorder();
    ImGui::Separator();
    drawPlayer();
    ImGui::End();

    // The dump's own GPU logger: frames are the dump's vsyncs, and a replay goes to the player's GPU.
    auto& player = g_emulator->m_gpuDumpPlayer;
    if (m_showLogger && player->loaded()) m_logger.draw(player->logger(), _("GPU Dump Logger"), !m_playing);

    if (m_saveDialog.draw()) {
        auto& selected = m_saveDialog.selected();
        if (!selected.empty()) {
            IO<File> file = new PosixFile(selected[0], FileOps::TRUNCATE);
            if (file->failed()) {
                m_error = _("Unable to create the dump file.");
            } else {
                m_error.clear();
                g_emulator->m_gpuDumper->start(file);
            }
        }
    }
    if (m_openDialog.draw()) {
        auto& selected = m_openDialog.selected();
        if (!selected.empty()) {
            m_playing = false;
            IO<File> file = new PosixFile(selected[0]);
            if (g_emulator->m_gpuDumpPlayer->load(file)) {
                m_error.clear();
            } else {
                m_error = _("Not a GPU dump file.");
            }
        }
    }
}

void PCSX::Widgets::GPUDump::drawRecorder() {
    auto& dumper = g_emulator->m_gpuDumper;
    ImGui::TextUnformatted(_("Recorder"));
    if (dumper->recording()) {
        ImGui::Text(_("Recording, %llu frames"), static_cast<unsigned long long>(dumper->frames()));
        ImGui::SameLine();
        if (ImGui::Button(_("Stop recording"))) dumper->stop();
    } else if (dumper->armed()) {
        ImGui::TextUnformatted(_("Waiting for the next vsync..."));
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel"))) dumper->stop();
    } else {
        if (ImGui::Button(_("Start recording..."))) m_saveDialog.openDialog();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", _("The capture starts at the next vsync, with the full VRAM and GPU state."));
    }
    if (!m_error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_error.c_str());
}

void PCSX::Widgets::GPUDump::drawPlayer() {
    auto& player = g_emulator->m_gpuDumpPlayer;
    ImGui::TextUnformatted(_("Player"));
    if (ImGui::Button(_("Open dump..."))) m_openDialog.openDialog();
    if (!player->loaded()) return;
    ImGui::SameLine();
    if (ImGui::Button(_("Close"))) {
        m_playing = false;
        player->unload();
        return;
    }

    if (!player->gameID().empty()) ImGui::Text(_("Game ID: %s"), player->gameID().c_str());
    if (!player->comment().empty()) ImGui::Text(_("Comment: %s"), player->comment().c_str());
    uint64_t frame = player->frame();
    const uint64_t first = 0, last = player->frameCount();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderScalar("##frame", ImGuiDataType_U64, &frame, &first, &last, _("Frame %llu"))) {
        m_playing = false;
        player->seek(frame);
    }

    if (ImGui::Button(m_playing ? C_("Playback", "Pause") : C_("Playback", "Play"))) m_playing = !m_playing;
    ImGui::SameLine();
    ImGui::BeginDisabled(m_playing);
    if (ImGui::Button(C_("Playback", "Step"))) player->step();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(_("Rewind"))) player->rewind();
    ImGui::SameLine();
    ImGui::Checkbox(_("Loop"), &m_loop);
    ImGui::SameLine();
    ImGui::Checkbox(_("Logger"), &m_showLogger);
    ImGui::SameLine();
    ImGui::Checkbox(_("Display area only"), &m_displayOnly);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat(_("Zoom"), &m_zoom, 0.5f, 4.0f, "%.1fx");

    if (m_playing && !player->step()) {
        if (m_loop) {
            player->rewind();
        } else {
            m_playing = false;
        }
    }

    updateTexture();

    ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
    ImVec2 size(1024.0f, 512.0f);
    if (m_displayOnly) {
        // Crop to what the video output shows, from the GP1(05) display start and GP1(08) display mode.
        auto gpu = player->gpu();
        uint32_t start = gpu->getStatusControl(5);
        uint32_t mode = gpu->getStatusControl(8);
        static constexpr unsigned widths[4] = {256, 320, 512, 640};
        unsigned w = (mode & 0x40) ? 368 : widths[mode & 3];
        unsigned h = ((mode & 0x24) == 0x24) ? 480 : 240;
        unsigned x = start & 0x3ff;
        unsigned y = (start >> 10) & 0x1ff;
        uv0 = ImVec2(x / 1024.0f, y / 512.0f);
        uv1 = ImVec2((x + w) / 1024.0f, (y + h) / 512.0f);
        size = ImVec2(float(w), float(h));
    }
    size.x *= m_zoom;
    size.y *= m_zoom;
    ImGui::BeginChild("vram", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image((ImTextureID)(intptr_t)m_texture, size, uv0, uv1);
    ImGui::EndChild();
}

void PCSX::Widgets::GPUDump::updateTexture() {
    auto gpu = g_emulator->m_gpuDumpPlayer->gpu();
    Slice vram = gpu->getVRAM();
    const uint16_t* src = vram.data<uint16_t>();
    m_pixels.resize(1024 * 512);
    // Expand 15-bit BGR to opaque RGBA; the mask bit is not transparency on the video output.
    for (unsigned i = 0; i < 1024 * 512; i++) {
        uint16_t p = src[i];
        uint32_t r = (p & 0x1f) << 3, g = ((p >> 5) & 0x1f) << 3, b = ((p >> 10) & 0x1f) << 3;
        m_pixels[i] = r | (g << 8) | (b << 16) | 0xff000000;
    }
    if (!m_texture) {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
    } else {
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
    }
}
