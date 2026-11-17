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

#include "gui/widgets/updater.h"

#include <string>

#include "core/psxemulator.h"
#include "core/system.h"
#include "gui/gui.h"
#include "imgui.h"

void PCSX::Widgets::Updater::checkForUpdate() {
    m_update.downloadUpdateInfo(
        g_system->getVersion(),
        [this](bool success) {
            if (success) {
                m_updateAvailable = true;
            }
        },
        g_system->getLoop());
}

bool PCSX::Widgets::Updater::draw(GUI* gui) {
    bool changed = false;
    auto& emuSettings = g_emulator->settings;
    if (!g_system->getArgs().isUpdateDisabled() && !g_system->getVersion().failed() &&
        g_system->getVersion().hasUpdateInfo() && !emuSettings.get<Emulator::SettingShownAutoUpdateConfig>().value) {
        if (ImGui::Begin(_("Update configuration"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted((_(R"(PCSX-Redux can automatically update itself.

If you enable the auto update option, it will check for new updates
on startup. No personal data nor identifiable token is sent for this
process. The update server is run by the PCSX-Redux project behind
Cloudflare, which can see your IP address like any CDN.

If an update is available, you will get prompted to download and
install it. You can still download versions of PCSX-Redux as usual
from its website.

If you want to change this setting later, you can go to the
Configuration -> System menu.)")));
            if (ImGui::Button(_("Enable auto update"))) {
                emuSettings.get<Emulator::SettingShownAutoUpdateConfig>().value = true;
                emuSettings.get<Emulator::SettingAutoUpdate>().value = true;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(_("No thanks"))) {
                emuSettings.get<Emulator::SettingShownAutoUpdateConfig>().value = true;
                changed = true;
            }
        }
        ImGui::End();
    }

    if (m_updateAvailable) {
        if (ImGui::Begin(_("Update available"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_update.canFullyApply()) {
                ImGui::TextUnformatted((_(R"(An update is available.
Click 'Update' to download and apply the update.
PCSX-Redux will automatically restart to apply it.

Click 'Download' to use your browser to download
the update and manually apply it.)")));
            } else {
                ImGui::TextUnformatted((_(R"(An update is available.
Click 'Update' to download it. While the update can be
downloaded, it won't be applied automatically. You will
have to install it manually, the way you previously did.
PCSX-Redux will quit once the update is downloaded.

Click 'Download' to use your browser to download
the update and manually apply it.)")));
            }
            ImGui::ProgressBar(m_update.progress());
            if (!m_updateDownloading) {
                if (ImGui::Button(C_("Update dialog button", "Update"))) {
                    m_updateDownloading = true;
                    bool started = m_update.downloadAndApplyUpdate(
                        g_system->getVersion(),
                        [this, gui](bool success) {
                            m_updateAvailable = false;
                            m_updateDownloading = false;
                            if (success) {
                                m_update.applyUpdate(g_system->getBinDir());
                                g_system->quit();
                            } else {
                                gui->addNotice(_("An error has occured while downloading\nand/or applying the update."),
                                               GUI::NoticeKind::Error);
                            }
                        },
                        g_system->getLoop());
                    if (!started) {
                        gui->addNotice(_("An error has occured while downloading\nand/or applying the update."),
                                       GUI::NoticeKind::Error);
                        m_updateAvailable = false;
                        m_updateDownloading = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(C_("Update dialog button", "Download"))) {
                    m_updateDownloading = true;
                    bool started = m_update.getDownloadUrl(
                        g_system->getVersion(),
                        [this](std::string url) {
                            GUI::openUrl(url);
                            m_updateDownloading = false;
                        },
                        g_system->getLoop());
                    if (!started) {
                        gui->addNotice(_("An error has occured while downloading the update."), GUI::NoticeKind::Error);
                        m_updateAvailable = false;
                        m_updateDownloading = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(_("Cancel"))) {
                    m_updateAvailable = false;
                }
            }
        }
        ImGui::End();
    }
    return changed;
}
