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

#pragma once

#include <stdint.h>

#include <vector>

#include "supportpsx/fundamental.h"
#include "supportpsx/spusample.h"

namespace PCSX {

namespace SPU {

// Decodes a sample of sound RAM from its start through its loop-end block, with the filter
// history cleared as key-on does.
std::vector<int16_t> decodeSample(const uint8_t* spuRam, const SPUSample::Extent& extent);

// The fundamental of a sample of sound RAM at its stored rate, which is what SPU pitch 1000h
// plays: estimated over its loop, or past its attack if it does not loop.
Fundamental::Estimate sampleFundamental(const uint8_t* spuRam, const SPUSample::Extent& extent);

}  // namespace SPU

}  // namespace PCSX
