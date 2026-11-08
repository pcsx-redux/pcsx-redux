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

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <mutex>
#include <numbers>

namespace {

using Complex = std::complex<double>;

constexpr size_t c_fftMax = std::bit_ceil(PCSX::Fundamental::c_maxInput);
constexpr size_t c_maxLag = PCSX::Fundamental::c_maxInput / 2 + 1;

struct Scratch {
    Scratch() {
        static std::once_flag flag;
        std::call_once(flag, []{
            for (size_t k = 0; k < c_fftMax / 2; k++) {
                const double angle = -2.0 * std::numbers::pi * double(k) / double(c_fftMax);
                twiddle[k] = Complex(std::cos(angle), std::sin(angle));
            }
        });
    }
    static Complex twiddle[c_fftMax / 2];
    Complex a[c_fftMax];
    Complex b[c_fftMax];
    double x[PCSX::Fundamental::c_maxInput];
    double energy[PCSX::Fundamental::c_maxInput + 1];
    double d[c_maxLag + 1];
    double cmnd[c_maxLag + 1];
};

Complex Scratch::twiddle[c_fftMax / 2] = {};

// In-place forward radix-2 FFT of size m, a power of two no larger than c_fftMax.
void fft(const Scratch& s, Complex* v, size_t m) {
    for (size_t i = 1, j = 0; i < m; i++) {
        size_t bit = m >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(v[i], v[j]);
    }
    for (size_t len = 2; len <= m; len <<= 1) {
        const size_t half = len / 2;
        const size_t stride = c_fftMax / len;
        for (size_t i = 0; i < m; i += len) {
            for (size_t k = 0; k < half; k++) {
                const Complex t = v[i + k + half] * s.twiddle[k * stride];
                v[i + k + half] = v[i + k] - t;
                v[i + k] += t;
            }
        }
    }
}

}  // namespace

// YIN, after de Cheveigné and Kawahara
std::optional<double> PCSX::Fundamental::estimate(std::span<const int16_t> pcm, uint32_t rate) {
    constexpr double threshold = 0.2;

    if (rate == 0) return std::nullopt;
    const double drate = double(rate);
    const size_t n = std::min(pcm.size(), c_maxInput);
    const size_t tauMin = std::max<size_t>(2, size_t(std::floor(drate / c_highestHz)));
    const size_t tauMax = std::min<size_t>(size_t(std::ceil(drate / c_lowestHz)) + 1, n / 2);
    if (tauMax <= tauMin + 1) return std::nullopt;
    const size_t window = n - tauMax;

    static thread_local Scratch s;
    double* x = s.x;
    double* energy = s.energy;
    double* d = s.d;
    double* cmnd = s.cmnd;

    double mean = 0.0;
    for (size_t i = 0; i < n; i++) mean += pcm[i];
    mean /= double(n);
    energy[0] = 0.0;
    for (size_t i = 0; i < n; i++) {
        x[i] = double(pcm[i]) - mean;
        energy[i + 1] = energy[i] + x[i] * x[i];
    }
    if (energy[n] < 1.0) return std::nullopt;

    const size_t m = std::bit_ceil(n);
    for (size_t i = 0; i < m; i++) {
        s.a[i] = i < window ? x[i] : 0.0;
        s.b[i] = i < n ? x[i] : 0.0;
    }
    fft(s, s.a, m);
    fft(s, s.b, m);
    for (size_t i = 0; i < m; i++) s.a[i] = std::conj(std::conj(s.a[i]) * s.b[i]);
    fft(s, s.a, m);

    // d[tau] = sum over the window of (x[j] - x[j + tau])^2
    for (size_t tau = 1; tau <= tauMax; tau++) {
        const double r = s.a[tau].real() / double(m);
        d[tau] = std::max(0.0, energy[window] + (energy[tau + window] - energy[tau]) - 2.0 * r);
    }

    double running = 0.0;
    for (size_t tau = 1; tau <= tauMax; tau++) {
        running += d[tau];
        cmnd[tau] = running > 0.0 ? d[tau] * double(tau) / running : 1.0;
    }

    size_t best = 0;
    for (size_t tau = tauMin; tau < tauMax; tau++) {
        if (cmnd[tau] < threshold) {
            while (tau + 1 < tauMax && cmnd[tau + 1] < cmnd[tau]) tau++;
            best = tau;
            break;
        }
    }
    if (best == 0) return std::nullopt;

    double period = double(best);
    if (best > 1 && best < tauMax) {
        const double a = d[best - 1], b = d[best], c = d[best + 1];
        const double denom = a - 2.0 * b + c;
        if (denom > 0.0) period += std::clamp(0.5 * (a - c) / denom, -1.0, 1.0);
    }
    if (period <= 0.0) return std::nullopt;

    return drate / period;
}
