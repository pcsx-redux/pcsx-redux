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

// PCdrv network names on wasm: there is no socket layer, so every network open
// fails the way a disabled one does on desktop. See pcdrv-net.cc.

#ifdef __EMSCRIPTEN__

#include "core/r3000a.h"

PCSX::R3000Acpu::PCdrvFile::~PCdrvFile() {}

void PCSX::R3000Acpu::pcdrvOpenNet(const std::string& filename) {
    auto& regs = m_regs.GPR.n;
    regs.v0 = -1;
    regs.v1 = -1;
}

void PCSX::R3000Acpu::pcdrvReadNet(PCdrvFile& filei, IO<File>& memFile) {
    auto& regs = m_regs.GPR.n;
    regs.v0 = -1;
    regs.v1 = -1;
}

#endif
