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

#include "gui/widgets/ui-config.h"

#include <functional>
#include <string>

#include "core/psxemulator.h"
#include "core/system.h"
#include "gui/gui.h"
#include "imgui.h"
#include "support/imgui-helpers.h"

bool PCSX::Widgets::UIConfig::draw(GUI* gui) {
    bool changed = false;
    if (ImGui::Begin(_("UI Configuration"), &m_show)) {
        changed |= showThemes(gui);
        bool needFontReload = false;
        {
            std::string currentLocale = g_system->localeName();
            if (currentLocale.length() == 0) currentLocale = "English";
            if (ImGui::BeginCombo(_("Locale"), currentLocale.c_str())) {
                if (ImGui::Selectable("English", currentLocale == "English")) {
                    g_system->activateLocale("English");
                    g_emulator->settings.get<Emulator::SettingLocale>() = "English";
                    needFontReload = true;
                }
                for (auto& l : g_system->localesNames()) {
                    if (ImGui::Selectable(l.c_str(), currentLocale == l)) {
                        g_system->activateLocale(l);
                        g_emulator->settings.get<Emulator::SettingLocale>() = l;
                        needFontReload = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button(_("Reload locales"))) {
                g_system->loadAllLocales();
                g_system->activateLocale(currentLocale);
            }
        }
        needFontReload |= ImGui::SliderInt(_("Main Font Size"), &gui->settings.get<GUI::MainFontSize>().value, 8, 48);
        needFontReload |= ImGui::SliderInt(_("Mono Font Size"), &gui->settings.get<GUI::MonoFontSize>().value, 8, 48);
        bool ratioChanged =
            ImGui::Checkbox(_("Use Widescreen Aspect Ratio"), &gui->settings.get<GUI::WidescreenRatio>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(Sets the output screen ratio to 16:9 instead of 4:3.

Note that this setting is NOT the popular hack
to force games into widescreen mode, but rather an
emulation of the fact users could change their
TV sets to be in 16:9 mode instead of 4:3.

Some games have a "wide screen" rendering setting,
which changes the rendering of the game accordingly,
and requires the user to change the settings of
their TV set to match the aspect ratio of the game.)"));
        changed |= needFontReload | ratioChanged;
        if (needFontReload) gui->m_reloadFonts = true;
        if (ratioChanged) gui->m_setupScreenSize = true;
    }
    ImGui::End();
    return changed;
}

bool PCSX::Widgets::UIConfig::showThemes(GUI* gui) {
    static const std::function<const char*()> imgui_themes[] = {
        l_("Default theme##Theme name"), l_("Classic##Theme name"), l_("Light##Theme name"), l_("Cherry##Theme name"),
        l_("Mono##Theme name"),          l_("Dracula##Theme name"), l_("Olive##Theme name"),
    };
    auto changed = false;
    auto& currentTheme = g_emulator->settings.get<Emulator::SettingGUITheme>().value;

    if (ImGui::BeginCombo(_("Themes"), imgui_themes[currentTheme](), ImGuiComboFlags_HeightLarge)) {
        for (int n = 0; n < IM_ARRAYSIZE(imgui_themes); n++) {
            bool selected = (currentTheme == n);
            if (ImGui::Selectable(imgui_themes[n](), selected)) {
                currentTheme = n;
                changed = true;
                gui->applyTheme(n);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    return changed;
}

bool PCSX::Widgets::SystemConfig::draw() {
    bool changed = false;
    auto& emuSettings = g_emulator->settings;
    if (ImGui::Begin(_("System Configuration"), &m_show)) {
        changed |=
            ImGui::Checkbox(_("Preload Disk Image files"), &emuSettings.get<Emulator::SettingFullCaching>().value);
        changed |= ImGui::Checkbox(_("Enable Auto Update"), &emuSettings.get<Emulator::SettingAutoUpdate>().value);
    }
    ImGui::End();
    return changed;
}
