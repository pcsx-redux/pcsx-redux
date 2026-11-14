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

#include "spu/sdlaudio.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include "core/psxemulator.h"
#include "core/system.h"
#include "spu/interface.h"
#include "supportpsx/iec-60908b.h"

int PCSX::SPU::SDLAudio::effectiveSpeed(int configured, int turboFactor) {
    if (configured <= 0 || configured > kMaxSpeed) return kMaxSpeed;
    if (turboFactor < 1) turboFactor = 1;
    // Both operands are clamped to kMaxSpeed first, so the product can't overflow.
    return std::min(configured * std::min(turboFactor, kMaxSpeed), kMaxSpeed);
}

static int currentSpeed(int configured) {
    const int turbo = PCSX::g_emulator ? PCSX::g_emulator->getTurboFactor() : 1;
    return PCSX::SPU::SDLAudio::effectiveSpeed(configured, turbo);
}

PCSX::SPU::SDLAudio::SDLAudio(PCSX::SPU::SettingsType& settings)
    : m_settings(settings), m_listener(g_system->m_eventBus) {
    // Enumerate compiled-in drivers. This is static information available before
    // SDL_InitSubSystem(SDL_INIT_AUDIO), so we can present the list in the UI without
    // forcing an early audio init.
    int n = SDL_GetNumAudioDrivers();
    for (int i = 0; i < n; i++) {
        const char* name = SDL_GetAudioDriver(i);
        if (name) m_backends.emplace_back(name);
    }

    m_listener.listen<Events::ExecutionFlow::Run>([this](const auto& event) {
        if (!m_audioInitialized) return;
        if (!SDL_ResumeAudioDevice(m_device)) {
            uninit();
            init(true);
        }
        if (m_settings.get<NullSync>()) startNullThread();
    });
    m_listener.listen<Events::ExecutionFlow::Pause>([this](const auto& event) {
        if (!m_audioInitialized) return;
        SDL_PauseAudioDevice(m_device);
        stopNullThread();
    });
    m_listener.listen<Events::SettingsLoaded>([this](const auto& event) { init(event.safe); });
}

void PCSX::SPU::SDLAudio::init(bool safe) {
    // Pick the audio driver. SDL_HINT_AUDIO_DRIVER must be set before SDL_InitSubSystem.
    if (safe) {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    } else {
        const std::string& wanted = m_settings.get<Backend>().value;
        bool found = false;
        for (const auto& b : m_backends) {
            if (b == wanted) {
                SDL_SetHint(SDL_HINT_AUDIO_DRIVER, b.c_str());
                found = true;
                break;
            }
        }
        if (!found) {
            m_settings.get<Backend>().reset();
            // Empty string lets SDL pick whatever default it likes.
            SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "");
        }
    }

    // Suggest a small period to keep latency low. SDL treats this as a hint; the actual
    // callback chunk size may differ, which is why streamCallback handles arbitrary sizes.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "64");

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (safe) {
            throw std::runtime_error(std::string("Unable to initialize SDL audio: ") + SDL_GetError());
        }
        uninit();
        init(true);
        return;
    }
    m_audioInitialized = true;

    // The dummy driver turns the period hint straight into its tick, in whole milliseconds:
    // io_delay = sample_frames * 1000 / spec.freq, integer division, in
    // third_party/SDL/src/audio/dummy/SDL_dummyaudio.c. 64 frames at 44100 Hz is 1.451 ms and
    // truncates to 1, so the sink drains about 30% fast, the CPU never gets ahead of it, and
    // the throttle in psxcounters.cc never binds - so anything the guest times against the SPU
    // is measured with a short ruler, and that is exactly what the SPU test suite runs on. 441
    // divides exactly (10 ms). Keyed off the driver actually in use rather than off `safe`,
    // because the failure fallbacks and an explicit backend choice both end up here too.
    const char* driver = SDL_GetCurrentAudioDriver();
    if (driver && std::string(driver) == "dummy") {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "441");
    }

    // Enumerate playback devices and locate the user's saved choice, if any.
    m_devices.clear();
    SDL_AudioDeviceID chosen = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    bool deviceFound = false;
    int devCount = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&devCount);
    if (ids) {
        const std::string& wantedDev = m_settings.get<Device>().value;
        for (int i = 0; i < devCount; i++) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (!name) continue;
            m_devices.emplace_back(name);
            if (name == wantedDev) {
                chosen = ids[i];
                deviceFound = true;
            }
        }
        SDL_free(ids);
    }
    if (!deviceFound) {
        m_settings.get<Device>().reset();
    }

#ifdef __EMSCRIPTEN__
    // SDL3's emscripten audio backend defaults to a 128-frame buffer, and the
    // Web Audio ScriptProcessorNode it drives rejects anything that is not a
    // power of two between 256 and 16384 - it throws IndexSizeError during
    // device open, which surfaces as an uncaught JS exception rather than an
    // SDL error. 1024 frames is ~21 ms at 48 kHz, comfortably inside the range
    // and still low enough not to add audible latency.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
#endif
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = kChannels;
    spec.freq = kSampleRate;

    m_device = SDL_OpenAudioDevice(chosen, &spec);
    if (m_device == 0) {
        if (safe) {
            throw std::runtime_error(std::string("Unable to open SDL audio device: ") + SDL_GetError());
        }
        uninit();
        init(true);
        return;
    }

    m_stream = SDL_CreateAudioStream(&spec, &spec);
    if (!m_stream) {
        const std::string err = SDL_GetError();
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
        if (safe) {
            throw std::runtime_error("Unable to create SDL audio stream: " + err);
        }
        uninit();
        init(true);
        return;
    }

    auto trampoline = [](void* userdata, SDL_AudioStream* stream, int additional, int /*total*/) {
        static_cast<SDLAudio*>(userdata)->streamCallback(stream, additional);
    };
    if (!SDL_SetAudioStreamGetCallback(m_stream, trampoline, this)) {
        const std::string err = SDL_GetError();
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
        throw std::runtime_error("Unable to set SDL audio stream callback: " + err);
    }

    if (!SDL_BindAudioStream(m_device, m_stream)) {
        const std::string err = SDL_GetError();
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
        throw std::runtime_error("Unable to bind SDL audio stream: " + err);
    }

    // What the device actually negotiated, which is not always what was hinted, and on the dummy
    // driver decides the drain rate the whole emulator is paced against. Worth one line at open:
    // guessing at it from the outside costs hours.
    SDL_AudioSpec got;
    int gotFrames = 0;
    m_periodFrames.store(0);
    if (SDL_GetAudioDeviceFormat(m_device, &got, &gotFrames)) {
        // The period is counted in device frames; everything that uses it counts stream frames.
        if (gotFrames > 0 && got.freq > 0) {
            m_periodFrames.store(
                static_cast<uint32_t>((static_cast<uint64_t>(gotFrames) * kSampleRate + got.freq - 1) / got.freq));
        }
        g_system->log(LogClass::SPU, "Audio: driver %s, %d Hz, %d frames per period\n", SDL_GetCurrentAudioDriver(),
                      got.freq, gotFrames);
    }

    // Devices come up in the resumed state; pause until execution actually starts so
    // we don't burn cycles streaming silence at boot.
    SDL_PauseAudioDevice(m_device);

    openPlaybackDevice(chosen);
}

void PCSX::SPU::SDLAudio::uninit() {
    stopNullThread();
    closePlaybackDevice();
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_device) {
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
    }
    if (m_audioInitialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_audioInitialized = false;
    }
}

void PCSX::SPU::SDLAudio::maybeRestart() {
    if (!g_system->running()) return;
    if (!m_audioInitialized) return;
    if (!SDL_ResumeAudioDevice(m_device)) {
        uninit();
        init(true);
        return;
    }
    if (m_settings.get<NullSync>()) startNullThread();
}

void PCSX::SPU::SDLAudio::streamCallback(SDL_AudioStream* stream, int additionalBytes) {
    constexpr int kFrameSizeBytes = sizeof(float) * kChannels;
    int requested = additionalBytes / kFrameSizeBytes;
    if (requested <= 0) return;
    if (static_cast<uint32_t>(requested) > m_periodFrames.load()) m_periodFrames.store(requested);

    const bool mono = m_settings.get<Mono>();
    const bool muted = m_settings.get<Mute>();

    static_assert(STREAMS == 2);

    // SDL doesn't promise a fixed callback chunk size, so feed in slices that fit our
    // mixing scratch buffer.
    while (requested > 0) {
        const uint32_t chunk = std::min<uint32_t>(requested, VoiceStream::BUFFER_SIZE);

        for (unsigned i = 0; i < STREAMS; i++) {
            size_t a = (i == 0) ? m_voicesStream.dequeue(m_mixBuffers[i].data(), chunk)
                                : m_audioStream.dequeue(m_mixBuffers[i].data(), chunk);
            for (size_t f = (muted ? 0 : a); f < chunk; f++) {
                // Same as the previous backend: silently zero-fill on underflow.
                // CDDA underflow on stream 1 is expected and fine.
                m_mixBuffers[i][f] = {};
            }
        }

        for (uint32_t f = 0; f < chunk; f++) {
            float l = 0.0f, r = 0.0f;
            for (unsigned i = 0; i < STREAMS; i++) {
                l += static_cast<float>(m_mixBuffers[i][f].L) / static_cast<float>(std::numeric_limits<int16_t>::max());
                r += static_cast<float>(m_mixBuffers[i][f].R) / static_cast<float>(std::numeric_limits<int16_t>::max());
            }

            if (mono) {
                const float lr = (l + r) * 0.5f;
                m_outputBuffer[f * 2 + 0] = lr;
                m_outputBuffer[f * 2 + 1] = lr;
            } else {
                m_outputBuffer[f * 2 + 0] = l;
                m_outputBuffer[f * 2 + 1] = r;
            }
        }

        SDL_PutAudioStreamData(stream, m_outputBuffer.data(), chunk * kFrameSizeBytes);

        // Fast-forward lives at the sink (the master clock). We have already kept the first `chunk`
        // frames of each stream for playback at native 44.1kHz -- pitch is preserved. To run the whole
        // emulator `speed` times faster we drain the producer rings `speed` times faster: discard the
        // additional (speed - 1) frame-runs from BOTH streams (SPU voices and CD-ROM XA). Draining the
        // rings faster paces the SPU thread faster, which through waitForGoal paces the CPU thread
        // faster. dequeue() naturally caps at what is available, so this is safe (and self-limits) at
        // any speed.
        const int speed = currentSpeed(m_settings.get<Speed>().value);
        for (int rep = 1; rep < speed; rep++) {
            m_voicesStream.dequeue(m_mixBuffers[0].data(), chunk);
            m_audioStream.dequeue(m_mixBuffers[1].data(), chunk);
        }

        // When NullSync is off, the real audio callback is the timing source. When it's
        // on, the dedicated null thread drives timing instead.
        if (!m_settings.get<NullSync>()) {
            advanceFrames(chunk);
        }

        requested -= chunk;
    }
}

void PCSX::SPU::SDLAudio::advanceFrames(uint32_t frameCount) {
    m_frameCount.store(frameCount);

    // Advance the master clock `speed` times faster than realtime. m_frames is the frame counter the
    // CPU thread waits on in waitForGoal(); the CPU's goalpost grows at the realtime rate per emulated
    // cycle, so advancing m_frames `speed` times faster makes the CPU reach its goalpost (and thus run)
    // `speed` times faster. At speed == 1 this is byte-identical to the previous fetch_add(frameCount).
    const int speed = currentSpeed(m_settings.get<Speed>().value);
    auto total = m_frames.fetch_add(frameCount * static_cast<uint32_t>(speed));

#if HAS_ATOMIC_WAIT
    auto goalpost = m_goalpost.load();
    if (goalpost == m_previousGoalpost) return;

    if (((int32_t)(goalpost - total)) > 0) return;
    m_previousGoalpost = goalpost;
    m_triggered++;
    m_triggered.notify_one();
#else
    std::unique_lock<std::mutex> l(m_mu);
    auto goalpost = m_goalpost;
    if (goalpost == m_previousGoalpost) return;

    if (((int32_t)(goalpost - total)) > 0) return;
    m_previousGoalpost = goalpost;
    m_triggered++;
    m_cv.notify_one();
#endif
}

void PCSX::SPU::SDLAudio::startNullThread() {
    if (m_nullThreadActive) return;
    m_nullThreadStop.store(false);
    m_nullThreadActive = true;
    m_nullThread = std::thread([this]() { nullThreadLoop(); });
}

void PCSX::SPU::SDLAudio::stopNullThread() {
    if (!m_nullThreadActive) return;
    m_nullThreadStop.store(true);
    if (m_nullThread.joinable()) m_nullThread.join();
    m_nullThreadActive = false;
}

void PCSX::SPU::SDLAudio::nullThreadLoop() {
    using namespace std::chrono;
    // 64 frames at 44100 Hz ~= 1.451 ms per tick. We pretend to consume that many frames
    // each tick, mirroring what miniaudio's null backend used to do for a stable timing
    // source independent of the real device's bursty callbacks.
    constexpr double periodSeconds = static_cast<double>(kPeriodFrames) / kSampleRate;
    const auto periodNs = duration_cast<nanoseconds>(duration<double>(periodSeconds));
    auto next = steady_clock::now();
    while (!m_nullThreadStop.load()) {
        next += periodNs;
        std::this_thread::sleep_until(next);
        advanceFrames(kPeriodFrames);
    }
}

// Standalone sound playback.

void PCSX::SPU::SDLAudio::openPlaybackDevice(SDL_AudioDeviceID physical) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = kChannels;
    spec.freq = kSampleRate;
    // Opening a physical device again yields a new logical device on the same hardware, with its own
    // pause state. This one is never paused, so sounds play regardless of the emulation state.
    m_playbackDevice = SDL_OpenAudioDevice(physical, &spec);
    if (m_playbackDevice == 0) {
        // Not fatal for the emulator itself; playSound will report the problem.
        g_system->log(LogClass::SPU, "Unable to open SDL audio device for sound playback: %s\n", SDL_GetError());
        return;
    }
    updatePlaybackMute();
}

void PCSX::SPU::SDLAudio::closePlaybackDevice() {
    // Destroying the streams before closing the device and quitting the audio subsystem. The sounds
    // themselves may still be referenced from Lua; they will then simply report as not playing.
    for (auto sound : m_sounds) sound->detach();
    m_sounds.clear();
    if (m_playbackDevice) {
        SDL_CloseAudioDevice(m_playbackDevice);
        m_playbackDevice = 0;
    }
}

void PCSX::SPU::SDLAudio::updatePlaybackMute() {
    if (!m_playbackDevice) return;
    SDL_SetAudioDeviceGain(m_playbackDevice, m_settings.get<Mute>() ? 0.0f : 1.0f);
}

PCSX::SPU::SDLAudio::Sound::~Sound() { stop(); }

void PCSX::SPU::SDLAudio::Sound::detach() {
    if (m_stream) {
        // This unbinds the stream first, which waits for any running callback to complete.
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    m_feeding.store(false);
    m_owner = nullptr;
}

void PCSX::SPU::SDLAudio::Sound::stop() {
    if (m_owner) m_owner->m_sounds.erase(this);
    detach();
}

bool PCSX::SPU::SDLAudio::Sound::isPlaying() {
    if (!m_stream) return false;
    if (m_feeding.load()) return true;
    return SDL_GetAudioStreamQueued(m_stream) > 0;
}

void PCSX::SPU::SDLAudio::Sound::setGain(float gain) {
    m_gain = gain;
    if (m_stream) SDL_SetAudioStreamGain(m_stream, gain);
}

void PCSX::SPU::SDLAudio::Sound::spuCallback(SDL_AudioStream* stream, int additionalBytes) {
    if (!m_feeding.load()) return;
    // Decode a little bit ahead of what SDL asks for, in batches of blocks.
    constexpr unsigned kBatchBlocks = 32;
    std::array<int16_t, 28 * kBatchBlocks> buffer;
    int needed = std::max(additionalBytes, int(28 * sizeof(int16_t) * 8));
    bool finished = false;
    while ((needed > 0) && !finished) {
        unsigned blocks = 0;
        while ((blocks < kBatchBlocks) && !finished) {
            const uint8_t* block = m_spuData.data() + m_spuPosition;
            uint8_t flags;
            m_decoder.decodeSPUBlock(block, buffer.data() + blocks * 28, &flags);
            blocks++;
            if (flags & ADPCM::Decoder::LoopStart) m_spuLoopStart = m_spuPosition;
            if (flags & ADPCM::Decoder::LoopEnd) {
                if (!(flags & ADPCM::Decoder::LoopRepeat)) {
                    // End+Mute: the voice is released here.
                    finished = true;
                } else {
                    // End+Repeat. A single block looping onto itself, with filter 0 and all zero nibbles,
                    // is the conventional silent terminator of a one-shot sample: treat it as the end.
                    const uint8_t* target = m_spuData.data() + m_spuLoopStart;
                    bool silent = (m_spuLoopStart == m_spuPosition) && ((target[0] & 0x70) == 0);
                    for (unsigned i = 2; silent && (i < 16); i++) silent = target[i] == 0;
                    if (silent) {
                        finished = true;
                    } else {
                        m_spuPosition = m_spuLoopStart;
                    }
                }
            } else {
                m_spuPosition += 16;
                if (m_spuPosition >= m_spuData.size()) finished = true;
            }
        }
        const int bytes = blocks * 28 * sizeof(int16_t);
        SDL_PutAudioStreamData(stream, buffer.data(), bytes);
        needed -= bytes;
    }
    if (finished) {
        // Make sure the tail of the data goes through the resampler.
        SDL_FlushAudioStream(stream);
        m_feeding.store(false);
    }
}

namespace {

// Decodes XA data, either raw 128-byte sound groups, or CD sectors, into interleaved 16-bit samples.
bool decodeXA(const uint8_t* data, size_t size, PCSX::SPU::SDLAudio::SoundDescriptor& descriptor,
              std::vector<int16_t>& output, std::string& error) {
    PCSX::ADPCM::Decoder decoder;
    decoder.reset();
    int16_t samples[224];
    auto decodeGroups = [&](const uint8_t* groups, unsigned count) {
        for (unsigned i = 0; i < count; i++) {
            unsigned n = decoder.decodeXASoundGroup(groups + i * 128, samples, descriptor.bits, descriptor.channels);
            output.insert(output.end(), samples, samples + n);
        }
    };

    if (!descriptor.sectors) {
        if ((size % 128) != 0) {
            error = "xa data size must be a multiple of 128 bytes (one sound group)";
            return false;
        }
        output.reserve(size / 128 * 224);
        decodeGroups(data, size / 128);
        return true;
    }

    // Sector mode. A buffer is made of 2352-byte raw sectors if its size is a multiple of 2352, and if
    // it starts with the CD sync pattern; otherwise, it is made of 2336-byte sectors, starting at the
    // subheader, if its size is a multiple of 2336. Each sector is then a subheader, a copy of it, and
    // 18 sound groups, followed by padding and EDC, which are ignored.
    size_t sectorSize = 0;
    size_t subheaderOffset = 0;
    if (((size % 2352) == 0) && (size >= sizeof(PCSX::IEC60908b::SYNC)) &&
        (memcmp(data, PCSX::IEC60908b::SYNC, sizeof(PCSX::IEC60908b::SYNC)) == 0)) {
        sectorSize = 2352;
        subheaderOffset = 16;
    } else if ((size % 2336) == 0) {
        sectorSize = 2336;
        subheaderOffset = 0;
    } else {
        error =
            "xa sector data size must be a multiple of 2352 bytes (raw sectors starting with a sync pattern) or of "
            "2336 bytes (sectors starting at the subheader)";
        return false;
    }
    const size_t sectorCount = size / sectorSize;
    bool formatKnown = false;
    uint8_t codingInfo = 0;
    for (size_t sector = 0; sector < sectorCount; sector++) {
        const uint8_t* s = data + sector * sectorSize;
        if ((sectorSize == 2352) && (memcmp(s, PCSX::IEC60908b::SYNC, sizeof(PCSX::IEC60908b::SYNC)) != 0)) {
            error = "xa sector " + std::to_string(sector) + " does not start with a sync pattern";
            return false;
        }
        const uint8_t* subheader = s + subheaderOffset;
        // Submode bit 2 flags audio sectors; everything else is skipped.
        if ((subheader[2] & 0x04) == 0) continue;
        if ((descriptor.xaFile >= 0) && (subheader[0] != descriptor.xaFile)) continue;
        if ((descriptor.xaChannel >= 0) && (subheader[1] != descriptor.xaChannel)) continue;
        // Without a filter, play the stream of the first audio sector only; files usually interleave several.
        if (descriptor.xaFile < 0) descriptor.xaFile = subheader[0];
        if (descriptor.xaChannel < 0) descriptor.xaChannel = subheader[1];
        const uint8_t ci = subheader[3];
        if (!formatKnown) {
            const unsigned stereo = ci & 3;
            const unsigned rate = (ci >> 2) & 3;
            const unsigned bits = (ci >> 4) & 3;
            if ((stereo > 1) || (rate > 1) || (bits > 1)) {
                error = "xa sector " + std::to_string(sector) + " has reserved coding info values";
                return false;
            }
            descriptor.channels = stereo ? 2 : 1;
            descriptor.rate = rate ? 18900 : 37800;
            descriptor.bits = bits ? 8 : 4;
            codingInfo = ci;
            formatKnown = true;
            output.reserve(sectorCount * 18 * 224);
        } else if ((ci & 0x3f) != (codingInfo & 0x3f)) {
            error = "xa sector " + std::to_string(sector) +
                    " has a different coding info than the first audio sector; mixed formats are not supported";
            return false;
        }
        decodeGroups(subheader + 8, 18);
    }
    if (!formatKnown) {
        error = "xa sector data contains no audio sector";
        return false;
    }
    return true;
}

}  // namespace

std::shared_ptr<PCSX::SPU::SDLAudio::Sound> PCSX::SPU::SDLAudio::playSound(const uint8_t* data, size_t size,
                                                                           const SoundDescriptor& descriptorIn,
                                                                           std::string& error) {
    SoundDescriptor descriptor = descriptorIn;
    if (!m_playbackDevice) {
        error = "no audio device available for sound playback";
        return nullptr;
    }
    if (size == 0) {
        error = "audio data is empty";
        return nullptr;
    }
    if (!(descriptor.gain >= 0.0f)) {
        error = "gain must be a non-negative number";
        return nullptr;
    }

    SDL_AudioSpec srcSpec;
    // The data put in the stream: either a pointer into the caller's buffer, or into decoded samples.
    const void* streamData = nullptr;
    size_t streamSize = 0;
    std::vector<int16_t> decoded;

    auto sound = std::shared_ptr<Sound>(new Sound());

    switch (descriptor.format) {
        case SoundDescriptor::Format::PCM: {
            if ((descriptor.bits != 8) && (descriptor.bits != 16)) {
                error = "pcm bits must be 8 or 16";
                return nullptr;
            }
            if ((descriptor.channels != 1) && (descriptor.channels != 2)) {
                error = "pcm channels must be 1 or 2";
                return nullptr;
            }
            if ((descriptor.rate == 0) || (descriptor.rate > 384000)) {
                error = "pcm rate must be between 1 and 384000 Hz";
                return nullptr;
            }
            if (descriptor.bits == 8) {
                srcSpec.format = descriptor.isSigned ? SDL_AUDIO_S8 : SDL_AUDIO_U8;
            } else {
                if (!descriptor.isSigned) {
                    error = "pcm 16 bits data must be signed";
                    return nullptr;
                }
                srcSpec.format = SDL_AUDIO_S16LE;
            }
            const size_t frameSize = descriptor.bits / 8 * descriptor.channels;
            if ((size % frameSize) != 0) {
                error = "pcm data size must be a multiple of the frame size (" + std::to_string(frameSize) + " bytes)";
                return nullptr;
            }
            srcSpec.channels = descriptor.channels;
            srcSpec.freq = descriptor.rate;
            streamData = data;
            streamSize = size;
            break;
        }
        case SoundDescriptor::Format::SPU: {
            if ((descriptor.rate == 0) || (descriptor.rate > 4 * 44100)) {
                error = "spu rate must be between 1 and 176400 Hz (pitch 0x0001 to 0x4000)";
                return nullptr;
            }
            if ((size % 16) != 0) {
                error = "spu data size must be a multiple of 16 bytes (one block)";
                return nullptr;
            }
            srcSpec.format = SDL_AUDIO_S16;
            srcSpec.channels = 1;
            srcSpec.freq = descriptor.rate;
            sound->m_spuData.assign(data, data + size);
            sound->m_decoder.reset();
            sound->m_feeding.store(true);
            break;
        }
        case SoundDescriptor::Format::XA: {
            if (!descriptor.sectors) {
                if ((descriptor.bits != 4) && (descriptor.bits != 8)) {
                    error = "xa bits must be 4 or 8";
                    return nullptr;
                }
                if ((descriptor.channels != 1) && (descriptor.channels != 2)) {
                    error = "xa channels must be 1 or 2";
                    return nullptr;
                }
                if ((descriptor.rate != 37800) && (descriptor.rate != 18900)) {
                    error = "xa rate must be 37800 or 18900";
                    return nullptr;
                }
            }
            if (!decodeXA(data, size, descriptor, decoded, error)) return nullptr;
            srcSpec.format = SDL_AUDIO_S16;
            srcSpec.channels = descriptor.channels;
            srcSpec.freq = descriptor.rate;
            streamData = decoded.data();
            streamSize = decoded.size() * sizeof(int16_t);
            break;
        }
        default:
            error = "unknown audio format";
            return nullptr;
    }

    SDL_AudioSpec dstSpec;
    dstSpec.format = SDL_AUDIO_F32;
    dstSpec.channels = kChannels;
    dstSpec.freq = kSampleRate;
    SDL_AudioStream* stream = SDL_CreateAudioStream(&srcSpec, &dstSpec);
    if (!stream) {
        error = std::string("unable to create SDL audio stream: ") + SDL_GetError();
        return nullptr;
    }
    sound->m_stream = stream;
    sound->m_owner = this;
    sound->m_gain = descriptor.gain;
    SDL_SetAudioStreamGain(stream, descriptor.gain);

    if (streamData) {
        // Put copies the data, so the caller's buffer, or our decoded one, can go away after this.
        if (!SDL_PutAudioStreamData(stream, streamData, static_cast<int>(streamSize)) ||
            !SDL_FlushAudioStream(stream)) {
            error = std::string("unable to queue audio data: ") + SDL_GetError();
            sound->detach();
            return nullptr;
        }
    } else {
        auto trampoline = [](void* userdata, SDL_AudioStream* stream, int additional, int /*total*/) {
            static_cast<Sound*>(userdata)->spuCallback(stream, additional);
        };
        if (!SDL_SetAudioStreamGetCallback(stream, trampoline, sound.get())) {
            error = std::string("unable to set SDL audio stream callback: ") + SDL_GetError();
            sound->detach();
            return nullptr;
        }
    }

    if (!SDL_BindAudioStream(m_playbackDevice, stream)) {
        error = std::string("unable to bind SDL audio stream: ") + SDL_GetError();
        sound->detach();
        return nullptr;
    }
    m_sounds.insert(sound.get());
    return sound;
}
