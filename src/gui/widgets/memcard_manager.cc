/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include "gui/widgets/memcard_manager.h"

#include <algorithm>
#include <cstdlib>

#include "core/pad.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "support/imgui-helpers.h"
#include "support/uvfile.h"

void PCSX::Widgets::MemcardManager::initTextures() {
    // Initialize the OpenGL textures used for the icon images
    // This must only be called when our OpenGL context is set up
    glGenTextures(15, m_iconTextures);
    for (int i = 0; i < 15; i++) {
        glBindTexture(GL_TEXTURE_2D, m_iconTextures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        if (!m_drawPocketstationIcons) {
            // Unlike VRAM, there is nowhere to move a 1555 unpack to here: these
            // are handed straight to ImGui::Image and sampled by ImGui's own
            // shader, which we do not own. So they become RGBA8 and the expansion
            // happens on the CPU at upload - 256 pixels, and only while the
            // memory card window is open.
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
        } else {
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32);
        }
    }
}

bool PCSX::Widgets::MemcardManager::draw(GUI* gui, const char* title) {
    bool changed = false;

    ImGui::SetNextWindowPos(ImVec2(600, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(title, &m_show, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return false;
    }

    bool showImportMemoryCardDialog = false;
    bool showExportMemoryCardDialog = false;

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(_("File"))) {
            if (ImGui::MenuItem(_("Import file into memory card 1"))) {
                showImportMemoryCardDialog = true;
                m_memoryCardImportExportIndex = 1;
            }
            if (ImGui::MenuItem(_("Import file into memory card 2"))) {
                showImportMemoryCardDialog = true;
                m_memoryCardImportExportIndex = 2;
            }
            if (ImGui::MenuItem(_("Export memory card 1 to file"))) {
                showExportMemoryCardDialog = true;
                m_memoryCardImportExportIndex = 1;
            }
            if (ImGui::MenuItem(_("Export memory card 2 to file"))) {
                showExportMemoryCardDialog = true;
                m_memoryCardImportExportIndex = 2;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (showImportMemoryCardDialog) {
        m_importMemoryCardDialog.openDialog();
    }

    if (m_importMemoryCardDialog.draw()) {
        std::vector<PCSX::u8string> fileToOpen = m_importMemoryCardDialog.selected();
        if (!fileToOpen.empty()) {
            g_emulator->m_sio->loadMcd(fileToOpen[0], m_memoryCardImportExportIndex);
            g_emulator->m_sio->saveMcd(m_memoryCardImportExportIndex);
            clearUndoBuffer();
        }
    }

    if (showExportMemoryCardDialog) {
        m_exportMemoryCardDialog.openDialog();
    }

    if (m_exportMemoryCardDialog.draw()) {
        std::vector<PCSX::u8string> fileToOpen = m_exportMemoryCardDialog.selected();
        if (!fileToOpen.empty()) {
            IO<File> out = new UvFile(fileToOpen[0], FileOps::TRUNCATE);
            if (!out->failed()) {
                const auto dataCard = g_emulator->m_sio->getMcdData(m_memoryCardImportExportIndex);
                Slice slice;
                slice.copy(dataCard, 128 * 1024);
                out->writeAt(std::move(slice), 0);
            }
        }
    }

    const bool undoDisabled = m_undo.size() == 0;
    if (undoDisabled) {
        ImGui::BeginDisabled();
    }
    bool isLatest = m_undo.size() == m_undoIndex;
    const bool wasLatest = isLatest;
    if (ImGui::SliderInt(_("Undo"), &m_undoIndex, 0, m_undo.size(), "")) {
        isLatest = m_undo.size() == m_undoIndex;
        const auto dataCard1 = g_emulator->m_sio->getMcdData(1);
        const auto dataCard2 = g_emulator->m_sio->getMcdData(2);
        if (isLatest) {
            std::memcpy(dataCard1, m_latest.get(), SIO::c_cardSize);
            std::memcpy(dataCard2, m_latest.get() + SIO::c_cardSize, SIO::c_cardSize);
        } else {
            if (wasLatest) {
                std::unique_ptr<uint8_t[]> latest = std::make_unique<uint8_t[]>(SIO::c_cardSize * 2);
                std::memcpy(latest.get(), dataCard1, SIO::c_cardSize);
                std::memcpy(latest.get() + SIO::c_cardSize, dataCard2, SIO::c_cardSize);
                m_latest.swap(latest);
            }
            std::memcpy(dataCard1, m_undo[m_undoIndex].second.get(), SIO::c_cardSize);
            std::memcpy(dataCard2, m_undo[m_undoIndex].second.get() + SIO::c_cardSize, SIO::c_cardSize);
        }
        g_emulator->m_sio->saveMcd(1);
        g_emulator->m_sio->saveMcd(2);
    }
    ImGui::TextUnformatted(_("Undo version:"));
    ImGui::SameLine();
    if (isLatest) {
        ImGui::TextUnformatted(_("Latest"));
    } else {
        ImGui::TextWrapped("%s", m_undo[m_undoIndex].first.c_str());
    }
    if (undoDisabled) {
        ImGui::EndDisabled();
    }
    if (ImGui::Button(_("Clear Undo buffer"))) {
        clearUndoBuffer();
    }

    // Insert or remove memory cards. Send a SIO IRQ to the emulator if this happens as well.
    if (ImGui::Checkbox(_("Memory Card 1 inserted"),
                        &g_emulator->settings.get<Emulator::SettingMcd1Inserted>().value)) {
        changed = true;
    }
    if (ImGui::Checkbox(_("Memory Card 2 inserted"),
                        &g_emulator->settings.get<Emulator::SettingMcd2Inserted>().value)) {
        changed = true;
    }

    // The cards in a multitap's slots B to D, for games only: the blocks below are cards 1 and 2.
    const auto drawTapCard = [&changed](const char* label, auto& inserted, auto& path) {
        if (ImGui::Checkbox(label, &inserted.value)) changed = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", reinterpret_cast<const char*>(path.string().c_str()));
    };
    auto& settings = g_emulator->settings;
    if (g_emulator->m_pads->hasMultitap(Pads::Port::Port1)) {
        drawTapCard(_("Port 1 tap slot B inserted"), settings.get<Emulator::SettingMcd1BInserted>(),
                    settings.get<Emulator::SettingMcd1B>());
        drawTapCard(_("Port 1 tap slot C inserted"), settings.get<Emulator::SettingMcd1CInserted>(),
                    settings.get<Emulator::SettingMcd1C>());
        drawTapCard(_("Port 1 tap slot D inserted"), settings.get<Emulator::SettingMcd1DInserted>(),
                    settings.get<Emulator::SettingMcd1D>());
    }
    if (g_emulator->m_pads->hasMultitap(Pads::Port::Port2)) {
        drawTapCard(_("Port 2 tap slot B inserted"), settings.get<Emulator::SettingMcd2BInserted>(),
                    settings.get<Emulator::SettingMcd2B>());
        drawTapCard(_("Port 2 tap slot C inserted"), settings.get<Emulator::SettingMcd2CInserted>(),
                    settings.get<Emulator::SettingMcd2C>());
        drawTapCard(_("Port 2 tap slot D inserted"), settings.get<Emulator::SettingMcd2DInserted>(),
                    settings.get<Emulator::SettingMcd2D>());
    }

    if (ImGui::Checkbox(_("Card 1 Pocketstation"),
                        &g_emulator->settings.get<Emulator::SettingMcd1Pocketstation>().value)) {
        g_emulator->m_sio->togglePocketstationMode();
        changed = true;
    }
    ImGuiHelpers::ShowHelpMarker(
        _("Experimental. Emulator will attempt to send artificial responses to Pocketstation commands, possibly "
          "allowing apps to be saved/exported."));
    if (ImGui::Checkbox(_("Card 2 Pocketstation"),
                        &g_emulator->settings.get<Emulator::SettingMcd2Pocketstation>().value)) {
        g_emulator->m_sio->togglePocketstationMode();
        changed = true;
    }
    ImGuiHelpers::ShowHelpMarker(
        _("Experimental. Emulator will attempt to send artificial responses to Pocketstation commands, possibly "
          "allowing apps to be saved/exported."));

    ImGui::SliderInt(_("Icon size"), &m_iconSize, 16, 512);
    if (ImGui::Checkbox(_("Draw Pocketstation icons"), &m_drawPocketstationIcons)) {
        glDeleteTextures(15, m_iconTextures);  // Recreate our textures to fit our new format
        initTextures();
    }

    static const auto draw = [this, gui](int card, int othercard) {
        static constexpr ImGuiTableFlags flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_SizingStretchProp;
        SIO::McdBlock block;  // The current memory card block we're looking into

        unsigned otherFreeSpace = g_emulator->m_sio->getFreeSpace(othercard);

        if (ImGui::BeginTable("Memory card information", 6, flags)) {
            ImGui::TableSetupColumn(_("Block number"));
            ImGui::TableSetupColumn(_("Icon"));
            ImGui::TableSetupColumn(_("Title"));
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn(_("Filename"));
            ImGui::TableSetupColumn(_("Action"));
            ImGui::TableHeadersRow();

            for (auto i = 1; i < 16; i++) {
                g_emulator->m_sio->getMcdBlockInfo(card, i, block);
                unsigned size = g_emulator->m_sio->getFileBlockCount(block);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", i);
                ImGui::TableSetColumnIndex(1);
                if (!block.isChained() && !block.isErased()) {
                    drawIcon(block);
                }

                ImGui::TableSetColumnIndex(2);
                if (block.isChained()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::TextUnformatted(_("Chained block"));
                    ImGui::PopStyleColor();
                    continue;
                } else if (block.isErased()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::TextUnformatted(_("Free block"));
                    ImGui::PopStyleColor();
                    continue;
                } else {
                    if (gui->hasJapanese()) {
                        ImGui::TextUnformatted(block.titleUtf8.c_str());
                    } else {
                        ImGui::TextUnformatted(block.titleAscii.c_str());
                    }
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(block.id.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text(_("%s (%dKB)"), block.name.c_str(), block.fileSize / 1024);
                ImGui::TableSetColumnIndex(5);

                // We have to suffix the action button names with ##number because Imgui
                // can't handle multiple buttons with the same name well
                auto buttonName = fmt::format(f_("Erase##{}"), i);
                if (ImGui::SmallButton(buttonName.c_str())) {
                    auto latest = getLatest();
                    g_emulator->m_sio->eraseMcdFile(block);
                    saveUndoBuffer(std::move(latest),
                                   fmt::format(f_("Erased file {}({}) off card {}"), block.number,
                                               gui->hasJapanese() ? block.titleUtf8 : block.titleAscii, block.mcd));
                    g_emulator->m_sio->saveMcd(card);
                }
                ImGui::SameLine();

                buttonName = fmt::format(f_("Copy##{}"), i);
                if (otherFreeSpace >= size) {
                    if (ImGui::SmallButton(buttonName.c_str())) {
                        auto latest = getLatest();
                        bool success = g_emulator->m_sio->copyMcdFile(block);
                        if (!success) {
                            gui->addNotification("Error while copying save file");
                        } else {
                            saveUndoBuffer(
                                std::move(latest),
                                fmt::format(f_("Copied file {}({}) from card {} to card {}"), block.number,
                                            gui->hasJapanese() ? block.titleUtf8 : block.titleAscii, card, othercard));
                            g_emulator->m_sio->saveMcd(othercard);
                        }
                    }
                } else {
                    ImGui::BeginDisabled();
                    ImGui::SmallButton(buttonName.c_str());
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();

                buttonName = fmt::format(f_("Move##{}"), i);
                if (otherFreeSpace >= size) {
                    if (ImGui::SmallButton(buttonName.c_str())) {
                        auto latest = getLatest();
                        bool success = g_emulator->m_sio->copyMcdFile(block);
                        if (!success) {
                            gui->addNotification("Error while copying save file");
                        } else {
                            g_emulator->m_sio->eraseMcdFile(block);
                            saveUndoBuffer(
                                std::move(latest),
                                fmt::format(f_("Moved file {}({}) from card {} to card {}"), block.number,
                                            gui->hasJapanese() ? block.titleUtf8 : block.titleAscii, card, othercard));
                            g_emulator->m_sio->saveMcd(1);
                            g_emulator->m_sio->saveMcd(2);
                        }
                    }
                } else {
                    ImGui::BeginDisabled();
                    ImGui::SmallButton(buttonName.c_str());
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();

                buttonName = fmt::format(f_("Export PNG##{}"), i);
                if (ImGui::SmallButton(buttonName.c_str())) {
                    exportPNG(block);
                }
                ImGui::SameLine();

                buttonName = fmt::format(f_("Copy icon##{}"), i);
                if (ImGui::SmallButton(buttonName.c_str())) {
                    copyToClipboard(block);
                }
            }
            ImGui::EndTable();
        }
    };

    if (ImGui::BeginTabBar("Cards")) {
        if (ImGui::BeginTabItem(_("Memory Card 1"))) {
            draw(1, 2);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(_("Memory Card 2"))) {
            draw(2, 1);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    m_frameCount = (m_frameCount + 1) % 60;
    ImGui::End();
    return changed;
}

void PCSX::Widgets::MemcardManager::drawIcon(const PCSX::SIO::McdBlock& block) {
    int currentFrame = 0;  // 1st frame = 0, 2nd frame = 1, 3rd frame = 2 and so on
    const auto texture = m_iconTextures[block.number - 1];
    glBindTexture(GL_TEXTURE_2D, texture);

    if (!m_drawPocketstationIcons) {
        const auto animationFrames = block.iconCount;
        // Check if we should display the 3rd frame, then check if we should display the 2nd one
        if (m_frameCount >= 40 && animationFrames == 3) {
            currentFrame = 2;
        } else if (m_frameCount >= 20 && animationFrames >= 2) {
            currentFrame = 1;
        }

        // Pointer to the current frame. Skip 16x16 pixels for each frame
        const auto icon = block.icon + (currentFrame * 16 * 16);
        uint32_t pixels[16 * 16];
        for (unsigned i = 0; i < 16 * 16; i++) {
            const uint16_t w = icon[i];
            // X1B5G5R5, the same word layout as VRAM. The 5-to-8 bit expansion
            // is bit replication, which is exact at both ends of the range.
            // Alpha stays the mask bit rather than becoming opaque: that is what
            // GL_UNSIGNED_SHORT_1_5_5_5_REV was feeding ImGui before, and these
            // are drawn with alpha blending on.
            const uint32_t r = (w >> 0) & 0x1f, g = (w >> 5) & 0x1f, b = (w >> 10) & 0x1f;
            pixels[i] = ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) |
                        ((w & 0x8000) ? 0xff000000u : 0u);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    } else {
        uint32_t pixels[32 * 32];
        getPocketstationIcon(pixels, block);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    ImGui::Image(texture, ImVec2(m_iconSize, m_iconSize));
}

// Extract the pocketstation icon from the block indicated by blockNumber into the pixels array (In RGBA8888)
void PCSX::Widgets::MemcardManager::getPocketstationIcon(uint32_t* pixels, const SIO::McdBlock& block) {
    const auto data = g_emulator->m_sio->getMcdData(block.mcd);
    const auto titleFrame = data + block.number * PCSX::SIO::c_blockSize;

    // Calculate icon offset using the header info documented here
    // https://psx-spx.consoledev.net/pocketstation/#pocketstation-file-headericons
    int iconOffset = 0x80 + (titleFrame[0x2] & 0xf) * 0x80;
    iconOffset += (titleFrame[0x57] * 8 + 0x7f) & ~0x7f;
    const auto icon = (uint32_t*)(titleFrame + iconOffset);

    int index = 0;
    for (auto scanline = 0; scanline < 32; scanline++) {
        auto line = icon[scanline];

        for (auto pixel = 0; pixel < 32; pixel++) {
            if ((line & 1) != 0) {
                pixels[index++] = 0xff000000;  // Black
            } else {
                pixels[index++] = 0xffffffff;  // White
            }

            line >>= 1;  // lsb = next pixel
        }
    }
}

clip::image PCSX::Widgets::MemcardManager::getIconRGBA8888(const SIO::McdBlock& block) {
    clip::image_spec spec;
    spec.bits_per_pixel = 32;
    spec.red_mask = 0xff;
    spec.green_mask = 0xff00;
    spec.blue_mask = 0xff0000;
    spec.alpha_mask = 0xff000000;
    spec.red_shift = 0;
    spec.green_shift = 8;
    spec.blue_shift = 16;
    spec.alpha_shift = 24;
    if (m_drawPocketstationIcons) {
        spec.width = 32;
        spec.height = 32;
        spec.bytes_per_row = spec.width * 4;
        clip::image ret(spec);
        getPocketstationIcon(reinterpret_cast<uint32_t*>(ret.data()), block);
        return ret;
    } else {  // PSX memcard icons - currently always dumps the 1st frame of the icon
        const auto toColor8 = [](uint8_t color5) {
            int color8 = (color5 << 3) | (color5 >> 2);
            return color8;
        };

        spec.width = 16;
        spec.height = 16;
        spec.bytes_per_row = spec.width * 4;
        clip::image ret(spec);
        uint32_t* pixels = reinterpret_cast<uint32_t*>(ret.data());
        for (auto i = 0; i < 16 * 16; i++) {       // Convert pixels from RGB555 to RGBA8888
            const uint16_t pixel = block.icon[i];  // Pixel in RGB555
            const int red = toColor8(pixel & 0x1f);
            const int green = toColor8((pixel >> 5) & 0x1f);
            const int blue = toColor8((pixel >> 10) & 0x1f);

            pixels[i] = 0xff000000 | (blue << 16) | (green << 8) | red;
        }
        return ret;
    }
}

void PCSX::Widgets::MemcardManager::exportPNG(const SIO::McdBlock& block) {
    const auto filename = fmt::format("icon{}.png", block.number);
    const auto pixels = getIconRGBA8888(block);
    pixels.export_to_png(filename);
}

void PCSX::Widgets::MemcardManager::copyToClipboard(const SIO::McdBlock& block) {
    const auto pixels = getIconRGBA8888(block);
    clip::set_image(pixels);
}

void PCSX::Widgets::MemcardManager::saveUndoBuffer(std::unique_ptr<uint8_t[]>&& tosave, const std::string& action) {
    m_undo.resize(m_undoIndex++);
    m_undo.push_back({action, std::move(tosave)});
}
