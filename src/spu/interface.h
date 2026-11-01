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

#pragma once

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/decode_xa.h"
#include "core/spu.h"
#include "core/sstate.h"
#include "json.hpp"
#include "spu/adsr.h"
#include "spu/noise.h"
#include "spu/reverb.h"
#include "spu/sdlaudio.h"
#include "spu/types.h"
#include "support/file.h"
#include "support/settings.h"

namespace PCSX {

namespace SPU {

// Compile-time mode axes for the per-voice synthesis loop. Both are per-voice
// mode flags set by register writes. They are resolved into template arguments
// ONCE, at the top of each mixed chunk. The mixer splits its chunks at every
// queued register write, so a write to either flag still takes effect at the
// sample it was stamped with.
//
// FModRole - this voice's part in frequency modulation. The values match the
//   Chan::FMod encoding, which registers.cc writes in PAIRS: setting the
//   pitch-mod bit for voice N makes N the Target and N-1 the Source. Hoisting
//   this hoists the ROLE only; the modulation data itself (fmodInput[ns]) stays
//   per-sample.
enum class FModRole { None = 0, Target = 1, Source = 2 };

// SampleSource - where the pre-envelope sample comes from. Note that a Noise
//   voice still runs the ADPCM decode loop: the decoded samples are discarded,
//   but the cursor advance, the IRQ address check and the ENDX latch all hang
//   off it.
enum class SampleSource { Adpcm, Noise };

// Two further per-voice flags were considered as axes and deliberately left as
// runtime branches. Chan::Mute/Chan::Solo are the debugger's mute, not
// something a game drives, and Chan::RVBActive gates a single call; each guards
// one statement, so templating them would double the instantiation matrix twice
// over to remove a pair of well-predicted compares. Adding either is a code-size
// decision, not a correctness one - the same call polys.cc records for its ABR
// axis.

class impl final : public SPUInterface {
  public:
    using json = nlohmann::json;
    bool open() final;
    // SPU functions.
    long init(void) final;
    long shutdown(void) final;
    long close(void) final;
    void wipeChannels();
    void writeRegister(uint32_t, uint16_t) final;
    uint16_t readRegister(uint32_t) final;
    void lockSPURAM() final;
    void unlockSPURAM() final;
    uint8_t *getSpuRam() final { return spuRamBase; }
    void setOverlay(std::shared_ptr<const Overlay> overlay) final {
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        m_overlay = std::move(overlay);
    }
    std::shared_ptr<const Overlay> getOverlay() final {
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        return m_overlay;
    }
    uint32_t getTransferAddr() final { return spuAddr; }
    void resetCaptureBuffer() final;
    void writeDMAMem(uint16_t *, int) final;
    void readDMAMem(uint16_t *, int) final;
    virtual void playADPCMchannel(xa_decode_t *) final;
    void setXAVolume(const uint8_t atv[4]) final {
        for (unsigned i = 0; i < 4; i++) xaAtv[i] = atv[i];
    }

    void save(SaveStates::SPU &) final;
    void load(const SaveStates::SPU &) final;

    virtual void setLua(Lua L) override;
    bool setWavDump(const std::filesystem::path &path) final;

    void playCDDAchannel(int16_t *, int) final;
    void registerCDDAVolume(void (*CDDAVcallback)(uint16_t, uint16_t));

    // Number of channels.
    static const size_t MAXCHAN = 24;

    // The per-voice capture mirrors are 0x200 samples each and the write pointer
    // wraps every 0x200; SPUSTAT bit 11 says which half it is in. Both the SPU
    // thread's own advance and the SPUSTAT read-time reconstruction key off these.
    static constexpr int kCaptureRegionSamples = 0x200;
    static constexpr int kCaptureHalfMarker = kCaptureRegionSamples / 2;  // 0x100
    // Number of characters for a channel tag.
    static constexpr unsigned CHANNEL_TAG = 32;
    // Number of samples for the debugger wave plot.
    static const unsigned DEBUG_SAMPLES = 1024;

    uint32_t getFrameCount() override { return m_audioOut.getFrameCount(); }

    void debug() final;
    bool configure() final;
    json getCfg() final { return settings.serialize(); }
    void setCfg(const json &j) final {
        if (j.count("SPU") && j["SPU"].is_object()) {
            settings.deserialize(j["SPU"]);
        } else {
            settings.reset();
        }
    }
    uint32_t getCurrentFrames() override { return m_audioOut.getCurrentFrames(); }
    void waitForGoal(uint32_t goal) override {
#ifdef __EMSCRIPTEN__
        // This is the emulator's real-time throttle: run ahead of the audio
        // device, then block until it has drained enough frames. It is called
        // from psxcounters, i.e. from inside Execute(), and blocking there is
        // exactly what a browser's main thread may not do - it is a futex wait,
        // and it is the reason this build used to need PROXY_TO_PTHREAD.
        //
        // In a browser the pacing belongs to the frame callback instead: the
        // main loop delivers ~60 calls a second and Execute() unwinds after one
        // emulated frame, so the rate is set by requestAnimationFrame rather
        // than by the audio clock. The caller advances m_audioFrames either way.
        //
        // Known imperfection rather than an oversight: that ties emulation speed
        // to the display refresh, so a PAL title or a 144 Hz monitor will not
        // pace correctly. Fixing that means driving from the audio callback,
        // which is a bigger change than moving the loop.
        (void)goal;
#else
        m_audioOut.waitForGoal(goal);
#endif
    }
    // The mixer cannot run past the CPU, so the CPU has to run ahead of the audio device by at
    // least one device period, or the device finds the ring short. On top of that: two NSSIZE
    // batches, because the mixer waits for whole batches before mixing, and the user's latency
    // margin for host jitter. Before the device has opened, assume a 1024-frame period.
    uint32_t getLeadFrames() override {
        const int marginMs = std::max(0, settings.get<LatencyMargin>().value);
        return devicePeriodFrames() + 2 * NSSIZE + marginMs * 441 / 10;
    }
    uint32_t devicePeriodFrames() {
        const uint32_t period = m_audioOut.getPeriodFrames();
        return period ? period : 1024;
    }
    void advanceTo(uint64_t cycle) override { publishHorizon(cycle); }

    // Embedding. A host that mixes on its own thread opens the SPU with openSynchronous()
    // instead of open(): the same state, but no mixer thread, and nothing goes to the audio
    // device. The host pulls the output with mixSynchronous() and closes with close() as usual.
    // Register writes are still stamped with the CPU cycle and applied at the matching sample,
    // so the host keeps PCSX::g_emulator's cycle on the same clock as the samples it mixes: a
    // write at cycle (n - 1) * 768 takes effect at sample n. CPU reads do not wait for the
    // mixer in this mode (catchUp is a no-op); the host is the mixer.
    bool openSynchronous();
    // Applies the queued writes as they come due and mixes the next `frames` samples into
    // `out`, as interleaved stereo (2 * frames values). The CPU side is then considered to be
    // at the last sample mixed. Only after openSynchronous(). In this mode the host is both
    // the CPU side and the mixer: register writes, advanceTo() and mixSynchronous() must all
    // come from one thread, since m_lastHorizon and the write queue are not locked against it.
    void mixSynchronous(int16_t *out, size_t frames);

  private:
    struct ADSRFlags {
        enum : uint16_t {
            AttackMode = 1 << 15,      // 15 0=Linear, 1=Exponential
            AttackShiftMask = 0x7c00,  // 14-10 0..1Fh = Fast..Slow
            AttackStepMask = 0x300,    // 9-8 0..3 = "+7,+6,+5,+4"
            DecayShiftMask = 0xf0,     // 7-4 0..0Fh = Fast..Slow
            SustainLevelMask = 0xf,    // 3-0 0..0Fh  ;Level=(N+1)*800h
            // Flags for the upper 16 bits of the register, shifted right by 16 bits.
            SustainMode = 1 << 15,       // 31 0=Linear, 1=Exponential
            SustainDirection = 1 << 14,  // 30  0=Increase, 1=Decrease (until Key OFF flag)
            SustainShiftMask = 0x1f00,   // 28-24 0..1Fh = Fast..Slow
            SustainStepMask = 0xc0,      // 23-22 0..3 = "+7,+6,+5,+4" or "-8,-7,-6,-5") (inc/dec)
            ReleaseMode = 1 << 5,        // 21 0=Linear, 1=Exponential
            ReleaseShiftMask = 0x1f      // 20-16 0..1Fh = Fast..Slow
        };
    };

    struct ControlFlags {
        enum : uint16_t {
            CDAudioEnable = 1 << 0,         // 0 0=Off, 1=On (for CD-DA and XA-ADPCM)
            ExternalAudioEnable = 1 << 1,   // 1 0=Off, 1=On
            CDReverbEnable = 1 << 2,        // 20=Off, 1=On (for CD-DA and XA-ADPCM)
            ExternalReverbEnable = 1 << 3,  // 3 0=Off, 1=On
            RAMTransferModeMask = 0x0030,   // 5-4 0=Stop, 1=ManualWrite, 2=DMAwrite, 3=DMAread
            IRQEnable = 1 << 6,             // 6 0=Disabled/Acknowledge, 1=Enabled; only when Bit15=1
            ReverbMasterEnable = 1 << 7,    // 7 0=Disabled, 1=Enabled
            NoiseStepMask = 0x0300,         // 9-8 0..03h = Step "4,5,6,7"
            NoiseShiftMask = 0x3c00,        // 13-10 0..0Fh = Low .. High Frequency
            Mute = 1 << 14,                 // 14 0=Mute, 1=Unmute
            Enable = 1 << 15                // 15 0=Off, 1=On
        };
    };

    struct StatusFlags {
        enum : uint16_t {
            SPUModeMask = 0x3f,        // 5-0 Current SPU Mode(same as SPUCNT.Bit5 - 0, but, applied a bit delayed)
            IRQFlag = 1 << 6,          // 6 IRQ9 Flag (0=No, 1=Interrupt Request)
            DMARWRequest = 1 << 7,     // 7 Data Transfer DMA Read/Write Request seems to be same as SPUCNT.Bit5
            DMAWriteRequest = 1 << 8,  // 8 Data Transfer DMA Write Request (0=No, 1=Yes)
            DMAReadRequest = 1 << 9,   // 9 Data Transfer DMA Read Request (0=No, 1=Yes)
            DMABusy = 1 << 10,         // 10 Data Transfer Busy Flag (0=Ready, 1=Busy)
            CBIndex = 1 << 11,         // 11 Writing to First/Second half of Capture Buffers (0=First, 1=Second)
            // 15-12 Unknown/Unused (seems to be usually zero)
        };
    };

    // Sound buffer sizes.
    // 400 ms complete sound buffer.
    static const size_t SOUNDSIZE = 70560;

    // Roughly 1 ms of data.
    static constexpr size_t NSSIZE = 45;

    // SPU.
    void MainThread();
    // Mixes n (1..NSSIZE) samples starting at m_mixPos and appends them to the output buffer.
    void mixChunk(int n);
    // Reads the voice's two mode flags once and calls the matching
    // synthesizeVoice instantiation. This is the only place the runtime flags
    // are turned into compile-time axes.
    void synthesizeChannel(int ch, SPUCHAN *voice, int32_t &capVoice1Index, int32_t &capVoice3Index, int n);
    template <FModRole Role, SampleSource Src>
    void synthesizeVoice(int ch, SPUCHAN *voice, int32_t &capVoice1Index, int32_t &capVoice3Index, int n);
    // Decodes the next ADPCM block for a voice, together with the IRQ check and the
    // loop/stop flag handling that hang off the block boundary. Returns false when the
    // voice has run past the end of its sample and must stop being synthesized.
    bool decodeNextBlock(int ch, SPUCHAN *voice);
    void triggerIrq();
    void walkSilentVoice(int ch, SPUCHAN *voice, int n);
    void captureVoiceSilence(int ch, int32_t &capVoice1Index, int32_t &capVoice3Index, int fromSample, int n);
    void captureVoiceSample(int ch, int32_t &capVoice1Index, int32_t &capVoice3Index, int sample);
    void writeCaptureBufferCD(int numbSamples);
    // Hands the mixed frames to the audio device. Blocks while the device ring is full; returns
    // false if the thread was asked to end while blocked.
    bool flushOutput();
    void SetupStreams();
    void RemoveStreams();
    void SetupMixBuffers();
    void SetupThread();
    void RemoveThread();
    bool openWith(bool startThread);
    void StartSound(SPUCHAN *voice);
    // Read-time reconstruction. A CPU read of ENVX or of SPUSTAT bit 11 asks what
    // the SPU is doing at the reader's own cycle, which the mixer thread, running
    // behind the CPU, may not have reached. Guests poll both, so waiting for the
    // mixer on each read (catchUp) would cost a thread round trip per sample of
    // polling. Both quantities are pure functions of elapsed samples, so evaluate
    // them against the reader's cycle on the CPU side instead. The mixer keeps
    // using its own live envelope.
    uint16_t reconstructEnvelope(int ch, uint64_t cycle);
    // Samples elapsed at a CPU cycle, on the hardware 768 cycles/sample ratio.
    uint64_t cycleToSample(uint64_t cycle) const;
    // The CPU cycle a sample is produced at; cycleToSample's inverse.
    uint64_t sampleToCycle(uint64_t sample) const;
    // Installs a new 16.16 pitch step, clamping zero, and notifies the interpolator.
    void setPitchStep(SPUCHAN *voice, int32_t step);
    void VoiceChangeFrequency(SPUCHAN *voice);
    void FModChangeFrequency(SPUCHAN *voice, int ns);

    // Emulated time.
    //
    // The mixer thread advances in emulated time, not in wall-clock time. The CPU thread
    // publishes how far it has run (m_horizonCycle) and the mixer never mixes a sample the CPU
    // has not reached. Every register write is stamped with the CPU cycle it happened at and
    // queued; the mixer applies it at that sample, splitting its batch there. So the mixer
    // always runs BEHIND the CPU, and a CPU read of anything the mixer owns (ENDX, SPU RAM,
    // the latched repeat address) first waits for the mixer to reach the reader's cycle
    // (catchUp). ENVX and SPUSTAT bit 11 are still reconstructed on the CPU side without
    // waiting, because guests poll them.
    //
    // Sample index s is the s-th output sample since the clock started, produced at cycle
    // s * 768. sampleAfter(c) is the first sample produced strictly after cycle c, which is
    // both where a write at c takes effect and how far the mixer may run once the CPU is at c.
    struct RegisterEvent {
        uint64_t sample;
        uint32_t reg;
        uint16_t value;
    };
    static constexpr size_t kEventQueueSize = 16384;
    static_assert((kEventQueueSize & (kEventQueueSize - 1)) == 0);
    uint64_t sampleAfter(uint64_t cycle) const { return cycleToSample(cycle) + 1; }
    // CPU thread. Lets the mixer run up to `cycle`. A jump backwards (reset) or far forwards
    // re-bases the mixer clock instead.
    void publishHorizon(uint64_t cycle);
    // CPU thread. Returns once every sample up to `cycle` is mixed and every queued write
    // applied. A no-op while the mixer thread is not running.
    void catchUp(uint64_t cycle);
    // CPU thread. Stops the mixer, applies whatever is queued, and restarts it at `cycle`.
    void resync(uint64_t cycle);
    void pushEvent(uint64_t cycle, uint32_t reg, uint16_t value);
    // Applies every queued write regardless of its stamp. Only with the mixer stopped.
    void drainEventsNow();
    // Mixer thread.
    void applyDueEvents();
    void waitForWork();
    void publishProgress();
    // The mixer-side half of a register write: everything the voices and the mix see.
    // writeRegister is the CPU-side half, which stamps and queues it.
    void applyRegister(uint32_t reg, uint16_t val);
    bool applyMixVolume(uint32_t r, uint16_t val);
    // CPU-side shadows of state the mixer owns, rebuilt from the mixer's state after a load.
    void rebuildShadows();
    void saveEnvelopeCheckpoints(SaveStates::EnvelopeCheckpoints &);
    void loadEnvelopeCheckpoints(const SaveStates::EnvelopeCheckpoints &);
    uint16_t readCtrl();
    void decodeAdsrLow(AdsrEnvelope &adsr, uint16_t val);
    void decodeAdsrHigh(AdsrEnvelope &adsr, uint16_t val);

    RegisterEvent m_events[kEventQueueSize];
    std::atomic<uint64_t> m_eventsPushed = 0;   // written by the CPU thread only
    std::atomic<uint64_t> m_eventsApplied = 0;  // written by the mixer thread only
    std::atomic<uint64_t> m_horizonCycle = 0;   // written by the CPU thread only
    std::atomic<uint64_t> m_mixedSamples = 0;   // written by the mixer thread only
    uint64_t m_mixPos = 0;                      // mixer thread: samples [0, m_mixPos) are mixed
    uint64_t m_lastHorizon = 0;                 // CPU thread
    std::atomic<bool> m_mixerRunning = false;
    std::atomic<bool> m_mixerWaiting = false;
    std::atomic<bool> m_cpuWaiting = false;
    std::mutex m_syncMutex;
    // Set from the UI thread, read by the mixer at key-on.
    std::mutex m_overlayMutex;
    std::shared_ptr<const Overlay> m_overlay;
    void startOverlay(SPUCHAN *voice);
    std::condition_variable m_mixerWake;
    std::condition_variable m_cpuWake;
    // SPUCNT as last written by the CPU, which is what a CPU read returns. The mixer's spuCtrl
    // may not have applied the write yet.
    uint16_t m_ctrlShadow = 0;
    // CTRL writes queued and not yet applied by the mixer. While any is, the mixer's SPUSTAT
    // bit 6 may predate an acknowledge the CPU has already written.
    std::atomic<uint32_t> m_ctrlWritesPending = 0;
    // The ENVX walk runs on the CPU thread and needs the voice configuration as of the
    // reader's cycle, which the mixer's copy may not have reached yet.
    AdsrEnvelope m_adsrShadow[MAXCHAN];
    uint16_t m_pitchShadow[MAXCHAN] = {};
    uint8_t m_fmodShadow[MAXCHAN] = {};

    // Registers. Each has a CPU-side half (bookkeeping for reads, the ENVX walk) and a
    // mixer-side half (Apply), run when the queued write comes due.
    void SoundOn(int start, int end, uint16_t val, uint64_t cycle);
    void SoundOnApply(int start, int end, uint16_t val);
    void SoundOff(int start, int end, uint16_t val, uint64_t cycle);
    void SoundOffApply(int start, int end, uint16_t val);
    void FModOn(int start, int end, uint16_t val, uint64_t cycle);
    void FModOnApply(int start, int end, uint16_t val);
    void NoiseOn(int start, int end, uint16_t val);
    void SetPitch(int ch, uint16_t val, uint64_t cycle);
    void SetPitchApply(int ch, uint16_t val);
    void ReverbOn(int start, int end, uint16_t val);

    // XA.
    void FeedXA(xa_decode_t *xap);

    int spuIsOpen;

    // PSX buffer and addresses.
    uint16_t regArea[10000];
    // Note that SPU ram is a uint16_t, so total size is 512KB.
    uint16_t spuMem[256 * 1024];
    // Byte-addressable view of spuMem; the base for every sound-RAM pointer and
    // for the offset math that stores/restores those pointers (e.g. savestates).
    uint8_t *spuRamBase = nullptr;
    uint8_t *irqAddress = 0;
    uint8_t *spuBuffer;
    uint8_t *mixIrqAddress = 0;

    struct CaptureBuffer {
        static const int CB_SIZE = 1024 * 16;
        // These buffers have to be large enough to allow the CD-XA to stream in enough data.
        uint16_t CDCapLeft[CB_SIZE] = {0};
        uint16_t CDCapRight[CB_SIZE] = {0};

        int32_t startIndex = 0;
        int32_t endIndex = 0;
        int32_t currIndex = 0;
    };
    std::mutex cbMtx;

    // The temporary capture buffer for CD audio left/right.
    CaptureBuffer captureBuffer;
    // Emulated cycle of the last CD audio fed to the capture buffer. While the CD is
    // feeding, an empty buffer means the emulation is behind the mixer, not silence.
    // 0 means the CD has not fed anything yet.
    std::atomic<uint64_t> cdFeedCycle = 0;
    // The capture buffer index for voice 1 and voice 3.
    int32_t capBufVoiceIndex = 0;

    // CD audio input (CD-DA, and XA-ADPCM after the decoder's 44.1 kHz resampler), queued on
    // the emulated sample clock. FeedXA stamps each frame with the sample it plays at; mixChunk
    // takes one frame per output sample and adds it to the mix through CD volume and SPUCNT
    // bit 0, ahead of main volume. Shared under cbMtx.
    struct CdAudioQueue {
        static constexpr size_t kSize = 16384;
        static_assert((kSize & (kSize - 1)) == 0);
        // A stream plays this many samples (about 46ms) after the sector that starts it
        // arrives, so the drive's sector-to-sector timing jitter and the small per-sector
        // difference between the fed sample count and real time do not gap the audio.
        static constexpr uint64_t kCushion = 2048;
        // A sector landing within this of where the stream expects it continues that stream,
        // jittered; anything further off (a stop, a seek, a new stream) starts fresh with a
        // cushion. Wide enough for the jitter, narrow enough to keep a real silence a silence.
        static constexpr uint64_t kResumeWindow = 2048;
        int16_t left[kSize] = {};
        int16_t right[kSize] = {};
        uint64_t stamp[kSize] = {};
        uint64_t pushed = 0;
        uint64_t popped = 0;
        // The sample right after the last queued frame: where the stream continues.
        uint64_t next = 0;
        void clear() {
            popped = pushed;
            next = 0;
        }
        // The stamp for the first frame of audio the CD delivers at sample `arrival`. A sector
        // close to where the stream left off continues it; anything else starts a fresh stream.
        uint64_t start(uint64_t arrival) {
            const bool sameStream =
                next != 0 && arrival <= next + kResumeWindow && next <= arrival + kCushion + kResumeWindow;
            if (sameStream) return next;
            popped = pushed;
            return arrival + kCushion;
        }
        void push(int16_t l, int16_t r, uint64_t s) {
            // Full: the oldest frame goes.
            if (pushed - popped == kSize) popped++;
            const size_t i = pushed & (kSize - 1);
            left[i] = l;
            right[i] = r;
            stamp[i] = s;
            pushed++;
            next = s + 1;
        }
    };
    CdAudioQueue m_cdQueue;
    void mixCdInput(int n);
    // CD audio input volume (1F801DB0h/1DB2h), mixer side: the signed register value, which
    // scales by value/0x8000. Like main volume, each side stays at unity (0x8000) until software
    // first writes it.
    static constexpr int32_t kCdVolumeUnity = 0x8000;
    int32_t m_cdVolumeLeft = kCdVolumeUnity;
    int32_t m_cdVolumeRight = kCdVolumeUnity;
    bool m_cdLeftWritten = false;
    bool m_cdRightWritten = false;
    // Savestate load: replays the written sides of main and CD volume from regArea.
    void replayMixVolumes(uint32_t writtenMask);
    uint32_t mixVolumeWrittenMask() const;

    // User settings.
    SettingsType settings;

    // Main info struct for each channel.

    SPUCHAN s_chan[MAXCHAN + 1];  // channel + 1 infos (1 is security for fmod handling)
    ReverbUnit m_reverb;          // global reverb unit: work state + Pete/Neill reverb DSP

    NoiseGenerator m_noise;  // global noise generator: LFSR + shift/step clock

    // Main volume (1F801D80h/1D82h), decoded like a voice volume and applied to the whole
    // mix. Each side stays at unity until software first writes it, so a program that never
    // programs main volume (or a BIOS that leaves it to the game) sounds as it always did.
    VoiceVolume m_mainVolume;
    bool m_mainLeftWritten = false;
    bool m_mainRightWritten = false;

    // ENDX (1F801D9C/1D9E): one bit per voice, set when the voice consumes an
    // ADPCM block carrying the end flag, cleared on key-on. Read-only.
    std::atomic<uint32_t> spuEndx = 0;

    // Storage for the PSX register values.
    // The mixer thread writes these; the CPU thread reads SPUSTAT, so they stay atomic.
    std::atomic<uint16_t> spuCtrl = 0;
    std::atomic<uint16_t> spuStat = 0;
    uint16_t spuIrq = 0;
    // Address into SPU memory.
    uint32_t spuAddr = 0xffffffff;
    // Thread handling.
    std::atomic<int> endThread = 0;
    std::atomic<int> threadEnded = 0;
    int bSpuInit = 0;

    std::thread hMainThread;
    // Flags for faster testing of whether a new channel starts.
    uint32_t newChannelMask = 0;

    // Per-voice envelope reconstruction state, owned by the CPU thread. Not
    // serialized: it is rebuilt from the next key-on, and a savestate that resumed
    // without it would only lose the walk cache, not correctness.
    //
    // The walk starts from the KEY ON write's cycle, stamped by SoundOn on the CPU
    // side. The mixer applies the same key-on at sampleAfter() of that cycle.
    struct EnvelopeCheckpoint {
        uint64_t keyOnCycle = 0;     // CPU cycle of the KEY ON write
        uint64_t serviceSample = 0;  // sample the mixer services that key-on on
        uint64_t keyOffCycle = 0;    // CPU cycle of the KEY OFF write, 0 while none
        bool keyedOn = false;
        // Walk cache. Reads arrive in increasing cycle order (the guest polls), so
        // stepping forward from the last answer makes each read O(1) amortised
        // instead of O(samples since key-on).
        uint64_t cachedSample = 0;
        int32_t cachedState = 0;
        int32_t cachedVol = 0;
        bool cachedOn = true;
        // The ADPCM cursor, walked alongside the envelope so the walk knows the sample
        // where an end block without repeat stops the voice. Offsets into SPU RAM.
        static constexpr uint32_t kNoLoop = UINT32_MAX;
        static constexpr uint32_t kStopped = UINT32_MAX - 1;
        uint32_t block = 0;           // next block to decode, or kStopped
        uint32_t loop = kNoLoop;      // repeat address, or kNoLoop
        bool ignoreLoop = false;      // the repeat address was written by the CPU
        int left = 0;                 // samples left in the current block
        int32_t pos = 0;              // 16.16 pitch counter, same as Interpolator
        int32_t pitchStep = 0x10000;  // 16.16 pitch step
        bool ended = false;           // an end block without repeat stopped the voice
        bool untracked = false;       // pitch-modulated since key-on, so the cursor is unknown
        // Where the cursor and the repeat address were at KEY ON, for a walk rebuilt from it.
        uint32_t keyOnBlock = 0;
        uint32_t keyOnLoop = kNoLoop;
    };
    EnvelopeCheckpoint m_envelopeCheckpoint[MAXCHAN];
    void resetAdpcmWalk(int ch);
    bool adpcmWalkReachedStop(EnvelopeCheckpoint &cp);

    void (*cddavCallback)(uint16_t, uint16_t) = 0;

    // These were local variables before, but the timer procedure requires them to be global.

    int SSumR[NSSIZE];
    int SSumL[NSSIZE];
    int fmodInput[NSSIZE];
    // The shared noise level for each sample of the batch. The LFSR is one per SPU and
    // steps once per output sample, but voices are mixed channel-major, so MainThread
    // steps it NSSIZE times up front and noise voices read their sample's level here.
    int noiseLevel[NSSIZE];
    int16_t *pS;

    // XA
    xa_decode_t *xapGlobal = 0;

    int iLeftXAVol = 32767;
    int iRightXAVol = 32767;

    // XA resampler ring (see FeedXA).
    int16_t xaRingL[32] = {0};
    int16_t xaRingR[32] = {0};
    unsigned xaRingPos = 0;
    int xaSixStep = 6;
    int16_t xaLastL = 0, xaLastR = 0;
    uint8_t xaAtv[4] = {0x80, 0, 0x80, 0};
    int16_t zigzag(const int16_t *ring, unsigned table);

    SDLAudio m_audioOut = {settings};

    // Optional capture of the mix, see setWavDump. Only the mixer thread writes to it
    // once it is running.
    void writeWavDump(const SDLAudio::Frame *frames, size_t count);
    IO<File> m_wavDump;
    uint32_t m_wavDumpBytes = 0;
    xa_decode_t m_cdda;

    // Debug window.
    unsigned m_selectedChannel = 0;
    std::chrono::time_point<std::chrono::steady_clock> m_lastUpdated;
    enum { EMPTY = 0, DATA, NOISE, FMOD1, FMOD2, IRQ, MUTED } m_channelDebugTypes[MAXCHAN][DEBUG_SAMPLES];
    float m_channelDebugData[MAXCHAN][DEBUG_SAMPLES];
    char m_channelTag[MAXCHAN][CHANNEL_TAG] = {};
    unsigned m_currentDebugSample = 0;
};

}  // namespace SPU

}  // namespace PCSX
