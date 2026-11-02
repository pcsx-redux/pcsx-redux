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

#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace PCSX {

namespace SF2 {

// One mono sample, exported as one instrument with a single zone and one preset playing it.
struct Sample {
    std::string name;  // at most 19 characters are kept
    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    uint8_t rootKey = 60;   // MIDI note that plays the sample at sampleRate; 69 = 440 Hz
    int8_t correction = 0;  // pitch correction in cents
    bool loops = false;
    uint32_t loopStart = 0;  // in samples, from the start of pcm
    uint32_t loopEnd = 0;    // one past the last looped sample
};

// Serializes a SoundFont 2.01 bank. Presets are numbered in order, 128 per bank.
std::vector<uint8_t> write(const std::vector<Sample>& samples, const std::string& bankName, const std::string& comment);

// Reads back the sample headers of a SoundFont 2 bank, with their data. A sample loops when
// an instrument zone playing it sets sampleModes to 1 or 3. Presets, and every other
// generator, are ignored. Returns an empty vector if the file is not a valid bank.
std::vector<Sample> read(const uint8_t* data, size_t size);

}  // namespace SF2

}  // namespace PCSX
