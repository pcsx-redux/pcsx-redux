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

/*
 * R3000A disassembler.
 */

#pragma once

#include <stdint.h>

#include <string>
#include <string_view>

namespace PCSX {

class Disasm {
  public:
    static const char *s_disRNameGPR[];
    static const char *s_disRNameCP2D[];
    static const char *s_disRNameCP2C[];
    static const char *s_disRNameCP0[];
    static const char *getSyscallName(uint32_t index);

    // Abstracts where operand values come from when disassembling "with values".
    // The live source reads the running CPU/memory; a playback source can feed
    // sparse values captured in a binary trace, so the exact same disassembly
    // traversal renders an identical annotated line from recorded data.
    struct ValueSource {
        virtual ~ValueSource() = default;
        virtual uint32_t gpr(uint8_t reg) = 0;
        virtual uint32_t cp0(uint8_t reg) = 0;
        virtual uint32_t cp2d(uint8_t reg) = 0;
        virtual uint32_t cp2c(uint8_t reg) = 0;
        virtual uint32_t hi() = 0;
        virtual uint32_t lo() = 0;
        virtual uint8_t mem8(uint32_t addr) = 0;
        virtual uint16_t mem16(uint32_t addr) = 0;
        virtual uint32_t mem32(uint32_t addr) = 0;
    };
    // The default source: reads live state from the running emulator. Used when
    // withValues is requested but no explicit source is supplied, and reused by
    // the trace recorder for its side-effect-free memory peeks.
    static ValueSource &liveValueSource();

#define declare(n) \
    void n(uint32_t code, uint32_t nextCode, uint32_t pc, bool *skipNext = nullptr, bool *delaySlotNext = nullptr)

    declare(process) {
        if (skipNext && *skipNext) {
            reset();
            *skipNext = false;
            return;
        }
        if (delaySlotNext) *delaySlotNext = false;
        cTdisR3000AF ptr = s_disR3000A[code >> 26];
        (*this.*ptr)(code, nextCode, pc, skipNext, delaySlotNext);
    }

    static std::string asString(uint32_t code, uint32_t nextCode, uint32_t pc, bool *skipNext = nullptr,
                                bool withValues = false, ValueSource *values = nullptr);
    virtual void reset() {}

  protected:
    virtual void Invalid() = 0;
    virtual void OpCode(std::string_view) = 0;
    virtual void GPR(uint8_t reg) = 0;
    virtual void CP0(uint8_t reg) = 0;
    virtual void CP2C(uint8_t reg) = 0;
    virtual void CP2D(uint8_t reg) = 0;
    virtual void HI() = 0;
    virtual void LO() = 0;
    virtual void Imm16(int16_t value) = 0;
    virtual void Imm16u(uint16_t value) = 0;
    virtual void Imm32(uint32_t value) = 0;
    virtual void Target(uint32_t value) = 0;
    virtual void Sa(uint8_t value) = 0;
    virtual void OfB(int16_t offset, uint8_t reg, int size) = 0;
    virtual void BranchDest(uint32_t offset) = 0;
    virtual void Offset(uint32_t offset, int size) = 0;
    virtual void SyscallName(const char *) = 0;

  private:
    // Type definition of our functions
    typedef void (Disasm::*TdisR3000AF)(uint32_t code, uint32_t nextCode, uint32_t pc, bool *skipNext,
                                        bool *delaySlotNext);
    typedef const TdisR3000AF cTdisR3000AF;
    static const TdisR3000AF s_disR3000A[];
    static const TdisR3000AF s_disR3000A_COP0[];
    static const TdisR3000AF s_disR3000A_COP2[];
    static const TdisR3000AF s_disR3000A_BASIC[];
    static const TdisR3000AF s_disR3000A_SPECIAL[];
    static const TdisR3000AF s_disR3000A_BCOND[];

    declare(disNULL);
    declare(disSPECIAL);
    declare(disBCOND);
    declare(disJ);
    declare(disJAL);
    declare(disBEQ);
    declare(disBNE);
    declare(disBLEZ);
    declare(disBGTZ);
    declare(disADDI);
    declare(disADDIU);
    declare(disSLTI);
    declare(disSLTIU);
    declare(disANDI);
    declare(disORI);
    declare(disXORI);
    declare(disLUI);
    declare(disCOP0);
    declare(disCOPn);
    declare(disCOP2);
    declare(disBCz);

    declare(disLB);
    declare(disLH);
    declare(disLWL);
    declare(disLW);
    declare(disLBU);
    declare(disLHU);
    declare(disLWR);
    declare(disSB);
    declare(disSH);
    declare(disSWL);
    declare(disSW);
    declare(disSWR);
    declare(disLWC2);
    declare(disSWC2);

    declare(disSLL);
    declare(disSRL);
    declare(disSRA);
    declare(disSLLV);
    declare(disSRLV);
    declare(disSRAV);
    declare(disJR);
    declare(disJALR);
    declare(disSYSCALL);
    declare(disBREAK);
    declare(disMFHI);
    declare(disMTHI);
    declare(disMFLO);
    declare(disMTLO);
    declare(disMULT);
    declare(disMULTU);
    declare(disDIV);
    declare(disDIVU);
    declare(disADD);
    declare(disADDU);
    declare(disSUB);
    declare(disSUBU);
    declare(disAND);
    declare(disOR);
    declare(disXOR);
    declare(disNOR);
    declare(disSLT);
    declare(disSLTU);

    declare(disBLTZ);
    declare(disBGEZ);
    declare(disBLTZAL);
    declare(disBGEZAL);

    declare(disMFC0);
    declare(disCFC0);
    declare(disMTC0);
    declare(disCTC0);
    declare(disRFE);
    declare(disMFC2);
    declare(disMTC2);
    declare(disCFC2);
    declare(disCTC2);

    declare(disBASIC);
    declare(disRTPS);
    declare(disNCLIP);
    declare(disOP);
    declare(disDPCS);
    declare(disINTPL);
    declare(disMVMVA);
    declare(disNCDS);
    declare(disCDP);
    declare(disNCDT);
    declare(disNCCS);
    declare(disCC);
    declare(disNCS);
    declare(disNCT);
    declare(disSQR);
    declare(disDCPL);
    declare(disDPCT);
    declare(disAVSZ3);
    declare(disAVSZ4);
    declare(disRTPT);
    declare(disGPF);
    declare(disGPL);
    declare(disNCCT);
#undef declare

    // li is the addiu/ori $zero pseudo-instruction; li $a0 followed by a plain syscall reads as the named syscall
    void disLI(uint8_t reg, uint16_t imm, bool zeroExtend, uint32_t nextCode, bool *skipNext);
};

}  // namespace PCSX
