/*

MIT License

Copyright (c) 2022 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <charconv>
#include <compare>
#include <string_view>

#include "fmt/format.h"
#include "support/strings-helpers.h"

namespace PCSX {

namespace IEC60908b {

enum class SectorMode {
    GUESS,     // will try and guess the sector mode based on flags found in the first sector
    RAW,       // 2352 bytes per sector
    M1,        // 2048 bytes per sector
    M2_RAW,    // 2336 bytes per sector, includes subheader; can't be guessed
    M2_FORM1,  // 2048 bytes per sector
    M2_FORM2,  // 2324 bytes per sector
};

static constexpr size_t FRAMESIZE_RAW = 2352;
static constexpr size_t DATA_SIZE = FRAMESIZE_RAW - 12;
// The 12 bytes a raw sector opens with, before its header.
static constexpr uint8_t SYNC[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
static constexpr size_t SUB_FRAMESIZE = 96;

union Sub {
    uint8_t raw[96];
    struct {
        uint8_t P[12];
        union {
            struct {
                uint8_t ControlAndADR;
                uint8_t TrackNumber;
                uint8_t IndexNumber;
                uint8_t RelativeAddress[3];
                uint8_t Zero;
                uint8_t AbsoluteAddress[3];
                uint8_t CRC[2];
            };
            uint8_t Q[12];
        };
        uint8_t R[12];
        uint8_t S[12];
        uint8_t T[12];
        uint8_t U[12];
        uint8_t V[12];
        uint8_t W[12];
    };
};

static inline constexpr uint8_t btoi(uint8_t b) { return ((b / 16) * 10) + (b % 16); }
static inline constexpr uint8_t itob(uint8_t i) { return ((i / 10) * 16) + (i % 10); }

struct MSF {
    MSF() : m(0), s(0), f(0) {}
    MSF(uint8_t m, uint8_t s, uint8_t f) : m(m), s(s), f(f) {}
    explicit MSF(uint32_t lba) {
        m = lba / 75 / 60;
        lba = lba - m * 75 * 60;
        s = lba / 75;
        lba = lba - s * 75;
        f = lba;
    }
    explicit MSF(const std::string_view &msf) {
        m = s = f = 0;
        auto tokens = StringsHelpers::split(msf, ":");
        auto conv = [&tokens](int index) -> uint8_t {
            if (index >= tokens.size()) return 0;
            auto &sv = tokens[index];
            uint8_t r;
            auto result = std::from_chars(sv.data(), sv.data() + sv.size(), r);
            if (result.ec == std::errc::invalid_argument) return 0;
            return r;
        };
        m = conv(0);
        s = conv(1);
        f = conv(2);
    }
    auto operator<=>(const MSF &other) const {
        if (m != other.m) return m <=> other.m;
        if (s != other.s) return s <=> other.s;
        return f <=> other.f;
    }
    bool operator==(const MSF &other) const { return m == other.m && s == other.s && f == other.f; }
    MSF &operator++() {
        f++;
        if (f >= 75) {
            f = 0;
            s++;
            if (s >= 60) {
                s = 0;
                m++;
            }
        }
        return *this;
    }
    MSF operator++(int) {
        MSF tmp = *this;
        ++(*this);
        return tmp;
    }
    MSF &operator--() {
        if (f == 0) {
            f = 74;
            if (s == 0) {
                s = 59;
                if (m == 0) {
                    m = 99;
                } else {
                    m--;
                }
            } else {
                s--;
            }
        } else {
            f--;
        }
        return *this;
    }
    MSF operator--(int) {
        MSF tmp = *this;
        --(*this);
        return tmp;
    }
    MSF operator+(const MSF &other) const {
        MSF tmp = *this;
        tmp += other;
        return tmp;
    }
    MSF operator-(const MSF &other) const {
        MSF tmp = *this;
        tmp -= other;
        return tmp;
    }
    MSF &operator+=(const MSF &other) {
        uint32_t lba = toLBA() + other.toLBA();
        *this = MSF(lba);
        return *this;
    }
    MSF &operator-=(const MSF &other) {
        uint32_t lba = toLBA() - other.toLBA();
        *this = MSF(lba);
        return *this;
    }
    constexpr uint32_t toLBA() const { return (m * 60 + s) * 75 + f; }
    constexpr void toBCD(uint8_t *dst) const {
        dst[0] = itob(m);
        dst[1] = itob(s);
        dst[2] = itob(f);
    }
    constexpr void fromBCD(const uint8_t *src) {
        m = btoi(src[0]);
        s = btoi(src[1]);
        f = btoi(src[2]);
    }
    void reset() { m = s = f = 0; }
    union {
        struct {
            uint8_t m;
            uint8_t s;
            uint8_t f;
            uint8_t pad;
        };
        uint8_t data[4];
    };
};

// Compute the EDC and ECC for a mode2 sector.
void computeEDCECC(uint8_t *sector);

// Compute the CRC-16 for the SubQ channel.
uint16_t subqCRC(const uint8_t *d, int len = 10);

struct SubHeaders {
    uint8_t fileNumber;
    uint8_t channelNumber;
    uint8_t subMode;
    uint8_t codingInfo;
    bool fromBuffer(const uint8_t buffer[]) {
        fileNumber = buffer[0];
        channelNumber = buffer[1];
        subMode = buffer[2];
        codingInfo = buffer[3];
        return (buffer[0] == buffer[4]) && (buffer[1] == buffer[5]) && (buffer[2] == buffer[6]) &&
               (buffer[3] == buffer[7]);
    }
    void toBuffer(uint8_t buffer[]) const {
        buffer[0] = buffer[4] = fileNumber;
        buffer[1] = buffer[5] = channelNumber;
        buffer[2] = buffer[6] = subMode;
        buffer[3] = buffer[7] = codingInfo;
    }
    bool isEndOfRecord() const { return subMode & 0x01; }
    bool isVideo() const { return subMode & 0x02; }
    bool isAudio() const { return subMode & 0x04; }
    bool isData() const { return subMode & 0x08; }
    bool isTrigger() const { return subMode & 0x10; }
    bool isForm2() const { return subMode & 0x20; }
    bool isRealTime() const { return subMode & 0x40; }
    bool isEOF() const { return subMode & 0x80; }

    void setEndOfRecord() { subMode |= 0x01; }
    void setVideo() { subMode |= 0x02; }
    void setAudio() { subMode |= 0x04; }
    void setData() { subMode |= 0x08; }
    void setTrigger() { subMode |= 0x10; }
    void setForm2() { subMode |= 0x20; }
    void setRealTime() { subMode |= 0x40; }
    void setEOF() { subMode |= 0x80; }

    void clearEndOfRecord() { subMode &= ~0x01; }
    void clearVideo() { subMode &= ~0x02; }
    void clearAudio() { subMode &= ~0x04; }
    void clearData() { subMode &= ~0x08; }
    void clearTrigger() { subMode &= ~0x10; }
    void clearForm2() { subMode &= ~0x20; }
    void clearRealTime() { subMode &= ~0x40; }
    void clearEOF() { subMode &= ~0x80; }
};

}  // namespace IEC60908b
}  // namespace PCSX

template <>
struct fmt::formatter<PCSX::IEC60908b::MSF> {
    template <typename ParseContext>
    constexpr auto parse(ParseContext &ctx) {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(PCSX::IEC60908b::MSF const &msf, FormatContext &ctx) const {
        return fmt::format_to(ctx.out(), "{0:02}:{1:02}:{2:02}", msf.m, msf.s, msf.f);
    }
};
