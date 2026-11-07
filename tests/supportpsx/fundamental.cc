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

#include "supportpsx/fundamental.h"

#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>
#include <vector>

#include "gtest/gtest.h"

namespace {

using PCSX::Fundamental::c_maxInput;
using PCSX::Fundamental::estimate;

constexpr uint32_t c_rate = 44100;
constexpr double c_pi = 3.14159265358979323846;

std::vector<int16_t> quantise(const std::vector<double>& v) {
    std::vector<int16_t> out;
    out.reserve(v.size());
    for (double x : v) out.push_back(int16_t(std::clamp(std::lround(x), -32768l, 32767l)));
    return out;
}

// Sum of harmonics of `hz`, amplitude `amps[k]` on harmonic k + 1, peak-normalised to `peak`.
std::vector<int16_t> tone(double hz, std::initializer_list<double> amps, double peak = 20000.0, uint32_t rate = c_rate,
                          size_t count = c_maxInput, double dc = 0.0) {
    std::vector<double> v(count);
    for (size_t i = 0; i < count; i++) {
        unsigned k = 1;
        for (double a : amps) v[i] += a * std::sin(2.0 * c_pi * hz * k++ * double(i) / rate + 0.3 * k);
    }
    double m = 0;
    for (double x : v) m = std::max(m, std::abs(x));
    for (double& x : v) x = x * peak / m + dc;
    return quantise(v);
}

uint32_t s_seed = 12345;
double noise() {
    s_seed = s_seed * 1103515245u + 12345u;
    return double(int32_t(s_seed) >> 8) / double(1 << 23);
}

double cents(double hz, double expected) { return 1200.0 * std::log2(hz / expected); }

void expectPitch(const std::vector<int16_t>& pcm, double expected, double tolerance = 5.0, uint32_t rate = c_rate) {
    auto e = estimate(pcm, rate);
    ASSERT_TRUE(e.has_value()) << "expected " << expected << " Hz";
    EXPECT_NEAR(cents(*e, expected), 0.0, tolerance) << "expected " << expected << " Hz, got " << *e;
}

}  // namespace

TEST(Fundamental, SineAcrossTheRange) {
    // A0 to A7, then C8 and two frequencies off the equal-tempered grid.
    for (double hz : {27.5, 55.0, 110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0, 4186.01, 263.8, 1234.5}) {
        expectPitch(tone(hz, {1.0}), hz);
    }
}

TEST(Fundamental, HarmonicsLouderThanTheFundamental) {
    expectPitch(tone(220.0, {0.2, 1.0, 1.0, 0.5}), 220.0);
    expectPitch(tone(146.83, {0.3, 0.1, 1.0, 0.2, 0.6}), 146.83);
    expectPitch(tone(98.0, {1.0, 0.5, 0.33, 0.25, 0.2, 0.17, 0.14, 0.125}), 98.0);  // sawtooth-like
}

TEST(Fundamental, OddHarmonicsOnly) {
    expectPitch(tone(330.0, {1.0, 0.0, 1.0 / 3, 0.0, 1.0 / 5, 0.0, 1.0 / 7}), 330.0);  // square-like
}

TEST(Fundamental, BassWithFewPeriods) { expectPitch(tone(41.2, {1.0, 0.6, 0.4}), 41.2); }

TEST(Fundamental, QuietAndOffset) {
    expectPitch(tone(440.0, {1.0}, 64.0), 440.0);
    expectPitch(tone(440.0, {1.0, 0.5}, 12000.0, c_rate, c_maxInput, 8000.0), 440.0);
}

TEST(Fundamental, ShortInput) { expectPitch(tone(523.25, {1.0, 0.4}, 20000.0, c_rate, 2048), 523.25); }

TEST(Fundamental, OtherRate) { expectPitch(tone(440.0, {1.0, 0.5}, 20000.0, 22050), 440.0, 5.0, 22050); }

TEST(Fundamental, PluckedString) {
    // Karplus-Strong: a 224-sample delay line with a two-tap average sounds at
    // rate / (224 + 0.5), and decays.
    constexpr size_t delay = 224;
    std::vector<double> line(delay);
    s_seed = 777;
    for (auto& x : line) x = 20000.0 * noise();
    std::vector<double> out(c_maxInput);
    size_t p = 0;
    for (size_t i = 0; i < out.size(); i++) {
        double cur = line[p];
        double next = line[(p + 1) % delay];
        out[i] = cur;
        line[p] = 0.996 * 0.5 * (cur + next);
        p = (p + 1) % delay;
    }
    expectPitch(quantise(out), double(c_rate) / (delay + 0.5), 10.0);
}

TEST(Fundamental, NoiseIsUnpitched) {
    s_seed = 1;
    std::vector<double> v(c_maxInput);
    for (auto& x : v) x = 16000.0 * noise();
    auto pcm = quantise(v);
    EXPECT_FALSE(estimate(pcm, c_rate).has_value());
}

TEST(Fundamental, DecayingNoiseBurstIsUnpitched) {
    s_seed = 99;
    std::vector<double> v(c_maxInput);
    for (size_t i = 0; i < v.size(); i++) v[i] = 30000.0 * noise() * std::exp(-double(i) / 3000.0);
    auto pcm = quantise(v);
    EXPECT_FALSE(estimate(pcm, c_rate).has_value());
}

TEST(Fundamental, SilenceIsUnpitched) {
    std::vector<int16_t> pcm(c_maxInput, 0);
    EXPECT_FALSE(estimate(pcm, c_rate).has_value());
    std::vector<int16_t> dc(c_maxInput, 1234);
    EXPECT_FALSE(estimate(dc, c_rate).has_value());
    EXPECT_FALSE(estimate(std::span<const int16_t>{}, c_rate).has_value());
}
