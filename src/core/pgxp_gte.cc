/***************************************************************************
 *   Copyright (C) 2016 by iCatButler                                      *
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

/**************************************************************************
 *  pgxp_gte.c
 *  PGXP - Parallel/Precision Geometry Xform Pipeline
 *
 *  Created on: 12 Mar 2016
 *      Author: iCatButler
 ***************************************************************************/

#include "core/pgxp_gte.h"

#include "core/gpu.h"
#include "core/pgxp_cpu.h"
#include "core/pgxp_debug.h"
#include "core/pgxp_mem.h"
#include "core/pgxp_value.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"

#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

// GTE registers
static PGXP_value s_GTE_data_reg_mem[32];
static PGXP_value s_GTE_ctrl_reg_mem[32];

PGXP_value* g_GTE_data_reg = s_GTE_data_reg_mem;
PGXP_value* g_GTE_ctrl_reg = s_GTE_ctrl_reg_mem;

void PGXP_InitGTE() {
    memset(s_GTE_data_reg_mem, 0, sizeof(s_GTE_data_reg_mem));
    memset(s_GTE_ctrl_reg_mem, 0, sizeof(s_GTE_ctrl_reg_mem));
}

// Instruction register decoding
#define op(_instr) (_instr >> 26)           // The op part of the instruction register
#define func(_instr) ((_instr) & 0x3F)      // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F)   // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F)  // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F)  // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F)  // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF)       // The immediate part of the instruction register

#define SX0 (g_GTE_data_reg[12].x)
#define SY0 (g_GTE_data_reg[12].y)
#define SX1 (g_GTE_data_reg[13].x)
#define SY1 (g_GTE_data_reg[13].y)
#define SX2 (g_GTE_data_reg[14].x)
#define SY2 (g_GTE_data_reg[14].y)

#define SXY0 (g_GTE_data_reg[12])
#define SXY1 (g_GTE_data_reg[13])
#define SXY2 (g_GTE_data_reg[14])
#define SXYP (g_GTE_data_reg[15])

void PGXP_pushSXYZ2f(float _x, float _y, float _z, unsigned int _v) {
    static unsigned int uCount = 0;
    low_value temp;
    // push values down FIFO
    SXY0 = SXY1;
    SXY1 = SXY2;

    SXY2.x = _x;
    SXY2.y = _y;
    SXY2.z = PCSX::g_emulator->config().PGXP_Texture ? _z : 1.f;
    SXY2.value = _v;
    SXY2.flags = VALID_ALL;
    SXY2.count = uCount++;

    // cache value in GPU plugin
    temp.word = _v;
    if (PCSX::g_emulator->config().PGXP_Cache) {
        PCSX::g_emulator->m_gpu->pgxpCacheVertex(temp.x, temp.y, reinterpret_cast<unsigned char*>(&SXY2));
    } else {
        PCSX::g_emulator->m_gpu->pgxpCacheVertex(0, 0, NULL);
    }

}

void PGXP_pushSXYZ2s(int64_t _x, int64_t _y, int64_t _z, uint32_t v) {
    float fx = (float)(_x) / (float)(1 << 16);
    float fy = (float)(_y) / (float)(1 << 16);
    float fz = (float)(_z);

    if (PCSX::g_emulator->config().PGXP_GTE) PGXP_pushSXYZ2f(fx, fy, fz, v);
}

#define VX(n) (PCSX::g_emulator->m_cpu->m_regs.CP2D.p[n << 1].sw.l)
#define VY(n) (PCSX::g_emulator->m_cpu->m_regs.CP2D.p[n << 1].sw.h)
#define VZ(n) (PCSX::g_emulator->m_cpu->m_regs.CP2D.p[(n << 1) + 1].sw.l)

void PGXP_RTPS(uint32_t _n, uint32_t _v) {
    // Transform
    float TRX = (int64_t)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[5].sd;
    float TRY = (int64_t)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[6].sd;
    float TRZ = (int64_t)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[7].sd;

    // Rotation with 12-bit shift
    float R11 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[0].sw.l / (float)(1 << 12);
    float R12 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[0].sw.h / (float)(1 << 12);
    float R13 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[1].sw.l / (float)(1 << 12);
    float R21 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[1].sw.h / (float)(1 << 12);
    float R22 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[2].sw.l / (float)(1 << 12);
    float R23 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[2].sw.h / (float)(1 << 12);
    float R31 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[3].sw.l / (float)(1 << 12);
    float R32 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[3].sw.h / (float)(1 << 12);
    float R33 = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[4].sw.l / (float)(1 << 12);

    // Bring vertex into view space
    float MAC1 = TRX + (R11 * VX(_n)) + (R12 * VY(_n)) + (R13 * VZ(_n));
    float MAC2 = TRY + (R21 * VX(_n)) + (R22 * VY(_n)) + (R23 * VZ(_n));
    float MAC3 = TRZ + (R31 * VX(_n)) + (R32 * VY(_n)) + (R33 * VZ(_n));

    float IR1 = max(min(MAC1, 0x7fff), -0x8000);
    float IR2 = max(min(MAC2, 0x7fff), -0x8000);
    float IR3 = max(min(MAC3, 0x7fff), -0x8000);

    float H = PCSX::g_emulator->m_cpu->m_regs.CP2C.p[26].sw.l;  // Near plane
    float F = 0xFFFF;                                           // Far plane?
    float SZ3 = max(min(MAC3, 0xffff), 0x0000);  // Clamp SZ3 to near plane because we have no clipping (no proper Z)
    //  float h_over_sz3 = H / SZ3;

    // Offsets with 16-bit shift
    float OFX = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[24].sd / (float)(1 << 16);
    float OFY = (float)PCSX::g_emulator->m_cpu->m_regs.CP2C.p[25].sd / (float)(1 << 16);

    float h_over_w = min(H / SZ3, (float)0x1ffff / (float)0xffff);
    h_over_w = (SZ3 == 0) ? ((float)0x1ffff / (float)0xffff) : h_over_w;

    // PSX Screen space X,Y,W components
    float sx = OFX + (IR1 * h_over_w) * (PCSX::g_emulator->config().Widescreen ? 0.75 : 1);
    float sy = OFY + (IR2 * h_over_w);
    float sw = SZ3;  // max(SZ3, 0.1);

    sx = max(min(sx, 1024.f), -1024.f);
    sy = max(min(sy, 1024.f), -1024.f);

    // float sx2 = SX2;
    // float sy2 = SY2;
    // float sz2 = SXY2.z;

    // float ftolerance = 5.f;

    // if ((fabs(sx - sx2) > ftolerance) ||
    //  (fabs(sy - sy2) > ftolerance) ||
    //  (fabs(sw - sz2) > ftolerance))
    //{
    //  float r = 5;
    //}

    PGXP_pushSXYZ2f(sx, sy, sw, _v);

    return;
}

int PGXP_NLCIP_valid(uint32_t sxy0, uint32_t sxy1, uint32_t sxy2) {
    Validate(&SXY0, sxy0);
    Validate(&SXY1, sxy1);
    Validate(&SXY2, sxy2);
    if (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_012) == VALID_012) && PCSX::g_emulator->config().PGXP_GTE &&
        (PCSX::g_emulator->config().PGXP_Mode > 0))
        return 1;
    return 0;
}

float PGXP_NCLIP() {
    float nclip = ((SX0 * SY1) + (SX1 * SY2) + (SX2 * SY0) - (SX0 * SY2) - (SX1 * SY0) - (SX2 * SY1));

    // ensure fractional values are not incorrectly rounded to 0
    float nclipAbs = fabs(nclip);
    if ((0.1f < nclipAbs) && (nclipAbs < 1.f)) nclip += (nclip < 0.f ? -1 : 1);

    // float AX = SX1 - SX0;
    // float AY = SY1 - SY0;

    // float BX = SX2 - SX0;
    // float BY = SY2 - SY0;

    //// normalise A and B
    // float mA = sqrt((AX*AX) + (AY*AY));
    // float mB = sqrt((BX*BX) + (BY*BY));

    //// calculate AxB to get Z component of C
    // float CZ = ((AX * BY) - (AY * BX)) * (1 << 12);

    return nclip;
}

static PGXP_value PGXP_MFC2_int(uint32_t reg) {
    switch (reg) {
        case 15:
            g_GTE_data_reg[reg] = SXYP = SXY2;
            break;
    }

    return g_GTE_data_reg[reg];
}

static void PGXP_MTC2_int(PGXP_value value, uint32_t reg) {
    switch (reg) {
        case 15:
            // push FIFO
            SXY0 = SXY1;
            SXY1 = SXY2;
            SXY2 = value;
            SXYP = SXY2;
            break;

        case 31:
            return;
    }

    g_GTE_data_reg[reg] = value;
}

////////////////////////////////////
// Data transfer tracking
////////////////////////////////////

void MFC2(int reg) {
    psx_value val;
    val.d = g_GTE_data_reg[reg].value;
    switch (reg) {
        case 1:
        case 3:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
            g_GTE_data_reg[reg].value = (int32_t)val.sw.l;
            g_GTE_data_reg[reg].y = 0.f;
            break;

        case 7:
        case 16:
        case 17:
        case 18:
        case 19:
            g_GTE_data_reg[reg].value = (uint32_t)val.w.l;
            g_GTE_data_reg[reg].y = 0.f;
            break;

        case 15:
            g_GTE_data_reg[reg] = SXY2;
            break;

        case 28:
        case 29:
            //  PCSX::g_emulator->m_cpu->m_regs.CP2D.p[reg].d = LIM(IR1 >> 7, 0x1f, 0, 0) | (LIM(IR2 >> 7, 0x1f,
            //  0, 0) << 5) | (LIM(IR3 >> 7,
            // 0x1f, 0, 0) << 10);
            break;
    }
}

void PGXP_GTE_MFC2(uint32_t instr, uint32_t rtVal, uint32_t rdVal) {
    // CPU[Rt] = GTE_D[Rd]
    Validate(&g_GTE_data_reg[rd(instr)], rdVal);
    // MFC2(rd(instr));
    g_CPU_reg[rt(instr)] = g_GTE_data_reg[rd(instr)];
    g_CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_GTE_MTC2(uint32_t instr, uint32_t rdVal, uint32_t rtVal) {
    // GTE_D[Rd] = CPU[Rt]
    Validate(&g_CPU_reg[rt(instr)], rtVal);
    PGXP_MTC2_int(g_CPU_reg[rt(instr)], rd(instr));
    g_GTE_data_reg[rd(instr)].value = rdVal;
}

void PGXP_GTE_CFC2(uint32_t instr, uint32_t rtVal, uint32_t rdVal) {
    // CPU[Rt] = GTE_C[Rd]
    Validate(&g_GTE_ctrl_reg[rd(instr)], rdVal);
    g_CPU_reg[rt(instr)] = g_GTE_ctrl_reg[rd(instr)];
    g_CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_GTE_CTC2(uint32_t instr, uint32_t rdVal, uint32_t rtVal) {
    // GTE_C[Rd] = CPU[Rt]
    Validate(&g_CPU_reg[rt(instr)], rtVal);
    g_GTE_ctrl_reg[rd(instr)] = g_CPU_reg[rt(instr)];
    g_GTE_ctrl_reg[rd(instr)].value = rdVal;
}

////////////////////////////////////
// Memory Access
////////////////////////////////////
void PGXP_GTE_LWC2(uint32_t instr, uint32_t rtVal, uint32_t addr) {
    // GTE_D[Rt] = Mem[addr]
    PGXP_value val;
    ValidateAndCopyMem(&val, addr, rtVal);
    PGXP_MTC2_int(val, rt(instr));
}

void PGXP_GTE_SWC2(uint32_t instr, uint32_t rtVal, uint32_t addr) {
    //  Mem[addr] = GTE_D[Rt]
    Validate(&g_GTE_data_reg[rt(instr)], rtVal);
    WriteMem(&g_GTE_data_reg[rt(instr)], addr);
}
