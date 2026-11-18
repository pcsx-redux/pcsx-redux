--   Copyright (C) 2026 PCSX-Redux authors
--
--   This program is free software; you can redistribute it and/or modify
--   it under the terms of the GNU General Public License as published by
--   the Free Software Foundation; either version 2 of the License, or
--   (at your option) any later version.
--
--   This program is distributed in the hope that it will be useful,
--   but WITHOUT ANY WARRANTY; without even the implied warranty of
--   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--   GNU General Public License for more details.
--
--   You should have received a copy of the GNU General Public License
--   along with this program; if not, write to the
--   Free Software Foundation, Inc.,
--   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
-- Boots from openbios.bin, which carries no symbols, then points the BIOS
-- setting at a file that does not exist and hard resets, so the BIOS reload
-- goes through the OpenBIOS fallback. The fallback must pick openbios.elf over
-- openbios.bin: only the elf brings in symbols, so _reset at the reset vector
-- and _boot prove which one got loaded. Exits with 0 on success, 1 when the
-- symbols are missing after the fallback, 2 when they were there before it.
local function hasSymbol(address, name)
    for a, n in PCSX.iterateSymbols() do if a == address and n == name then return true end end
    return false
end

if hasSymbol(0xbfc00000, '_reset') then
    print('_reset was already present before the fallback')
    PCSX.quit(2)
    return
end

PCSX.settings.emulator.Bios = 'src/mips/openbios/does-not-exist.bin'
PCSX.hardResetEmulator()

local vsyncs = 0
Listener = PCSX.Events.createEventListener('GPU::Vsync', function()
    vsyncs = vsyncs + 1
    if vsyncs < 2 then return end
    local reset = hasSymbol(0xbfc00000, '_reset')
    local boot = hasSymbol(0xbfc000d8, '_boot')
    print('_reset: ' .. tostring(reset) .. ', _boot: ' .. tostring(boot))
    PCSX.quit((reset and boot) and 0 or 1)
end)
