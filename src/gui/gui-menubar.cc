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

#include <magic_enum/magic_enum_all.hpp>
#include <string>
#include <type_traits>

#include "core/cdrom.h"
#include "core/gpu.h"
#include "core/gpulogger.h"
#include "core/pad.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/sio1.h"
#include "core/sstate.h"
#include "gui/gui.h"
#include "gui/shaders/crt-lottes.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "spu/interface.h"
#include "support/imgui-helpers.h"

bool PCSX::GUI::drawMainMenuBar(ImGuiContext* context, ImGuiID dockspaceId, bool& showOpenIsoFileDialog,
                                bool& showOpenBinaryDialog, bool& showOpenArchiveDialog) {
    bool changed = false;
    auto& debugSettings = g_emulator->settings.get<Emulator::SettingDebugSettings>();
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(_("File"))) {
            showOpenIsoFileDialog = ImGui::MenuItem(_("Open Disk Image"));
            auto currentIso = PCSX::g_emulator->m_cdrom->getIso();
            if (ImGui::MenuItem(_("Reload Disk Image"), nullptr, nullptr, currentIso && !currentIso->failed())) {
                PCSX::g_emulator->m_cdrom->clearIso();
                PCSX::g_emulator->m_cdrom->setIso(new CDRIso(currentIso->getIsoPath()));
                PCSX::g_emulator->m_cdrom->parseIso();
                g_system->hardReset();
            }
            if (ImGui::MenuItem(_("Close Disk Image"))) {
                PCSX::g_emulator->m_cdrom->setIso(new CDRIso(new FailedFile));
                PCSX::g_emulator->m_cdrom->parseIso();
            }
            if (ImGui::MenuItem(_("Load binary"))) {
                showOpenBinaryDialog = true;
            }
            if (ImGui::MenuItem(_("Add Lua archive"))) {
                showOpenArchiveDialog = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(_("Dump save state proto schema"))) {
                std::ofstream schema("sstate.proto");
                SaveStates::ProtoFile::dumpSchema(schema);
            }

            const auto globalSaveStateName = fmt::format(f_("global{}"), getSaveStatePostfix());

            if (ImGui::BeginMenu(_("Save state slots"))) {
                if (ImGui::MenuItem(_("Quick-save slot"), "F1")) {
                    saveSaveState(buildSaveStateFilename(0));
                }

                for (auto i = 1; i < 10; i++) {
                    const auto str = fmt::format(f_("Slot {}"), i);
                    if (ImGui::MenuItem(str.c_str())) {
                        saveSaveState(buildSaveStateFilename(i));
                    }
                }

                ImGui::Separator();
                ImGui::MenuItem(_("Show named save states"), nullptr, &m_namedSaveStates.m_show);

                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(_("Save global state"))) saveSaveState(globalSaveStateName);

            if (ImGui::BeginMenu(_("Load state slots"))) {
                auto saveFilename = buildSaveStateFilename(0);
                ImGui::BeginDisabled(!saveStateExists(saveFilename));
                if (ImGui::MenuItem(_("Quick-load slot"), "F2")) loadSaveState(saveFilename);
                ImGui::EndDisabled();

                for (auto i = 1; i < 10; i++) {
                    const auto str = fmt::format(f_("Slot {}"), i);
                    saveFilename = buildSaveStateFilename(i);
                    ImGui::BeginDisabled(!saveStateExists(saveFilename));
                    if (ImGui::MenuItem(str.c_str())) loadSaveState(saveFilename);
                    ImGui::EndDisabled();
                }

                auto namedSaves = m_namedSaveStates.getNamedSaveStates(this);
                if (!namedSaves.empty()) {
                    ImGui::Separator();
                    for (auto filenamePair : namedSaves) {
                        if (ImGui::MenuItem(filenamePair.second.c_str())) {
                            loadSaveState(filenamePair.first);
                        }
                    }
                }

                ImGui::EndMenu();
            }

            ImGui::BeginDisabled(!saveStateExists(globalSaveStateName));
            if (ImGui::MenuItem(_("Load global state"))) loadSaveState(globalSaveStateName);
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::MenuItem(_("Open LID"))) {
                PCSX::g_emulator->m_cdrom->openLid();
            }
            if (ImGui::MenuItem(_("Close LID"))) {
                PCSX::g_emulator->m_cdrom->closeLid();
            }
            if (ImGui::MenuItem(_("Open and close LID"))) {
                PCSX::g_emulator->m_cdrom->scheduleCloseLid();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(_("Reset settings..."))) {
                m_showResetSettings = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(_("Reboot"))) {
                g_system->quit(0x12eb007);
            }
            if (ImGui::MenuItem(_("Quit"))) {
                g_system->quit();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(_("Emulation"))) {
            if (ImGui::MenuItem(_("Start emulation"), "F5", nullptr, !g_system->running())) {
                g_system->resume();
            }
            if (ImGui::MenuItem(_("Pause emulation"), "F6", nullptr, g_system->running())) {
                g_system->pause();
            }
            if (ImGui::MenuItem(_("Soft Reset"), "F8")) {
                g_system->softReset();
            }
            if (ImGui::MenuItem(_("Hard Reset"), "Shift+F8")) {
                g_system->hardReset();
            }
            if (ImGui::MenuItem(_("Rewind"), "Backspace", nullptr, g_emulator->rewindStateCount() > 0)) {
                g_emulator->rewindState();
            }
            if (ImGui::MenuItem(_("Take Screenshot"), "F12")) {
                saveScreenShot();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(_("Configuration"))) {
            if (ImGui::MenuItem(_("Fullscreen"), nullptr, &m_fullscreen)) {
                setFullscreen(m_fullscreen);
                m_setupScreenSize = true;
            }
            if (ImGui::MenuItem(_("Full window render"), nullptr, &m_fullWindowRender)) {
                m_setupScreenSize = true;
                // full window render mode can't have anything docked in the dockspace
                ImGui::DockContextClearNodes(context, dockspaceId, true);
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Shader presets"))) {
                if (ImGui::MenuItem(_("Default shader"))) {
                    setDefaultShaders();
                }
                if (ImGui::MenuItem(_("CRT-lottes shader"))) {
                    m_offscreenShaderEditor.setText(Shaders::CrtLottes::Offscreen::vert().data(),
                                                    Shaders::CrtLottes::Offscreen::frag().data(),
                                                    Shaders::CrtLottes::Offscreen::lua().data());
                    auto offscreenStatus = m_offscreenShaderEditor.compile(this);
                    m_outputShaderEditor.setText(Shaders::CrtLottes::Output::vert().data(),
                                                 Shaders::CrtLottes::Output::frag().data(),
                                                 Shaders::CrtLottes::Output::lua().data());
                    auto outputStatus = m_outputShaderEditor.compile(this);

                    if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
                        m_offscreenShaderEditor.setFallbacks();
                        m_outputShaderEditor.setFallbacks();
                        offscreenStatus = m_offscreenShaderEditor.compile(this);
                        outputStatus = m_outputShaderEditor.compile(this);
                    }
                    if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
                        g_system->log(LogClass::UI,
                                      "Failed to compile output shaders. You won't be able to see anything");
                        g_system->log(LogClass::UI, "%s", offscreenStatus.getError());
                        g_system->log(LogClass::UI, "%s", outputStatus.getError());
                    }
                    m_offscreenShaderEditor.reset(this);
                    m_outputShaderEditor.reset(this);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(_("Configure Shaders"))) {
                m_offscreenShaderEditor.setConfigure();
                m_outputShaderEditor.setConfigure();
            }
            ImGui::Separator();
            ImGui::MenuItem(_("Controls"), nullptr, &g_emulator->m_pads->m_showCfg);
            if (ImGui::MenuItem(_("Manage Memory Cards"), nullptr, &m_memcardManager.m_show)) {
                m_memcardManager.m_frameCount = 0;  // Reset frame count when memcard manager is toggled
            }
            ImGui::Separator();
            ImGui::MenuItem(_("Emulation"), nullptr, &m_showCfg);
            ImGui::MenuItem(_("GPU"), nullptr, &PCSX::g_emulator->m_gpu->m_showCfg);
            ImGui::MenuItem(_("SPU"), nullptr, &PCSX::g_emulator->m_spu->m_showCfg);
            ImGui::MenuItem(_("PIO Cartridge"), nullptr, &m_pioCart.m_show);
            ImGui::MenuItem(_("Network"), nullptr, &m_network.m_show);
            ImGui::Separator();
            ImGui::MenuItem(_("UI"), nullptr, &m_showUiCfg);
            ImGui::MenuItem(_("System"), nullptr, &m_showSysCfg);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(_("Debug"))) {
            ImGui::MenuItem(_("Show Logs"), nullptr, &m_log.m_show);
            if (ImGui::BeginMenu("Lua")) {
                ImGui::MenuItem(_("Show Lua Console"), nullptr, &m_luaConsole.m_show);
                ImGui::MenuItem(_("Show Lua Inspector"), nullptr, &m_luaInspector.m_show);
                ImGui::MenuItem(_("Show Lua editor"), nullptr, &m_luaEditor.m_show);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("CPU"))) {
                ImGui::MenuItem(_("Show Registers"), nullptr, &m_registers.m_show);
                ImGui::MenuItem(_("Show Assembly"), nullptr, &m_assembly.m_show);
                if (PCSX::g_emulator->m_cpu->isDynarec()) {
                    ImGui::MenuItem(_("Show DynaRec Disassembly"), nullptr, &m_disassembly.m_show);
                } else {
                    ImGui::MenuItem(_("Show DynaRec Disassembly"), nullptr, false, false);
                    ImGuiHelpers::ShowHelpMarker(_(
                        R"(DynaRec Disassembler is not available in Interpreted CPU mode. Try enabling [Dynarec CPU]
in Configuration->Emulation, restart PCSX-Redux, then try again.)"));
                }
                ImGui::MenuItem(_("Show CPU Trace"), nullptr, &m_cpuTrace.m_show);
                ImGui::MenuItem(_("Show Hardware Events"), nullptr, &m_hwEvents.m_show);
                ImGui::MenuItem(_("Show Breakpoints"), nullptr, &m_breakpoints.m_show);
                ImGui::MenuItem(_("Show Callstacks"), nullptr, &m_callstacks.m_show);
                ImGui::MenuItem(_("Show Profiler"), nullptr, &m_callProfiler.m_show);
                if (ImGui::BeginMenu(_("Memory Editors"))) {
                    for (auto& editor : m_mainMemEditors) {
                        editor.MenuItem();
                    }
                    m_parallelPortEditor.MenuItem();
                    m_scratchPadEditor.MenuItem();
                    m_hwrEditor.MenuItem();
                    m_biosEditor.MenuItem();
                    m_vramEditor.MenuItem();
                    m_spuRamEditor.MenuItem();
                    ImGui::EndMenu();
                }
                ImGui::MenuItem(_("Show Memory Observer"), nullptr, &m_memoryObserver.m_show);
                ImGui::MenuItem(_("Show RAM viewer"), nullptr, &m_ramViewer.m_show);
                ImGui::MenuItem(_("Show MSAN Viewer"), nullptr, &m_msanViewer.m_show);
                ImGui::MenuItem(_("Show Typed Debugger"), nullptr, &m_typedDebugger.m_show);
                ImGui::MenuItem(_("Show Patches"), nullptr, &m_patches.m_show);
                ImGui::MenuItem(_("Show Interrupts Scaler"), nullptr, &m_showInterruptsScaler);
                if (ImGui::BeginMenu(_("First Chance Exceptions"))) {
                    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
                    constexpr auto& exceptions = magic_enum::enum_entries<PCSX::R3000Acpu::Exception>();
                    unsigned& s = debugSettings.get<Emulator::DebugSettings::FirstChanceException>().value;
                    for (auto& e : exceptions) {
                        unsigned f = 1 << static_cast<std::underlying_type<PCSX::R3000Acpu::Exception>::type>(e.first);
                        bool selected = s & f;
                        changed |= ImGui::MenuItem(std::string(e.second).c_str(), nullptr, &selected);
                        if (selected) {
                            s |= f;
                        } else {
                            s &= ~f;
                        }
                    }
                    ImGui::PopItemFlag();
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_("GPU"))) {
                if (ImGui::BeginMenu(_("VRAM viewers"))) {
                    ImGui::MenuItem(_("Show main VRAM viewer"), nullptr, &m_mainVRAMviewer.m_show);
                    ImGui::MenuItem(_("Show CLUT VRAM viewer"), nullptr, &m_clutVRAMviewer.m_show);
                    unsigned counter = 1;
                    for (auto& viewer : m_VRAMviewers) {
                        std::string title = _("Show VRAM viewer #") + std::to_string(counter);
                        ImGui::MenuItem(title.c_str(), nullptr, &viewer.m_show);
                        counter++;
                    }
                    ImGui::EndMenu();
                }
                ImGui::MenuItem(_("Show GPU logger"), nullptr, &m_gpuLogger.m_show);
                ImGui::MenuItem(_("Show GPU dump recorder / player"), nullptr, &m_gpuDump.m_show);
                ImGui::MenuItem(_("Show GPU debug"), nullptr, &PCSX::g_emulator->m_gpu->m_showDebug);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_("SPU"))) {
                ImGui::MenuItem(_("Show SPU debug"), nullptr, &PCSX::g_emulator->m_spu->m_showDebug);
                ImGui::MenuItem(_("Show SPU logger"), nullptr, &m_spuLoggerWidget.m_show);
                ImGui::MenuItem(_("Show SPU samples"), nullptr, &m_spuSamplesWidget.m_show);
                ImGui::EndMenu();
            }
            ImGui::MenuItem(_("Show Network"), nullptr, &m_network.m_show);
            if (ImGui::BeginMenu(_("CD-Rom"))) {
                ImGui::MenuItem(_("Show Iso Browser"), nullptr, &m_isoBrowser.m_show);
                ImGui::MenuItem(_("Show CD-ROM viewer"), nullptr, &m_cdromViewer.m_show);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_("Misc hardware"))) {
                ImGui::MenuItem(_("Show HW Registers"), nullptr, &m_hwRegs.m_show);
                ImGui::MenuItem(_("Show SIO1 debug"), nullptr, &m_sio1.m_show);
                ImGui::EndMenu();
            }
            ImGui::MenuItem(_("Show PSYQo heap viewer"), nullptr, &m_heapViewer.m_show);
            ImGui::MenuItem(_("Show PSYQo scope profiler"), nullptr, &m_scopeProfiler.m_show);
            ImGui::MenuItem(_("Show PocketStation LCD"), nullptr, &m_pocketStationLCD.m_show);
            ImGui::MenuItem(_("Show allocator tracker"), nullptr, &m_allocTrackerViewer.m_show);
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Kernel"))) {
                ImGui::MenuItem(_("Kernel Events"), nullptr, &m_events.m_show);
                ImGui::MenuItem(_("Kernel Handlers"), nullptr, &m_handlers.m_show);
                ImGui::MenuItem(_("Kernel Calls"), nullptr, &m_kernelLog.m_show);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_("Rendering"))) {
                ImGui::MenuItem(_("Show Output Shader Editor"), nullptr, &m_outputShaderEditor.m_show);
                ImGui::MenuItem(_("Show Offscreen Shader Editor"), nullptr, &m_offscreenShaderEditor.m_show);
                if (ImGui::MenuItem(_("Reset shaders"), nullptr)) {
                    setDefaultShaders();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(_("Dalos"))) {
            ImGui::MenuItem(_("Show Canvas"), nullptr, &m_dalos.m_show);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(_("Help"))) {
            ImGui::MenuItem(_("Show ImGui Demo"), nullptr, &m_showDemo);
            ImGui::MenuItem(_("Show ImPlot Demo"), nullptr, &m_showImPlotDemo);
            ImGui::Separator();
            ImGui::MenuItem(_("Show UvFile information"), nullptr, &m_showHandles);
            ImGui::Separator();
            ImGui::MenuItem(_("About"), nullptr, &m_showAbout);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::Separator();
        ImGui::Text(_("CPU: %s"), g_emulator->m_cpu->getName().c_str());
        ImGui::Separator();
        ImGui::Text(_("GAME ID: %s"), g_emulator->m_cdrom->getCDRomID().c_str());
        ImGui::Separator();
        if (g_system->running()) {
            ImGui::Text(_("%.2f FPS (%.2f ms)"), ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
            ImGui::Separator();
            ImGui::Text(_("%.2f guest FPS"), g_emulator->m_gpuLogger->getGuestFPS());
            ImGui::Separator();
            uint32_t frameCount = g_emulator->m_spu->getFrameCount();
            ImGui::Text(_("%.2f ms audio buffer (%i frames)"), 1000.0f * frameCount / 44100.0f, frameCount);
        } else {
            ImGui::TextUnformatted(C_("Audio status", "Idle"));
        }
        if (m_turboEnabled) {
            ImGui::Separator();
            ImGui::TextUnformatted(_("Turbo"));
        }

        ImGui::EndMainMenuBar();
    }
    return changed;
}

void PCSX::GUI::drawResetSettingsModal() {
    if (m_showResetSettings) {
        ImGui::OpenPopup(_("Reset settings"));
        m_showResetSettings = false;
    }
    if (ImGui::BeginPopupModal(_("Reset settings"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            _("This will restore every setting to its default value, including the window layout, the input "
              "bindings, the paths to the BIOS and the memory cards, and the contents of the shader "
              "editors.\n\nSave states and memory card contents are left alone.\n\nThe emulator will reboot to "
              "complete the operation."));
        ImGui::Separator();
        if (ImGui::Button(_("Reset and reboot"), ImVec2(160, 0))) {
            ImGui::CloseCurrentPopup();
            resetSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
