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

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "spu/externals.h"
#include "spu/interface.h"
#include "spu/registers.h"

void PCSX::SPU::impl::save(SaveStates::SPU &spu) {
    // Mix up to the CPU's cycle so every queued write is applied: the saved voice state is
    // then the state at that cycle, and the queue itself never needs saving.
    catchUp(PCSX::g_emulator->m_cpu->m_regs.cycle);
    const bool running = m_mixerRunning;
    if (running) RemoveThread();

    // Capture buffer.
    spu.get<SaveStates::CBCDLeft>().copyFrom(reinterpret_cast<uint8_t *>(captureBuffer.CDCapLeft));
    spu.get<SaveStates::CBCDRight>().copyFrom(reinterpret_cast<uint8_t *>(captureBuffer.CDCapRight));
    spu.get<SaveStates::CBCurrIndex>().value = captureBuffer.currIndex;
    spu.get<SaveStates::CBEndIndex>().value = captureBuffer.endIndex;
    spu.get<SaveStates::CBStartIndex>().value = captureBuffer.startIndex;
    spu.get<SaveStates::CBVoiceIndex>().value = capBufVoiceIndex;

    spu.get<SaveStates::SPURam>().copyFrom(reinterpret_cast<uint8_t *>(spuMem));
    spu.get<SaveStates::SPUPorts>().copyFrom(reinterpret_cast<uint8_t *>(regArea));
    auto &xa = spu.get<SaveStates::XAField>();
    if (xapGlobal) {
        xa.get<SaveStates::XAFrequency>().value = xapGlobal->freq;
        xa.get<SaveStates::XANBits>().value = xapGlobal->nbits;
        xa.get<SaveStates::XANSamples>().value = xapGlobal->nsamples;
        xa.get<SaveStates::XAStereo>().value = xapGlobal->stereo;
        auto &left = xa.get<SaveStates::XAADPCMLeft>();
        left.get<SaveStates::ADPCMDecodeY0>().value = xapGlobal->left.y0;
        left.get<SaveStates::ADPCMDecodeY1>().value = xapGlobal->left.y1;
        auto &right = xa.get<SaveStates::XAADPCMRight>();
        right.get<SaveStates::ADPCMDecodeY0>().value = xapGlobal->right.y0;
        right.get<SaveStates::ADPCMDecodeY1>().value = xapGlobal->right.y1;
        xa.get<SaveStates::XAPCM>().copyFrom(reinterpret_cast<uint8_t *>(xapGlobal->pcm));
    }
    spu.get<SaveStates::SPUIrq>().value = spuIrq;
    if (irqAddress) spu.get<SaveStates::SPUIrqPtr>().value = uintptr_t(irqAddress - spuRamBase);

    for (unsigned i = 0; i < MAXCHAN; i++) {
        auto &channel = spu.get<SaveStates::Channels>().value[i];
        auto &data = channel.get<SaveStates::Data>();
        data = s_chan[i].data;
        channel.get<SaveStates::ADSRInfo>() = s_chan[i].adsr.legacy();
        channel.get<SaveStates::ADSRInfoEx>() = s_chan[i].adsr.ex();
        // Each per-voice helper owns the conversion between its runtime state and
        // its savestate-mirror fields; we just hand it the fields.
        s_chan[i].adpcm.saveTo(data.get<Chan::s_1>(), data.get<Chan::s_2>(), data.get<Chan::StartPtr>(),
                               data.get<Chan::CurrPtr>(), data.get<Chan::LoopPtr>(), spuRamBase,
                               data.get<Chan::SB>().value.data(), data.get<Chan::SBPos>());
        s_chan[i].volume.saveTo(data.get<Chan::LeftVolume>(), data.get<Chan::RightVolume>(),
                                data.get<Chan::LeftVolRaw>(), data.get<Chan::RightVolRaw>());
        s_chan[i].interp.saveTo(data.get<Chan::SB>().value.data(), data.get<Chan::spos>(), data.get<Chan::sinc>());
    }

    spu.get<SaveStates::SPUAddr>().value = spuAddr;
    spu.get<SaveStates::SPUCtrl>().value = spuCtrl;
    spu.get<SaveStates::SPUStat>().value = spuStat;
    spu.get<SaveStates::SPUEndx>().value = spuEndx;
    spu.get<SaveStates::SPUMainVolWritten>().value = mixVolumeWrittenMask();

    m_noise.saveTo(spu.get<SaveStates::SPUNoiseClock>(), spu.get<SaveStates::SPUNoiseCount>(),
                   spu.get<SaveStates::SPUNoiseVal>());

    saveEnvelopeCheckpoints(spu.get<SaveStates::EnvelopeCheckpoints>());

    if (running) SetupThread();
}

void PCSX::SPU::impl::load(const SaveStates::SPU &spu) {
    // Processing is stopped while the state is restored.
    const bool running = m_mixerRunning;
    if (running) RemoveThread();
    // Writes still queued from before the load belong to the state being replaced.
    m_eventsApplied = m_eventsPushed.load();
    // The CPU registers are already restored: restart the mixer clock at their cycle.
    resync(PCSX::g_emulator->m_cpu->m_regs.cycle);

    spu.get<SaveStates::CBCDLeft>().copyTo(reinterpret_cast<uint8_t *>(captureBuffer.CDCapLeft));
    spu.get<SaveStates::CBCDRight>().copyTo(reinterpret_cast<uint8_t *>(captureBuffer.CDCapRight));
    captureBuffer.currIndex = spu.get<SaveStates::CBCurrIndex>().value;
    captureBuffer.endIndex = spu.get<SaveStates::CBEndIndex>().value;
    captureBuffer.startIndex = spu.get<SaveStates::CBStartIndex>().value;
    capBufVoiceIndex = spu.get<SaveStates::CBVoiceIndex>().value;

    spu.get<SaveStates::SPURam>().copyTo(reinterpret_cast<uint8_t *>(spuMem));
    // A replacement playhead is not saved, so loaded voices play their original samples.
    for (auto &voice : s_chan) voice.overlay.reset();
    spu.get<SaveStates::SPUPorts>().copyTo(reinterpret_cast<uint8_t *>(regArea));

#if 0
// The xa_decode pointer is grabbed here, which is messy. This needs to be fixed up later.
    if (pF->xa.nsamples <= 4032)  // Start XA again.
        playADPCMchannel(&pF->xa);
#endif

    xapGlobal = 0;

    spuIrq = spu.get<SaveStates::SPUIrq>().value;
    const auto &pSpuIrqIn = spu.get<SaveStates::SPUIrqPtr>().value;
    irqAddress = pSpuIrqIn ? pSpuIrqIn + spuRamBase : nullptr;

    for (unsigned i = 0; i < MAXCHAN; i++) {
        const auto &channel = spu.get<SaveStates::Channels>().value[i];
        const auto &data = channel.get<SaveStates::Data>();
        s_chan[i].data = data;
        s_chan[i].adsr.legacy() = channel.get<SaveStates::ADSRInfo>();
        s_chan[i].adsr.ex() = channel.get<SaveStates::ADSRInfoEx>();
        s_chan[i].adpcm.loadFrom(data.get<Chan::s_1>(), data.get<Chan::s_2>(), data.get<Chan::StartPtr>(),
                                 data.get<Chan::CurrPtr>(), data.get<Chan::LoopPtr>(), spuRamBase,
                                 data.get<Chan::SB>().value.data(), data.get<Chan::SBPos>());
        s_chan[i].volume.loadFrom(data.get<Chan::LeftVolume>(), data.get<Chan::RightVolume>(),
                                  data.get<Chan::LeftVolRaw>(), data.get<Chan::RightVolRaw>());
        s_chan[i].interp.loadFrom(data.get<Chan::SB>().value.data(), data.get<Chan::spos>(), data.get<Chan::sinc>());
        s_chan[i].data.get<Chan::Mute>().value = false;
        s_chan[i].data.get<Chan::Solo>().value = false;
        s_chan[i].data.get<Chan::IrqDone>().value = 0;
    }

    spuAddr = spu.get<SaveStates::SPUAddr>().value;
    spuCtrl = spu.get<SaveStates::SPUCtrl>().value;
    // Only the IRQ9 flag and the capture-half bit are the SPU's own; older states can carry
    // whatever a guest wrote over the rest.
    spuStat = spu.get<SaveStates::SPUStat>().value & (StatusFlags::IRQFlag | StatusFlags::CBIndex);
    spuEndx = spu.get<SaveStates::SPUEndx>().value & 0xffffff;

    m_noise.loadFrom(spu.get<SaveStates::SPUNoiseClock>(), spu.get<SaveStates::SPUNoiseCount>(),
                     spu.get<SaveStates::SPUNoiseVal>());

    // Repair some globals. The reverb unit is reset first so no filter history or L/R parity from
    // before the load survives; the register replay below rebuilds its configuration.
    m_reverb.reset();
    for (unsigned i = 0; i <= 62; i += 2) writeRegister(H_Reverb + i, regArea[(H_Reverb + i - 0xc00) >> 1]);
    writeRegister(H_SPUReverbAddr, regArea[(H_SPUReverbAddr - 0xc00) >> 1]);
    writeRegister(H_SPUrvolL, regArea[(H_SPUrvolL - 0xc00) >> 1]);
    writeRegister(H_SPUrvolR, regArea[(H_SPUrvolR - 0xc00) >> 1]);
    replayMixVolumes(spu.get<SaveStates::SPUMainVolWritten>().value);

    writeRegister(H_SPUctrl, regArea[(H_SPUctrl - 0xc00) >> 1]);
    // Fix to prevent new interpolations from crashing.
    for (unsigned i = 0; i < MAXCHAN; i++) s_chan[i].interp.resetAfterLoad();

    // Repair LDChen's ADSR changes: replay each voice's two ADSR registers, and nothing else.
    // Replaying key on or the data port here would retrigger voices and write sound RAM.
    if (spuAddr < 0x7ffff) {
        for (unsigned i = 0; i < MAXCHAN; i++) {
            writeRegister(0x1f801c08 + (i << 4), regArea[(i << 3) + 4]);
            writeRegister(0x1f801c0a + (i << 4), regArea[(i << 3) + 5]);
        }
    }

    // The replays above were queued: apply them now, with the mixer stopped.
    drainEventsNow();
    rebuildShadows();
    loadEnvelopeCheckpoints(spu.get<SaveStates::EnvelopeCheckpoints>());

    // Start sound processing again.
    if (running) SetupThread();
}

void PCSX::SPU::impl::loadEnvelopeCheckpoints(const SaveStates::EnvelopeCheckpoints &checkpoints) {
    const uint64_t now = cycleToSample(PCSX::g_emulator->m_cpu->m_regs.cycle);
    for (unsigned i = 0; i < MAXCHAN; i++) {
        auto &cp = m_envelopeCheckpoint[i];
        cp = EnvelopeCheckpoint{};
        if (checkpoints.count == MAXCHAN) {
            const auto &in = checkpoints.value[i];
            cp.keyOnCycle = in.get<SaveStates::ECPKeyOnCycle>().value;
            cp.serviceSample = in.get<SaveStates::ECPServiceSample>().value;
            cp.keyOffCycle = in.get<SaveStates::ECPKeyOffCycle>().value;
            cp.keyedOn = in.get<SaveStates::ECPKeyedOn>().value;
            cp.cachedSample = in.get<SaveStates::ECPCachedSample>().value;
            cp.cachedState = in.get<SaveStates::ECPCachedState>().value;
            cp.cachedVol = in.get<SaveStates::ECPCachedVol>().value;
            cp.cachedOn = in.get<SaveStates::ECPCachedOn>().value;
            cp.block = in.get<SaveStates::ECPBlock>().value;
            cp.loop = in.get<SaveStates::ECPLoop>().value;
            cp.ignoreLoop = in.get<SaveStates::ECPIgnoreLoop>().value;
            cp.left = in.get<SaveStates::ECPLeft>().value;
            cp.pos = in.get<SaveStates::ECPPos>().value;
            cp.pitchStep = in.get<SaveStates::ECPPitchStep>().value;
            cp.ended = in.get<SaveStates::ECPEnded>().value;
            cp.untracked = in.get<SaveStates::ECPUntracked>().value;
            cp.keyOnBlock = in.get<SaveStates::ECPKeyOnBlock>().value;
            cp.keyOnLoop = in.get<SaveStates::ECPKeyOnLoop>().value;
            continue;
        }
        // A state saved without the walk: re-anchor it on the restored mixer envelope, one step
        // taken at the load sample. The ADPCM cursor is unknown, so the voice is not walked for
        // its end block until the next KEY ON.
        if (!s_chan[i].data.get<Chan::On>().value) continue;
        const auto &ex = s_chan[i].adsr.ex();
        cp.keyedOn = true;
        cp.serviceSample = now >= AdpcmDecoder::kKeyOnSilentSamples ? now - AdpcmDecoder::kKeyOnSilentSamples : 0;
        cp.cachedSample = 1;
        cp.cachedState = ex.get<exState>().value;
        cp.cachedVol = ex.get<exEnvelopeVol>().value;
        cp.cachedOn = true;
        cp.untracked = true;
    }
}

void PCSX::SPU::impl::saveEnvelopeCheckpoints(SaveStates::EnvelopeCheckpoints &checkpoints) {
    for (unsigned i = 0; i < MAXCHAN; i++) {
        const auto &cp = m_envelopeCheckpoint[i];
        auto &out = checkpoints.value[i];
        out.get<SaveStates::ECPKeyOnCycle>().value = cp.keyOnCycle;
        out.get<SaveStates::ECPServiceSample>().value = cp.serviceSample;
        out.get<SaveStates::ECPKeyOffCycle>().value = cp.keyOffCycle;
        out.get<SaveStates::ECPKeyedOn>().value = cp.keyedOn;
        out.get<SaveStates::ECPCachedSample>().value = cp.cachedSample;
        out.get<SaveStates::ECPCachedState>().value = cp.cachedState;
        out.get<SaveStates::ECPCachedVol>().value = cp.cachedVol;
        out.get<SaveStates::ECPCachedOn>().value = cp.cachedOn;
        out.get<SaveStates::ECPBlock>().value = cp.block;
        out.get<SaveStates::ECPLoop>().value = cp.loop;
        out.get<SaveStates::ECPIgnoreLoop>().value = cp.ignoreLoop;
        out.get<SaveStates::ECPLeft>().value = cp.left;
        out.get<SaveStates::ECPPos>().value = cp.pos;
        out.get<SaveStates::ECPPitchStep>().value = cp.pitchStep;
        out.get<SaveStates::ECPEnded>().value = cp.ended;
        out.get<SaveStates::ECPUntracked>().value = cp.untracked;
        out.get<SaveStates::ECPKeyOnBlock>().value = cp.keyOnBlock;
        out.get<SaveStates::ECPKeyOnLoop>().value = cp.keyOnLoop;
    }
    // Only deserialisation counts elements; load() reads a zero count as a state saved without them.
    checkpoints.count = MAXCHAN;
}

void PCSX::SPU::impl::replayMixVolumes(uint32_t writtenMask) {
    // Replay only the sides software wrote, so an unwritten side stays at unity. A state saved
    // without a written mask (presence bit clear) treats a nonzero register as written.
    struct Side {
        uint32_t reg;
        uint32_t written;
        uint32_t present;
    };
    static constexpr Side c_sides[] = {{H_SPUmvolL, 1, 4}, {H_SPUmvolR, 2, 4}, {H_CDLeft, 8, 32}, {H_CDRight, 16, 32}};
    m_mainVolume.reset();
    m_mainLeftWritten = m_mainRightWritten = false;
    m_cdVolumeLeft = m_cdVolumeRight = kCdVolumeUnity;
    m_cdLeftWritten = m_cdRightWritten = false;
    for (const auto &side : c_sides) {
        const uint16_t value = regArea[(side.reg - 0xc00) >> 1];
        const bool written = (writtenMask & side.present) ? (writtenMask & side.written) != 0 : value != 0;
        if (written) writeRegister(side.reg, value);
    }
}

// Bits 0-1: main volume sides written, bit 2: that mask is present. Bits 3-4: the same for CD
// volume, with bit 5 its presence bit. replayMixVolumes reads it back.
uint32_t PCSX::SPU::impl::mixVolumeWrittenMask() const {
    uint32_t mask = 4 | 32;
    if (m_mainLeftWritten) mask |= 1;
    if (m_mainRightWritten) mask |= 2;
    if (m_cdLeftWritten) mask |= 8;
    if (m_cdRightWritten) mask |= 16;
    return mask;
}
