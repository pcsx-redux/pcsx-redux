/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include "gui/widgets/assembly.h"

#include <GL/gl3w.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>

#include "core/debug.h"
#include "core/disr3000a.h"
#include "core/patchmanager.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "support/gnu-c++-demangler.h"

static ImVec4 s_constantColor = ImColor(0x03, 0xda, 0xc6);
static ImVec4 s_invalidColor = ImColor(0xb0, 0x00, 0x20);
static ImVec4 s_labelColor = ImColor(0x01, 0x87, 0x86);

static std::string displaySymbol(const std::string& symbol) {
    auto& debugSettings = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>();
    if (debugSettings.get<PCSX::Emulator::DebugSettings::DemangledSymbols>()) {
        return PCSX::GNUDemangler::demangle(symbol);
    }
    return symbol;
}
static ImVec4 s_bpColor = ImColor(0xba, 0x00, 0x0d);
static ImVec4 s_currentColor = ImColor(0xff, 0xeb, 0x3b);
static ImVec4 s_arrowColor = ImColor(0x61, 0x61, 0x61);
static ImVec4 s_arrowOutlineColor = ImColor(0x37, 0x37, 0x37);

namespace {

// The disassembly view shows one physical memory region at a time, picked from a
// dropdown. Each region is a contiguous block backed by one of the Memory buffers;
// the displayed CPU address is reconstructed as segmentBase (kuser/kseg0/kseg1) +
// physBase + offset. This table is the single place the region layout is encoded -
// no separate forward/reverse address maps to keep in sync.
struct DisasmRegion {
    const char* label;
    uint32_t physBase;
    uint32_t size;
    uint8_t* PCSX::Memory::*buffer;
};

const DisasmRegion c_regions[] = {
    {"RAM", 0x00000000, 0x00800000, &PCSX::Memory::m_wram},
    {"EXP1", 0x1f000000, 0x00800000, &PCSX::Memory::m_exp1},
    {"SRAM", 0x1fa00000, 0x00200000, &PCSX::Memory::m_sram},
    {"BIOS", 0x1fc00000, 0x00080000, &PCSX::Memory::m_bios},
};
constexpr int c_regionCount = sizeof(c_regions) / sizeof(c_regions[0]);

// Index of the region containing a physical address (the CPU address with its
// segment bits masked off), or -1 if it lands outside every mapped region.
int regionForPhys(uint32_t phys) {
    for (int i = 0; i < c_regionCount; i++) {
        if (phys >= c_regions[i].physBase && phys < c_regions[i].physBase + c_regions[i].size) return i;
    }
    return -1;
}

void DButton(const char* label, bool enabled, std::function<void(void)> clicked) {
    if (!enabled) {
        const ImVec4 lolight = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImGui::PushStyleColor(ImGuiCol_Button, lolight);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lolight);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, lolight);
    }
    if (ImGui::Button(label) && enabled) clicked();
    if (!enabled) ImGui::PopStyleColor(3);
}

class DummyAsm : public PCSX::Disasm {
    virtual void Invalid() final {}
    virtual void OpCode(std::string_view str) final {}
    virtual void GPR(uint8_t reg) final {}
    virtual void CP0(uint8_t reg) final {}
    virtual void CP2C(uint8_t reg) final {}
    virtual void CP2D(uint8_t reg) final {}
    virtual void HI() final {}
    virtual void LO() final {}
    virtual void Imm16(int16_t value) final {}
    virtual void Imm16u(uint16_t value) final {}
    virtual void Imm32(uint32_t value) final {}
    virtual void Target(uint32_t value) final {}
    virtual void Sa(uint8_t value) final {}
    virtual void OfB(int16_t offset, uint8_t reg, int size) final {}
    virtual void BranchDest(uint32_t offset) final {}
    virtual void Offset(uint32_t offset, int size) final {}
    virtual void SyscallName(const char*) final {}
};

}  // namespace

uint8_t PCSX::Widgets::Assembly::mem8(uint32_t addr) { return *ptr(addr); }
uint16_t PCSX::Widgets::Assembly::mem16(uint32_t addr) { return SWAP_LE16(*(int16_t*)ptr(addr)); }
uint32_t PCSX::Widgets::Assembly::mem32(uint32_t addr) { return SWAP_LE32(*(int32_t*)ptr(addr)); }
void PCSX::Widgets::Assembly::sameLine() { ImGui::SameLine(0.0f, 0.0f); }
void PCSX::Widgets::Assembly::comma() {
    if (m_gotArg) {
        sameLine();
        ImGui::TextUnformatted(",");
    }
    m_gotArg = true;
}
void PCSX::Widgets::Assembly::Invalid() {
    m_gotArg = false;
    sameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, s_invalidColor);
    ImGui::TextUnformatted("(**invalid**)");
    ImGui::PopStyleColor();
}

void PCSX::Widgets::Assembly::OpCode(std::string_view str) {
    m_gotArg = false;
    sameLine();
    if (m_notch || m_notchAfterSkip[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted("~ ");
        ImGui::PopStyleColor();
        sameLine();
        ImGui::Text("%-6s", str.data());
    } else {
        ImGui::Text("%-8s", str.data());
    }
}
void PCSX::Widgets::Assembly::GPR(uint8_t reg) {
    comma();
    sameLine();
    ImGui::TextUnformatted(" $");
    sameLine();
    ImGui::TextUnformatted(s_disRNameGPR[reg]);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("$%s = %8.8x", s_disRNameGPR[reg], m_registers->GPR.r[reg]);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void PCSX::Widgets::Assembly::CP0(uint8_t reg) {
    comma();
    sameLine();
    ImGui::TextUnformatted(" $");
    sameLine();
    ImGui::TextUnformatted(s_disRNameCP0[reg]);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("$%s = %8.8x", s_disRNameCP0[reg], m_registers->CP0.r[reg]);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void PCSX::Widgets::Assembly::CP2C(uint8_t reg) {
    comma();
    sameLine();
    ImGui::TextUnformatted(" $");
    sameLine();
    ImGui::TextUnformatted(s_disRNameCP2C[reg]);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("$%s = %8.8x", s_disRNameCP2C[reg], m_registers->CP2C.r[reg]);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void PCSX::Widgets::Assembly::CP2D(uint8_t reg) {
    comma();
    sameLine();
    ImGui::TextUnformatted(" $");
    sameLine();
    ImGui::TextUnformatted(s_disRNameCP2D[reg]);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("$%s = %8.8x", s_disRNameCP2D[reg], m_registers->CP2D.r[reg]);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void PCSX::Widgets::Assembly::HI() {
    comma();
    sameLine();
    ImGui::TextUnformatted(" $hi");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("$hi = %8.8x", m_registers->GPR.n.hi);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void PCSX::Widgets::Assembly::LO() {
    comma();
    sameLine();
    ImGui::TextUnformatted(" $lo");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("$lo = %8.8x", m_registers->GPR.n.lo);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void PCSX::Widgets::Assembly::Imm16(int16_t value) {
    comma();
    sameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, s_constantColor);
    if (value < 0) {
        ImGui::Text(" -0x%4.4x", -value);
    } else {
        ImGui::Text(" 0x%4.4x", value);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("= %u", value);
        if (value < 0) {
            union {
                uint16_t x;
                int16_t y;
            } v;
            v.y = value;
            ImGui::Text("= 0x%4.4x", v.x);
            ImGui::Text("= -%i", -value);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor();
}
void PCSX::Widgets::Assembly::Imm16u(uint16_t value) {
    comma();
    sameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, s_constantColor);
    ImGui::Text(" 0x%4.4x", value);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("= %u", value);
        if (value >= 0x8000) {
            union {
                uint16_t x;
                int16_t y;
            } v;
            v.x = value;
            ImGui::Text("= -0x%4.4x", -v.y);
            ImGui::Text("= -%i", -v.y);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor();
}
void PCSX::Widgets::Assembly::Imm32(uint32_t value) {
    comma();
    sameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, s_constantColor);
    ImGui::Text(" 0x%8.8x", value);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::Text("= %u", value);
        if (value >= 0x80000000) {
            union {
                uint32_t x;
                int32_t y;
            } v;
            v.x = value;
            ImGui::Text("= -0x%8.8x", -v.y);
            ImGui::Text("= -%i", -v.y);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor();
}
void PCSX::Widgets::Assembly::Target(uint32_t value) {
    comma();
    sameLine();
    char label[21];
    ImGui::TextUnformatted("");
    ImGui::SameLine();
    if (m_displayArrowForJumps) m_arrows.push_back({m_currentAddr, value});
    std::snprintf(label, sizeof(label), "0x%8.8x##%8.8x", value, m_currentAddr);
    std::string longLabel = label;
    std::string* symbol = g_emulator->m_cpu->getSymbolAt(value);
    if (symbol) longLabel = displaySymbol(*symbol) + " ;" + label;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    if (ImGui::Button(longLabel.c_str())) {
        m_jumpToPC = value;
    }
    ImGui::PopStyleVar();
}
void PCSX::Widgets::Assembly::SyscallName(const char* name) {
    sameLine();
    ImGui::Text(" %s", name);
}
void PCSX::Widgets::Assembly::Sa(uint8_t value) {
    comma();
    sameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, s_constantColor);
    ImGui::Text(" 0x%2.2x", value);
    ImGui::PopStyleColor();
}
const uint8_t* PCSX::Widgets::Assembly::ptr(uint32_t addr) {
    auto ptr = static_cast<const uint8_t*>(m_memory->pointerRead(addr));
    if (ptr != nullptr) {
        return ptr;
    } else {
        static uint8_t dummy[4] = {0, 0, 0, 0};
        return dummy;
    }
}
void PCSX::Widgets::Assembly::jumpToMemory(uint32_t addr, unsigned size, unsigned editorIndex /* = 0*/) {
    g_system->m_eventBus->signal(PCSX::Events::GUI::JumpToMemory{addr, size, editorIndex});
}
void PCSX::Widgets::Assembly::addMemoryEditorContext(uint32_t addr, int size) {
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem(_("Go to in Memory Editor #1 (Default Click)"))) jumpToMemory(addr, size, 0);
        if (ImGui::MenuItem(_("Go to in Memory Editor #2 (Shift+Click)"))) jumpToMemory(addr, size, 1);
        if (ImGui::MenuItem(_("Go to in Memory Editor #3 (Ctrl+Click)"))) jumpToMemory(addr, size, 2);
        std::string itemLabel;
        for (unsigned i = 3; i < 8; ++i) {
            itemLabel = fmt::format(f_("Go to in Memory Editor #{}"), i + 1);
            if (ImGui::MenuItem(itemLabel.c_str())) jumpToMemory(addr, size, i);
        }
        if (ImGui::MenuItem(_("Create Memory Read Breakpoint"))) {
            g_emulator->m_debug->addBreakpoint(addr, Debug::BreakpointType::Read, size, _("GUI"));
        }
        if (ImGui::MenuItem(_("Create Memory Write Breakpoint"))) {
            g_emulator->m_debug->addBreakpoint(addr, Debug::BreakpointType::Write, size, _("GUI"));
        }
        ImGui::EndPopup();
    }
}
void PCSX::Widgets::Assembly::addMemoryEditorSubMenu(uint32_t addr, int size) {
    if (ImGui::BeginMenu(_("Go to in Memory Editor..."))) {
        std::string itemLabel;
        for (unsigned i = 0; i < 8; ++i) {
            itemLabel = fmt::format("#{}", i + 1);
            if (ImGui::MenuItem(itemLabel.c_str())) jumpToMemory(addr, size, i);
        }
        ImGui::EndMenu();
    }
}

void PCSX::Widgets::Assembly::OfB(int16_t offset, uint8_t reg, int size) {
    comma();
    sameLine();
    char label[32];
    if (offset < 0) {
        std::snprintf(label, sizeof(label), "-0x%4.4x($%s)##%08x", -offset, s_disRNameGPR[reg], m_currentAddr);
    } else {
        std::snprintf(label, sizeof(label), "0x%4.4x($%s)##%08x", offset, s_disRNameGPR[reg], m_currentAddr);
    }
    uint32_t addr = m_registers->GPR.r[reg] + offset;

    std::string longLabel;
    std::string* symbol = g_emulator->m_cpu->getSymbolAt(addr);
    if (symbol) longLabel = displaySymbol(*symbol) + " ; ";

    const auto& io = ImGui::GetIO();
    unsigned targetEditorIndex = io.KeyShift ? 1 : (io.KeyCtrl ? 2 : 0);

    ImGui::TextUnformatted(" ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    if (ImGui::Button(label)) jumpToMemory(addr, size, targetEditorIndex);
    ImGui::PopStyleVar();
    addMemoryEditorContext(addr, size);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        switch (size) {
            case 1:
                ImGui::Text("%s[%8.8x] = %2.2x", longLabel.c_str(), addr, mem8(addr));
                break;
            case 2:
                ImGui::Text("%s[%8.8x] = %4.4x", longLabel.c_str(), addr, mem16(addr));
                break;
            case 4:
                ImGui::Text("%s[%8.8x] = %8.8x", longLabel.c_str(), addr, mem32(addr));
                break;
        }
        ImGui::Text(_("Go to in Memory Editor #%d"), targetEditorIndex + 1);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void PCSX::Widgets::Assembly::BranchDest(uint32_t value) {
    comma();
    sameLine();
    char label[21];
    ImGui::TextUnformatted(" ");
    sameLine();
    m_arrows.push_back({m_currentAddr, value});
    std::snprintf(label, sizeof(label), "0x%8.8x##%8.8x", value, m_currentAddr);
    std::string* symbol = g_emulator->m_cpu->getSymbolAt(value);
    if (symbol) {
        std::string longLabel = displaySymbol(*symbol) + " ;" + label;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        if (ImGui::Button(longLabel.c_str())) {
            m_jumpToPC = value;
        }
        ImGui::PopStyleVar();
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        if (ImGui::Button(label)) {
            m_jumpToPC = value;
        }
        ImGui::PopStyleVar();
    }
}
void PCSX::Widgets::Assembly::Offset(uint32_t addr, int size) {
    comma();
    sameLine();
    char label[32];
    std::snprintf(label, sizeof(label), "0x%8.8x##%8.8x", addr, m_currentAddr);
    std::string longLabel = label;
    std::string* symbol = g_emulator->m_cpu->getSymbolAt(addr);
    if (symbol) longLabel = displaySymbol(*symbol) + " ;" + label;

    const auto& io = ImGui::GetIO();
    unsigned targetEditorIndex = io.KeyShift ? 1 : (io.KeyCtrl ? 2 : 0);

    ImGui::TextUnformatted(" ");
    sameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    if (ImGui::Button(longLabel.c_str())) jumpToMemory(addr, size, targetEditorIndex);
    ImGui::PopStyleVar();
    addMemoryEditorContext(addr, size);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        switch (size) {
            case 1:
                ImGui::Text("[%8.8x] = %2.2x", addr, mem8(addr));
                break;
            case 2:
                ImGui::Text("[%8.8x] = %4.4x", addr, mem16(addr));
                break;
            case 4:
                ImGui::Text("[%8.8x] = %8.8x", addr, mem32(addr));
                break;
        }
        ImGui::Text(_("Go to in Memory Editor #%u"), targetEditorIndex + 1);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool PCSX::Widgets::Assembly::draw(GUI* gui, psxRegisters* registers, Memory* memory, const char* title) {
    bool changed = false;
    auto& cpu = g_emulator->m_cpu;
    m_registers = registers;
    m_memory = memory;
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return changed;
    }

    float glyphWidth = ImGui::GetFontSize();

    bool openSymbolsDialog = false;

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(_("File"))) {
            openSymbolsDialog = ImGui::MenuItem(_("Load symbols map"));
            if (ImGui::MenuItem(_("Reset symbols map"))) {
                cpu->m_symbols.clear();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_("Debug"))) {
            if (ImGui::MenuItem(C_("Emulation", "Pause"), "F6", nullptr, g_system->running())) g_system->pause();
            if (ImGui::MenuItem(C_("Emulation", "Resume"), "F5", nullptr, !g_system->running())) g_system->resume();
            ImGui::Separator();
            if (ImGui::MenuItem(_("Step In"), "F11", nullptr, !g_system->running())) g_emulator->m_debug->stepIn();
            if (ImGui::MenuItem(_("Step Over"), "F10", nullptr, !g_system->running())) g_emulator->m_debug->stepOver();
            if (ImGui::MenuItem(_("Step Out"), "Shift+F11", nullptr, !g_system->running()))
                g_emulator->m_debug->stepOut();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_("Options"))) {
            ImGui::MenuItem(_("Combined pseudo-instructions"), nullptr, &m_pseudo);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(glyphWidth * 35.0f);
                ImGui::TextUnformatted(
                    _("When two instructions are detected to be a single pseudo-instruction, combine them into the "
                      "actual pseudo-instruction."));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::MenuItem(_("Pseudo-instructions filling"), nullptr, &m_pseudoFilling, m_pseudo);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(glyphWidth * 35.0f);
                ImGui::TextUnformatted(
                    _("When combining two instructions into a single pseudo-instruction, add a placeholder for the "
                      "second one."));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::MenuItem(_("Delay slot notch"), nullptr, &m_delaySlotNotch);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(glyphWidth * 35.0f);
                ImGui::TextUnformatted(
                    _("Add a small visible notch to indicate instructions that are on the delay slot of a branch."));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::MenuItem(_("Draw arrows for jumps"), nullptr, &m_displayArrowForJumps);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(glyphWidth * 35.0f);
                ImGui::TextUnformatted(_("Display arrows for jumps. This might crowd the display a bit too much."));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::SliderInt(_("Columns"), &m_numColumns, 0, 32);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    DummyAsm dummy;

    auto& debugSettings = g_emulator->settings.get<Emulator::SettingDebugSettings>();
    if (ImGui::Checkbox(_("Enable Debugger"), &debugSettings.get<Emulator::DebugSettings::Debug>().value)) {
        if (g_emulator->settings.get<Emulator::SettingDynarec>() &&
            debugSettings.get<Emulator::DebugSettings::Debug>()) {
            gui->addNotification(R"(Debugger and dynarec enabled at the same time.
Consider turning the dynarec off in the main Emulation
settings, otherwise debugging features may not work.)");
        }
    }
    ImGui::SameLine();
    // Same setting as the Capture checkbox in the CPU Trace window.
    ImGui::Checkbox(_("Capture CPU trace"), &debugSettings.get<Emulator::DebugSettings::Trace>().value);
    ImGui::SetItemTooltip(
        "%s", _("Records executed instructions into the CPU Trace window. Interpreter only; nothing is logged."));
    ImGui::SameLine();
    ImGui::Checkbox(_("Skip ISR"), &debugSettings.get<Emulator::DebugSettings::SkipISR>().value);
    ImGui::SameLine();
    ImGui::Checkbox(_("Demangle C++"), &debugSettings.get<Emulator::DebugSettings::DemangledSymbols>().value);
    ImGui::SameLine();
    ImGui::Checkbox(_("Follow PC"), &m_followPC);
    ImGui::SameLine();
    if (ImGui::Button(_("Jump to PC"))) {
        m_jumpToPC = m_registers->pc;
    }
    DButton(C_("Emulation", "Pause"), g_system->running(), [&]() mutable { g_system->pause(); });
    ImGui::SameLine();
    DButton(C_("Emulation", "Resume"), !g_system->running(), [&]() mutable { g_system->resume(); });
    ImGui::SameLine();
    DButton(_("Step In"), !g_system->running(), [&]() mutable { g_emulator->m_debug->stepIn(); });
    ImGui::SameLine();
    DButton(_("Step Over"), !g_system->running(), [&]() mutable { g_emulator->m_debug->stepOver(); });
    ImGui::SameLine();
    DButton(_("Step Out"), !g_system->running(), [&]() mutable { g_emulator->m_debug->stepOut(); });
    ImGui::SameLine();
    ImGui::Text(_("In ISR: %s"), cpu->m_inISR ? _("yes") : _("no"));

    gui->useMonoFont();

    ImGuiStyle& style = ImGui::GetStyle();
    const float heightSeparator = style.ItemSpacing.y;
    float footerHeight = 0;
    footerHeight += heightSeparator * 2 + ImGui::GetTextLineHeightWithSpacing();

    if (m_region < 0 || m_region >= c_regionCount) m_region = 0;
    const DisasmRegion& region = c_regions[m_region];
    uint8_t* const regionBuffer = m_memory->*region.buffer;
    const uint32_t pcPhys = m_registers->pc & 0x1fffffff;

    ImGui::BeginChild("##ScrollingRegion", ImVec2(0, -footerHeight), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(region.size / 4);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->ChannelsSplit(129);
    drawList->ChannelsSetCurrent(128);
    ImVec2 topleft = drawList->GetClipRectMin();
    ImVec2 bottomright = drawList->GetClipRectMax();

    char* endPtr;
    uint32_t jumpAddressValue = strtoul(m_jumpAddressString, &endPtr, 16);
    bool jumpAddressValid = *m_jumpAddressString && !*endPtr;

    std::map<uint32_t, ImVec2> linesStartPos;
    m_arrows.clear();

    bool openAssembler = false;
    bool openSymbolAdder = false;

    float lineHeight = ImGui::GetTextLineHeight();
    float previousSymbolY = topleft.y;
    std::optional<std::string> previousSymbol;

    while (clipper.Step()) {
        bool skipNext = false;
        bool delaySlotNext = false;
        typedef std::function<void(uint32_t, uint32_t)> prependType;
        auto process = [&](uint32_t offset, prependType prepend, PCSX::Disasm* disasm) {
            uint32_t code = 0;
            uint32_t nextCode = 0;
            if (offset + 4 <= region.size) {
                code = *reinterpret_cast<uint32_t*>(regionBuffer + offset);
                if (offset + 8 <= region.size) {
                    nextCode = *reinterpret_cast<uint32_t*>(regionBuffer + offset + 4);
                }
            }
            uint32_t dispAddr = m_ramBase + region.physBase + offset;
            prepend(code, dispAddr);
            disasm->process(code, nextCode, dispAddr, m_pseudo ? &skipNext : nullptr, &delaySlotNext);
            m_notch = delaySlotNext && m_delaySlotNotch;
            m_notchAfterSkip[1] = delaySlotNext && m_delaySlotNotch && m_pseudo && skipNext;
        };
        if (clipper.DisplayStart != 0) {
            uint32_t offset = clipper.DisplayStart * 4 - 4;
            process(offset, [](uint32_t, uint32_t) {}, &dummy);
        }
        auto& tree = g_emulator->m_debug->getTree();
        for (int x = clipper.DisplayStart; x < clipper.DisplayEnd; x++) {
            uint32_t offset = x * 4;
            const Debug::Breakpoint* currentBP = nullptr;
            prependType l = [&](uint32_t code, uint32_t dispAddr) mutable {
                bool hasBP = false;
                bool isBPEnabled = false;

                for (auto intersect = tree.find(dispAddr & ~0xe0000000, Debug::BreakpointTreeType::INTERVAL_SEARCH);
                     intersect != tree.end(); intersect++) {
                    if (intersect->type() == Debug::BreakpointType::Exec) {
                        hasBP = true;
                        isBPEnabled = intersect->enabled();
                        currentBP = &*intersect;
                        break;
                    }
                }

                m_currentAddr = dispAddr;
                uint8_t b[4];
                auto tc = [](uint8_t c) -> char {
                    if (c <= 0x20) return '.';
                    if (c >= 0x7f) return '.';
                    return c;
                };
                uint32_t tcode = code;
                b[0] = tcode & 0xff;
                tcode >>= 8;
                b[1] = tcode & 0xff;
                tcode >>= 8;
                b[2] = tcode & 0xff;
                tcode >>= 8;
                b[3] = tcode & 0xff;

                std::pair<const uint32_t, std::string>* symbol = cpu->findContainingSymbol(dispAddr);
                if (symbol) {
                    if (symbol->first == dispAddr) {
                        ImGui::PushStyleColor(ImGuiCol_Text, s_labelColor);
                        std::string displayName = displaySymbol(symbol->second);
                        ImGui::Text("%s:", displayName.c_str());
                        ImGui::PopStyleColor();
                    } else {
                        // if this is the first visible line and it's not a label itself, store the previous symbol
                        float y = ImGui::GetCursorScreenPos().y;
                        if (y + lineHeight >= topleft.y && y <= topleft.y + lineHeight) {
                            previousSymbol = displaySymbol(symbol->second);
                            // if the second visible line is a symbol, push the previous symbol display up
                            if (cpu->getSymbolAt(dispAddr + 4) && y < previousSymbolY) {
                                previousSymbolY = y;
                            }
                        }
                    }
                }

                for (int i = 0; i < m_numColumns * ImGui::GetWindowDpiScale(); i++) {
                    ImGui::TextUnformatted(" ");
                    ImGui::SameLine(0.0f, 0.0f);
                }

                ImVec2 pos = ImGui::GetCursorScreenPos();
                linesStartPos[dispAddr] = pos;

                if (jumpAddressValid && dispAddr == jumpAddressValue) {
                    const ImColor bgcolor = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                    float height = ImGui::GetTextLineHeight();
                    float width = glyphWidth * 64;
                    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bgcolor);
                }
                if (hasBP) {
                    float x = pos.x + ImGui::GetTextLineHeight() / 2;
                    float y = pos.y + glyphWidth / 2;
                    if (isBPEnabled) {
                        drawList->AddCircleFilled(ImVec2(x, y), glyphWidth / 2, ImColor(s_bpColor));
                    } else {
                        drawList->AddCircle(ImVec2(x, y), glyphWidth / 2, ImColor(s_bpColor), 20, 1.5f);
                    }
                }
                if (region.physBase + offset == pcPhys) {
                    /*
                       a
                    d  +
                    +--+\
                    |  | + c
                    +--+/e
                       +
                       b
                    */
                    ImVec2 a, b, c, d, e;
                    const float dist = glyphWidth / 2;
                    const float w2 = ImGui::GetTextLineHeight() / 4;
                    a.x = pos.x + dist;
                    a.y = pos.y;
                    b.x = pos.x + dist;
                    b.y = pos.y + ImGui::GetTextLineHeight();
                    c.x = pos.x + glyphWidth;
                    c.y = pos.y + ImGui::GetTextLineHeight() / 2;
                    d.x = pos.x;
                    d.y = pos.y + ImGui::GetTextLineHeight() / 2 - w2;
                    e.x = pos.x + dist;
                    e.y = pos.y + ImGui::GetTextLineHeight() / 2 + w2;
                    drawList->AddTriangleFilled(a, b, c, ImColor(s_currentColor));
                    drawList->AddRectFilled(d, e, ImColor(s_currentColor));
                }
                ImGui::Text("  %s:%8.8x %c%c%c%c %8.8x: ", region.label, dispAddr, tc(b[0]), tc(b[1]), tc(b[2]),
                            tc(b[3]), code);
                auto toggleBP = [&]() {
                    if (hasBP) {
                        if (currentBP->enabled()) {
                            currentBP->disable();
                        } else {
                            currentBP->enable();
                        }
                    } else {
                        g_emulator->m_debug->addBreakpoint(dispAddr, Debug::BreakpointType::Exec, 4, _("GUI"));
                    }
                };
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    toggleBP();
                }
                std::string contextMenuID = fmt::format("assembly address menu {}", dispAddr);
                if (ImGui::BeginPopupContextItem(contextMenuID.c_str())) {
                    if (symbol) {
                        if (ImGui::MenuItem(_("Remove symbol"))) {
                            cpu->m_symbols.erase(dispAddr);
                        }
                    } else {
                        if (ImGui::MenuItem(_("Create symbol here"))) {
                            openSymbolAdder = true;
                            m_symbolAddress = dispAddr;
                        }
                    }
                    if (ImGui::MenuItem(_("Copy Address"))) {
                        char fmtAddr[10];
                        std::snprintf(fmtAddr, sizeof(fmtAddr), "%8.8x", dispAddr);
                        ImGui::SetClipboardText(fmtAddr);
                    }
                    addMemoryEditorSubMenu(dispAddr, 4);
                    if (ImGui::MenuItem(_("Run to Cursor"), nullptr, false, !PCSX::g_system->running())) {
                        g_emulator->m_debug->addBreakpoint(
                            dispAddr, Debug::BreakpointType::Exec, 4, _("GUI"),
                            [](const Debug::Breakpoint* bp, uint32_t address, unsigned width, const char* cause) {
                                g_system->pause();
                                return false;
                            });
                        g_system->resume();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                        ImGui::TextUnformatted(
                            _("Run the emulation until the program counter reaches this address, then pause."));
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    if (ImGui::MenuItem(_("Jump to Cursor"), nullptr, false, !PCSX::g_system->running())) {
                        g_emulator->m_cpu->setPC(dispAddr);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                        ImGui::TextUnformatted(
                            _("Set the program counter to this address. This will skip over any instruction between "
                              "the current one and the one you're selecting. This can cause inconsistencies, so use "
                              "with caution."));
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    if (ImGui::MenuItem(_("Toggle Breakpoint"))) {
                        toggleBP();
                    }
                    if (hasBP) {
                        if (ImGui::MenuItem(_("Remove Breakpoint"))) {
                            g_emulator->m_debug->removeBreakpoint(currentBP);
                        }
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                        ImGui::TextUnformatted(_("Remove Breakpoint"));
                        ImGui::PopStyleColor();
                    }
                    if (region.physBase == 0) {
                        PatchManager& pm = *g_emulator->m_patchManager;
                        int patchIdx = pm.findPatch(dispAddr);
                        if (patchIdx == -1) {
                            if (ImGui::MenuItem(_("Patch in Return"))) {
                                pm.registerPatch(dispAddr, PatchManager::Patch::Type::Return);
                            }
                            if (ImGui::MenuItem(_("Patch in NOP"))) {
                                pm.registerPatch(dispAddr, PatchManager::Patch::Type::NOP);
                            }
                        } else {
                            PatchManager::Patch& patch = pm.getPatch(patchIdx);
                            switch (patch.type) {
                                case PatchManager::Patch::Type::Return:
                                    if (ImGui::MenuItem(_("Delete Return Patch"))) {
                                        pm.deletePatch(patchIdx);
                                    }
                                    break;

                                case PatchManager::Patch::Type::NOP:
                                    if (ImGui::MenuItem(_("Delete NOP Patch"))) {
                                        pm.deletePatch(patchIdx);
                                    }
                                    break;
                            }
                        }

                        if (ImGui::MenuItem(_("Assemble"))) {
                            openAssembler = true;
                            m_assembleAddress = dispAddr;
                            m_assembleStatus = "";
                        }
                    }
                    ImGui::EndPopup();
                }
                if (skipNext && m_pseudoFilling) {
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::TextUnformatted(" (pseudo)");
                    ImGui::PopStyleColor();
                }
            };
            m_notchAfterSkip[0] = m_notchAfterSkip[1];
            process(offset, l, this);
        }
    }
    std::sort(m_arrows.begin(), m_arrows.end(), [](const auto& a, const auto& b) -> bool {
        int64_t distA = static_cast<int64_t>(a.first) - static_cast<int64_t>(a.second);
        int64_t distB = static_cast<int64_t>(b.first) - static_cast<int64_t>(b.second);
        return distA > distB;
    });
    std::map<unsigned, decltype(m_arrows)> allocated;
    for (auto& arrowData : m_arrows) {
        unsigned column = 0;
        bool found = false;
        uint32_t top = arrowData.first;
        uint32_t bottom = arrowData.second;
        if (top > bottom) std::swap(top, bottom);
        while (!found) {
            found = true;
            for (auto& toCompare : allocated[column]) {
                uint32_t topCompare = toCompare.first;
                uint32_t bottomCompare = toCompare.second;
                if (topCompare > bottomCompare) std::swap(topCompare, bottomCompare);
                if (((top < topCompare) && (topCompare < bottom)) ||
                    ((top < bottomCompare) && (bottomCompare < bottom)) ||
                    ((topCompare < top) && (bottom < bottomCompare))) {
                    found = false;
                    break;
                }
            }
            if (found || (column == 63)) {
                allocated[column].push_back(arrowData);
                break;
            } else {
                column++;
            }
        }
    }
    for (auto& columnData : allocated) {
        unsigned column = columnData.first;
        for (auto& arrowData : columnData.second) {
            const float thickness = glyphWidth / 4;
            auto src = linesStartPos.find(arrowData.first);
            auto dst = linesStartPos.find(arrowData.second);
            float sx = src->second.x + ImGui::GetTextLineHeight() / 2;
            float sy = src->second.y + glyphWidth / 2;
            float columnX = sx - glyphWidth * (column + 1);
            float direction = arrowData.first < arrowData.second ? 1.0f : -1.0f;
            ImVec2 p0, p1, cp0, cp1;
            p0.x = sx + glyphWidth / 4;
            p0.y = sy;
            p1.x = columnX;
            p1.y = sy + direction * ImGui::GetTextLineHeight() / 2;
            cp0.x = p1.x - thickness;
            cp0.y = p0.y;
            cp1.x = columnX;
            cp1.y = p0.y - thickness * direction;
            drawList->ChannelsSetCurrent(1 + column * 2);
            drawList->AddBezierCubic(p0, cp0, cp1, p1, ImColor(s_arrowColor), thickness);
            drawList->ChannelsSetCurrent(0 + column * 2);
            drawList->AddBezierCubic(p0, cp0, cp1, p1, ImColor(s_arrowOutlineColor), thickness + 4);
            if (dst != linesStartPos.end()) {
                float dx = dst->second.x + ImGui::GetTextLineHeight() / 2;
                float dy = dst->second.y + glyphWidth / 2;
                p0.x = columnX;
                p0.y = dy - direction * ImGui::GetTextLineHeight() / 2;
                drawList->ChannelsSetCurrent(1 + column * 2);
                drawList->AddBezierCubic(p0, p1, p0, p1, ImColor(s_arrowColor), thickness);
                drawList->ChannelsSetCurrent(0 + column * 2);
                drawList->AddBezierCubic(p0, p1, p0, p1, ImColor(s_arrowOutlineColor), thickness + 4);
                p1.x = dx + glyphWidth / 4;
                p1.y = dy;
                cp1.x = p0.x - thickness;
                cp1.y = p1.y;
                cp0.x = columnX;
                cp0.y = p1.y + thickness * direction;
                p1.x -= thickness;
                drawList->ChannelsSetCurrent(1 + column * 2);
                drawList->AddBezierCubic(p0, cp0, cp1, p1, ImColor(s_arrowColor), thickness);
                drawList->ChannelsSetCurrent(0 + column * 2);
                drawList->AddBezierCubic(p0, cp0, cp1, p1, ImColor(s_arrowOutlineColor), thickness + 4);
                ImVec2 a, b, c;
                a = b = c = p1;
                a.x += thickness;
                b.x -= thickness;
                b.y -= thickness;
                c.x -= thickness;
                c.y += thickness;
                drawList->ChannelsSetCurrent(1 + column * 2);
                drawList->AddTriangleFilled(a, b, c, ImColor(s_arrowColor));
                a.x += 2;
                b.x -= 2;
                b.y -= 2;
                c.x -= 2;
                c.y += 2;
                drawList->ChannelsSetCurrent(0 + column * 2);
                drawList->AddTriangleFilled(a, b, c, ImColor(s_arrowOutlineColor));
            } else {
                float height = bottomright.y - topleft.y;
                ImVec2 out;
                out.x = p1.x;
                out.y = height * 2 * direction;
                drawList->ChannelsSetCurrent(1 + column * 2);
                drawList->AddBezierCubic(p1, out, p1, out, ImColor(s_arrowColor), thickness);
                drawList->ChannelsSetCurrent(0 + column * 2);
                drawList->AddBezierCubic(p1, out, p1, out, ImColor(s_arrowOutlineColor), thickness + 4);
            }
        }
    }
    drawList->ChannelsMerge();
    if (previousSymbol) {
        ImVec2 pos(ImGui::GetCursorScreenPos().x, previousSymbolY);
        ImVec2 posEnd(pos.x + glyphWidth * 64, pos.y + ImGui::GetTextLineHeight());
        drawList->AddRectFilled(pos, posEnd, ImGui::GetColorU32(ImGuiCol_WindowBg));
        std::string text = "^ " + previousSymbol.value() + " ^";
        drawList->AddText(pos, ImGui::GetColorU32(s_labelColor), text.data(), text.data() + text.size());
    }
    ImGui::EndChild();
    ImGui::PopFont();
    if (m_jumpToPC.has_value()) {
        std::snprintf(m_jumpAddressString, 19, "%08x", m_jumpToPC.value());
    }
    ImGui::PushItemWidth(10 * glyphWidth + style.FramePadding.x);
    if (ImGui::InputText(_("Address"), m_jumpAddressString, 20,
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        char* endPtr;
        uint32_t jumpAddress = strtoul(m_jumpAddressString, &endPtr, 16);
        if (*m_jumpAddressString && !*endPtr) {
            m_jumpToPC = jumpAddress;
        }
    }
    static const char* baseStrs[] = {"00000000", "80000000", "a0000000"};
    static const uint32_t baseValues[] = {0x00000000, 0x80000000, 0xa0000000};
    int base = 0;
    if (m_ramBase == 0x80000000) base = 1;
    if (m_ramBase == 0xa0000000) base = 2;
    ImGui::SameLine();
    if (ImGui::BeginCombo(_("RAM base"), baseStrs[base])) {
        for (int i = 0; i < 3; i++) {
            if (ImGui::Selectable(baseStrs[i], base == i)) {
                m_ramBase = baseValues[i];
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::BeginCombo(_("Region"), c_regions[m_region].label)) {
        for (int i = 0; i < c_regionCount; i++) {
            if (ImGui::Selectable(c_regions[i].label, m_region == i)) {
                m_region = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(_("Symbols"))) m_showSymbols = true;
    ImGui::PopItemWidth();
    ImGui::BeginChild("##ScrollingRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if ((m_followPC && (m_registers->pc != m_previousPC)) || m_jumpToPC.has_value()) {
        m_previousPC = m_registers->pc;
        uint32_t targetAddr = m_jumpToPC ? m_jumpToPC.value() : m_registers->pc;
        uint32_t targetPhys = targetAddr & 0x1fffffff;
        int targetRegion = regionForPhys(targetPhys);
        if (targetRegion >= 0) {
            uint32_t targetSeg = targetAddr & 0xe0000000;
            if (targetSeg != 0x80000000 && targetSeg != 0xa0000000) targetSeg = 0x00000000;
            m_region = targetRegion;
            m_ramBase = targetSeg;
            double row = (targetPhys - c_regions[targetRegion].physBase) / 4;
            double scroll_to_px = row * clipper.ItemsHeight;
            ImGui::SetScrollFromPosY(ImGui::GetCursorStartPos().y + scroll_to_px, 0.5f);
        }
        m_jumpToPC.reset();
    }
    ImGui::EndChild();
    if (openSymbolAdder) {
        ImGui::OpenPopup(_("Add symbol"));
    }
    if (ImGui::BeginPopupModal(_("Add symbol"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(_("Add symbol for address 0x%08x:"), m_symbolAddress);
        ImGui::InputText("##symbol", &m_addSymbolName);
        if (ImGui::Button(_("Add"))) {
            cpu->m_symbols[m_symbolAddress] = m_addSymbolName;
            m_addSymbolName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel"))) {
            m_addSymbolName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (openAssembler) {
        ImGui::OpenPopup(_("Assemble"));
    }
    if (ImGui::BeginPopupModal(_("Assemble"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(_("Assemble code for address 0x%08x:"), m_assembleAddress);
        gui->useMonoFont();
        ImGui::InputTextMultiline("##assemble", &m_assembleCode);
        ImGui::Text("%s", m_assembleStatus.c_str());
        ImGui::PopFont();
        if (ImGui::Button(_("Assemble"))) {
            auto L = *g_emulator->m_lua;
            std::string assembler = fmt::format(R"(
local success, msg = pcall(function() PCSX.Assembler.New():parse([[
{}
]]):compileToMemory(PCSX.getMemPtr(), 0x{:08x}, 0x{:08x}) end)
if not success then return msg else return nil end
)",
                                                m_assembleCode, m_assembleAddress, m_ramBase);
            L.load(assembler, "inline:assembler");
            if (L.isnil()) {
                m_assembleStatus.clear();
                // Invalidate iCache on successful assemble to ensure cached instructions are not executed
                g_emulator->m_cpu->invalidateCache();
            } else {
                m_assembleStatus = L.tostring();
            }
            L.pop();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Clear"))) m_assembleCode.clear();
        ImGui::SameLine();
        if (ImGui::Button(_("Close"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();

    auto& mapPath = g_emulator->settings.get<Emulator::SettingMapBrowsePath>();

    if (openSymbolsDialog) {
        if (!mapPath.empty()) {
            m_symbolsFileDialog.m_currentPath = mapPath.value;
        }
        m_symbolsFileDialog.openDialog();
    }
    if (m_symbolsFileDialog.draw()) {
        mapPath.value = m_symbolsFileDialog.m_currentPath;
        changed = true;
        std::vector<PCSX::u8string> filesToOpen = m_symbolsFileDialog.selected();
        for (auto fileName : filesToOpen) {
            std::ifstream file;
            // oh the irony
            file.open(reinterpret_cast<const char*>(fileName.c_str()));
            if (!file) continue;
            while (!file.eof()) {
                std::string addressString;
                std::string name;
                file >> addressString >> name;
                char* endPtr;
                uint32_t address = strtoul(addressString.c_str(), &endPtr, 16);
                bool addressValid = addressString[0] && !*endPtr;
                if (!addressValid) continue;
                cpu->m_symbols[address] = name;
            }
        }
    }

    if (m_showSymbols) {
        if (ImGui::Begin(_("Symbols"), &m_showSymbols)) {
            if (ImGui::Button(_("Refresh"))) rebuildSymbolsCache();
            ImGui::SameLine();
            ImGui::InputText(_("Filter"), &m_symbolFilter);
            ImGui::BeginChild("symbolsList");
            auto up = [](const std::string& in) -> std::string {
                std::string str = in;
                std::transform(str.begin(), str.end(), str.begin(), ::toupper);
                return str;
            };
            std::string filter = up(m_symbolFilter);
            bool empty = filter.empty();
            for (auto& symbol : m_symbolsCache) {
                std::string display = displaySymbol(symbol.first);
                int pos = up(display).find(filter);
                if (pos < 0) pos = up(symbol.first).find(filter);
                bool found = pos >= 0;
                if (empty || found) {
                    std::string label = fmt::format("{} - {:08x}", display, symbol.second);
                    std::string codeLabel = fmt::format(f_("Code##{}{:08x}"), symbol.first, symbol.second);
                    std::string dataLabel = fmt::format(f_("Data##{}{:08x}"), symbol.first, symbol.second);
                    if (ImGui::Button(codeLabel.c_str())) {
                        m_jumpToPC = symbol.second;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(dataLabel.c_str())) {
                        jumpToMemory(symbol.second, 1);
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(label.c_str());
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
    return changed;
}

void PCSX::Widgets::Assembly::rebuildSymbolsCache() {
    auto& cpu = g_emulator->m_cpu;
    m_symbolsCache.clear();
    for (auto& symbol : cpu->m_symbols) {
        m_symbolsCache.insert(std::pair(symbol.second, symbol.first));
    }
    m_symbolsCacheValid = true;
}
