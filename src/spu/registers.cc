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

#include "spu/registers.h"

#include <algorithm>

#include "core/logger.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/spulogger.h"
#include "spu/externals.h"
#include "spu/interface.h"

namespace {
// Hardware runs the SPU at 44100 Hz off the 33.8688 MHz CPU clock: 768 cycles per
// sample, exactly. Derived rather than spelled out so it tracks a reconfigured clock.
uint32_t cyclesPerSample() { return PCSX::g_emulator->m_psxClockSpeed / 44100; }
// The reader's own cycle. Every reconstruction below is evaluated against this and
// nothing else, which is the whole point: the SPU thread's position stops being an
// input to what a CPU read returns.
uint64_t readerCycle() { return PCSX::g_emulator->m_cpu->m_regs.cycle; }
}  // namespace

uint64_t PCSX::SPU::impl::cycleToSample(uint64_t cycle) const { return cycle / cyclesPerSample(); }

uint64_t PCSX::SPU::impl::sampleToCycle(uint64_t sample) const { return sample * cyclesPerSample(); }

// Evaluate voice `ch`'s envelope at `cycle` by stepping a COPY forward from the
// key-on checkpoint. The mixer's own adsr.step() in synthesizeVoice is untouched;
// this is a second, read-only walk that happens to use the same arithmetic.
//
// Cost is O(samples since the last read of this voice), so it is O(1) amortised
// over a polling guest and O(total emulated samples) over a session - the same
// order as the mixer's own envelope work. A single read after a long silence walks
// the whole gap in one call, which is a hitch and not unbounded growth.
uint16_t PCSX::SPU::impl::reconstructEnvelope(int ch, uint64_t cycle) {
    auto& cp = m_envelopeCheckpoint[ch];
    // Never keyed on, or a read stamped before the key-on: hardware reads 0 here.
    if (!cp.keyedOn || cycle < cp.keyOnCycle) return 0;

    // The envelope's first step is the mixer's sample serviceSample + kKeyOnSilentSamples
    // (see AdpcmDecoder::startKeyOn), and a step taken at sample s is visible from cycle
    // s * cyclesPerSample. Everything below counts STEPS TAKEN, not samples elapsed, so
    // the frozen prefix stays out of the cache and out of the key-off comparison.
    const uint64_t firstStep = cp.serviceSample + AdpcmDecoder::kKeyOnSilentSamples;
    const uint64_t now = cycleToSample(cycle);
    const uint64_t steps = now >= firstStep ? now - firstStep + 1 : 0;

    // Copy the CPU-side shadow of the envelope for its CONFIGURATION only - attack/
    // decay/sustain/release rates, the exponential flags, the sustain level - as of
    // the last register write, which the mixer's own copy may not have applied yet.
    // Every dynamic field is overwritten below.
    AdsrEnvelope walk = m_adsrShadow[ch];

    if (steps < cp.cachedSample) {
        // Cycles went backwards under us. Rebuild from key-on rather than trust it.
        // Pitch changes since key-on are lost here, the walk replays the current one.
        walk.keyOn();
        cp.cachedSample = 0;
        cp.cachedState = walk.ex().get<exState>().value;
        cp.cachedVol = 0;
        cp.cachedOn = true;
        resetAdpcmWalk(ch);
    }

    // An end block without repeat stopped the voice: Release, envelope 0, and nothing
    // the envelope does afterwards can bring it back short of another KEY ON.
    if (cp.ended) {
        cp.cachedSample = steps;
        return 0;
    }

    walk.ex().get<exState>().value = cp.cachedState;
    walk.ex().get<exEnvelopeVol>().value = cp.cachedVol;

    // Key-off is a state transition at a known cycle, so the walk can cross it.
    // Expressed in STEPS, not samples, so it stays aligned with the frozen prefix.
    uint64_t releaseAt = UINT64_MAX;
    if (cp.keyOffCycle > cp.keyOnCycle) {
        // The mixer applies the key-off before its sample sampleAfter(keyOffCycle).
        const uint64_t off = sampleAfter(cp.keyOffCycle);
        releaseAt = off > firstStep ? off - firstStep : 0;
    }

    // A pitch-modulated voice's rate depends on the voice below it and is not
    // modelled. Once a voice has been a modulation target since its key-on, its
    // cursor position is unknown, so it stops being walked and keeps reading its
    // envelope until the next KEY ON.
    if (m_fmodShadow[ch] == 1) cp.untracked = true;
    const bool walkAdpcm = !cp.untracked;

    bool on = cp.cachedOn;
    for (uint64_t s = cp.cachedSample; s < steps; s++) {
        if (walkAdpcm && adpcmWalkReachedStop(cp)) {
            cp.ended = true;
            cp.cachedSample = steps;
            return 0;
        }
        walk.step(s >= releaseAt, on, firstStep + s);
        if (walkAdpcm) cp.pos += cp.pitchStep;
    }

    cp.cachedSample = steps;
    cp.cachedState = walk.ex().get<exState>().value;
    cp.cachedVol = walk.ex().get<exEnvelopeVol>().value;
    cp.cachedOn = on;
    return (uint16_t)cp.cachedVol;
}

// Advance the walk's ADPCM cursor the way synthesizeVoice does before an envelope
// step: consume every sample the pitch counter owes, reading the flag byte of each
// new block straight out of SPU RAM. Returns true when a sample is owed past an end
// block without repeat, which is where the mixer turns the voice off.
bool PCSX::SPU::impl::adpcmWalkReachedStop(EnvelopeCheckpoint& cp) {
    while (cp.pos >= 0x10000) {
        if (cp.left == 0) {
            if (cp.block == EnvelopeCheckpoint::kStopped) return true;
            const uint8_t flags = spuRamBase[cp.block + 1];
            uint32_t next = cp.block + 16;
            if ((flags & 4) && !cp.ignoreLoop) cp.loop = cp.block;
            if (flags & 1) {
                const bool repeat = flags == 3 && cp.loop != EnvelopeCheckpoint::kNoLoop;
                next = repeat ? cp.loop : EnvelopeCheckpoint::kStopped;
            }
            if (next != EnvelopeCheckpoint::kStopped && next >= sizeof(spuMem)) next -= sizeof(spuMem);
            cp.block = next;
            cp.left = 28;
        }
        cp.left--;
        cp.pos -= 0x10000;
    }
    return false;
}

// Put the ADPCM half of the walk back at KEY ON: the start address, an empty block,
// the pitch counter seeded the way Interpolator::keyOn seeds it, and the current pitch.
// The addresses are the ones SoundOn recorded at the key-on write.
void PCSX::SPU::impl::resetAdpcmWalk(int ch) {
    auto& cp = m_envelopeCheckpoint[ch];
    cp.block = cp.keyOnBlock;
    cp.loop = cp.keyOnLoop;
    cp.ignoreLoop = false;
    cp.left = 0;
    cp.pos = settings.get<Interpolation>() >= 2 ? 0x30000 : 0x10000;
    cp.pitchStep = std::max(1, m_pitchShadow[ch] << 4);
    cp.ended = false;
    cp.untracked = m_fmodShadow[ch] == 1;
}

// ADSR time values in milliseconds, by James Higgs; see the end of the adsr.c source for details. The original values
// were ATTACK_MS 514, DECAYHALF_MS 292, DECAY_MS 584, SUSTAIN_MS 450 and RELEASE_MS 446. The timebase here is
// 1.020408 ms rather than 1 ms, so the values below are adjusted.
#define ATTACK_MS 494L
#define DECAYHALF_MS 286L
#define DECAY_MS 572L
#define SUSTAIN_MS 441L
#define RELEASE_MS 437L

// Decode the two ADSR configuration registers into an envelope. Shared by the mixer's
// voice and the CPU-side shadow the ENVX walk reads.
void PCSX::SPU::impl::decodeAdsrLow(AdsrEnvelope& adsr, uint16_t val) {
    adsr.ex().get<exAttackModeExp>().value = (val & ADSRFlags::AttackMode) ? 1 : 0;
    adsr.ex().get<exAttackRate>().value = (val & (ADSRFlags::AttackShiftMask | ADSRFlags::AttackStepMask)) >> 8;
    adsr.ex().get<exDecayRate>().value = (val & ADSRFlags::DecayShiftMask) >> 4;
    adsr.ex().get<exSustainLevel>().value = val & ADSRFlags::SustainLevelMask;

    // The code below is only for debug mode.

    adsr.legacy().get<AttackModeExp>().value = (val & ADSRFlags::AttackMode) ? 1 : 0;

    // Attack time to run from 0 to 100% volume.
    uint32_t lx = (val & ADSRFlags::AttackShiftMask) >> 10;
    // No overflow on shift.
    lx = std::min(31U, lx);
    if (lx) {
        lx = (1 << lx);
        if (lx < 2147483) {
            // Another overflow check.
            lx = (lx * ATTACK_MS) / 10000L;
        } else {
            lx = (lx / 10000L) * ATTACK_MS;
        }
        if (!lx) {
            lx = 1;
        }
    }
    adsr.legacy().get<AttackTime>().value = lx;

    // The ADSR volume runs from 0 to 1024, so scale the sustain level.
    adsr.legacy().get<SustainLevel>().value = (1024 * (val & ADSRFlags::SustainLevelMask)) / 15;

    // Decay.
    lx = (val & ADSRFlags::DecayShiftMask) >> 4;
    // The constant decay value is the time it takes to run from 100% to 0% of volume.
    if (lx) {
        lx = ((1 << (lx)) * DECAY_MS) / 10000L;
        if (!lx) {
            lx = 1;
        }
    }
    // Calculate how long it takes to run from 100% to the wanted sustain level.
    adsr.legacy().get<DecayTime>().value = (lx * (1024 - adsr.legacy().get<SustainLevel>().value)) / 1024;
}

void PCSX::SPU::impl::decodeAdsrHigh(AdsrEnvelope& adsr, uint16_t val) {
    adsr.ex().get<exSustainModeExp>().value = (val & ADSRFlags::SustainMode) ? 1 : 0;
    adsr.ex().get<exSustainIncrease>().value = (val & ADSRFlags::SustainDirection) ? 0 : 1;
    adsr.ex().get<exSustainRate>().value = (val & (ADSRFlags::SustainShiftMask | ADSRFlags::SustainStepMask)) >> 6;
    adsr.ex().get<exReleaseModeExp>().value = (val & ADSRFlags::ReleaseMode) ? 1 : 0;
    adsr.ex().get<exReleaseRate>().value = val & ADSRFlags::ReleaseShiftMask;

    // The code below is only for debug mode.

    adsr.legacy().get<SustainModeExp>().value = (val & ADSRFlags::SustainMode) ? 1 : 0;
    adsr.legacy().get<ReleaseModeExp>().value = (val & ADSRFlags::ReleaseMode) ? 1 : 0;

    // Sustain time. Very high values are often used to hold the volume until a sound stop occurs. Due to
    // the overflow checking below, the highest value reached is 94704 seconds, which is 1578 minutes, or
    // 26 hours. That is assumed to be enough; a stop that does not come within that time span is not
    // accounted for.
    uint32_t lx = (val & ADSRFlags::SustainShiftMask) >> 8;
    lx = std::min(31U, lx);
    if (lx) {
        lx = (1 << lx);
        if (lx < 2147483) {
            lx = (lx * SUSTAIN_MS) / 10000L;
        } else {
            lx = (lx / 10000L) * SUSTAIN_MS;
        }
        if (!lx) {
            lx = 1;
        }
    }
    adsr.legacy().get<SustainTime>().value = lx;

    lx = (val & ADSRFlags::ReleaseShiftMask);
    adsr.legacy().get<ReleaseVal>().value = lx;
    // Release time from 100% to 0%. Note that the release time is adjusted when a stop is coming, so at
    // that point the ADSR volume runs from the current volume to 0%.
    if (lx) {
        lx = (1 << lx);
        if (lx < 2147483) {
            lx = (lx * RELEASE_MS) / 10000L;
        } else {
            lx = (lx / 10000L) * RELEASE_MS;
        }
        if (!lx) {
            lx = 1;
        }
    }
    adsr.legacy().get<ReleaseTime>().value = lx;

    // Add/decrement flag.
    if (val & 0x4000) {
        adsr.legacy().get<SustainModeDec>().value = -1;
    } else {
        adsr.legacy().get<SustainModeDec>().value = 1;
    }
}

////////////////////////////////////////////////////////////////////////
// Write registers: called by the main emulator.
////////////////////////////////////////////////////////////////////////

// The CPU-side half of a register write. Everything a later CPU read or the ENVX walk
// needs is updated here, at the write's own cycle; the rest is queued for the mixer,
// which applies it at the matching sample (applyRegister).
void PCSX::SPU::impl::writeRegister(uint32_t reg, uint16_t val) {
    const uint32_t r = reg & 0xfff;
    const uint64_t cycle = readerCycle();
    publishHorizon(cycle);

    g_emulator->m_spuLogger->logRegWrite(r - 0xc00, val, regArea);
    regArea[(r - 0xc00) >> 1] = val;

    if (r >= 0x0c00 && r < 0x0d80) {
        const int ch = (r >> 4) - 0xc0;
        switch (r & 0x0f) {
            case 0:
                s_chan[ch].volume.setLeft(val, cycleToSample(cycle));
                break;
            case 2:
                s_chan[ch].volume.setRight(val, cycleToSample(cycle));
                break;
            case 4:
                SetPitch(ch, val, cycle);
                break;
            case 8:
                decodeAdsrLow(m_adsrShadow[ch], val);
                break;
            case 10:
                decodeAdsrHigh(m_adsrShadow[ch], val);
                break;
            case 6:
                // Logged post-truncation, with the raw register value kept alongside, so a game
                // setting a start address off the block grid stays visible.
                g_emulator->m_spuLogger->log(SPULogger::Kind::VoiceStart, (uint32_t)((val << 3) & ~0xf), 0, val, ch);
                break;
            case 14: {
                g_emulator->m_spuLogger->log(SPULogger::Kind::VoiceLoop, (uint32_t)((val << 3) & ~0xf), 0, val, ch);
                // Bring the ENVX walk up to this write before it sees the new address.
                auto& cp = m_envelopeCheckpoint[ch];
                if (cp.keyedOn) {
                    reconstructEnvelope(ch, cycle);
                    cp.loop = (uint32_t)((val << 3) & ~0xf);
                    // Same rule as the mixer: the write holds once the third header fetch is past.
                    cp.ignoreLoop |= sampleAfter(cycle) >= cp.serviceSample + AdpcmDecoder::kKeyOnHeaderFetches;
                }
            } break;
        }
    } else {
        switch (r) {
            case H_SPUaddr:
                spuAddr = (uint32_t)val * 8;
                g_emulator->m_spuLogger->log(SPULogger::Kind::TransferAddr, spuAddr, 0, val);
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Data Transfer Address = %04x\n", val);
                return;

            case H_SPUdata: {
                // The mixer reads sound RAM as it plays, so it has to be exactly here when the
                // word changes under it.
                catchUp(cycle);
                std::lock_guard<std::mutex> lock(cbMtx);
                g_emulator->m_spuLogger->log(SPULogger::Kind::PIOWrite, spuAddr, 2, val);
                spuMem[spuAddr >> 1] = val;
                spuAddr += 2;
                if (spuAddr > 0x7ffff) {
                    spuAddr = 0;
                }
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Data Transfer Fifo = %04x\n", val);
                return;
            }

            case H_SPUctrl:
                m_ctrlShadow = val;
                m_ctrlWritesPending++;
                break;

            case H_SPUirqAddr:
                spuIrq = val;
                break;

            // SPUSTAT is read-only: a write changes nothing, the IRQ9 flag included.
            case H_SPUstat:
                PCSX::PSXSPU_LOGGER::Log("SPU.write, STAT = %04x, (read-only)\n", val);
                return;
            case H_SPUReverbAddr:
                // The mixer applies the base later; log the byte offset it will settle on.
                g_emulator->m_spuLogger->log(SPULogger::Kind::ReverbAddr,
                                             (val == 0xffff || val <= 0x200) ? 0 : (uint32_t)val << 3, 0, val);
                break;

            // CD audio volume: the mixer applies it to the CD input (applyRegister).
            case H_CDLeft:
                iLeftXAVol = val & 0x7fff;
                if (cddavCallback) {
                    cddavCallback(0, val);
                }
                break;

            case H_CDRight:
                iRightXAVol = val & 0x7fff;
                if (cddavCallback) {
                    cddavCallback(1, val);
                }
                break;

            case H_SPUon1:
                SoundOn(0, 16, val, cycle);
                break;
            case H_SPUon2:
                SoundOn(16, 24, val, cycle);
                break;
            case H_SPUoff1:
                SoundOff(0, 16, val, cycle);
                break;
            case H_SPUoff2:
                SoundOff(16, 24, val, cycle);
                break;
            case H_FMod1:
                FModOn(0, 16, val, cycle);
                break;
            case H_FMod2:
                FModOn(16, 24, val, cycle);
                break;
        }
    }

    pushEvent(cycle, r, val);
}

// The mixer-side half of a register write, run on the mixer thread when the write comes due
// (or on the CPU thread with the mixer stopped).
// The mixer-side half of a main volume or CD volume write. Returns false for any other register.
bool PCSX::SPU::impl::applyMixVolume(uint32_t r, uint16_t val) {
    switch (r) {
        case H_SPUmvolL:
            m_mainVolume.applyLeft(val);
            m_mainLeftWritten = true;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, Main Volume Left = %04x\n", val);
            return true;
        case H_SPUmvolR:
            m_mainVolume.applyRight(val);
            m_mainRightWritten = true;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, Main Volume Right = %04x\n", val);
            return true;
        case H_CDLeft:
            m_cdVolumeLeft = static_cast<int16_t>(val);
            m_cdLeftWritten = true;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, CD Audio Input Volume Left = %04x\n", val);
            return true;
        case H_CDRight:
            m_cdVolumeRight = static_cast<int16_t>(val);
            m_cdRightWritten = true;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, CD Audio Input Volume Right = %04x\n", val);
            return true;
    }
    return false;
}

void PCSX::SPU::impl::applyRegister(uint32_t reg, uint16_t val) {
    const uint32_t r = reg & 0xfff;

    // Check if this is one of the voice configuration registers.
    if (r >= 0x0c00 && r < 0x0d80) {
        // Figure out which voice it is.
        int ch = (r >> 4) - 0xc0;
        switch (r & 0x0f) {
            // Left volume.
            case 0:
                s_chan[ch].volume.applyLeft(val);
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] Set Volume L = %04x\n", ch, val);
                break;
            // Right volume.
            case 2:
                s_chan[ch].volume.applyRight(val);
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] Set Volume R = %04x\n", ch, val);
                break;
            // Pitch.
            case 4:
                SetPitchApply(ch, val);
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] ADPCM Sample Rate = %04x\n", ch, val);
                break;
            // Sample start address.
            case 6:
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] ADPCM Start Address = %04x\n", ch, val);
                // Brain Dead 13: align to a 16-byte boundary.
                s_chan[ch].adpcm.setStart(spuRamBase + (uint32_t)((val << 3) & ~0xf));
                break;
            // Attack/Decay/Sustain/Release (ADSR).
            case 8:
                decodeAdsrLow(s_chan[ch].adsr, val);
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] ADSR(lo) = %04x\n", ch, val);
                break;
            case 10:
                decodeAdsrHigh(s_chan[ch].adsr, val);
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] ADSR(hi) = %04x\n", ch, val);
                break;
            // TODO: emulate the ADSR volume.
            case 12:
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] ADSR Volume = %04x, unimplemented\n", ch, val);
                break;
            // Loop address.
            case 14: {
                // Align to a 16-byte boundary.
                s_chan[ch].adpcm.setLoop(spuRamBase + ((uint32_t)((val << 3) & ~0xf)));
                // Until a new voice's third header fetch, the next loop-start flag still wins.
                if (!s_chan[ch].data.get<Chan::New>().value && s_chan[ch].adpcm.repeatWriteLatches()) {
                    s_chan[ch].data.get<Chan::IgnoreLoop>().value = true;
                }
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice[%02i] ADPCM Repeat Address = %04x\n", ch, val);
            } break;
        }

        return;
    }

    if (applyMixVolume(r, val)) return;

    switch (r) {
        case H_SPUctrl:
            // Writing IRQ9 Enable = 0 is the acknowledge: it is the only thing that clears
            // the SPUSTAT flag, and writing it back to 1 is what re-arms the interrupt.
            if (!(val & ControlFlags::IRQEnable)) spuStat &= ~StatusFlags::IRQFlag;
            spuCtrl = val;
            m_ctrlWritesPending--;
            m_noise.setClock((spuCtrl & (ControlFlags::NoiseShiftMask | ControlFlags::NoiseStepMask)) >> 8);
            PCSX::PSXSPU_LOGGER::Log("SPU.write, CTRL = %04x\n", val);
            break;

        case H_SPUReverbAddr:
            if (val == 0xFFFF || val <= 0x200) {
                m_reverb.rvb.StartAddr = m_reverb.rvb.CurrAddr = 0;
            } else {
                const long iv = (uint32_t)val << 2;
                if (m_reverb.rvb.StartAddr != iv) {
                    m_reverb.rvb.StartAddr = (uint32_t)val << 2;
                    m_reverb.rvb.CurrAddr = m_reverb.rvb.StartAddr;
                }
            }
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mBASE = %04x\n", val);
            break;

        case H_SPUirqAddr:
            irqAddress = spuRamBase + ((uint32_t)val << 3);
            PCSX::PSXSPU_LOGGER::Log("SPU.write, IRQ Address = %04x\n", val);
            break;

        case H_SPUrvolL:
            m_reverb.rvb.VolLeft = val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vLOUT = %04x\n", val);
            break;

        case H_SPUrvolR:
            m_reverb.rvb.VolRight = val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vROUT = %04x\n", val);
            break;

        case H_ExtLeft:
            PCSX::PSXSPU_LOGGER::Log("SPU.write, External Audio Input Volume Left = %04x, unimplemented\n", val);
            break;

        case H_ExtRight:
            PCSX::PSXSPU_LOGGER::Log("SPU.write, External Audio Input Volume Right = %04x, unimplemented\n", val);
            break;

        // Voice 0..23 ON/OFF (status) (ENDX), read-only.
        case H_SPUMute1:
            break;

        // Voice 0..23 ON/OFF2 (status) (ENDX), read-only.
        case H_SPUMute2:
            break;

        case H_SPUon1:
            SoundOnApply(0, 16, val);
            break;

        case H_SPUon2:
            SoundOnApply(16, 24, val);
            break;

        case H_SPUoff1:
            SoundOffApply(0, 16, val);
            break;

        case H_SPUoff2:
            SoundOffApply(16, 24, val);
            break;

        case H_FMod1:
            FModOnApply(0, 16, val);
            break;

        case H_FMod2:
            FModOnApply(16, 24, val);
            break;

        case H_Noise1:
            NoiseOn(0, 16, val);
            break;

        case H_Noise2:
            NoiseOn(16, 24, val);
            break;

        case H_RVBon1:
            ReverbOn(0, 16, val);
            break;

        case H_RVBon2:
            ReverbOn(16, 24, val);
            break;

        case H_Reverb + 0:
            m_reverb.rvb.FB_SRC_A = (int16_t)val;

            // Fake reverb: depending on the effect, more or less delay and repeats are applied.
            PCSX::PSXSPU_LOGGER::Log("SPU.write, dAPF1 = %04x\n", val);
            break;

        case H_Reverb + 2:
            m_reverb.rvb.FB_SRC_B = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, dAPF2 = %04x\n", val);
            break;
        case H_Reverb + 4:
            m_reverb.rvb.IIR_ALPHA = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vIIR = %04x\n", val);
            break;
        case H_Reverb + 6:
            m_reverb.rvb.ACC_COEF_A = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vCOMB1 = %04x\n", val);
            break;
        case H_Reverb + 8:
            m_reverb.rvb.ACC_COEF_B = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vCOMB2 = %04x\n", val);
            break;
        case H_Reverb + 10:
            m_reverb.rvb.ACC_COEF_C = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vCOMB3 = %04x\n", val);
            break;
        case H_Reverb + 12:
            m_reverb.rvb.ACC_COEF_D = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vCOMB4 = %04x\n", val);
            break;
        case H_Reverb + 14:
            m_reverb.rvb.IIR_COEF = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vWALL = %04x\n", val);
            break;
        case H_Reverb + 16:
            m_reverb.rvb.FB_ALPHA = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vAPF1 = %04x\n", val);
            break;
        case H_Reverb + 18:
            m_reverb.rvb.FB_X = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vAPF2 = %04x\n", val);
            break;
        case H_Reverb + 20:
            m_reverb.rvb.IIR_DEST_A0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLSAME = %04x\n", val);
            break;
        case H_Reverb + 22:
            m_reverb.rvb.IIR_DEST_A1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRSAME = %04x\n", val);
            break;
        case H_Reverb + 24:
            m_reverb.rvb.ACC_SRC_A0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLCOMB1 = %04x\n", val);
            break;
        case H_Reverb + 26:
            m_reverb.rvb.ACC_SRC_A1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRCOMB1 = %04x\n", val);
            break;
        case H_Reverb + 28:
            m_reverb.rvb.ACC_SRC_B0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLCOMB2 = %04x\n", val);
            break;
        case H_Reverb + 30:
            m_reverb.rvb.ACC_SRC_B1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRCOMB2 = %04x\n", val);
            break;
        case H_Reverb + 32:
            m_reverb.rvb.IIR_SRC_A0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, dLSAME = %04x\n", val);
            break;
        case H_Reverb + 34:
            m_reverb.rvb.IIR_SRC_A1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, dRSAME = %04x\n", val);
            break;
        case H_Reverb + 36:
            m_reverb.rvb.IIR_DEST_B0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLDIFF = %04x\n", val);
            break;
        case H_Reverb + 38:
            m_reverb.rvb.IIR_DEST_B1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRDIFF = %04x\n", val);
            break;
        case H_Reverb + 40:
            m_reverb.rvb.ACC_SRC_C0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLCOMB3 = %04x\n", val);
            break;
        case H_Reverb + 42:
            m_reverb.rvb.ACC_SRC_C1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRCOMB3 = %04x\n", val);
            break;
        case H_Reverb + 44:
            m_reverb.rvb.ACC_SRC_D0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLCOMB4 = %04x\n", val);
            break;
        case H_Reverb + 46:
            m_reverb.rvb.ACC_SRC_D1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRCOMB4 = %04x\n", val);
            break;
        case H_Reverb + 48:
            m_reverb.rvb.IIR_SRC_B1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, dLDIFF = %04x\n", val);
            break;
        case H_Reverb + 50:
            m_reverb.rvb.IIR_SRC_B0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, dRDIFF = %04x\n", val);
            break;
        case H_Reverb + 52:
            m_reverb.rvb.MIX_DEST_A0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLAPF1 = %04x\n", val);
            break;
        case H_Reverb + 54:
            m_reverb.rvb.MIX_DEST_A1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRAPF1 = %04x\n", val);
            break;
        case H_Reverb + 56:
            m_reverb.rvb.MIX_DEST_B0 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mLAPF2 = %04x\n", val);
            break;
        case H_Reverb + 58:
            m_reverb.rvb.MIX_DEST_B1 = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, mRAPF2 = %04x\n", val);
            break;
        case H_Reverb + 60:
            m_reverb.rvb.IN_COEF_L = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vLIN = %04x\n", val);
            break;
        case H_Reverb + 62:
            m_reverb.rvb.IN_COEF_R = (int16_t)val;
            PCSX::PSXSPU_LOGGER::Log("SPU.write, vRIN = %04x\n", val);
            break;
    }
}

////////////////////////////////////////////////////////////////////////
// Read register: called by the main emulator.
////////////////////////////////////////////////////////////////////////

uint16_t PCSX::SPU::impl::readRegister(uint32_t reg) {
    const uint32_t r = reg & 0xfff;

    // VOLXL/VOLXR: the current per-voice volume, answered at the reader's cycle.
    if (r >= 0x0e00 && r < 0x0e60) {
        const int ch = (r - 0x0e00) >> 2;
        const uint64_t now = cycleToSample(readerCycle());
        const uint16_t volx = (r & 2) ? s_chan[ch].volume.currentRight(now) : s_chan[ch].volume.currentLeft(now);
        PCSX::PSXSPU_LOGGER::Log("SPU.read, Voice[%02i] Current Volume %c = %04x\n", ch, (r & 2) ? 'R' : 'L', volx);
        return volx;
    }

    if (r >= 0x0c00 && r < 0x0d80) {
        switch (r & 0x0f) {
            // Get the ADSR volume.
            case 12: {
                const int ch = (r >> 4) - 0xc0;

                // ENVX is the live envelope level and nothing else (0..7FFFh). A voice that
                // has been keyed on but has not stepped its envelope yet reads 0, exactly
                // like an idle one - software keys on and then spins on ENVX until it goes
                // nonzero to synchronise with the SPU's own sample clock. The old "started
                // but not processed, return 1" fudge broke that: the spin exited at key-on
                // instead of at the first envelope step, so every subsequent read landed one
                // envelope period early.
                //
                // Answered at the READER'S cycle, not from the live value. The live
                // value is wherever the SPU thread's batch loop happened to be, which
                // is why the same assertion reads a different number every run.
                const uint16_t envx = reconstructEnvelope(ch, readerCycle());
                PCSX::PSXSPU_LOGGER::Log("SPU.read, Voice[%02i] Current ADSR Volume = %04x\n", ch, envx);
                return envx;
            }

            // Get the loop address.
            case 14: {
                const int ch = (r >> 4) - 0xc0;
                // The mixer latches this from the sample data as it plays.
                catchUp(readerCycle());
                if (s_chan[ch].adpcm.loop() == nullptr) {
                    PCSX::PSXSPU_LOGGER::Log("SPU.read, Voice[%02i] ADPCM Repeat Address = 00000\n", ch);
                    return 0;
                }
                PCSX::PSXSPU_LOGGER::Log("SPU.read, Voice[%02i] ADPCM Repeat Address = %04x\n", ch,
                                         (uint16_t)((s_chan[ch].adpcm.loop() - spuRamBase) >> 3));
                return (uint16_t)((s_chan[ch].adpcm.loop() - spuRamBase) >> 3);
            }
        }
    }

    switch (r) {
        // ENDX low 16 voices (read-only).
        // ENDX is latched by the mixer as each voice reads an end block.
        case H_SPUMute1:
            catchUp(readerCycle());
            return (uint16_t)(spuEndx & 0xffff);

        // ENDX high 8 voices (read-only).
        case H_SPUMute2:
            catchUp(readerCycle());
            return (uint16_t)((spuEndx >> 16) & 0xff);

        case H_SPUctrl: {
            const uint16_t ctrl = readCtrl();
            PCSX::PSXSPU_LOGGER::Log("SPU.read, CTRL = %04x\n", ctrl);
            return ctrl;
        }

        case H_SPUstat: {
            // Bit 11 (CBIndex) is reconstructed on the SAME clock as ENVX above, and
            // that pairing is the load-bearing part. Guests sync their ENVX polls on
            // this bit's edges, so if the two come off different clocks the read
            // INSTANT is chosen by one ruler and the read VALUE computed against
            // another. The SPU thread's own copy of this bit advances a whole NSSIZE
            // batch at a time (spu.cc, capBufVoiceIndex), so an edge it publishes can
            // sit up to 45 samples from the true 0x100-sample boundary.
            // Bit 6 (the IRQ9 flag) is the mixer's. If a CTRL write, which may be the acknowledge
            // that clears it, has not reached the mixer yet, wait for it. Otherwise SPUSTAT is
            // polled freely.
            if (m_ctrlWritesPending.load()) catchUp(readerCycle());
            const uint64_t half = cycleToSample(readerCycle()) % kCaptureRegionSamples;
            uint16_t stat = (spuStat & ~StatusFlags::SPUModeMask) | (readCtrl() & StatusFlags::SPUModeMask);
            if (half & kCaptureHalfMarker) {
                stat |= StatusFlags::CBIndex;
            } else {
                stat &= ~StatusFlags::CBIndex;
            }
            PCSX::PSXSPU_LOGGER::Log("SPU.read, STAT = %04x\n", stat);
            return stat;
        }

        case H_SPUaddr:
            PCSX::PSXSPU_LOGGER::Log("SPU.read, Data Transfer Address = %04x\n", (uint16_t)(spuAddr >> 3));
            return (uint16_t)(spuAddr >> 3);

        case H_SPUdata: {
            catchUp(readerCycle());
            std::lock_guard<std::mutex> lock(cbMtx);
            uint16_t s = spuMem[spuAddr >> 1];
            spuAddr += 2;

            if (spuAddr > 0x7ffff) {
                spuAddr = 0;
            }
            PCSX::PSXSPU_LOGGER::Log("SPU.read, Data Transfer Fifo = %04x\n", s);
            return s;
        }

        case H_SPUirqAddr:
            PCSX::PSXSPU_LOGGER::Log("SPU.read, Data Transfer Fifo = %04x\n", spuIrq);
            return spuIrq;
    }

    PCSX::PSXSPU_LOGGER::Log("SPU.read, regArea[%03x] = %04x\n", r, regArea[(r - 0xc00) >> 1]);
    return regArea[(r - 0xc00) >> 1];
}

// Start ADSR for voices [start, end] depending on val. CPU side: stamp the ENVX walk.
void PCSX::SPU::impl::SoundOn(int start, int end, uint16_t val, uint64_t cycle) {
    if ((val & ((1u << (end - start)) - 1)) == 0) return;
    // The walk starts from the voice's start and repeat addresses as they stand at this
    // write. The repeat address is latched by the mixer from the sample data, so bring the
    // mixer up to here before reading either.
    catchUp(cycle);
    for (int ch = start; ch < end; ch++, val >>= 1) {
        // The start address has to be set before key on.
        if ((val & 1) && s_chan[ch].adpcm.start()) {
            // Everything the read-time reconstruction does is measured from this cycle.
            auto& cp = m_envelopeCheckpoint[ch];
            const uint8_t* startAddr = s_chan[ch].adpcm.start();
            const uint8_t* loopAddr = s_chan[ch].adpcm.loop();
            cp.keyOnBlock = (uint32_t)(startAddr - spuRamBase);
            cp.keyOnLoop = loopAddr ? (uint32_t)(loopAddr - spuRamBase) : EnvelopeCheckpoint::kNoLoop;
            cp.keyOnCycle = cycle;
            cp.serviceSample = AdpcmDecoder::keyOnServiceSample(sampleAfter(cycle));
            cp.keyOffCycle = 0;
            cp.keyedOn = true;
            cp.cachedSample = 0;
            cp.cachedState = 0;  // AdsrEnvelope's Attack
            cp.cachedVol = 0;
            cp.cachedOn = true;
            resetAdpcmWalk(ch);
            // One entry per voice actually keyed, carrying the address it is about to decode
            // from: the only sample entry point that is ground truth rather than inference.
            g_emulator->m_spuLogger->log(SPULogger::Kind::KeyOn, cp.keyOnBlock, 0, 1, ch);
        }
    }
}

// Mixer side of KEY ON: the voice starts at this sample.
void PCSX::SPU::impl::SoundOnApply(int start, int end, uint16_t val) {
    for (int ch = start; ch < end; ch++, val >>= 1) {
        if ((val & 1) && s_chan[ch].adpcm.start()) {
            s_chan[ch].data.get<Chan::IgnoreLoop>().value = false;
            s_chan[ch].data.get<Chan::New>().value = true;
            // Bitfield for faster testing.
            newChannelMask |= (1 << ch);
            PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i ON\n", ch);
        }
    }
}

// Stop sound for voices [start, end] if the corresponding bit in val is set to 1.
void PCSX::SPU::impl::SoundOffApply(int start, int end, uint16_t val) {
    for (int ch = start; ch < end; ch++, val >>= 1) {
        if (val & 1) {
            if (s_chan[ch].data.get<Chan::Stop>().value != true) {
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i OFF\n", ch);
            }
            s_chan[ch].data.get<Chan::Stop>().value = true;
        }
    }
}

// CPU side of KEY OFF.
void PCSX::SPU::impl::SoundOff(int start, int end, uint16_t val, uint64_t cycle) {
    for (int ch = start; ch < end; ch++, val >>= 1) {
        if (val & 1) {
            // Release is a state transition at a known cycle, so the reconstruction
            // walk can cross it. First key-off after a key-on wins; a repeat is not
            // a second transition. SoundOn clears this.
            auto& cp = m_envelopeCheckpoint[ch];
            if (cp.keyedOn && cp.keyOffCycle == 0) {
                cp.keyOffCycle = cycle;
                // The decode cursor belongs to the mixer thread; the released sample is named
                // by the start address it was keyed on with.
                g_emulator->m_spuLogger->log(SPULogger::Kind::KeyOff, cp.keyOnBlock, 0, 0, ch);
            }
        }
    }
}

// Set pitch modulation for voices [start, end] depending on val.
// CPU side of pitch modulation: the shadow the ENVX walk reads.
void PCSX::SPU::impl::FModOn(int start, int end, uint16_t val, uint64_t cycle) {
    for (int ch = start; ch < end; ch++, val >>= 1) {
        if (val & 1) {
            // Pitch modulation does not work for voice 0.
            if (ch > 0) {
                // Sound channel. Its ADPCM position stops being knowable to the ENVX walk,
                // so bring the walk up to this write first: an end block reached before it
                // still stops the voice.
                auto& cp = m_envelopeCheckpoint[ch];
                if (cp.keyedOn && !cp.untracked && m_fmodShadow[ch] != 1) reconstructEnvelope(ch, cycle);
                m_fmodShadow[ch] = 1;
                if (cp.keyedOn) cp.untracked = true;
                m_fmodShadow[ch - 1] = 2;
            }
        } else {
            // A target that was modulated during this play stays untracked until its next KEY ON.
            m_fmodShadow[ch] = 0;
        }
    }
}

// Set pitch modulation for voices [start, end] depending on val.
void PCSX::SPU::impl::FModOnApply(int start, int end, uint16_t val) {
    for (int ch = start; ch < end; ch++, val >>= 1) {
        // Check if modulation should be enabled for this voice.
        if (val & 1) {
            // Pitch modulation does not work for voice 0.
            if (ch > 0) {
                if (s_chan[ch].data.get<Chan::FMod>().value != 1) {
                    PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i Pitch Modulation ON\n", ch);
                }
                // Sound channel.
                s_chan[ch].data.get<Chan::FMod>().value = 1;
                // Frequency channel.
                s_chan[ch - 1].data.get<Chan::FMod>().value = 2;
            }
        } else {
            if (s_chan[ch].data.get<Chan::FMod>().value != 0) {
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i Pitch Modulation OFF\n", ch);
            }
            // Turn off frequency modulation.
            s_chan[ch].data.get<Chan::FMod>().value = 0;
        }
    }
}

// Set voices [start, end] to output ADPCM or noise, depending on whether the corresponding bit in val is set to 0 or
// 1 respectively.
void PCSX::SPU::impl::NoiseOn(int start, int end, uint16_t val) {
    for (int ch = start; ch < end; ch++, val >>= 1) {
        if (val & 1) {
            if (s_chan[ch].data.get<Chan::Noise>().value != true) {
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i Noise ON\n", ch);
            }
            s_chan[ch].data.get<Chan::Noise>().value = true;
        } else {
            if (s_chan[ch].data.get<Chan::Noise>().value != false) {
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i Noise OFF\n", ch);
            }
            s_chan[ch].data.get<Chan::Noise>().value = false;
        }
    }
}

// Hardware saturates the pitch register at 3FFFh.
static int clampPitch(uint16_t val) { return val > 0x3fff ? 0x3fff : val; }

// CPU side of a pitch write. The ENVX walk runs the old pitch up to this write and the new
// one after it.
void PCSX::SPU::impl::SetPitch(int ch, uint16_t val, uint64_t cycle) {
    auto& cp = m_envelopeCheckpoint[ch];
    if (cp.keyedOn) reconstructEnvelope(ch, cycle);
    const int NP = clampPitch(val);
    m_pitchShadow[ch] = NP;
    cp.pitchStep = std::max(1, NP << 4);
}

// Set the pitch for voice ch.
void PCSX::SPU::impl::SetPitchApply(int ch, uint16_t val) {
    int NP = clampPitch(val);
    s_chan[ch].data.get<Chan::RawPitch>().value = NP;

    // Calculate the frequency.
    NP = (44100L * NP) / 4096L;
    // Some security.
    if (NP < 1) {
        NP = 1;
    }
    // Store the frequency.
    s_chan[ch].data.get<Chan::ActFreq>().value = NP;
}

// Enable/disable reverb for voices [start, end] depending on val.
void PCSX::SPU::impl::ReverbOn(int start, int end, uint16_t val) {
    for (int ch = start; ch < end; ch++, val >>= 1) {
        if (val & 1) {
            if (s_chan[ch].data.get<Chan::Reverb>().value != true) {
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i Reverb ON\n", ch);
            }
            s_chan[ch].data.get<Chan::Reverb>().value = true;

        } else {
            if (s_chan[ch].data.get<Chan::Reverb>().value != false) {
                PCSX::PSXSPU_LOGGER::Log("SPU.write, Voice %02i Reverb OFF\n", ch);
            }
            s_chan[ch].data.get<Chan::Reverb>().value = false;
        }
    }
}

// SPUCNT as the CPU sees it: the last value written. Nothing on the SPU side changes it.
uint16_t PCSX::SPU::impl::readCtrl() { return m_ctrlShadow; }

void PCSX::SPU::impl::rebuildShadows() {
    for (unsigned ch = 0; ch < MAXCHAN; ch++) {
        m_adsrShadow[ch] = s_chan[ch].adsr;
        m_pitchShadow[ch] = s_chan[ch].data.get<Chan::RawPitch>().value;
        m_fmodShadow[ch] = s_chan[ch].data.get<Chan::FMod>().value;
    }
    m_ctrlShadow = spuCtrl;
    m_ctrlWritesPending = 0;
}
