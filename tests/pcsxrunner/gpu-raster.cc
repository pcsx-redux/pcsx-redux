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

#include "gtest/gtest.h"
#include "main/main.h"

// The gpu-raster phase suites characterize the software GPU against
// hardware-captured truth values. Each phase binary is built with
// PCSX_TESTS=1 so probes that depend on uninitialized-VRAM boot
// junk on real silicon (and therefore can't be reproduced under
// emulation) are skipped via CESTER_MAYBE_TEST; everything else
// asserts. The software GPU is the only renderer being verified
// here.

#define GPU_RASTER_TEST(name, dir)                                                                        \
    TEST(GPURaster, name) {                                                                               \
        MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",     \
                            "-interpreter", "-loadexe",                                                   \
                            "src/mips/tests/gpu/raster/" #dir "/gpu-raster-" #dir ".ps-exe");             \
        int ret = invoker.invoke();                                                                       \
        EXPECT_EQ(ret, 0);                                                                                \
    }

// The same phases with the soft GPU rasterizing on its worker thread. Every
// probe reads VRAM back through GPUREAD, so these also check that readbacks
// wait for the draws queued ahead of them.
#define GPU_RASTER_THREADED_TEST(name, dir)                                                               \
    TEST(GPURasterThreaded, name) {                                                                       \
        MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode",     \
                            "-interpreter", "-softgputhreaded", "-loadexe",                               \
                            "src/mips/tests/gpu/raster/" #dir "/gpu-raster-" #dir ".ps-exe");             \
        int ret = invoker.invoke();                                                                       \
        EXPECT_EQ(ret, 0);                                                                                \
    }

#define GPU_RASTER_BOTH(name, dir) \
    GPU_RASTER_TEST(name, dir)     \
    GPU_RASTER_THREADED_TEST(name, dir)

GPU_RASTER_BOTH(EdgesClipping, edges-clipping)
GPU_RASTER_BOTH(LineEndpointsMaskBit, line-endpoints-mask-bit)
GPU_RASTER_BOTH(EdgeWalkerStress, edge-walker-stress)
GPU_RASTER_BOTH(TextureBasic, texture-basic)
GPU_RASTER_BOTH(CrossPrimitiveMask, cross-primitive-mask)
GPU_RASTER_BOTH(EdgeWalkerPrecision, edge-walker-precision)
GPU_RASTER_BOTH(GouraudColor, gouraud-color)
GPU_RASTER_BOTH(QuadFlat, quad-flat)
GPU_RASTER_BOTH(TextureWindow815, texture-window-8-15)
GPU_RASTER_BOTH(Lines, lines)
GPU_RASTER_BOTH(Dither, dither)
GPU_RASTER_BOTH(AbrMatrix, abr-matrix)
GPU_RASTER_BOTH(TexRect, tex-rect)
GPU_RASTER_BOTH(CullThresholds, cull-thresholds)
GPU_RASTER_BOTH(TexWindowExhaustive, tex-window-exhaustive)
GPU_RASTER_BOTH(UvBoundary, uv-boundary)
GPU_RASTER_BOTH(Affine, affine)
GPU_RASTER_BOTH(StrideSweep, stride-sweep)
GPU_RASTER_BOTH(StrideSignCross, stride-sign-cross)
GPU_RASTER_BOTH(RowWalkDrift, row-walk-drift)
GPU_RASTER_BOTH(UvOverflow, uv-overflow)
GPU_RASTER_BOTH(SlantedGouraud, slanted-gouraud)
