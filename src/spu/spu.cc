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

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "spu/adsr.h"
#include "spu/externals.h"
#include "spu/interface.h"
#include "supportpsx/spusample.h"

namespace {
// The ADSR step returns the full 15-bit (0..0x7fff) envelope volume; the
// enveloped sample is `sample * envelope >> 15`, exactly as the hardware applies
// it (a signed arithmetic shift, SAR 15).
constexpr int kAdsrEnvelopeShift = 15;
// Per-voice volume is a 0..0x3fff level with 0x4000 as unity.
constexpr int kVoiceVolumeUnity = 0x4000;
// The capture area mirrors are 0x200 samples each; voice 1 lands at +0x400 and
// voice 3 at +0x600 (half-word sample indices into spuMem). The write pointer
// wraps every 0x200 samples and bit 11 of SPUSTAT tracks which half it is in.
// kCaptureRegionSamples / kCaptureHalfMarker moved to impl in interface.h - the
// SPUSTAT read path reconstructs bit 11 from them too, and two copies of a
// period would rot apart silently.
constexpr int kCaptureVoice1Offset = 0x400;
constexpr int kCaptureVoice3Offset = 0x600;
// The post-ADSR sample saturates to the signed 16-bit capture cell before it lands in
// the capture mirror; anything wider would wrap on the uint16_t store.
constexpr int kCaptureSampleMin = -32768;
constexpr int kCaptureSampleMax = 32767;
// The final stereo mix is clamped to this symmetric signed-16-bit range.
constexpr int kMixSampleClamp = 32767;
}  // namespace

// Called by the main thread to set up a new sound on a channel.
inline void PCSX::SPU::impl::StartSound(SPUCHAN *voice) {
    voice->adsr.keyOn();
    m_reverb.start(voice, spuCtrl);

    // Rewind the decode cursor to the sample start and clear the IIR history.
    voice->adpcm.keyOn();
    // Key-on reaches the mixer at the head of this chunk; see AdpcmDecoder::startKeyOn.
    voice->adpcm.startKeyOn(m_mixPos);

    // Force a block decode on the first sample.
    voice->adpcm.emptyBuffer();

    // Initialize the channel flags.
    voice->data.get<Chan::New>().value = false;
    voice->data.get<Chan::Stop>().value = false;
    voice->data.get<Chan::On>().value = true;

    voice->interp.keyOn(settings.get<Interpolation>());

    startOverlay(voice);
}

// Look the sample being keyed on up in the overlay pack, by the signature of its blocks.
void PCSX::SPU::impl::startOverlay(SPUCHAN *voice) {
    voice->overlay.reset();
    auto overlay = getOverlay();
    uint8_t *start = voice->adpcm.start();
    if (!overlay || !start) return;
    const auto extent = SPUSample::walk(spuRamBase, uint32_t(start - spuRamBase));
    if (extent.truncated) return;
    const auto *entry = overlay->find(SPUSample::signature(spuRamBase, extent));
    if (entry) voice->overlay.start(std::move(overlay), entry);
}

////////////////////////////////////////////////////////////////////////
// Helpers.
////////////////////////////////////////////////////////////////////////

// Both frequency paths end the same way: install the new 16.16 pitch step, and tell the
// interpolator the frequency moved so simple mode recomputes. The zero clamp matters - a
// zero step never advances the pitch counter, so the voice would sit on one sample forever.
inline void PCSX::SPU::impl::setPitchStep(SPUCHAN *voice, int32_t step) {
    voice->interp.setStep(step);
    voice->interp.onFrequencyChanged(settings.get<Interpolation>());
}

inline void PCSX::SPU::impl::VoiceChangeFrequency(SPUCHAN *voice) {
    auto &actFreq = voice->data.get<Chan::ActFreq>().value;
    // Take the new frequency and recompute the pitch step.
    voice->data.get<Chan::UsedFreq>().value = actFreq;
    setPitchStep(voice, voice->data.get<Chan::RawPitch>().value << 4);
}

////////////////////////////////////////////////////////////////////////

inline void PCSX::SPU::impl::FModChangeFrequency(SPUCHAN *voice, int ns) {
    int NP = voice->data.get<Chan::RawPitch>().value;

    NP = ((32768L + fmodInput[ns]) * NP) / 32768L;
    NP = std::clamp(NP, 0x1, 0x3fff);
    // Calculate the frequency.
    NP = (44100L * NP) / (4096L);

    voice->data.get<Chan::ActFreq>().value = NP;
    voice->data.get<Chan::UsedFreq>().value = NP;
    setPitchStep(voice, ((NP / 10) << 16) / 4410);

    fmodInput[ns] = 0;
}

////////////////////////////////////////////////////////////////////////
// Voice 1 and voice 3 mirror their post-ADSR / pre-volume output into the
// capture area (voice 1 at +0x400, voice 3 at +0x600, half-word sample indices
// into spuMem). These helpers fold the per-voice dispatch and the shared write
// cursor bookkeeping; ch selects the voice (1 or 3), other channels and the
// capture-disabled case are no-ops. The cursors are per-batch state shared
// across channels, so they are passed by reference.
////////////////////////////////////////////////////////////////////////

void PCSX::SPU::impl::captureVoiceSilence(int ch, int32_t &capVoice1Index, int32_t &capVoice3Index, int fromSample,
                                          int n) {
    if (ch != 1 && ch != 3) return;
    std::lock_guard<std::mutex> lock(cbMtx);
    if (!mixIrqAddress) return;
    if (ch == 1) {
        for (int c = fromSample; c < n; c++) {
            spuMem[capVoice1Index + kCaptureVoice1Offset] = 0;
            capVoice1Index = (capVoice1Index + 1) % kCaptureRegionSamples;
        }
    } else {
        for (int c = fromSample; c < n; c++) {
            spuMem[capVoice3Index + kCaptureVoice3Offset] = 0;
            capVoice3Index = (capVoice3Index + 1) % kCaptureRegionSamples;
        }
    }
}

void PCSX::SPU::impl::captureVoiceSample(int ch, int32_t &capVoice1Index, int32_t &capVoice3Index, int sample) {
    if (ch != 1 && ch != 3) return;
    std::lock_guard<std::mutex> lock(cbMtx);
    if (!mixIrqAddress) return;
    if (ch == 1) {
        spuMem[capVoice1Index + kCaptureVoice1Offset] = sample;
        capVoice1Index = (capVoice1Index + 1) % kCaptureRegionSamples;
    } else {
        spuMem[capVoice3Index + kCaptureVoice3Offset] = sample;
        capVoice3Index = (capVoice3Index + 1) % kCaptureRegionSamples;
    }
}

////////////////////////////////////////////////////////////////////////
// Synthesize one channel's contribution to this NSSIZE-sample batch: start a
// pending voice, advance and decode its ADPCM stream, interpolate, apply the
// ADSR envelope, mirror voices 1 and 3 into the capture buffer, and either
// accumulate into the stereo mix or, for an FMod source, into fmodInput. Returns
// early when the voice is idle or stops mid-batch. The two capture write
// cursors are shared across the batch, so they are passed by reference.
//
// The body is templated on the two mode axes declared in interface.h and
// synthesizeChannel below is the dispatcher that resolves them. Both live in
// this translation unit and MainThread only ever calls the dispatcher, so no
// explicit instantiations are needed.
////////////////////////////////////////////////////////////////////////

// Raise SPU IRQ9. A match latches the SPUSTAT bit 6 flag and leaves SPUCNT bit 6 set;
// while the flag is set, further matches raise nothing, however many times a voice
// passes the address. Writing SPUCNT bit 6 = 0 is the acknowledge that clears the flag,
// and the next match after the enable is written back to 1 fires again.
void PCSX::SPU::impl::triggerIrq() {
    if (spuStat & StatusFlags::IRQFlag) return;
    spuStat |= StatusFlags::IRQFlag;
    // Notify the main emulator.
    scheduleInterrupt();
}

// Everything that happens at an ADPCM block boundary: decode the next 16-byte block into
// the voice's 28-sample buffer, then the two things that hang off that boundary - the IRQ
// address check, and the loop/stop flag that decides where the cursor goes next. Split out
// of synthesizeChannel because it is the only part of the pitch loop that is not per-sample.
// Returns false when the voice ended on a previous pass and the caller must stop synthesizing
// it. Once it is silent, the readout carries on from the repeat address.
bool PCSX::SPU::impl::decodeNextBlock(int ch, SPUCHAN *voice) {
    // Current decode position.
    uint8_t *cursor = voice->adpcm.curr();
    if (cursor == AdpcmDecoder::kStopped) {
        // kStopped after an end block is a mute still owed to an audible voice. Once the voice
        // is silent the cursor goes to the repeat address, as it does on hardware for every end
        // block: the readout never stops, and a silent voice still raises IRQ9.
        if (voice->data.get<Chan::On>().value || voice->adpcm.loop() == nullptr) return false;
        cursor = voice->adpcm.loop();
    }

    auto &irqDone = voice->data.get<Chan::IrqDone>().value;
    auto &ignoreLoop = voice->data.get<Chan::IgnoreLoop>().value;

    // The decoder owns the predictor/shift parse and the s_1/s_2 IIR history; it hands back
    // the address just past the block and the flag byte.
    const auto decoded = voice->adpcm.decodeBlock(cursor);
    cursor = decoded.blockEnd;
    const int blockFlags = decoded.flags;

    if (spuCtrl & ControlFlags::IRQEnable) {
        const bool addrReached = irqAddress > cursor - 16 && irqAddress <= cursor;
        // Special case: IRQ on the looping address, when the stop/loop flag is set.
        const bool loopAddrReached = (blockFlags & 1) && voice->adpcm.loop() != nullptr &&
                                     irqAddress > voice->adpcm.loop() - 16 && irqAddress <= voice->adpcm.loop();
        if (addrReached || loopAddrReached) {
            // Debug flag.
            irqDone = 1;
            triggerIrq();
        }
    }

    // Latch the loop address.
    if ((blockFlags & 4) && !ignoreLoop) voice->adpcm.setLoop(cursor - 16);

    // Stop/loop flag: this is the last block of the sample.
    if (blockFlags & 1) {
        // ENDX latches on the end flag.
        spuEndx |= 1u << ch;

        // Keep playing only when the flag byte is exactly 3 (loop-end + repeat) and a loop address
        // was latched. Requiring exactly 3 avoids loop hang-ups (e.g. DQ4), and the null-loop guard
        // avoids following an address that was never set. Anything else mutes the voice once this
        // block has played, and kStopped holds that until it does; the jump to the repeat address
        // happens at the top of the next call.
        cursor = (blockFlags != 3 || voice->adpcm.loop() == nullptr) ? AdpcmDecoder::kStopped : voice->adpcm.loop();
    }

    // The address counter is 19 bits wide: a stream with no end flag runs off the top of
    // the 512KiB sound RAM and continues from address 0, rather than out of spuMem.
    if (cursor != AdpcmDecoder::kStopped && cursor >= spuRamBase + sizeof(spuMem)) cursor -= sizeof(spuMem);

    // Store the cursor for the next cycle.
    voice->adpcm.setCurr(cursor);

    return true;
}

// The readout half of a voice that is not being synthesized. Advance the pitch counter
// over the batch and decode blocks as it consumes them, throwing the samples away: what
// is wanted is the block boundary, where the IRQ address check and the ENDX latch live.
// A voice with no pitch programmed never advances, which is the one way psx-spx suggests
// the readout can actually be stopped ("except, probably they CAN be stopped, by setting
// the sample rate to zero?"). A voice silenced by an end block resumes here from its repeat
// address, see decodeNextBlock.
void PCSX::SPU::impl::walkSilentVoice(int ch, SPUCHAN *voice, int n) {
    for (int ns = 0; ns < n; ns++) {
        while (voice->interp.owesSample()) {
            if (voice->adpcm.bufferExhausted() && !decodeNextBlock(ch, voice)) return;
            // Read and discard: only the cursor motion matters here.
            voice->adpcm.takeSample();
            voice->interp.tookSample();
        }
        voice->interp.advance();
    }
}

template <PCSX::SPU::FModRole Role, PCSX::SPU::SampleSource Src>
void PCSX::SPU::impl::synthesizeVoice(int ch, SPUCHAN *voice, int32_t &capVoice1Index, int32_t &capVoice3Index, int n) {
    // Being the frequency-modulator SOURCE decides three things at once: the
    // voice bypasses the resampler, it skips the volume and reverb stage, and
    // its output goes to fmodInput rather than the stereo mix.
    constexpr bool kIsFModSource = Role == FModRole::Source;

    // A source that is off, in its key-on delay, or stops mid-batch writes no sample for
    // those slots; zero the whole batch up front so the target never reads a stale one.
    if constexpr (kIsFModSource) std::fill(std::begin(fmodInput), std::end(fmodInput), 0);

    // The mixing state still lives in the savestate protobuf, so bind it once here
    // instead of spelling the accessor out at every use. Register writes are applied
    // by this thread between chunks, never during one.
    auto &isNew = voice->data.get<Chan::New>().value;
    auto &on = voice->data.get<Chan::On>().value;
    auto &stop = voice->data.get<Chan::Stop>().value;
    auto &sval = voice->data.get<Chan::sval>().value;
    auto &mute = voice->data.get<Chan::Mute>().value;
    auto &solo = voice->data.get<Chan::Solo>().value;
    auto &rvbActive = voice->data.get<Chan::RVBActive>().value;
    auto &actFreq = voice->data.get<Chan::ActFreq>().value;
    auto &usedFreq = voice->data.get<Chan::UsedFreq>().value;

    if (isNew) {
        // Start the new sound.
        StartSound(voice);
        // Clear the new-channel bit.
        newChannelMask &= ~(1 << ch);
    }

    if (!on) {
        // The volume sweep runs whether or not the voice is playing.
        for (int ns = 0; ns < n; ns++) voice->volume.step();
        // Silent is not stopped. "All voices are permanently reading data from SPU RAM -
        // even in Noise mode, even if the Voice Volume is zero, and even if the ADSR
        // pattern has finished the Release period - so even inaudible voices can trigger
        // IRQs" (psx-spx, SPU Interrupt / Voice Interrupt). The noise path below says the
        // same thing about a voice whose samples are discarded, and this is the same case.
        walkSilentVoice(ch, voice, n);
        // Nothing reaches the mix, but the capture mirror keeps filling.
        captureVoiceSilence(ch, capVoice1Index, capVoice3Index, 0, n);
        return;
    }

    // A new PSX frequency was programmed.
    if (actFreq != usedFreq) VoiceChangeFrequency(voice);

    // Collect this chunk of the channel's audio.
    for (int ns = 0; ns < n; ns++) {
        int rawSample;

        voice->volume.step();

        // Key-on timeline: silence with decode, pitch and envelope frozen until the voice is
        // serviced and has fetched its first header three times.
        if (voice->adpcm.startupDelayActive()) {
            const int fetch = voice->adpcm.keyOnAge() - voice->adpcm.keyOnService();
            if (fetch >= 0 && fetch < AdpcmDecoder::kKeyOnHeaderFetches) {
                // Key-on clears this voice's ENDX bit when it is serviced.
                if (fetch == 0) spuEndx &= ~(1u << ch);
                uint8_t *first = voice->adpcm.start();
                if ((first[1] & 4) && !voice->data.get<Chan::IgnoreLoop>().value) voice->adpcm.setLoop(first);
            }
            voice->adpcm.tickStartupDelay();
            sval = 0;
            captureVoiceSample(ch, capVoice1Index, capVoice3Index, 0);
            continue;
        }

        if constexpr (Role == FModRole::Target) {
            // Modulated by the voice below us.
            if (fmodInput[ns]) FModChangeFrequency(voice, ns);
        }

        // A noise voice still walks its ADPCM stream. The decoded samples are
        // thrown away below, but the cursor advance, the IRQ address check and
        // the ENDX latch all hang off the block boundary.
        while (voice->interp.owesSample()) {
            if (voice->adpcm.bufferExhausted() && !decodeNextBlock(ch, voice)) {
                // The voice ran off the end of its sample on a previous pass. It is silent
                // now, but its capture mirror still fills: ns samples are already done this
                // chunk, so write silence for the remaining n-ns.
                on = false;
                voice->adsr.ex().get<exVolume>().value = 0;
                voice->adsr.ex().get<exEnvelopeVol>().value = 0;
                captureVoiceSilence(ch, capVoice1Index, capVoice3Index, ns, n);
                // The sweep keeps running for the rest of the chunk, and so does the readout.
                for (int rest = ns + 1; rest < n; rest++) voice->volume.step();
                walkSilentVoice(ch, voice, n - ns);
                // Done with this channel.
                return;
            }

            rawSample = voice->adpcm.takeSample();

            // Store the value for interpolation.
            voice->interp.storeVal(rawSample, settings.get<Interpolation>(), kIsFModSource,
                                   (spuCtrl & ControlFlags::Mute) != 0);

            voice->interp.tookSample();
        }

        if constexpr (Src == SampleSource::Noise) {
            // Get the noise value.
            rawSample = noiseLevel[ns];
            voice->interp.parkExternalSample(rawSample, settings.get<Interpolation>());
        } else {
            rawSample = voice->interp.getVal(settings.get<Interpolation>(), kIsFModSource);
        }

        // Apply the ADSR envelope (hardware: sample*env>>15).
        const int32_t envelope = voice->adsr.step(stop, on, m_mixPos + ns);
        int32_t mixedSample = (envelope * rawSample) >> kAdsrEnvelopeShift;
        sval = mixedSample;

        // The capture mirror holds the voice 1/3 sample after ADSR but before volume.
        mixedSample = std::clamp(mixedSample, kCaptureSampleMin, kCaptureSampleMax);
        captureVoiceSample(ch, capVoice1Index, capVoice3Index, mixedSample);

        if constexpr (Role == FModRole::Source) {
            // Hand the sample to the voice above us to modulate with.
            fmodInput[ns] = sval;
        } else {
            // An overlaid voice is heard through its replacement from here on; the capture
            // mirror above keeps the original sample.
            if constexpr (Src == SampleSource::Adpcm) {
                if (voice->overlay.active()) {
                    sval = (envelope * voice->overlay.next(voice->interp.step())) >> kAdsrEnvelopeShift;
                }
            }
            // Left/right sound volume (PSX volume goes from 0 to 0x3fff).
            if (mute && !solo) {
                // Debug mute.
                sval = 0;
            } else {
                SSumL[ns] += (sval * voice->volume.left()) / kVoiceVolumeUnity;
                SSumR[ns] += (sval * voice->volume.right()) / kVoiceVolumeUnity;
            }

            // Store for reverb.
            if (rvbActive) m_reverb.store(voice, ns);
        }

        voice->interp.advance();
    }
}

// Read the voice's two mode flags and pick the matching instantiation. This is
// the only place the runtime flags become compile-time axes; everything below it
// sees them as template arguments. Anything other than 1 or 2 in the FMod field
// means the voice is not part of a modulation pair, which is exactly what the
// per-sample equality tests this replaced did with it.
void PCSX::SPU::impl::synthesizeChannel(int ch, SPUCHAN *voice, int32_t &capVoice1Index, int32_t &capVoice3Index,
                                        int n) {
    const bool noise = voice->data.get<Chan::Noise>().value;

    switch (voice->data.get<Chan::FMod>().value) {
        case static_cast<int>(FModRole::Target):
            if (noise) {
                synthesizeVoice<FModRole::Target, SampleSource::Noise>(ch, voice, capVoice1Index, capVoice3Index, n);
            } else {
                synthesizeVoice<FModRole::Target, SampleSource::Adpcm>(ch, voice, capVoice1Index, capVoice3Index, n);
            }
            break;
        case static_cast<int>(FModRole::Source):
            if (noise) {
                synthesizeVoice<FModRole::Source, SampleSource::Noise>(ch, voice, capVoice1Index, capVoice3Index, n);
            } else {
                synthesizeVoice<FModRole::Source, SampleSource::Adpcm>(ch, voice, capVoice1Index, capVoice3Index, n);
            }
            break;
        default:
            if (noise) {
                synthesizeVoice<FModRole::None, SampleSource::Noise>(ch, voice, capVoice1Index, capVoice3Index, n);
            } else {
                synthesizeVoice<FModRole::None, SampleSource::Adpcm>(ch, voice, capVoice1Index, capVoice3Index, n);
            }
            break;
    }
}

////////////////////////////////////////////////////////////////////////
// Emulated time. See interface.h, "Emulated time".
////////////////////////////////////////////////////////////////////////

void PCSX::SPU::impl::publishHorizon(uint64_t cycle) {
    // A reset or a savestate load moves the CPU clock backwards, and nothing moves it forwards by
    // more than a few scanlines between two calls. Either kind of jump is a discontinuity: the
    // mixer clock is moved to the new time instead of being run up to it.
    const uint64_t maxForwardJump = PCSX::g_emulator->m_psxClockSpeed;
    if (cycle < m_lastHorizon || cycle - m_lastHorizon > maxForwardJump) {
        resync(cycle);
        return;
    }
    m_lastHorizon = cycle;
    m_horizonCycle.store(cycle);
    // The mixer only works in whole NSSIZE batches unless the CPU is waiting on it, so there is
    // nothing to wake it for until a batch is available. Waking it per scanline instead costs
    // both threads a round trip for 2-3 samples of work.
    if (m_mixerWaiting.load() && sampleAfter(cycle) >= m_mixedSamples.load() + NSSIZE) {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        m_mixerWake.notify_one();
    }
}

void PCSX::SPU::impl::catchUp(uint64_t cycle) {
    publishHorizon(cycle);
    if (!m_mixerRunning.load()) return;
    const uint64_t target = sampleAfter(cycle);
    auto done = [this, target]() {
        return m_mixedSamples.load() >= target && m_eventsApplied.load() == m_eventsPushed.load();
    };
    if (done()) return;
    std::unique_lock<std::mutex> lock(m_syncMutex);
    m_cpuWaiting.store(true);
    m_mixerWake.notify_one();
    while (!done()) m_cpuWake.wait_for(lock, std::chrono::milliseconds(10));
    m_cpuWaiting.store(false);
}

void PCSX::SPU::impl::resync(uint64_t cycle) {
    const bool running = m_mixerRunning.load();
    if (running) RemoveThread();
    drainEventsNow();
    m_mixPos = sampleAfter(cycle);
    {
        // The queued CD audio is stamped on the clock being left behind.
        std::lock_guard<std::mutex> lock(cbMtx);
        m_cdQueue.clear();
    }
    m_mixedSamples.store(m_mixPos);
    m_lastHorizon = cycle;
    m_horizonCycle.store(cycle);
    if (running) SetupThread();
}

void PCSX::SPU::impl::pushEvent(uint64_t cycle, uint32_t reg, uint16_t value) {
    const uint64_t pushed = m_eventsPushed.load(std::memory_order_relaxed);
    if (pushed - m_eventsApplied.load() >= kEventQueueSize) {
        // Full. Running the mixer up to here applies everything queued.
        if (m_mixerRunning.load()) {
            catchUp(cycle);
        } else {
            drainEventsNow();
        }
    }
    auto &event = m_events[pushed & (kEventQueueSize - 1)];
    event.sample = sampleAfter(cycle);
    event.reg = reg;
    event.value = value;
    m_eventsPushed.store(pushed + 1, std::memory_order_release);
}

void PCSX::SPU::impl::drainEventsNow() {
    uint64_t applied = m_eventsApplied.load();
    const uint64_t pushed = m_eventsPushed.load();
    for (; applied != pushed; applied++) {
        const auto &event = m_events[applied & (kEventQueueSize - 1)];
        applyRegister(event.reg, event.value);
    }
    m_eventsApplied.store(applied);
}

void PCSX::SPU::impl::applyDueEvents() {
    uint64_t applied = m_eventsApplied.load(std::memory_order_relaxed);
    const uint64_t pushed = m_eventsPushed.load(std::memory_order_acquire);
    while (applied != pushed) {
        const auto &event = m_events[applied & (kEventQueueSize - 1)];
        if (event.sample > m_mixPos) break;
        applyRegister(event.reg, event.value);
        applied++;
    }
    m_eventsApplied.store(applied);
}

void PCSX::SPU::impl::waitForWork() {
    std::unique_lock<std::mutex> lock(m_syncMutex);
    m_mixerWaiting.store(true);
    // The timeout only bounds the cost of a missed wakeup; every producer notifies.
    m_mixerWake.wait_for(lock, std::chrono::milliseconds(10), [this]() {
        if (endThread.load()) return true;
        if (m_cpuWaiting.load()) return true;
        if (sampleAfter(m_horizonCycle.load()) >= m_mixPos + NSSIZE) return true;
        const uint64_t applied = m_eventsApplied.load(std::memory_order_relaxed);
        if (applied == m_eventsPushed.load()) return false;
        return m_events[applied & (kEventQueueSize - 1)].sample <= m_mixPos;
    });
    m_mixerWaiting.store(false);
}

void PCSX::SPU::impl::publishProgress() {
    m_mixedSamples.store(m_mixPos);
    if (m_cpuWaiting.load()) {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        m_cpuWake.notify_one();
    }
}

////////////////////////////////////////////////////////////////////////
// Main SPU job handler. This is where the sound processing happens.
////////////////////////////////////////////////////////////////////////

void PCSX::SPU::impl::MainThread() {
    while (!endThread) {
        applyDueEvents();
        // Read the horizon before the queue: every write stamped before this horizon is then
        // visible below, so the chunk cannot run past one.
        const uint64_t limit = sampleAfter(m_horizonCycle.load());
        // Mix in whole NSSIZE batches, and short of one only when the CPU is waiting to read.
        if (m_mixPos >= limit || (limit - m_mixPos < NSSIZE && !m_cpuWaiting.load())) {
            // Caught up with the CPU. Hand over what is mixed and wait for it to move on.
            publishProgress();
            if (!flushOutput()) break;
            waitForWork();
            continue;
        }

        uint64_t n = std::min<uint64_t>(NSSIZE, limit - m_mixPos);
        const uint64_t applied = m_eventsApplied.load(std::memory_order_relaxed);
        if (applied != m_eventsPushed.load(std::memory_order_acquire)) {
            // End the chunk where the next queued write comes due.
            const uint64_t next = m_events[applied & (kEventQueueSize - 1)].sample;
            n = next > m_mixPos ? std::min(n, next - m_mixPos) : 0;
        }
        if (n == 0) continue;

        mixChunk(static_cast<int>(n));
        m_mixPos += n;
        publishProgress();

        const size_t buffered = (reinterpret_cast<uint8_t *>(pS) - spuBuffer) / sizeof(SDLAudio::Frame);
        if (buffered >= NSSIZE && !flushOutput()) break;
    }

    threadEnded = 1;
}

// MainThread's loop, run on the caller's thread for a fixed number of samples, with the output
// handed to the caller instead of the audio device.
void PCSX::SPU::impl::mixSynchronous(int16_t *out, size_t frames) {
    const uint64_t target = m_mixPos + frames;
    while (m_mixPos < target) {
        applyDueEvents();
        uint64_t n = std::min<uint64_t>(NSSIZE, target - m_mixPos);
        const uint64_t applied = m_eventsApplied.load(std::memory_order_relaxed);
        if (applied != m_eventsPushed.load(std::memory_order_acquire)) {
            // End the chunk where the next queued write comes due.
            const uint64_t next = m_events[applied & (kEventQueueSize - 1)].sample;
            n = next > m_mixPos ? std::min(n, next - m_mixPos) : 0;
        }
        if (n == 0) continue;

        mixChunk(static_cast<int>(n));
        m_mixPos += n;

        const size_t values = pS - reinterpret_cast<int16_t *>(spuBuffer);
        memcpy(out, spuBuffer, values * sizeof(int16_t));
        out += values;
        pS = (int16_t *)spuBuffer;
    }
    m_mixedSamples.store(m_mixPos);
    // The CPU side is now at the last sample mixed, so a write the host makes next lands on the
    // next sample, and a host that mixes more than a second between two writes is not mistaken
    // for a forward jump (publishHorizon).
    if (m_mixPos == 0) return;
    const uint64_t cycle = sampleToCycle(m_mixPos - 1);
    if (cycle > m_lastHorizon) {
        m_lastHorizon = cycle;
        m_horizonCycle.store(cycle);
    }
}

bool PCSX::SPU::impl::flushOutput() {
    const size_t frames = (reinterpret_cast<uint8_t *>(pS) - spuBuffer) / sizeof(SDLAudio::Frame);
    if (frames == 0) return true;
    if (m_wavDump) writeWavDump(reinterpret_cast<SDLAudio::Frame *>(spuBuffer), frames);
    // When the host emulates a stretch slower than realtime, the device plays silence until the
    // CPU catches up, and the CPU then runs the same lead ahead of the device as before: what was
    // mixed during the hitch stays queued, and the sound would stay that much later than the
    // picture from then on. Normally the queue holds at most the lead, so drop the oldest frames
    // once it holds more than the lead plus a device period.
    const size_t lead = getLeadFrames();
    const size_t buffered = m_audioOut.getFramesBuffered(0);
    if (buffered + frames > lead + devicePeriodFrames()) {
        m_audioOut.dropVoiceFrames(buffered + frames - lead);
    }
    // Blocks while the device ring is full, which is what paces this thread when the CPU is
    // further ahead than the ring holds.
    while (!m_audioOut.feedStreamData(reinterpret_cast<SDLAudio::Frame *>(spuBuffer), frames)) {
        if (endThread) return false;
    }
    pS = (int16_t *)spuBuffer;
    return true;
}

void PCSX::SPU::impl::mixChunk(int n) {
    const int volumeDivisor = 4 - settings.get<Volume>();
    int ns, ch;

    // The capture mirrors, the CD capture area and the decode-buffer IRQ walk all sit at the
    // same position of their 0x200-sample ring, and that position is the emulated sample clock.
    const int32_t ringPos = static_cast<int32_t>(m_mixPos % kCaptureRegionSamples);
    int32_t capVoice1Index = ringPos;
    int32_t capVoice3Index = ringPos;

    // Clock the shared noise generator once per output sample of the chunk.
    for (ns = 0; ns < n; ns++) {
        m_noise.step();
        noiseLevel[ns] = m_noise.getVal();
    }

    // Collect the chunk from every channel into the mix accumulators.
    for (ch = 0; ch < MAXCHAN; ch++) {
        synthesizeChannel(ch, &s_chan[ch], capVoice1Index, capVoice3Index, n);
    }

    // Write from our temporary capture buffer to the actual SPU RAM.
    writeCaptureBufferCD(n);
    mixCdInput(n);

    // Reflect which half of the 0x200-sample capture buffer the write pointer is now in, in
    // SPUSTAT bit 11 (0=first half 0x000-0x0ff, 1=second half 0x100-0x1ff). A CPU read of
    // SPUSTAT reconstructs this bit from its own cycle; this copy is what a savestate keeps.
    {
        std::lock_guard<std::mutex> lock(cbMtx);
        capBufVoiceIndex = static_cast<int32_t>((m_mixPos + n) % kCaptureRegionSamples);
        if (capBufVoiceIndex & kCaptureHalfMarker) {
            spuStat |= StatusFlags::CBIndex;
        } else {
            spuStat &= ~StatusFlags::CBIndex;
        }
    }

    ///////////////////////////////////////////////////////
    // Mix all channels, including reverb, into one buffer.

    // Main volume scales linearly in value/0x4000 (SCPH-1001: 0x3800 -> -1.16 dB,
    // 0x2000 -> -6.02 dB against 0x3fff). Measured with reverb off, so where the reverb
    // output joins relative to it is not settled; it is applied to the sum here. A chunk
    // ends where the next queued write comes due, so one value holds for the whole chunk.
    // CD audio has joined the voices already (mixCdInput), ahead of main volume.
    const int64_t mainLeft = m_mainLeftWritten ? m_mainVolume.left() : kVoiceVolumeUnity;
    const int64_t mainRight = m_mainRightWritten ? m_mainVolume.right() : kVoiceVolumeUnity;
    for (ns = 0; ns < n; ns++) {
        SSumL[ns] += m_reverb.mixLeft(ns, spuMem, spuCtrl);
        SSumL[ns] = static_cast<int>(SSumL[ns] * mainLeft / kVoiceVolumeUnity);
        *pS++ = std::clamp(SSumL[ns] / volumeDivisor, -kMixSampleClamp, kMixSampleClamp);
        SSumL[ns] = 0;

        SSumR[ns] += m_reverb.mixRight();
        SSumR[ns] = static_cast<int>(SSumR[ns] * mainRight / kVoiceVolumeUnity);
        *pS++ = std::clamp(SSumR[ns] / volumeDivisor, -kMixSampleClamp, kMixSampleClamp);
        SSumR[ns] = 0;
    }

    //////////////////////////////////////////////////////
    // Special IRQ handling in the decode buffers (0x0000-0x1000).
    //
    // The decode buffers are located in SPU memory as follows, with decoded data being 16 bits
    // per sample:
    // 0x0000-0x03ff  CD audio left
    // 0x0400-0x07ff  CD audio right
    // 0x0800-0x0bff  Voice 1
    // 0x0c00-0x0fff  Voice 3
    //
    // Even if voices 1 and 3 are off, or no CD audio is playing, the internal play positions
    // keep moving and wrap after 0x400 bytes. Therefore a single pointer from spuMem+0 to
    // spuMem+0x3ff suffices, increased by 2 bytes on each sample. If that pointer, or one of the
    // 0x400 offsets of it, hits the SPU IRQ address, an IRQ is generated. Note also that the
    // channel 0-3 IRQ debug display is reused for these IRQs, as that is the simplest way to
    // display them in debug mode.

    // mixIrqAddress is armed by resetCaptureBuffer on the emulation thread, so the walk
    // runs under cbMtx. triggerIrq only sets flags (scheduleInterrupt stores an atomic),
    // so nothing in here takes another lock. The hold is n * 4 compares.
    {
        std::lock_guard<std::mutex> irqLock(cbMtx);
        if (mixIrqAddress) {
            mixIrqAddress = spuRamBase + ringPos * 2;
            for (ns = 0; ns < n; ns++) {
                if ((spuCtrl & ControlFlags::IRQEnable) && irqAddress && irqAddress < spuRamBase + 0x1000) {
                    for (ch = 0; ch < 4; ch++) {
                        if (irqAddress >= mixIrqAddress + (ch * 0x400) &&
                            irqAddress < mixIrqAddress + (ch * 0x400) + 2) {
                            triggerIrq();
                            s_chan[ch].data.get<PCSX::SPU::Chan::IrqDone>().value = 1;
                        }
                    }
                }
                mixIrqAddress += 2;
                if (mixIrqAddress > spuRamBase + 0x3ff) mixIrqAddress = spuRamBase;
            }
        }
    }

    m_reverb.init(n);
}

void PCSX::SPU::impl::writeCaptureBufferCD(int numbSamples) {
    std::lock_guard<std::mutex> lock(cbMtx);
    if (mixIrqAddress) {
        // CD audio arrives once per sector, 1/75 s apart in emulated time. Within a few
        // sectors of the last one the CD is still feeding, and an empty buffer means the next
        // sector has not been decoded yet: leave the slots alone instead of writing silence.
        const uint64_t feeding = PCSX::g_emulator->m_psxClockSpeed * 4 / 75;
        const uint64_t lastFeed = cdFeedCycle;
        const uint64_t now = m_horizonCycle.load();
        const bool cdFeeding = (lastFeed != 0) && (now >= lastFeed) && (now - lastFeed < feeding);
        captureBuffer.currIndex = static_cast<int32_t>(m_mixPos % 0x200);
        for (int n = 0; n < numbSamples; n++) {
            if (captureBuffer.startIndex == captureBuffer.endIndex) {
                if (cdFeeding) break;
                // Nothing is feeding the CD input: the capture records silence.
                spuMem[captureBuffer.currIndex] = 0;
                spuMem[captureBuffer.currIndex + 0x200] = 0;
            } else {
                spuMem[captureBuffer.currIndex] = captureBuffer.CDCapLeft[captureBuffer.startIndex];
                spuMem[captureBuffer.currIndex + 0x200] = captureBuffer.CDCapRight[captureBuffer.startIndex];
                captureBuffer.startIndex = (captureBuffer.startIndex + 1) % CaptureBuffer::CB_SIZE;
            }
            captureBuffer.currIndex = (captureBuffer.currIndex + 1) % 0x200;
        }
    }
}

// Takes this chunk's CD audio off the queue, one frame per sample, and adds it to the voice mix
// through CD volume and SPUCNT bit 0. The frames are taken whether or not the input is enabled.
void PCSX::SPU::impl::mixCdInput(int n) {
    int cdLeft[NSSIZE];
    int cdRight[NSSIZE];
    {
        std::lock_guard<std::mutex> lock(cbMtx);
        auto &q = m_cdQueue;
        for (int ns = 0; ns < n; ns++) {
            const size_t i = q.popped & (CdAudioQueue::kSize - 1);
            // Nothing queued, or the next frame is not due yet: the CD input is silent here.
            const bool due = q.popped != q.pushed && q.stamp[i] <= m_mixPos + ns;
            cdLeft[ns] = due ? q.left[i] : 0;
            cdRight[ns] = due ? q.right[i] : 0;
            q.popped += due ? 1 : 0;
        }
    }
    if (!(spuCtrl & ControlFlags::CDAudioEnable)) return;
    for (int ns = 0; ns < n; ns++) {
        SSumL[ns] += cdLeft[ns] * m_cdVolumeLeft / kCdVolumeUnity;
        SSumR[ns] += cdRight[ns] * m_cdVolumeRight / kCdVolumeUnity;
    }
}

////////////////////////////////////////////////////////////////////////
// XA audio.
////////////////////////////////////////////////////////////////////////

void PCSX::SPU::impl::playADPCMchannel(xa_decode_t *xap) {
    // Nothing to do when XA streaming is disabled.
    if (!settings.get<Streaming>()) return;
    if (!xap) return;
    // Nothing to do without an XA frequency.
    if (!xap->freq) return;

    // Call the main XA feeder.
    FeedXA(xap);
}

////////////////////////////////////////////////////////////////////////
// Init and exit.
////////////////////////////////////////////////////////////////////////

// Called first by the main emulator.
long PCSX::SPU::impl::init(void) {
    spuRamBase = (uint8_t *)spuMem;

    wipeChannels();
    return 0;
}

void PCSX::SPU::impl::wipeChannels() {
    for (unsigned i = 0; i < MAXCHAN; i++) {
        s_chan[i].adsr.reset();
        s_chan[i].adpcm.reset();
        s_chan[i].volume.reset();
        s_chan[i].data.reset();
        m_adsrShadow[i].reset();
        m_pitchShadow[i] = 0;
        m_fmodShadow[i] = 0;
    }
    m_reverb.reset();
}

// Initialization of the mixing buffers.
void PCSX::SPU::impl::SetupMixBuffers() {
    memset(SSumR, 0, NSSIZE * sizeof(int));
    memset(SSumL, 0, NSSIZE * sizeof(int));
    memset(fmodInput, 0, NSSIZE * sizeof(int));

    // Set up the sound buffer pointer.
    pS = (int16_t *)spuBuffer;
}

// Initialization of certain buffers and of the mixing thread.
void PCSX::SPU::impl::SetupThread() {
    SetupMixBuffers();

    // Initialize the thread variables.
    endThread = 0;
    threadEnded = 0;
    // Flag that initialization is complete.
    bSpuInit = 1;

    m_mixerRunning = true;
    hMainThread = std::thread([this]() { MainThread(); });
}

// Kill the mixing thread.
void PCSX::SPU::impl::RemoveThread() {
    // Raise the flag to end the thread.
    endThread = 1;
    {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        m_mixerWake.notify_one();
    }

    using namespace std::chrono_literals;
    // Wait until the thread has ended.
    while (!threadEnded) {
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(5ms);

    hMainThread.join();

    // No more SPU is running.
    threadEnded = 0;
    bSpuInit = 0;
    m_mixerRunning = false;
}

// Initialize most of the SPU buffers.
void PCSX::SPU::impl::SetupStreams() {
    int i;

    // Allocate the mixing buffer.
    spuBuffer = (uint8_t *)malloc(32768);

    // Allocate the reverb mixing buffer: one interleaved stereo pair per sample.
    i = NSSIZE * 2;
    m_reverb.mixStart = (int *)malloc(i * 4);
    memset(m_reverb.mixStart, 0, i * 4);

    // Loop over the sound channels.
    for (i = 0; i < MAXCHAN; i++) {
        // No per-channel mutex synchronization is used here: it is not needed, and would only slow
        // things down.
        // Initialize the sustain level.
        s_chan[i].adsr.ex().get<exSustainLevel>().value = ADSRFlags::SustainLevelMask;
        m_adsrShadow[i].ex().get<exSustainLevel>().value = ADSRFlags::SustainLevelMask;
        s_chan[i].data.get<PCSX::SPU::Chan::Mute>().value = false;
        s_chan[i].data.get<PCSX::SPU::Chan::Solo>().value = false;
        s_chan[i].data.get<PCSX::SPU::Chan::IrqDone>().value = 0;
        s_chan[i].adpcm.setLoop(spuRamBase);
        s_chan[i].adpcm.setStart(spuRamBase);
        s_chan[i].adpcm.setCurr(spuRamBase);
    }
}

// Free most of the SPU buffers.
void PCSX::SPU::impl::RemoveStreams(void) {
    // Free the mixing buffer.
    free(spuBuffer);
    spuBuffer = NULL;
    // Free the reverb buffer.
    free(m_reverb.mixStart);
    m_reverb.mixStart = 0;
}

// Called by the main emulator after init.
bool PCSX::SPU::impl::open() { return openWith(true); }

// Called by a host that mixes on its own thread, instead of open().
bool PCSX::SPU::impl::openSynchronous() { return openWith(false); }

bool PCSX::SPU::impl::openWith(bool startThread) {
    // Guard against a redundant open.
    if (spuIsOpen) return true;

    spuIrq = 0;
    spuAddr = 0xffffffff;
    endThread = 0;
    threadEnded = 0;
    spuRamBase = (uint8_t *)spuMem;
    mixIrqAddress = 0;
    wipeChannels();
    m_mainVolume.reset();
    m_mainLeftWritten = m_mainRightWritten = false;
    m_cdVolumeLeft = m_cdVolumeRight = kCdVolumeUnity;
    m_cdLeftWritten = m_cdRightWritten = false;
    m_cdQueue.clear();
    irqAddress = 0;
    spuStat &= ~StatusFlags::IRQFlag;

    // The mixer clock starts at sample 0; the first horizon the CPU publishes re-bases it if the
    // CPU is already far along.
    m_eventsPushed = 0;
    m_eventsApplied = 0;
    m_horizonCycle = 0;
    m_mixedSamples = 0;
    m_mixPos = 0;
    m_lastHorizon = 0;
    m_ctrlShadow = 0;
    m_ctrlWritesPending = 0;

    // Prepare streaming.
    SetupStreams();

    if (startThread) {
        // Start the thread that feeds data.
        SetupThread();
    } else {
        SetupMixBuffers();
    }

    spuIsOpen = 1;

    m_lastUpdated = std::chrono::steady_clock::now();

    resetCaptureBuffer();

    return true;
}

// Called before shutdown.
long PCSX::SPU::impl::close(void) {
    // Guard against closing when not open.
    if (!spuIsOpen) return 0;

    spuIsOpen = 0;

    // No more feeding. There is no thread to stop after openSynchronous().
    if (m_mixerRunning.load()) RemoveThread();
    // No more streaming.
    RemoveStreams();

    m_wavDump.reset();

    return 0;
}

// The mixer runs at 44.1kHz, the rate the pitch computation assumes, and the sink plays it
// at that same rate. The header sizes are rewritten after every block, so the file is a
// valid WAV at any point, however the emulator ends up exiting.
bool PCSX::SPU::impl::setWavDump(const std::filesystem::path &path) {
    static constexpr uint32_t kRate = 44100;
    static constexpr uint16_t kChannels = 2;
    static constexpr uint16_t kBits = 16;
    static constexpr uint16_t kBlockAlign = kChannels * kBits / 8;

    IO<File> file(new PosixFile(path, FileOps::TRUNCATE));
    if (file->failed()) return false;
    file->writeString("RIFF");
    file->write<uint32_t>(36);
    file->writeString("WAVE");
    file->writeString("fmt ");
    file->write<uint32_t>(16);
    file->write<uint16_t>(1);
    file->write<uint16_t>(kChannels);
    file->write<uint32_t>(kRate);
    file->write<uint32_t>(kRate * kBlockAlign);
    file->write<uint16_t>(kBlockAlign);
    file->write<uint16_t>(kBits);
    file->writeString("data");
    file->write<uint32_t>(0);
    m_wavDump = file;
    m_wavDumpBytes = 0;
    return true;
}

void PCSX::SPU::impl::writeWavDump(const SDLAudio::Frame *frames, size_t count) {
    static_assert(sizeof(SDLAudio::Frame) == 4);
    // The RIFF size fields are 32-bit, which caps a capture at about 6h45m.
    constexpr uint64_t kMaxBytes = UINT32_MAX - 36;
    uint64_t room = (kMaxBytes - m_wavDumpBytes) / sizeof(SDLAudio::Frame);
    bool full = count > room;
    if (full) count = room;
    size_t size = count * sizeof(SDLAudio::Frame);
    try {
        ssize_t written;
        if constexpr (std::endian::native == std::endian::little) {
            written = m_wavDump->write(frames, size);
        } else {
            auto swap16 = [](int16_t v) { return int16_t(uint16_t(v) >> 8 | uint16_t(v) << 8); };
            std::vector<int16_t> le(count * 2);
            for (size_t i = 0; i < count; i++) {
                le[i * 2] = swap16(frames[i].L);
                le[i * 2 + 1] = swap16(frames[i].R);
            }
            written = m_wavDump->write(le.data(), size);
        }
        if (written > 0) m_wavDumpBytes += written;
        m_wavDump->writeAt<uint32_t>(36 + m_wavDumpBytes, 4);
        m_wavDump->writeAt<uint32_t>(m_wavDumpBytes, 40);
        if (written != (ssize_t)size) {
            g_system->log(LogClass::SPU, "WAV dump: write failed, capture stopped\n");
            m_wavDump.reset();
            return;
        }
    } catch (const std::exception &e) {
        g_system->log(LogClass::SPU, "WAV dump: %s, capture stopped\n", e.what());
        m_wavDump.reset();
        return;
    }
    if (full) {
        g_system->log(LogClass::SPU, "WAV dump: reached the 4 GB WAV limit, capture stopped\n");
        m_wavDump.reset();
    }
}

// Called by the main emulator on final exit.
long PCSX::SPU::impl::shutdown(void) { return 0; }

////////////////////////////////////////////////////////////////////////
// Callback setup. Called once, and passes a callback that is invoked on an
// SPU IRQ or a CDDA volume change.
////////////////////////////////////////////////////////////////////////

void PCSX::SPU::impl::registerCDDAVolume(void (*CDDAVcallback)(uint16_t, uint16_t)) { cddavCallback = CDDAVcallback; }

////////////////////////////////////////////////////////////////////////

void PCSX::SPU::impl::playCDDAchannel(int16_t *data, int size) {
    m_cdda.freq = 44100;
    m_cdda.nsamples = size / 4;
    m_cdda.stereo = 1;
    m_cdda.nbits = 16;
    memcpy(m_cdda.pcm, data, size);
    FeedXA(&m_cdda);
}

void PCSX::SPU::impl::setLua(Lua L) {
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.getfieldtable("settings");
    L.push("spu");
    settings.pushValue(L);
    L.settable();
    L.pop();
    L.pop();
    m_audioOut.setLua(L);
}
