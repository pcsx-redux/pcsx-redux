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

#include <filesystem>
#include <string>
#include <vector>

#include "core/gpu.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/system.h"
#include "gui/gui.h"
#include "imgui.h"
#include "spu/interface.h"
#include "support/file.h"

void PCSX::GUI::initMemoryEditors() {
    unsigned counter = 1;
    for (auto& editor : m_mainMemEditors) {
        editor.title = [counter, this]() {
            m_stringHolder = (_("Memory Editor #") + std::to_string(counter));
            return m_stringHolder.c_str();
        };
        counter++;
    }
    m_parallelPortEditor.title = l_("Parallel Port");
    m_scratchPadEditor.title = l_("Scratch Pad");
    m_hwrEditor.title = l_("Hardware Registers");
    m_biosEditor.title = []() { return "BIOS"; };
    m_vramEditor.title = l_("VRAM");
    m_spuRamEditor.title = l_("SPU RAM");
    auto makeExportFn = [this](MemoryEditorWrapper& wrapper, std::string postfixName) {
        return [this, &wrapper, postfixName](size_t len, size_t base_addr) {
            std::filesystem::path writeFilepath =
                g_system->getPersistentDir() / (getSaveStatePrefix(true) + "mem_" + postfixName + ".bin");
            IO<File> out(new PosixFile(writeFilepath.string(), FileOps::TRUNCATE));
            if (!out->failed()) {
                std::vector<uint8_t> buf(len);
                if (wrapper.editor.Cache.BulkReadFn) {
                    wrapper.editor.Cache.BulkReadFn(buf.data(), 0, len);
                }
                out->write(buf.data(), len);
                out->close();
                g_system->log(LogClass::UI, "Memory exported to: %s\n", writeFilepath.string().c_str());
            } else {
                g_system->log(LogClass::UI, "Failed to export memory to: %s\n", writeFilepath.string().c_str());
            }
        };
    };
    for (auto& editor : m_mainMemEditors) {
        editor.editor.ExportFn = makeExportFn(editor, "wram");
    }
    m_parallelPortEditor.editor.ExportFn = makeExportFn(m_parallelPortEditor, "parallel");
    m_scratchPadEditor.editor.ExportFn = makeExportFn(m_scratchPadEditor, "scratch");
    m_hwrEditor.editor.ExportFn = makeExportFn(m_hwrEditor, "hwr");
    m_biosEditor.editor.ExportFn = makeExportFn(m_biosEditor, "bios");
    m_vramEditor.editor.ExportFn = makeExportFn(m_vramEditor, "vram");
    m_spuRamEditor.editor.ExportFn = makeExportFn(m_spuRamEditor, "spuram");
}

void PCSX::GUI::drawMemoryEditors() {
    IO<File> memFile = g_emulator->m_mem->getMemoryAsFile();
    unsigned counter = 0;
    for (auto& editor : m_mainMemEditors) {
        if (editor.m_show) {
            ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
            editor.draw(memFile, 8 * 1024 * 1024);
        }
        counter++;
    }
    if (m_parallelPortEditor.m_show) {
        ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
        m_parallelPortEditor.draw(memFile, 512 * 1024);
    }
    counter++;
    if (m_scratchPadEditor.m_show) {
        ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
        m_scratchPadEditor.draw(memFile, 1024);
    }
    counter++;
    if (m_hwrEditor.m_show) {
        ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
        m_hwrEditor.draw(memFile, 8 * 1024);
    }
    counter++;
    if (m_biosEditor.m_show) {
        ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
        m_biosEditor.draw(memFile, 512 * 1024);
    }
    counter++;
    if (m_spuRamEditor.m_show) {
        ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
        // Null until the SPU has been init'd; the editor would otherwise read off a wild pointer.
        auto* spuRam = g_emulator->m_spu->getSpuRam();
        if (spuRam) m_spuRamEditor.draw(spuRam, SPUInterface::c_spuRamSize);
    }
    counter++;
    if (m_vramEditor.m_show) {
        ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);

        const Slice vram = g_emulator->m_gpu->getVRAM();
        auto* vramData = (const ImU8*)vram.data();
        size_t vramSize = vram.size();
        m_vramEditor.editor.ReadFn = [vramData](size_t off) -> ImU8 { return vramData[off]; };
        m_vramEditor.editor.WriteFn = [vramData](size_t off, ImU8 writtenByte) {
            constexpr size_t vramWidth = 1024;
            constexpr size_t stride = vramWidth * sizeof(uint16_t);

            const auto x = (off % stride) / sizeof(uint16_t);
            const auto y = off / stride;
            const bool offsetIsOdd = (off & 1) == 1;
            const auto maskedOffset = off & ~(size_t)1;
            uint16_t newPixel;

            if (offsetIsOdd) {
                newPixel = (writtenByte << 8) | vramData[maskedOffset];
            } else {
                newPixel = writtenByte | (vramData[maskedOffset + 1] << 8);
            }

            g_emulator->m_gpu->partialUpdateVRAM(x, y, 1, 1, &newPixel);
        };
        m_vramEditor.editor.Cache.BulkReadFn = [vramData](void* dest, size_t off, size_t len) {
            memcpy(dest, vramData + off, len);
        };
        m_vramEditor.editor.DrawWindow(m_vramEditor.title(), vramSize);
    }
}
