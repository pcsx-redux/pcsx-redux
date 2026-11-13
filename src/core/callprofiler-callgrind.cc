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

#include "core/callprofiler.h"

// Not written yet. The output must be the callgrind format KCachegrind reads:
// https://valgrind.org/docs/manual/cl-format.html
// tests/pcsxrunner/profiler.cc, Profiler.CallgrindMatchesControl, pins what it has to say
// about src/mips/tests/profiler. Everything it needs is public on Profiler: samples() for
// the buckets, frames() to walk a bucket's call path, and symbolize() for names.
std::string PCSX::Profiler::serializeCallgrind() const { return {}; }
