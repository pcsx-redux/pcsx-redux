/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
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

#include <filesystem>
#include <memory>

#include "core/decode_xa.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/sstate.h"
#include "json.hpp"
#include "lua/luawrapper.h"

namespace PCSX {

namespace SPU {
class Overlay;
}

class SPUInterface {
  public:
    using json = nlohmann::json;

    virtual ~SPUInterface() {}

    void interrupt();
    virtual bool open() = 0;
    virtual long init(void) = 0;
    virtual long shutdown(void) = 0;
    virtual long close(void) = 0;
    virtual uint16_t readRegister(uint32_t) = 0;
    virtual void writeRegister(uint32_t, uint16_t) = 0;
    virtual void playCDDAchannel(int16_t *, int) = 0;
    virtual void playADPCMchannel(xa_decode_t *) = 0;
    // CD audio volume matrix (ATV0..ATV3) for XA, applied after the interpolator.
    virtual void setXAVolume(const uint8_t atv[4]) = 0;
    virtual void writeDMAMem(uint16_t *, int) = 0;
    virtual void readDMAMem(uint16_t *, int) = 0;
    virtual void lockSPURAM() = 0;
    virtual void unlockSPURAM() = 0;
    // Byte-addressable view of the sound RAM. Mixing happens on the SPU's own
    // thread, so bracket bulk reads with lockSPURAM() / unlockSPURAM().
    virtual uint8_t *getSpuRam() = 0;
    // Replacement samples played in place of matching ones from the next key-on; null clears.
    virtual void setOverlay(std::shared_ptr<const SPU::Overlay>) = 0;
    virtual std::shared_ptr<const SPU::Overlay> getOverlay() = 0;
    static constexpr size_t c_spuRamSize = 512 * 1024;
    // Current sound-RAM transfer cursor, as set by 1F801DA6h and advanced by every transfer.
    // Read it before handing a DMA off, since the transfer is what moves it.
    virtual uint32_t getTransferAddr() = 0;
    virtual void resetCaptureBuffer() = 0;
    virtual json getCfg() = 0;
    virtual void setCfg(const json &j) = 0;
    virtual void debug() = 0;
    virtual bool configure() = 0;
    virtual void save(SaveStates::SPU &) = 0;
    virtual void load(const SaveStates::SPU &) = 0;
    virtual uint32_t getCurrentFrames() = 0;
    virtual void waitForGoal(uint32_t goal) = 0;
    // How many audio frames the CPU may run ahead of the audio device.
    virtual uint32_t getLeadFrames() = 0;
    // The CPU has reached `cycle`: the SPU may produce audio up to it.
    virtual void advanceTo(uint64_t cycle) = 0;
    virtual uint32_t getFrameCount() = 0;
    virtual void setLua(Lua L) = 0;
    // Writes the SPU mix to a 16-bit stereo WAV file, as it leaves the mixer and before the
    // audio sink converts it. Returns false if the file could not be created.
    virtual bool setWavDump(const std::filesystem::path &path) = 0;

    bool m_showDebug = false;
    bool m_showCfg = false;

  protected:
    void scheduleInterrupt();
};

}  // namespace PCSX
