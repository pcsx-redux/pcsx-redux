/***************************************************************************
 *   Copyright (C) 2023 PCSX-Redux authors                                 *
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

#include "gui/widgets/named_savestates.h"

#include "core/cdrom.h"
#include "core/debug.h"
#include "core/system.h"
#include "gui/gui.h"
#include "imgui.h"
#include "imgui_internal.h"

void PCSX::Widgets::NamedSaveStates::draw(GUI* gui, const char* title) {
    ImGui::SetNextWindowPos(ImVec2(520, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    const float heightSeparator = style.ItemSpacing.y;
    const float footerHeight = (heightSeparator * 2 + ImGui::GetTextLineHeightWithSpacing()) * 4;
    const float glyphWidth = ImGui::GetFontSize();

    // Any ImGui::Text() preceeding a ImGui::InputText() will be incorrectly aligned, use this value to awkwardly
    // account for this Luckily this value is consistent no matter what the UI main font size is set to
    const float verticalAlignAdjust = 3.0f;

    // Create a button for each named save state
    ImGui::BeginChild("SaveStatesList", ImVec2(0, -footerHeight), true);
    auto saveStates = getNamedSaveStates(gui);
    for (const auto& saveStatePair : saveStates) {
        // The button is invisible so we can adjust the highlight separately
        if (ImGui::InvisibleButton(saveStatePair.second.c_str(),
                                   ImVec2(-FLT_MIN, glyphWidth + style.FramePadding.y * 2),
                                   ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_PressedOnDoubleClick)) {
            // Copy the name to the input box
            strcpy(m_namedSaveNameString, saveStatePair.second.c_str());
            // Load the save state if it was a double-click
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                loadSaveState(gui, saveStatePair.first);
            }
        }
        // Add a base coloured rect - highlight it if hovering over the button or the current InputText below matches it
        bool matches = StringsHelpers::strcasecmp(m_namedSaveNameString, saveStatePair.second.c_str());
        bool hovered = ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetCurrentContext()->LastItemData.Rect.Min, ImGui::GetCurrentContext()->LastItemData.Rect.Max,
            ImGui::ColorConvertFloat4ToU32(
                ImGui::GetStyle()
                    .Colors[matches ? ImGuiCol_HeaderActive : (hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header)]));
        // Finally the butotn text
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(ImGui::GetCurrentContext()->LastItemData.Rect.Min.x + style.FramePadding.y,
                   ImGui::GetCurrentContext()->LastItemData.Rect.Min.y + style.FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_Text), saveStatePair.second.c_str());
    }
    ImGui::EndChild();

    // Move the leading text down to align with the following InputText()
    float posY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(posY + verticalAlignAdjust);

    ImGui::TextUnformatted(_("Filename:"));
    ImGui::SameLine();
    ImGui::TextUnformatted(gui->getSaveStatePrefix(true).c_str());
    ImGui::SameLine(0.0f, 0.0f);

    // Restore the vertical value
    ImGui::SetCursorPosY(posY);

    // Ensure that we don't add invalid characters to the filename
    // This also filters on pasted text
    ImGui::InputTextWithHint("##SaveNameInput", _("Enter the name of your save state here"), m_namedSaveNameString,
                             NAMED_SAVE_STATE_LENGTH_MAX, ImGuiInputTextFlags_CallbackCharFilter,
                             TextFilters::filterNonPathCharacters);
    ImGui::SameLine(0.0f, 0.0f);

    // Trailing text alignment also needs adjusting, but in the opposite direction
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - verticalAlignAdjust);
    ImGui::TextUnformatted(gui->getSaveStatePostfix().c_str());

    ImGui::Separator();

    // Add various buttons based on whether a save state exists with that name
    auto found = std::find_if(saveStates.begin(), saveStates.end(), [=](auto saveStatePair) {
        return strlen(m_namedSaveNameString) > 0 &&
               StringsHelpers::strcasecmp(m_namedSaveNameString, saveStatePair.second.c_str());
    });
    bool exists = found != saveStates.end();

    float width = ImGui::GetCurrentContext()->LastItemData.Rect.GetWidth();
    ImVec2 buttonDims = ImVec2(-FLT_MIN, glyphWidth + style.FramePadding.y * 2);
    ImVec2 halfDims = ImVec2((width - (style.FramePadding.x * 6.0f)) * 0.5f, buttonDims.y);

    if (!exists) {
        if (strlen(m_namedSaveNameString) > 0) {
            // The save state doesn't exist, and the name is valid
            std::filesystem::path newPath = createSaveStatePath(gui, m_namedSaveNameString);
            if (ImGui::Button(_("Create save"), buttonDims)) {
                saveSaveState(gui, newPath);
            }
        }
    } else {
        // The save state exists
        if (ImGui::Button(_("Overwrite save"), halfDims)) {
            saveSaveState(gui, found->first);
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Load save"), halfDims)) {
            loadSaveState(gui, found->first);
        }
        // Add a deliberate spacer before the delete button
        // There is no delete confirmation, and this makes a mis-click less likely to hit it
        ImGui::Dummy(buttonDims);
        if (ImGui::Button(_("Delete save"), buttonDims)) {
            deleteSaveState(gui, found->first);
        }
    }

    ImGui::End();
}

std::vector<std::pair<std::filesystem::path, std::string>> PCSX::Widgets::NamedSaveStates::getNamedSaveStates(
    GUI* gui) {
    std::vector<std::pair<std::filesystem::path, std::string>> names;

    // Get the filename prefix to use, which follows the typical save state format, excluding the separator between
    // gameID and name
    std::string prefix = gui->getSaveStatePrefix(false);
    std::string postfix = gui->getSaveStatePostfix();

    // Loop the root directory
    std::error_code ec;
    std::filesystem::path save_path = gui->getSaveStateDir() / prefix;
    if (std::filesystem::exists(save_path, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(save_path, ec)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.rfind(postfix) == filename.length() - postfix.length()) {
                    std::string niceName = filename.substr(0, filename.length() - (postfix.length()));
                    // Only support names that fit within the character limit
                    if (niceName.length() < NAMED_SAVE_STATE_LENGTH_MAX) {
                        names.emplace_back(entry.path(), niceName);
                    }
                }
            }
        }
    }

    return names;
}

bool PCSX::Widgets::NamedSaveStates::saveSaveState(GUI* gui, std::filesystem::path saveStatePath) {
    g_system->log(LogClass::UI, "Saving named save state: %s\n", saveStatePath.filename().string().c_str());
    return gui->saveSaveState(saveStatePath);
}

bool PCSX::Widgets::NamedSaveStates::loadSaveState(GUI* gui, std::filesystem::path saveStatePath) {
    g_system->log(LogClass::UI, "Loading named save state: %s\n", saveStatePath.filename().string().c_str());
    return gui->loadSaveState(saveStatePath);
}

bool PCSX::Widgets::NamedSaveStates::deleteSaveState(GUI* gui, std::filesystem::path saveStatePath) {
    g_system->log(LogClass::UI, "Deleting named save state: %s\n", saveStatePath.filename().string().c_str());
    return gui->deleteSaveState(saveStatePath);
}

std::filesystem::path PCSX::Widgets::NamedSaveStates::createSaveStatePath(GUI* gui, std::string saveStateName) {
    // Check sub-folder for active game exists.
    std::error_code ec;
    std::string sub_folder = gui->getSaveStatePrefix(false);
    std::filesystem::path base_path = gui->getSaveStateDir() / sub_folder;
    if (!std::filesystem::exists(base_path, ec)) {
        if (!std::filesystem::create_directory(base_path, ec)) {
            if (ec) {
                g_system->log(LogClass::UI, "Failed to create save state folder '%s': %s\n", base_path.string().c_str(),
                              ec.message().c_str());
            }
        } else {
            g_system->log(LogClass::UI, "Created save state folder: %s\n", sub_folder.c_str());
        }
    }

    // Merge base_path with the potential save-state name.
    base_path = base_path / saveStateName;

    // Append file format.
    base_path += gui->getSaveStatePostfix();
    return base_path;
}

int PCSX::Widgets::NamedSaveStates::TextFilters::testChar(ImWchar c) {
    // Filter the core problematic characters for Windows and Linux
    // Anything remaining outside of [a-zA-Z0-9._-] is also allowed
    switch (c) {
        case '\\':
        case '/':
        case '<':
        case '>':
        case '|':
        case '\"':
        case ':':
        case '?':
        case '*':
        case 0:
            return 1;
    }
    return 0;
}
