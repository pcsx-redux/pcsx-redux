/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include <memory>
#include <string>
#include <string_view>

#include "cdrom/cdriso.h"
#include "core/decode_xa.h"
#include "core/psxemulator.h"
#include "core/psxhw.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "supportpsx/iec-60908b.h"

namespace PCSX {

namespace SaveStates {
struct Access;
}

namespace Widgets {
class IsoBrowser;
}

class CDRom {
  public:
    using MSF = IEC60908b::MSF;
    CDRom() : m_iso(new CDRIso(new FailedFile)) {}
    virtual ~CDRom() {}
    static CDRom* factory();
    // The jitter generator is a multiplicative LCG, so its state is always odd.
    static constexpr uint64_t c_defaultSeed = 9223521712174600777ull;
    // A save state written before the seed was saved restores it as 0.
    void defaultSeedIfMissing() {
        if (m_seed == 0) m_seed = m_powerOnSeed;
    }
    bool isLidOpen();
    void closeLid() {
        m_lidOpen = false;
        m_lidCloseScheduled = false;
    }
    void openLid() {
        m_lidOpen = true;
        m_wasLidOpened = true;
        m_lidCloseScheduled = false;
    }
    void scheduleCloseLid() {
        m_lidOpen = true;
        m_wasLidOpened = true;
        m_lidCloseScheduled = true;
        using namespace std::chrono_literals;
        m_lidCloseAtCycles = g_emulator->m_cpu->m_regs.getFutureCycle(1s);
    }
    void parseIso();

    std::shared_ptr<CDRIso> getIso() { return m_iso; }
    void clearIso() {
        m_iso.reset();
        g_system->m_eventBus->signal(Events::IsoMounted{});
    }
    void setIso(CDRIso* iso) {
        m_iso.reset(iso);
        g_system->m_eventBus->signal(Events::IsoMounted{});
    }
    void setIso(std::shared_ptr<CDRIso> iso) {
        m_iso = std::move(iso);
        g_system->m_eventBus->signal(Events::IsoMounted{});
    }

    const std::string& getCDRomID() { return m_cdromId; }
    const std::string& getCDRomLabel() { return m_cdromLabel; }

    virtual void reset() = 0;

    virtual void fifoScheduledCallback() = 0;
    virtual void commandsScheduledCallback() = 0;
    virtual void readScheduledCallback() = 0;
    virtual void scheduledDmaCallback() = 0;
    virtual uint8_t read0() = 0;
    virtual uint8_t read1() = 0;
    virtual uint8_t read2() = 0;
    virtual uint8_t read3() = 0;
    virtual void write0(uint8_t rt) = 0;
    virtual void write1(uint8_t rt) = 0;
    virtual void write2(uint8_t rt) = 0;
    virtual void write3(uint8_t rt) = 0;

    virtual void dma(uint32_t madr, uint32_t bcr, uint32_t chcr) = 0;

    std::shared_ptr<CDRIso> getIso() const { return m_iso; }

  protected:
    std::shared_ptr<CDRIso> m_iso;
    friend struct SaveStates::Access;

    bool dataFIFOHasData() { return m_dataFIFOIndex != m_dataFIFOSize; }

    bool m_lidOpen = false;
    bool m_wasLidOpened = false;
    bool m_lidCloseScheduled = false;
    uint32_t m_lidCloseAtCycles = 0;

    // to save/init
    uint64_t m_seed = c_defaultSeed;
    uint64_t m_powerOnSeed = c_defaultSeed;
    uint8_t m_dataFIFO[2352] = {0};
    uint32_t m_dataFIFOIndex = 0;
    uint32_t m_dataFIFOSize = 0;
    uint32_t m_dataFIFOPending = 0;
    uint8_t m_registerAddress = 0;
    bool m_motorOn = false;
    bool m_speedChanged = false;
    bool m_invalidLocL = false;
    // Set once a data sector has been read since the last seek; m_currentPosition is then one past it.
    bool m_sectorReadSinceSeek = false;
    bool m_scexCountersReset = false;
    bool m_dataRequested = false;
    bool m_subheaderFilter = false;
    bool m_realtime = false;
    enum class ReadingState : uint8_t {
        None,
        Seeking,
        Reading,
    } m_readingState = ReadingState::None;
    bool m_startPlaying = false;
    enum class ReadingType : uint8_t {
        None,
        Normal,
        Streaming,
        Play,
    } m_readingType = ReadingType::None;
    bool m_autoPause = false;
    bool m_report = false;
    bool m_setLocPending = false;
    bool m_muted = false;
    bool m_peakFlag = false;
    uint8_t m_playTrack = 0;
    uint32_t m_playStartCycle = 0;
    enum class Status : uint8_t {
        Idle,
        ReadingData,
        Seeking,
        PlayingCDDA,
    } m_status = Status::Idle;
    enum class Speed : uint8_t { Simple, Double } m_speed;
    enum class ReadSpan : uint8_t { S2048, S2328, S2340 } m_readSpan;
    uint8_t m_interruptCauseMask = 0x1f;
    uint8_t m_atv[4] = {0x80, 0, 0x80, 0};
    uint8_t m_atvPending[4] = {0x80, 0, 0x80, 0};
    bool m_adpcmMuted = false;
    uint8_t m_mode = 0;
    uint8_t m_filterFile = 0;
    uint8_t m_filterChannel = 0;
    bool m_xaEnded = false;
    bool m_xaFirstSector = true;
    xa_decode_t m_xa;
    xa_decode_t m_xaOut;
    bool m_soundMapEnabled = false;

    enum class Cause : uint8_t {
        None = 0,
        DataReady = 1,
        Complete = 2,
        Acknowledge = 3,
        End = 4,
        Error = 5,
    };

    MSF m_currentPosition;
    MSF m_seekPosition;
    uint8_t m_lastLocP[8] = {0};
    uint8_t m_lastLocL[8] = {0};

    struct QueueElement {
        uint8_t payload[16];
        uint8_t value;
        bool valueRead = false;
        bool hasValue = false;
        bool hitMax = false;
        uint8_t payloadSize = 0;
        uint8_t payloadIndex = 0;
        bool isPayloadAtEnd() const { return payloadIndex == payloadSize; }
        bool isPayloadEmpty() const { return (payloadSize == 0) || hitMax; }
        bool isPayloadFull() const { return payloadSize == sizeof(payload); }
        bool empty() const { return valueEmpty() && isPayloadEmpty(); }
        bool valueEmpty() const { return !hasValue || valueRead; }
        void clear() {
            hasValue = false;
            valueRead = false;
            payloadSize = 0;
            payloadIndex = 0;
        }
        void setValue(uint8_t newValue) {
            value = newValue;
            hasValue = true;
        }
        void setValue(Cause cause) { setValue(static_cast<uint8_t>(cause)); }
        void pushPayloadData(uint8_t value) {
            if (payloadSize < sizeof(payload)) payload[payloadSize++] = value;
        }
        void pushPayloadData(std::string_view values) {
            for (auto value : values) {
                pushPayloadData(value);
            }
        }
        uint8_t getValue() const { return valueRead ? 0 : value; }
        uint8_t readPayloadByte() {
            while (payloadIndex >= 16) payloadIndex -= 16;
            uint8_t r = 0;
            if (payloadIndex < payloadSize) {
                r = payload[payloadIndex];
            }
            if (++payloadIndex == payloadSize) hitMax = true;
            return r;
        }
    };

    QueueElement m_commandFifo;
    QueueElement m_commandExecuting;
    QueueElement m_responseFifo[2];
    bool responseFifoFull() { return !m_responseFifo[0].valueEmpty() && !m_responseFifo[1].empty(); }

  private:
    friend class Widgets::IsoBrowser;
    std::string m_cdromId;
    std::string m_cdromLabel;
};

}  // namespace PCSX
