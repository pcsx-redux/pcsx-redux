/*

MIT License

Copyright (c) 2023 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "supportpsx/ps1-packer.h"

#include <assert.h>
#include <stdint.h>

#include <algorithm>
#include <exception>
#include <vector>

#include "mips-common/util/encoder.hh"
#include "n2e-d.h"
#include "support/polyfills.h"
#include "supportpsx/ucl-utils.h"
#include "ucl/ucl.h"

using namespace Mips::Encoder;
constexpr uint64_t PSEXE = 0x45584520582d5350;

namespace {

template <PCSX::PolyFill::IntegralConcept T>
void pushBytes(std::vector<uint8_t>& data, T value) {
    for (unsigned i = 0; i < sizeof(T); i++) {
        data.push_back(value & 0xff);
        value >>= 8;
    }
}

template <PCSX::PolyFill::IntegralConcept T, size_t S>
void pushBytes(std::vector<uint8_t>& data, const T (&a)[S]) {
    for (unsigned i = 0; i < S; i++) {
        pushBytes(data, a[i]);
    }
}

template <PCSX::PolyFill::IntegralConcept T>
void pushBytes(std::vector<uint8_t>& data, const std::vector<T>& in) {
    for (auto b : in) {
        pushBytes(data, b);
    }
}

int16_t getHI(uint32_t v) {
    int16_t lo = v & 0xffff;
    int16_t hi = v >> 16;
    return lo < 0 ? hi + 1 : hi;
}

int16_t getLO(uint32_t v) {
    int16_t ret = v & 0xffff;
    return ret;
}

// Datel carts (Action Replay, GameShark) map the first 128KB of their flash
// at 0x1f000000 and the second 128KB at 0x1f040000, with I/O registers in
// between. The rom image is linear in flash, so its second 128KB lands in the
// second window. The bootstrap gathers the compressed stream from both windows
// into RAM, at the same spot an in-place PS-X EXE would hold it, then runs the
// usual decompression. Returns the entry point of the bootstrap.
uint32_t packDatel(std::vector<uint8_t>& dataOut, size_t inSize, size_t outSize, uint32_t addr, uint32_t pc,
                   const PCSX::PS1Packer::Options& options) {
    // The last 8KB of each window is left alone.
    constexpr uint32_t window1Base = 0x1f000000;
    constexpr uint32_t window2Base = 0x1f040000;
    constexpr uint32_t window2Offset = 0x20000;
    constexpr uint32_t windowUsable = 0x1e000;
    constexpr uint32_t tload = 0x110;

    size_t margin = PCSX::UCLUtils::inPlaceOverlapMargin(dataOut.data(), outSize, inSize);
    size_t relMargin = margin - inSize + outSize;
    relMargin = (relMargin + 3) & ~3;
    uint32_t staging = addr + inSize + relMargin + 8 - dataOut.size();
    uint32_t rawStaging = staging & 0x1fffffff;
    if (rawStaging < 0x10000) staging += 0x10000 - rawStaging;
    if (((staging + dataOut.size()) & 0x1fffffff) > 0x1f0000) {
        throw std::runtime_error("Binary too large to be staged in RAM for a Datel rom.\n");
    }

    auto emit = [&](uint32_t size1, uint32_t size2) {
        std::vector<uint8_t> code;
        pushBytes(code, n2e_d::code);
        if (options.resetstack) {
            pushBytes(code, lui(Reg::SP, 0x807f));
        } else {
            pushBytes(code, addiu(Reg::T8, Reg::RA, 0));
        }
        // Kill interrupts by setting IMASK to 0.
        pushBytes(code, lui(Reg::V1, 0x1f80));
        pushBytes(code, sw(Reg::R0, 0x1074, Reg::V1));
        // Copies $t1 words from $a2 to $a3.
        auto copy = [&](uint32_t src, uint32_t size) {
            pushBytes(code, lui(Reg::A2, getHI(src)));
            pushBytes(code, addiu(Reg::A2, getLO(src)));
            pushBytes(code, lui(Reg::T1, getHI(size / 4)));
            pushBytes(code, addiu(Reg::T1, getLO(size / 4)));
            pushBytes(code, lw(Reg::T0, 0, Reg::A2));
            pushBytes(code, addiu(Reg::T1, -1));
            pushBytes(code, addiu(Reg::A2, 4));
            pushBytes(code, sw(Reg::T0, 0, Reg::A3));
            pushBytes(code, bnez(Reg::T1, -20));
            pushBytes(code, addiu(Reg::A3, 4));
        };
        pushBytes(code, lui(Reg::A3, getHI(staging)));
        pushBytes(code, addiu(Reg::A3, getLO(staging)));
        uint32_t codeSize = sizeof(n2e_d::code);
        uint32_t fixedSize = 38 * 4;
        copy(window1Base + tload + codeSize + fixedSize, size1);
        if (size2 != 0) copy(window2Base, size2);
        // Calls the ucl-nrv2e decompressor.
        pushBytes(code, lui(Reg::A0, getHI(staging)));
        pushBytes(code, addiu(Reg::A0, getLO(staging)));
        pushBytes(code, lui(Reg::A1, getHI(addr)));
        pushBytes(code, bal(-((int16_t)(code.size() + 4))));
        pushBytes(code, addiu(Reg::A1, getLO(addr)));
        // Calls A0:44 - FlushCache, then jumps into the decompressed binary.
        pushBytes(code, li(Reg::T0, 0xa0));
        pushBytes(code, jalr(Reg::T0));
        pushBytes(code, li(Reg::T1, 0x44));
        pushBytes(code, lui(Reg::T0, getHI(pc)));
        pushBytes(code, addiu(Reg::T0, getLO(pc)));
        pushBytes(code, jr(Reg::T0));
        if (options.resetstack) {
            pushBytes(code, ori(Reg::SP, 0xfff0));
        } else {
            pushBytes(code, addiu(Reg::RA, Reg::T8, 0));
        }
        while (code.size() < codeSize + fixedSize) pushBytes(code, nop());
        if (code.size() != codeSize + fixedSize) {
            throw std::runtime_error("Internal error: Datel bootstrap size mismatch.\n");
        }
        return code;
    };

    uint32_t codeSize = emit(0, 0).size();
    uint32_t dataStart = tload + codeSize;
    uint32_t size1 = std::min<uint32_t>(dataOut.size(), windowUsable - dataStart);
    uint32_t size2 = dataOut.size() - size1;
    if (size2 > windowUsable) {
        throw std::runtime_error("Binary too large for a Datel rom, even split over both flash windows.\n");
    }

    std::vector<uint8_t> rom = emit(size1, size2);
    rom.insert(rom.end(), dataOut.begin(), dataOut.begin() + size1);
    if (size2 != 0) {
        rom.resize(window2Offset - tload, 0);
        rom.insert(rom.end(), dataOut.begin() + size1, dataOut.end());
    }
    dataOut = std::move(rom);
    return window1Base + tload + sizeof(n2e_d::code);
}

}  // namespace

void PCSX::PS1Packer::pack(IO<File> src, IO<File> dest, uint32_t addr, uint32_t pc, uint32_t gp, uint32_t sp,
                           const Options& options) {
    constexpr size_t stubSize = 12 * 4;
    std::vector<uint8_t> dataIn;
    dataIn.resize(src->size());
    src->read(dataIn.data(), dataIn.size());
    while ((dataIn.size() & 3) != 0) dataIn.push_back(0);

    std::vector<uint8_t> dataOut;
    dataOut.resize(dataIn.size() * 1.2 + 2064);
    ucl_uint outSize;
    int r;

    // Compress the binary using ucl-nrv2e, and store the compressed
    // binary in dataOut, potentially offset by the size of our stub,
    // if we're outputting a raw file.
    r = ucl_nrv2e_99_compress(dataIn.data(), dataIn.size(), dataOut.data() + (options.raw ? stubSize : 0), &outSize,
                              nullptr, 10, nullptr, nullptr);
    if (r != UCL_E_OK) {
        throw std::runtime_error("Fatal error during data compression.\n");
    }
    dataOut.resize(outSize + (options.raw ? stubSize : 0));
    while ((dataOut.size() & 3) != 0) {
        dataOut.push_back(0);
    }
    std::vector<uint8_t> compressed;
    if (options.datel) compressed = dataOut;
    uint32_t newPC = dataOut.size();
    uint32_t compLoad = 0;

    // If we're outputting a rom file, our tload will be fixed
    // at 0x1f000110, after the license string.
    uint32_t tload = options.raw ? 0 : (options.rom || options.datel) ? 0x1f000110 : options.tload;

    if (tload != 0) {
        newPC = tload + dataOut.size();
        compLoad = tload;
    } else if (!options.raw) {
        // If we don't have a tload, it means we're doing in-place
        // decompression. The compressed data has to sit far enough above the
        // decompression target `addr` that the decompressor's write head never
        // overtakes its read head. That distance is data-dependent, not a
        // constant, so measure it exactly for this stream. (It used to be a
        // hardcoded 16, which silently corrupts any payload whose worst-case
        // match lands late enough to need more.)
        size_t margin = UCLUtils::inPlaceOverlapMargin(dataOut.data(), outSize, dataIn.size());
        size_t relMargin = margin - dataIn.size() + outSize;
        relMargin = (relMargin + 3) & ~3;
        newPC = addr + dataIn.size() + relMargin + 8;
        compLoad = newPC - dataOut.size();
        uint32_t rawCompLoad = compLoad & 0x1fffffff;
        if (rawCompLoad < 0x10000) {
            // We're loading inside the kernel, bump out of it.
            newPC += 0x10000 - rawCompLoad;
            compLoad += 0x10000 - rawCompLoad;
        }
    }
    newPC += sizeof(n2e_d::code);

    if (options.raw) {
        // If outputting a raw file, our start address is going
        // to be the same as our tload address, so we need to inject
        // a jump to the start of our code. We are doing this in
        // a fully position-independent way, so raw files can be
        // loaded anywhere in memory. We're also going to ensure
        // that we're jumping to a segment where the i-cache is
        // enabled, so the decompressor code can run fast.
        auto offset = newPC - 4 * 4;
        std::vector<uint8_t> stub;
        if (options.resetstack) {
            pushBytes(stub, lui(Reg::SP, 0x807f));
        } else {
            pushBytes(stub, addiu(Reg::T8, Reg::RA, 0));
        }
        pushBytes(stub, lui(Reg::T1, getHI(offset)));
        pushBytes(stub, bal(4));
        pushBytes(stub, addiu(Reg::T1, getLO(offset)));
        pushBytes(stub, lui(Reg::T0, 0x1fff));
        pushBytes(stub, ori(Reg::T0, 0xffff));
        pushBytes(stub, andd(Reg::RA, Reg::T0));
        pushBytes(stub, lui(Reg::T0, 0x8000));
        pushBytes(stub, orr(Reg::RA, Reg::T0));
        pushBytes(stub, addu(Reg::T0, Reg::RA, Reg::T1));
        pushBytes(stub, jr(Reg::T0));
        pushBytes(stub, addiu(Reg::A0, Reg::RA, 32));

        assert(stub.size() == stubSize);

        std::copy(stub.begin(), stub.end(), dataOut.begin());
        compLoad += stub.size();
    }

    // Now we start writing our decompressor stub.
    // First, dump the decompressor code.
    size_t n2estart = dataOut.size();
    pushBytes(dataOut, n2e_d::code);

    // At this point in the dataOut buffer, we have the following:
    // 1. Maybe a small stub to jump over the compressed data
    //    and decompressor code, if we're a raw file.
    // 2. The compressed data.
    // 3. The decompressor code.
    //
    // What comes next is the bootstrapper code, which will
    // set things up for the decompressor code to run, and
    // call it, then run the decompressed code.
    //
    // If not a raw file, the current location in dataOut
    // will be the PC we're setting up in the output
    // binary file, and so the next instructions will
    // be the very first our binary will run.

    if (!options.shell && !options.raw && !options.resetstack) {
        // We save $ra to $t8, so we can restore it later. This breaks ABI,
        // but the ucl-nrv2e decompressor won't use it. This isn't useful
        // for the shell trick, since we're just going to reboot the machine.
        pushBytes(dataOut, addiu(Reg::T8, Reg::RA, 0));
    } else if (options.resetstack && !options.raw) {
        pushBytes(dataOut, lui(Reg::SP, 0x807f));
    }
    // Kill interrupts by setting IMASK to 0.
    pushBytes(dataOut, lui(Reg::V1, 0x1f80));
    pushBytes(dataOut, sw(Reg::R0, 0x1074, Reg::V1));
    // Calls the ucl-nrv2e decompressor.
    if (!options.raw) {
        pushBytes(dataOut, lui(Reg::A0, getHI(compLoad)));
        pushBytes(dataOut, addiu(Reg::A0, getLO(compLoad)));
    }
    pushBytes(dataOut, lui(Reg::A1, getHI(addr)));
    pushBytes(dataOut, bal(-((int16_t)(dataOut.size() + 4 - n2estart))));
    pushBytes(dataOut, addiu(Reg::A1, getLO(addr)));

    // Then, bootstrap our newly-decompressed binary.
    if (options.shell) {
        std::vector<uint8_t> breakHandler;
        std::vector<uint8_t> shellCode;

        // This goes to 0x40. This is our break handler,
        // which will prevent the shell from being memcpy()ed
        // to 0x80030000, and run our code instead.
        pushBytes(breakHandler, mtc0(Reg::R0, 7));
        pushBytes(breakHandler, jr(Reg::RA));
        pushBytes(breakHandler, rfe());

        // This goes to 0x80030000. We just bootstrap our decompressed
        // binary in memory.
        pushBytes(shellCode, nop());
        pushBytes(shellCode, lui(Reg::T0, getHI(pc)));
        pushBytes(shellCode, addiu(Reg::T0, getLO(pc)));
        pushBytes(shellCode, lui(Reg::GP, getHI(gp)));
        pushBytes(shellCode, jr(Reg::T0));
        pushBytes(shellCode, addiu(Reg::GP, getLO(gp)));

        // Jumps over the two blocks of code above, grabbing their address
        // in $ra using bal.
        pushBytes(dataOut, bal(breakHandler.size() + shellCode.size()));
        // $s0 = 0xa0
        pushBytes(dataOut, li(Reg::S0, 0xa0));

        // Insert the two pieces of code we need to copy.
        pushBytes(dataOut, breakHandler);
        pushBytes(dataOut, shellCode);

        // Copying stuff around, calling into the kernel.
        // $s1 = address of the break handler.
        pushBytes(dataOut, addiu(Reg::S1, Reg::RA, 0));

        pushBytes(dataOut, li(Reg::A0, 0x40));
        pushBytes(dataOut, addiu(Reg::A1, Reg::S1, 0));
        pushBytes(dataOut, li(Reg::A2, breakHandler.size()));
        // Call A0:2A - memcpy.
        pushBytes(dataOut, jalr(Reg::S0));
        pushBytes(dataOut, li(Reg::T1, 0x2a));

        pushBytes(dataOut, lui(Reg::A0, 0x8003));
        pushBytes(dataOut, addiu(Reg::A1, Reg::S1, breakHandler.size()));
        pushBytes(dataOut, li(Reg::A2, shellCode.size()));
        // Call A0:2A - memcpy.
        pushBytes(dataOut, jalr(Reg::S0));
        pushBytes(dataOut, li(Reg::T1, 0x2a));

        // And reboot, leaving cop0's registers set to break
        // on writes to 0x80030000.
        constexpr uint32_t partialReboot = 0xbfc00390;

        pushBytes(dataOut, lui(Reg::RA, getHI(partialReboot)));
        pushBytes(dataOut, addiu(Reg::RA, getLO(partialReboot)));

        pushBytes(dataOut, lui(Reg::T0, 0b1100101010000000));
        pushBytes(dataOut, lui(Reg::T1, 0x8003));
        pushBytes(dataOut, li(Reg::T2, -1));
        pushBytes(dataOut, mtc0(Reg::R0, 7));
        pushBytes(dataOut, mtc0(Reg::T1, 5));
        pushBytes(dataOut, mtc0(Reg::T2, 9));
        pushBytes(dataOut, mtc0(Reg::T0, 7));

        // Call A0:44 - FlushCache, after mangling $ra to
        // almost be the address of the start of the bios,
        // skipping over SBUS settings, and the resetting
        // of all the cop0 registers.
        pushBytes(dataOut, jr(Reg::S0));
        pushBytes(dataOut, li(Reg::T1, 0x44));
    } else if (options.nokernel) {
        // We can't call into the kernel, so we need to
        // flush the cache ourselves first.
        pushBytes(dataOut, bal(4));
        pushBytes(dataOut, lui(Reg::T1, 0xa000));
        pushBytes(dataOut, orr(Reg::T1, Reg::RA, Reg::T1));
        pushBytes(dataOut, addiu(Reg::T1, 16));
        pushBytes(dataOut, jr(Reg::T1));
        pushBytes(dataOut, mtc0(Reg::R0, 12));
        pushBytes(dataOut, lui(Reg::T5, 0xfffe));
        pushBytes(dataOut, lui(Reg::T2, 0x0001));
        pushBytes(dataOut, ori(Reg::T2, 0xe90c));
        pushBytes(dataOut, sw(Reg::T2, 0x0130, Reg::T5));
        pushBytes(dataOut, lui(Reg::T1, 1));
        pushBytes(dataOut, mtc0(Reg::T1, 12));
        pushBytes(dataOut, move(Reg::T3, Reg::R0));
        pushBytes(dataOut, li(Reg::T4, 0x0ff0));
        pushBytes(dataOut, sw(Reg::R0, 0, Reg::T3));
        pushBytes(dataOut, bne(Reg::T3, Reg::T4, -8));
        pushBytes(dataOut, addiu(Reg::T3, 0x10));
        pushBytes(dataOut, mtc0(Reg::R0, 12));
        pushBytes(dataOut, addiu(Reg::T2, 0x7c));
        pushBytes(dataOut, sw(Reg::T2, 0x0130, Reg::T5));
        // Then jumps into the decompressed binary, restoring
        // $ra if needed, so the decompressed binary can return
        // to the caller gracefully.
        pushBytes(dataOut, lui(Reg::T0, getHI(pc)));
        pushBytes(dataOut, addiu(Reg::T0, getLO(pc)));
        pushBytes(dataOut, jr(Reg::T0));
        if (options.resetstack) {
            pushBytes(dataOut, ori(Reg::SP, Reg::SP, 0xfff0));
        } else {
            pushBytes(dataOut, addiu(Reg::RA, Reg::T8, 0));
        }
    } else {
        // Calls A0:44 - FlushCache
        pushBytes(dataOut, li(Reg::T0, 0xa0));
        pushBytes(dataOut, jalr(Reg::T0));
        pushBytes(dataOut, li(Reg::T1, 0x44));
        // Then jumps into the decompressed binary, restoring
        // $ra if needed, so the decompressed binary can return
        // to the caller gracefully.
        pushBytes(dataOut, lui(Reg::T0, getHI(pc)));
        pushBytes(dataOut, addiu(Reg::T0, getLO(pc)));
        pushBytes(dataOut, jr(Reg::T0));
        if (options.resetstack) {
            pushBytes(dataOut, ori(Reg::SP, 0xfff0));
        } else {
            pushBytes(dataOut, addiu(Reg::RA, Reg::T8, 0));
        }
    }

    if (options.datel) {
        newPC = packDatel(compressed, dataIn.size(), outSize, addr, pc, options);
        dataOut = std::move(compressed);
    }

    // Pad our PS-X EXE to a multiple of 2048 bytes, because
    // that's what the PS1's BIOS expects when reading from CD-Rom.
    while (!options.cpe && !options.booty && !options.rom && !options.datel && !options.raw && !options.nopad &&
           ((dataOut.size() & 0x7ff) != 0)) {
        dataOut.push_back(0);
    }

    // Finally, create the header of our binary.

    // For a rom file, this is going to be the most complex
    // header, as we still need to push some more code.
    std::vector<uint8_t> header;
    if (options.booty || options.rom || options.datel) {
        // We'll use the same bootstrap code for both the normal rom code
        // and the booty code, cause we're lazy. The stage2 code is
        // going to be executed by the breakpoint we're setting on top
        // of 0x80030000 during the rom boot process. The booty
        // code is too large to fit properly, so it'll fit at 0x24.
        // The normal code fits properly at 0x40.
        std::vector<uint32_t> stage2;
        if (options.booty) {
            // What the booty code will do here is continue pumping
            // bytes out of the parallel port, which will be the
            // compressed binary, to place it in its proper location
            // in RAM. Once it's done, it'll jump to the start of
            // the compressed binary through FlushCache.
            /* 0x24 */ stage2.push_back(lw(Reg::A3, 0, Reg::A1));
            /* 0x28 */ stage2.push_back(addiu(Reg::A2, -1));
            /* 0x2c */ stage2.push_back(sw(Reg::A3, 0, Reg::A0));
            /* 0x30 */ stage2.push_back(bnez(Reg::A2, -16));
            /* 0x34 */ stage2.push_back(addiu(Reg::A0, 4));
            /* 0x38 */ stage2.push_back(j(0xa0));
            /* 0x3c */ stage2.push_back(li(Reg::T1, 0x44));
            // This is actually the entry point.
            /* 0x40 */ stage2.push_back(mtc0(Reg::R0, 7));
            /* 0x44 */ stage2.push_back(lui(Reg::A0, compLoad >> 16));
            if ((compLoad & 0xffff) != 0) {
                /* 0x48 */ stage2.push_back(ori(Reg::A0, compLoad));
            }
            /* 0x4c */ stage2.push_back(lui(Reg::RA, newPC >> 16));
            if ((newPC & 0xffff) != 0) {
                /* 0x50 */ stage2.push_back(ori(Reg::RA, newPC & 0xffff));
            }
            /* 0x54 */ stage2.push_back(lui(Reg::A1, 0xbf00));
            /* 0x58 */ stage2.push_back(j(0x24));
            /* 0x5c */ stage2.push_back(ori(Reg::A2, Reg::R0, dataOut.size() / 4));
        } else {
            // Stage 2 for a normal rom file is much more simple. We just
            // need to jump to the start of our compressed binary. We only
            // need roughly 3 or 4 instructions.
            stage2.push_back(lui(Reg::V0, newPC >> 16));
            if ((newPC & 0xffff) != 0) {
                stage2.push_back(ori(Reg::V0, newPC & 0xffff));
            }
            stage2.push_back(jr(Reg::V0));
            stage2.push_back(mtc0(Reg::R0, 7));

            // We still need to build the rom header file.
            static constexpr char disclaimer[] =
                "This is a self-decompressing binary "
                "suitable for a flash cart rom "
                "created by ps1-packer (https://bit.ly/pcsx-redux). "
                "It is NOT ";
            for (auto b : disclaimer) {
                header.push_back(b);
            }
            assert(header.size() <= 0x80);
            while (header.size() < 0x80) {
                header.push_back(0);
            }
            pushBytes<uint32_t>(header, 0x1f0000b4);
        }

        static constexpr uint8_t license[] = {
            0x4c, 0x69, 0x63, 0x65, 0x6e, 0x73, 0x65, 0x64, 0x20, 0x62, 0x79, 0x20, 0x53, 0x6f, 0x6e, 0x79,
            0x20, 0x43, 0x6f, 0x6d, 0x70, 0x75, 0x74, 0x65, 0x72, 0x20, 0x45, 0x6e, 0x74, 0x65, 0x72, 0x74,
            0x61, 0x69, 0x6e, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x49, 0x6e, 0x63, 0x2e, 0x00, 0x00, 0x00, 0x1f,
        };

        pushBytes(header, license);

        // Break on writes and/or exec, to call our break handler which we
        // will place in memory next.
        pushBytes(header, mtc0(Reg::R0, 7));
        pushBytes(header, li(Reg::T2, -1));
        pushBytes(header, lui(Reg::T1, 0x8003));
        pushBytes(header, lui(Reg::T0, 0b1110101110000000));
        pushBytes(header, mtc0(Reg::T2, 11));
        pushBytes(header, mtc0(Reg::T2, 9));
        pushBytes(header, mtc0(Reg::T1, 5));
        pushBytes(header, mtc0(Reg::T1, 3));
        pushBytes(header, mtc0(Reg::T0, 7));

        // As said before, the break handler will be placed at 0x40 or 0x24.
        int16_t base = options.booty ? 0x24 : 0x40;
        // We just put the bytes of the stage2 in ram, short by short, with
        // no loop. Works great for booty as it's becoming very deterministic,
        // and only requires a few extra instructions as a rom file.
        uint32_t last = 0;
        for (auto b : stage2) {
            if (b == 0) {
                last = sw(Reg::R0, base, Reg::R0);
            } else {
                pushBytes(header, lui(Reg::T0, b >> 16));
                uint16_t rest = b;
                if (rest != 0) {
                    pushBytes(header, ori(Reg::T0, rest));
                }
                last = sw(Reg::T0, base, Reg::R0);
            }
            pushBytes(header, last);
            base += 4;
        }
        // Flip the jr $ra with the last instruction, to place
        // it into the delay slot.
        header.pop_back();
        header.pop_back();
        header.pop_back();
        header.pop_back();
        pushBytes(header, jr(Reg::RA));
        pushBytes(header, last);
        if (options.rom || options.datel) {
            // Our tload for a rom file is fixed at 0x1f000110, so we
            // have some leeway for stage2 above, and so we need to
            // pad until we arrive at 0x110.
            while (header.size() < (tload & 0xffffff)) {
                header.push_back(0);
            }
        }
    } else if (options.cpe) {
        // Basic CPE file format.
        pushBytes<uint32_t>(header, 0x1455043);
        pushBytes<uint16_t>(header, 0x0008);
        pushBytes<uint8_t>(header, 3);
        pushBytes<uint16_t>(header, 0x0090);
        pushBytes<uint32_t>(header, newPC);
        pushBytes<uint8_t>(header, 1);
        pushBytes<uint32_t>(header, compLoad);
        pushBytes<uint32_t>(header, dataOut.size());
        dataOut.push_back(0);
    } else if (!options.raw) {
        // Basic PS-X EXE file format.
        pushBytes(header, PSEXE);
        pushBytes<uint32_t>(header, 0);
        pushBytes<uint32_t>(header, 0);
        pushBytes(header, newPC);
        pushBytes(header, gp);
        pushBytes(header, compLoad);
        pushBytes(header, dataOut.size());
        pushBytes<uint32_t>(header, 0);
        pushBytes<uint32_t>(header, 0);
        pushBytes<uint32_t>(header, 0);
        pushBytes<uint32_t>(header, 0);
        pushBytes(header, sp);
        while (header.size() < 0x800) {
            header.push_back(0);
        }
    }

    // All done. Write the header, and then the code blob.
    dest->write(header.data(), header.size());
    dest->write(dataOut.data(), dataOut.size());
}
