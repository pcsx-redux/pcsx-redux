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

#include "core/ui.h"

#include <uv.h>

#include <fstream>

#include "core/callstacks.h"
#include "core/debug.h"
#include "core/gpu.h"
#include "core/pad.h"
#include "core/psxemulator.h"
#include "core/spu.h"
#include "supportpsx/binloader.h"

PCSX::UI::UI() : m_listener(g_system->m_eventBus) {
    m_listener.listen<Events::ExecutionFlow::ShellReached>([this](const auto& event) { shellReached(); });
}

bool PCSX::UI::loadSettings() {
    std::ifstream cfg(g_system->getPersistentDir() / "pcsx.json");
    auto& emuSettings = g_emulator->settings;
    if (cfg.is_open() && !g_system->getArgs().isSafeModeEnabled()) {
        try {
            cfg >> m_settingsJson;
        } catch (...) {
        }
        if ((m_settingsJson.count("emulator") == 1) && m_settingsJson["emulator"].is_object()) {
            emuSettings.deserialize(m_settingsJson["emulator"]);
        }

        PCSX::g_emulator->m_spu->setCfg(m_settingsJson);
        PCSX::g_emulator->m_pads->setCfg(m_settingsJson);
        return true;
    } else {
        PCSX::g_emulator->m_pads->setDefaults();
        return false;
    }
}

void PCSX::UI::finishLoadSettings() {
    auto& emuSettings = g_emulator->settings;
    g_system->activateLocale(emuSettings.get<Emulator::SettingLocale>());
    g_system->m_eventBus->signal(Events::SettingsLoaded{g_system->getArgs().isSafeModeEnabled()});
}

void PCSX::UI::setLuaCommon(Lua L) {
    L.load(R"(
print("PCSX-Redux Lua Console")
-- `jit` exists only on LuaJIT. On the PUC-Lua backend it is nil, and reading
-- jit.version indexes a nil value, which killed this whole chunk at startup.
if jit and jit.status then
  print(jit.version)
  print((function(status, ...)
    local ret = "JIT: " .. (status and "ON" or "OFF")
    for i, v in ipairs({...}) do
      ret = ret .. " " .. v
    end
    return ret
  end)(jit.status()))
else
  print(_VERSION)
end
)",
           "ui startup");
}

void PCSX::UI::tick() {
    uv_run(g_system->getLoop(), UV_RUN_NOWAIT);
    auto L = *g_emulator->m_lua;
    L.getfield("AfterPollingCleanup", LUA_GLOBALSINDEX);
    if (!L.isnil()) {
        try {
            L.pcall();
        } catch (...) {
        }
        L.push();
        L.setfield("AfterPollingCleanup", LUA_GLOBALSINDEX);
    } else {
        L.pop();
    }
}

void PCSX::UI::shellReached() {
    auto& regs = g_emulator->m_cpu->m_regs;
    uint32_t oldPC = regs.pc;
    // The shell is only reached once per boot: m_shellStarted isn't part of a save state, so a
    // replay from a rewind snapshot taken before now would run straight into the shell, without
    // the hijack. When there is one, the older snapshots go and the ring starts again from here.
    const bool fastBoot = g_emulator->settings.get<Emulator::SettingFastBoot>();
    if (fastBoot) {
        g_emulator->m_cpu->setPC(regs.GPR.n.ra);
        // Enables display as some games like SaGa Frontier (USA)
        // rely on the side effect of the shell to do that for them.
        g_emulator->m_gpu->writeStatus(0x03000000);
    }

    if (m_exeToLoad.empty()) {
        if (fastBoot) g_emulator->restartRewind();
        return;
    }
    PCSX::u8string filename = m_exeToLoad.get();
    std::filesystem::path p = filename;

    g_system->log(LogClass::UI, "Hijacked shell, loading %s...\n", p.string());
    bool success = false;
    try {
        BinaryLoader::Info info;
        IO<File> in(new PosixFile(filename));
        if (in->failed()) {
            throw std::runtime_error("Failed to open file.");
        }
        success = BinaryLoader::load(in, g_emulator->m_mem->getMemoryAsFile(), info, g_emulator->m_cpu->m_symbols);
        // Writes through the memory file don't invalidate anything, and the shell we're hijacking
        // has already been compiled by the dynarec, so drop it before running the loaded binary.
        g_emulator->m_cpu->invalidateCache();
        if (!info.pc.has_value()) {
            throw std::runtime_error("Binary loaded without any PC to jump to.");
        }
        g_emulator->m_cpu->setPC(info.pc.value());
        if (info.sp.has_value()) regs.GPR.n.sp = info.sp.value();
        if (info.gp.has_value()) regs.GPR.n.gp = info.gp.value();
        if (g_emulator->settings.get<Emulator::SettingAutoVideo>() && info.region.has_value()) {
            switch (info.region.value()) {
                case BinaryLoader::Region::NTSC:
                    g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_NTSC;
                    break;
                case BinaryLoader::Region::PAL:
                    g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_PAL;
                    break;
            }
        }
    } catch (std::exception& e) {
        g_system->log(LogClass::UI, "Failed to load %s: %s\n", p.string(), e.what());
    } catch (...) {
        g_system->log(LogClass::UI, "Failed to load %s: unknown error\n", p.string());
    }
    if (success) {
        g_system->log(LogClass::UI, "Successful: new PC = %08x...\n", regs.pc);
    } else {
        g_system->log(LogClass::UI, "Failed to load %s, unknown file format.\n", p.string());
    }

    if (m_exeToLoad.hasToPause()) {
        g_system->pause();
    }

    if (oldPC != regs.pc) {
        g_emulator->m_callStacks->potentialRA(regs.pc, regs.GPR.n.sp);
        g_emulator->m_debug->updatedPC(regs.pc);
    }
    g_emulator->restartRewind();
}
