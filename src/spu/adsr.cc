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

#include "spu/adsr.h"

#include <stdint.h>

#include <algorithm>

#include "support/table-generator.h"

// The ADSR rate/level model derives from Neill Corlett's PlayStation SPU
// envelope-timing notes; psx-spx (soundprocessingunitspu.md) is the
// authoritative hardware reference. The live envelope level is a 15-bit value
// (0..0x7fff). The volume factor handed to the mixer, and reported by voice
// register 0xC, is that level shifted down to 10 bits (0..0x400).

namespace EnvelopeTables {
// The rate tables are built at compile time (consteval; table math by Nic).
// They encode the classic SPU rate curve whose period doubles every four rate
// steps: a phase counts samples in the fraction accumulator up to
// denominator[rate], then steps the level by numerator_increase[rate] (rising)
// or numerator_decrease[rate] (falling, negative).
// The hardware adds 0x8000 >> (shift - 11) to its step counter each sample and clamps
// that increment to at least 1, so every shift from 26 up steps once per 32768 samples
// (psx-spx: "a rate of 0x76 behaves like 0x6A").
constexpr int kMaxEffectiveShift = 26;
struct DenominatorGenerator {
    static consteval int32_t calculateValue(std::size_t rate) {
        const int shift = static_cast<int>(rate >> 2);
        return (rate < 48) ? 1 : (1 << (std::min(shift, kMaxEffectiveShift) - 11));
    }
};

struct NumeratorIncreaseGenerator {
    static consteval int32_t calculateValue(std::size_t rate) {
        return (rate < 48) ? (7 - (rate & 3)) << (11 - (rate >> 2)) : (7 - (rate & 3));
    }
};

struct NumeratorDecreaseGenerator {
    static consteval int32_t calculateValue(std::size_t rate) {
        return (rate < 48) ? (-8 + (rate & 3)) << (11 - (rate >> 2)) : (-8 + (rate & 3));
    }
};

constexpr auto denominator = PCSX::generateTable<128, DenominatorGenerator>();
constexpr auto numerator_increase = PCSX::generateTable<128, NumeratorIncreaseGenerator>();
constexpr auto numerator_decrease = PCSX::generateTable<128, NumeratorDecreaseGenerator>();

static_assert(denominator.data[0x6a] == 32768);
static_assert(denominator.data[0x76] == denominator.data[0x6a]);
static_assert(denominator.data[0x7c] == 32768);
}  // namespace EnvelopeTables

namespace {
constexpr int32_t kEnvelopeMax = 0x7fff;       // 15-bit envelope ceiling (32767)
constexpr int kEnvelopeToLevelShift = 5;       // 15-bit level -> 10-bit volume factor
constexpr int32_t kExponentialKnee = 0x6000;   // above here the exponential rise flattens...
constexpr int kExponentialKneeRateStep = 8;    // ...by stepping to a slower rate-table entry
constexpr int kExponentialDecreaseShift = 15;  // Q15 scale for an exponential decrease
constexpr int kCoarseRateScale = 4;            // decay/release rate -> fine rate-table index
constexpr int kSustainLevelShift = 11;         // top four bits of the level...
constexpr int32_t kSustainLevelMask = 0xf;     // ...compared against the 0..15 sustain level
// The attack and sustain rates arrive from the register decode as a full 0..127 (attack is
// (val & 0x7f00) >> 8, sustain is (val & 0x1fc0) >> 6), so the knee's rate-step lands past the
// end of the 128-entry tables on the slowest rates. Saturate instead of reading off the end.
constexpr int kMaxRateIndex = 127;
// A rate field of all ones (step and shift bits together) never steps. That is 0x7f for
// attack and sustain and 0x1f for release, tested on the register value before the knee.
constexpr int kFrozenRate = 0x7f;
constexpr int kFrozenCoarseRate = 0x1f;

// A rate with a period of P samples steps on the samples where the low bits of a free-running
// counter shared by all voices read P/2. Key-on does not reset it, so two voices on the same rate
// step on the same samples whenever they were keyed on. On silicon the counter's phase against the
// capture position (SPUSTAT bit 11) changes from one power-on to the next; this value puts a
// 256-sample period's steps 49 samples after a bit 11 edge, as on one SCPH-1001 run.
constexpr uint64_t kStepCounterPhase = 79;
bool stepLands(int rateIndex, uint64_t sample) {
    const uint64_t period = EnvelopeTables::denominator.data[rateIndex];
    return ((sample + kStepCounterPhase) & (period - 1)) == (period >> 1);
}
}  // namespace

// Write the freshly computed envelope state back and return the full 15-bit
// (0..0x7fff) envelope volume. The mixer applies it as `sample * envelope >> 15`,
// matching the hardware SPU. The 0..0x400 `exVolume` mirror (envelope >> 5) is
// still maintained for the voice register / debugger, but it is no longer the
// value handed to the mixer (that lossy 10-bit gain + /1023 divide ran enveloped
// samples ~0.1% high and discarded the envelope's low 5 bits).
int PCSX::SPU::AdsrEnvelope::commit(int32_t envelopeVol) {
    const int level = envelopeVol >> kEnvelopeToLevelShift;
    m_adsrx.get<exEnvelopeVol>().value = envelopeVol;
    m_adsrx.get<exVolume>().value = level;
    return envelopeVol;
}

int PCSX::SPU::AdsrEnvelope::Attack(uint64_t sample) {
    int rateIndex = m_adsrx.get<exAttackRate>().value;
    int32_t envelopeVol = m_adsrx.get<exEnvelopeVol>().value;
    const bool exponential = m_adsrx.get<exAttackModeExp>().value != 0;

    if (rateIndex == kFrozenRate) return commit(envelopeVol);

    // Past the knee the exponential attack curve flattens to a slower rate.
    if (exponential && envelopeVol >= kExponentialKnee) {
        rateIndex = std::min(rateIndex + kExponentialKneeRateStep, kMaxRateIndex);
    }

    if (stepLands(rateIndex, sample)) {
        envelopeVol += EnvelopeTables::numerator_increase.data[rateIndex];
    }

    if (envelopeVol >= kEnvelopeMax) {
        envelopeVol = kEnvelopeMax;
        m_adsrx.get<exState>().value = ADSRState::Decay;
    }

    return commit(envelopeVol);
}

int PCSX::SPU::AdsrEnvelope::Decay(uint64_t sample) {
    const int rateIndex = m_adsrx.get<exDecayRate>().value * kCoarseRateScale;
    int32_t envelopeVol = m_adsrx.get<exEnvelopeVol>().value;

    // Decay has no mode bit of its own: psx-spx lists it as "Fixed, always Exponential".
    // The release mode bit only governs Release.
    // The sustain level is only checked on samples where a step lands: on silicon a slow
    // decay with sustain level 15 holds 0x7fff until its first step, takes it (0x7ff7),
    // and only then moves to Sustain.
    if (stepLands(rateIndex, sample)) {
        envelopeVol += (EnvelopeTables::numerator_decrease.data[rateIndex] * envelopeVol) >> kExponentialDecreaseShift;
        envelopeVol = std::max(envelopeVol, 0);

        if (((envelopeVol >> kSustainLevelShift) & kSustainLevelMask) <= m_adsrx.get<exSustainLevel>().value) {
            m_adsrx.get<exState>().value = ADSRState::Sustain;
        }
    }

    return commit(envelopeVol);
}

int PCSX::SPU::AdsrEnvelope::Sustain(uint64_t sample) {
    int rateIndex = m_adsrx.get<exSustainRate>().value;
    int32_t envelopeVol = m_adsrx.get<exEnvelopeVol>().value;
    const bool exponential = m_adsrx.get<exSustainModeExp>().value != 0;
    const bool increase = m_adsrx.get<exSustainIncrease>().value != 0;

    if (rateIndex == kFrozenRate) return commit(envelopeVol);

    if (increase) {
        // Past the knee the exponential rise flattens to a slower rate.
        if (exponential && envelopeVol >= kExponentialKnee) {
            rateIndex = std::min(rateIndex + kExponentialKneeRateStep, kMaxRateIndex);
        }

        if (stepLands(rateIndex, sample)) {
            envelopeVol += EnvelopeTables::numerator_increase.data[rateIndex];
        }

        envelopeVol = std::min(envelopeVol, kEnvelopeMax);
    } else {
        if (stepLands(rateIndex, sample)) {
            envelopeVol += exponential ? (EnvelopeTables::numerator_decrease.data[rateIndex] * envelopeVol) >>
                                             kExponentialDecreaseShift
                                       : EnvelopeTables::numerator_decrease.data[rateIndex];
        }

        envelopeVol = std::max(envelopeVol, 0);
    }

    return commit(envelopeVol);
}

int PCSX::SPU::AdsrEnvelope::Release(bool &channelOn, uint64_t sample) {
    const int rateIndex = m_adsrx.get<exReleaseRate>().value * kCoarseRateScale;
    int32_t envelopeVol = m_adsrx.get<exEnvelopeVol>().value;
    const bool exponential = m_adsrx.get<exReleaseModeExp>().value != 0;

    if (m_adsrx.get<exReleaseRate>().value == kFrozenCoarseRate) return commit(envelopeVol);

    if (stepLands(rateIndex, sample)) {
        envelopeVol += exponential ? (EnvelopeTables::numerator_decrease.data[rateIndex] * envelopeVol) >>
                                         kExponentialDecreaseShift
                                   : EnvelopeTables::numerator_decrease.data[rateIndex];
    }

    if (envelopeVol < 0) {
        m_adsrx.get<exState>().value = ADSRState::Stopped;
        envelopeVol = 0;
        channelOn = false;
    }

    return commit(envelopeVol);
}

void PCSX::SPU::AdsrEnvelope::keyOn() {
    // Reset the live envelope to the very start of the Attack phase. The level is
    // genuinely zero until the first Attack step lands; ENVX reads it as such.
    m_adsrx.get<exVolume>().value = 0;
    m_adsrx.get<exState>().value = ADSRState::Attack;
    m_adsrx.get<exEnvelopeVol>().value = 0;
}

int PCSX::SPU::AdsrEnvelope::step(bool stopRequested, bool &channelOn, uint64_t sample) {
    if (stopRequested) {
        m_adsrx.get<exState>().value = ADSRState::Release;
    }

    switch (m_adsrx.get<exState>().value) {
        case ADSRState::Attack:
            return Attack(sample);
        case ADSRState::Decay:
            return Decay(sample);
        case ADSRState::Sustain:
            return Sustain(sample);
        case ADSRState::Release:
            return Release(channelOn, sample);
    }

    return 0;
}
