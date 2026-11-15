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

#include "gui/widgets/emulation-config.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <system_error>

#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "imgui.h"
#include "support/imgui-helpers.h"

PCSX::Widgets::EmulationConfig::EmulationConfig(bool& show, int& idleSwapInterval, std::vector<std::string>& favorites)
    : m_show(show),
      m_idleSwapInterval(idleSwapInterval),
      m_selectBiosDialog(l_("Select BIOS"), favorites),
      m_selectScreenshotDirDialog(l_("Select screenshot folder"), favorites),
      m_selectSaveStateDirDialog(l_("Select save state folder"), favorites) {}

bool PCSX::Widgets::EmulationConfig::draw(GUI* gui) {
    bool changed = false;
    bool selectBiosDialog = false;
    bool selectEXP1Dialog = false;
    bool selectScreenshotDirDialog = false;
    bool selectSaveStateDirDialog = false;
    bool showDynarecDebugWarning = false;
    bool showDynarecWarning = false;
    auto& settings = g_emulator->settings;
    auto& debugSettings = settings.get<Emulator::SettingDebugSettings>();

    ImGui::SetNextWindowPos(ImVec2(50, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Emulation Configuration"), &m_show)) {
        if (ImGui::SliderInt(_("Idle Swap Interval"), &m_idleSwapInterval, 0, 10)) {
            changed = true;
            if (!g_system->running()) SDL_GL_SetSwapInterval(m_idleSwapInterval);
        }
        ImGui::Separator();
        // Emulation speed now lives at the audio sink (SPU::Speed), set in the SPU configuration window.
        changed |= ImGui::Checkbox(_("Enable XA decoder"), &settings.get<Emulator::SettingXa>().value);
        changed |= ImGui::Checkbox(_("Always enable SPU IRQ"), &settings.get<Emulator::SettingSpuIrq>().value);
        changed |= ImGui::Checkbox(_("Decode MDEC videos in B&W"), &settings.get<Emulator::SettingBnWMdec>().value);
        if (ImGui::Checkbox(_("Dynarec CPU"), &settings.get<Emulator::SettingDynarec>().value)) {
            changed = true;
            showDynarecWarning = true;
            if (debugSettings.get<PCSX::Emulator::DebugSettings::Debug>() && settings.get<Emulator::SettingDynarec>()) {
                showDynarecDebugWarning = true;
            }
        }

        ImGuiHelpers::ShowHelpMarker(_(R"(Activates the dynamic recompiler CPU core.
It is significantly faster than the interpreted CPU,
however it doesn't play nicely with the debugger.
Changing this setting requires a reboot to take effect.
The dynarec core isn't available for all CPUs, so
this setting may not have any effect for you.)"));
        bool memChanged = ImGui::Checkbox(_("8MB"), &settings.get<Emulator::Setting8MB>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(Emulates an installed 8MB system,
instead of the normal 2MB. Useful for working
with development binaries and games.)"));
        changed |= ImGui::InputScalar(_("Memory Fill Value"), ImGuiDataType_U8,
                                      (void*)&settings.get<Emulator::SettingMemoryFillValue>().value, nullptr, nullptr,
                                      "%02x", 0);
        ImGuiHelpers::ShowHelpMarker(_(R"(The SDRAM of the console will be
filled with this value at startup.
On real hardware, the SDRAM decays over time,
and its contents may not be 0 on cold boot.
This setting is useful for debugging purposes,
to detect uninitialized memory reads that may
cause issues in some cases.
)"));
        changed |= ImGui::Checkbox(_("OpenGL GPU *ALPHA STATE*"), &settings.get<Emulator::SettingHardwareRenderer>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(Enables the OpenGL GPU renderer.
This is not recommended for normal use at the moment,
as it is not fully implemented yet. It is recommended
to use the software renderer instead. Requires a restart
when changing this setting.)"));
        changed |= ImGui::Checkbox(_("2MB VRAM"), &settings.get<Emulator::Setting2MBVRAM>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(Emulates a console fitted with 2MB of VRAM
instead of the stock 1MB, exposing the upper bank through
GP1(09h). Found on a handful of modded consoles and on
some arcade boards. Requires a restart when changing
this setting.)"));

        ImGui::Separator();
        {
            int interval = settings.get<Emulator::SettingRewindInterval>().value;
            if (ImGui::SliderInt(_("Rewind capture interval (frames; 0 = disabled)"), &interval, 0, 60)) {
                settings.get<Emulator::SettingRewindInterval>().value = interval < 0 ? 0 : interval;
                changed = true;
            }
            int count = settings.get<Emulator::SettingRewindCount>().value;
            if (ImGui::SliderInt(_("Rewind history depth (snapshots)"), &count, 1, 600)) {
                settings.get<Emulator::SettingRewindCount>().value = count < 1 ? 1 : count;
                changed = true;
            }
            ImGuiHelpers::ShowHelpMarker(_(R"(Captures an in-memory save state every N frames
and keeps the last M of them; press Backspace (or
Emulation -> Rewind) to step backwards through them.
Each snapshot is currently a full state (~18MB), so
depth x 18MB is roughly the memory cost. Set the
interval to 0 to disable.)"));
        }

        if (memChanged) {
            changed = true;
            g_emulator->m_mem->setLuts();
        }

        {
            static const std::function<const char*()> types[] = {l_("Auto"), l_("NTSC"), l_("PAL")};
            auto& autodetect = settings.get<Emulator::SettingAutoVideo>().value;
            auto& type = settings.get<Emulator::SettingVideo>().value;
            if (ImGui::BeginCombo(_("System Type"), types[autodetect ? 0 : (type + 1)]())) {
                if (ImGui::Selectable(types[0](), autodetect)) {
                    changed = true;
                    autodetect = true;
                }
                if (ImGui::Selectable(types[1](), !autodetect && (type == PCSX::Emulator::PSX_TYPE_NTSC))) {
                    changed = true;
                    type = PCSX::Emulator::PSX_TYPE_NTSC;
                    autodetect = false;
                }
                if (ImGui::Selectable(types[2](), !autodetect && (type == PCSX::Emulator::PSX_TYPE_PAL))) {
                    changed = true;
                    type = PCSX::Emulator::PSX_TYPE_PAL;
                    autodetect = false;
                }
                ImGui::EndCombo();
            }
        }

        changed |= ImGui::Checkbox(_("Fast boot"), &settings.get<Emulator::SettingFastBoot>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(This will cause the BIOS to skip the shell,
which may include additional checks.
Also will make the boot time substantially
faster by not displaying the logo.)"));
        changed |=
            ImGui::Checkbox(_("Randomize CD-ROM timings"), &settings.get<Emulator::SettingCDRomEntropySeed>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(The CD-ROM drive timings carry a small
amount of jitter, like a real drive. By default
it is the same on every run, so a session can be
replayed exactly. Enabling this picks a different
sequence at each power-on.)"));
        auto bios = settings.get<Emulator::SettingBios>().string();
        ImGui::InputText(_("BIOS file"), const_cast<char*>(reinterpret_cast<const char*>(bios.c_str())),
                         bios.length() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        selectBiosDialog = ImGui::Button("...");
        auto directoryPicker = [&changed](const char* label, const char* id, std::filesystem::path& dir) {
            auto str = dir.u8string();
            const char* shown = dir.empty() ? _("(settings folder)") : reinterpret_cast<const char*>(str.c_str());
            // Label on its own line and the field sized to leave room, so the buttons stay visible in a
            // narrow window.
            const auto& style = ImGui::GetStyle();
            const char* reset = _("Reset");
            float buttons = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2 + style.ItemSpacing.x;
            if (!dir.empty()) buttons += ImGui::CalcTextSize(reset).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(-buttons);
            ImGui::InputText(fmt::format("##{}", id).c_str(), const_cast<char*>(shown), strlen(shown) + 1,
                             ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            bool open = ImGui::Button(fmt::format("...##{}", id).c_str());
            if (!dir.empty()) {
                ImGui::SameLine();
                if (ImGui::Button(fmt::format("{}##{}", reset, id).c_str())) {
                    dir.clear();
                    changed = true;
                }
            }
            return open;
        };
        selectScreenshotDirDialog = directoryPicker(_("Screenshot folder"), "ScreenshotDir",
                                                    settings.get<Emulator::SettingScreenshotDir>().value);
        selectSaveStateDirDialog = directoryPicker(_("Save state folder"), "SaveStateDir",
                                                   settings.get<Emulator::SettingSaveStateDir>().value);
        if (ImGui::Checkbox(_("Enable Debugger"), &debugSettings.get<Emulator::DebugSettings::Debug>().value)) {
            changed = true;
            if (debugSettings.get<PCSX::Emulator::DebugSettings::Debug>() && settings.get<Emulator::SettingDynarec>()) {
                showDynarecDebugWarning = true;
            }
        }

        ImGuiHelpers::ShowHelpMarker(_(R"(This will enable the usage of various breakpoints
throughout the execution of mips code. Enabling this
can slow down emulation to a noticeable extent.)"));
    }
    ImGui::End();

    {  // Select BIOS Dialog
        auto& biospath = settings.get<Emulator::SettingBiosBrowsePath>();
        if (selectBiosDialog) {
            if (!biospath.empty()) {
                m_selectBiosDialog.m_currentPath = biospath.value;
            }

            m_selectBiosDialog.openDialog();
        }
        if (m_selectBiosDialog.draw()) {
            biospath = m_selectBiosDialog.m_currentPath;
            std::vector<PCSX::u8string> fileToOpen = m_selectBiosDialog.selected();
            if (!fileToOpen.empty()) {
                settings.get<Emulator::SettingBios>().value = fileToOpen[0];
                changed = true;
            }
        }
    }
    auto directoryDialog = [&changed](auto& dialog, bool open, std::filesystem::path& dir) {
        if (open) {
            dialog.m_currentPath = dir.empty() ? g_system->getPersistentDir() : dir;
            dialog.openDialog();
        }
        if (dialog.draw()) {
            const auto& selected = dialog.selected();
            std::error_code ec;
            if (!selected.empty() && std::filesystem::is_directory(std::filesystem::path(selected[0]), ec)) {
                dir = selected[0];
                changed = true;
            }
        }
    };
    directoryDialog(m_selectScreenshotDirDialog, selectScreenshotDirDialog,
                    settings.get<Emulator::SettingScreenshotDir>().value);
    directoryDialog(m_selectSaveStateDirDialog, selectSaveStateDirDialog,
                    settings.get<Emulator::SettingSaveStateDir>().value);
    if (showDynarecDebugWarning && showDynarecWarning) {
        gui->addNotification(R"(Debugger and dynarec enabled at the same time.
Consider turning either one off, otherwise
debugging features may not work. Additionally,
changing the dynarec option requires a restart
of the emulator to take effect.)");
    } else if (showDynarecDebugWarning) {
        gui->addNotification(R"(Debugger and dynarec enabled at the same time.
Consider turning either one off, otherwise
debugging features may not work.)");
    } else if (showDynarecWarning) {
        gui->addNotification(R"(Toggling the Dynarec option requires a restart
of the emulator to take effect.)");
    }
    return changed;
}
