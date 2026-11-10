/*

MIT License

Copyright (c) 2026 PCSX-Redux authors

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

#include "supportpsx/str.h"

#include <string.h>

#include <algorithm>

#include "fmt/format.h"
#include "supportpsx/adpcm.h"
#include "supportpsx/iec-60908b.h"

namespace {

// The 32 bytes in front of each 2016 of a frame in its sectors.
void putChunkHeader(uint8_t *out, unsigned index, unsigned count, uint32_t frameNumber, uint32_t bytesUsed,
                    unsigned width, unsigned height, const uint8_t *bsHeader) {
    auto put16 = [&](unsigned at, uint32_t v) {
        out[at] = static_cast<uint8_t>(v);
        out[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    put16(0x00, 0x0160);
    put16(0x02, 0x8001);  // an MDEC frame's chunk
    put16(0x04, index);
    put16(0x06, count);
    put16(0x08, frameNumber);
    put16(0x0a, frameNumber >> 16);
    put16(0x0c, bytesUsed);
    put16(0x0e, bytesUsed >> 16);
    put16(0x10, width);
    put16(0x12, height);
    memcpy(out + 0x14, bsHeader, 8);  // a copy of the frame's BS header
    memset(out + 0x1c, 0, 4);
}

}  // namespace

void PCSX::STR::Planes::fromRGB(const uint8_t *rgb, unsigned w, unsigned h, size_t stride) {
    width = (w + 15) & ~15u;
    height = (h + 15) & ~15u;
    y.assign(static_cast<size_t>(width) * height, 0);
    cb.assign(static_cast<size_t>(width / 2) * (height / 2), 0);
    cr.assign(cb.size(), 0);
    auto at = [&](unsigned x, unsigned yy, unsigned c) {
        const unsigned sx = std::min(x, w - 1), sy = std::min(yy, h - 1);
        return static_cast<int>(rgb[sy * stride + sx * 3 + c]);
    };
    for (unsigned j = 0; j < height; j++) {
        for (unsigned i = 0; i < width; i++) {
            const int r = at(i, j, 0), g = at(i, j, 1), b = at(i, j, 2);
            y[static_cast<size_t>(j) * width + i] =
                static_cast<uint8_t>(std::clamp((299 * r + 587 * g + 114 * b) / 1000, 0, 255));
        }
    }
    for (unsigned j = 0; j < height / 2; j++) {
        for (unsigned i = 0; i < width / 2; i++) {
            int rs = 0, gs = 0, bs = 0;
            for (unsigned dy = 0; dy < 2; dy++) {
                for (unsigned dx = 0; dx < 2; dx++) {
                    rs += at(i * 2 + dx, j * 2 + dy, 0);
                    gs += at(i * 2 + dx, j * 2 + dy, 1);
                    bs += at(i * 2 + dx, j * 2 + dy, 2);
                }
            }
            const int r = rs / 4, g = gs / 4, b = bs / 4;
            const int luma = (299 * r + 587 * g + 114 * b) / 1000;
            const size_t o = static_cast<size_t>(j) * (width / 2) + i;
            cb[o] = static_cast<uint8_t>(std::clamp(128 + (b - luma) * 564 / 1000, 0, 255));
            cr[o] = static_cast<uint8_t>(std::clamp(128 + (r - luma) * 713 / 1000, 0, 255));
        }
    }
}

PCSX::DCT::Frame PCSX::STR::Planes::frame() const {
    DCT::Frame f;
    f.y = y.data();
    f.cb = cb.data();
    f.cr = cr.data();
    f.width = width;
    f.height = height;
    f.yStride = width;
    f.cStride = width / 2;
    return f;
}

std::string PCSX::STR::check(const Settings &settings) {
    const unsigned fps = settings.fps, speed = settings.speed, channels = settings.channels, rate = settings.rate;
    if (fps < 1 || fps > 60) return "the frame rate is 1..60";
    if (speed != 1 && speed != 2) return "the speed is 1 or 2";
    if (channels > 2) return "XA audio is mono or stereo";
    if (channels && rate != 37800 && rate != 18900) return "XA audio is at 37800 or 18900 Hz";
    return {};
}

PCSX::STR::Result PCSX::STR::encode(const Settings &settings, const FrameSource &frames, std::span<const int16_t> pcm,
                                    std::vector<uint8_t> &out) {
    Result result;
    const size_t first = out.size();
    auto fail = [&](std::string error) {
        out.resize(first);
        result.failed = true;
        result.error = std::move(error);
        return result;
    };
    if (auto error = check(settings); !error.empty()) return fail(std::move(error));
    const unsigned fps = settings.fps, speed = settings.speed, channels = settings.channels, rate = settings.rate;

    // An audio sector every `interleave` sectors carries the 18 sound groups that
    // play while the sectors between are read; the video gets the rest.
    const unsigned perSecond = 75 * speed;
    const unsigned interleave = channels ? perSecond * 18 * (channels == 2 ? 112 : 224) / rate : 0;
    // A frame's share of sectors is num / den, the remainder carried to the next frame.
    const unsigned num = interleave ? perSecond * (interleave - 1) : perSecond;
    const unsigned den = (interleave ? interleave : 1) * fps;
    if (num < den) return fail(fmt::format("{} fps at speed {} leaves a frame less than one sector", fps, speed));

    DCT::Encoder encoder(0, settings.transform);
    ADPCM::Encoder xa;
    xa.reset(ADPCM::Encoder::Mode::XA);

    Planes planes;
    std::vector<int16_t> coefficients;
    std::vector<uint16_t> stream;
    std::vector<uint8_t> bs;  // the frame being cut into sectors
    unsigned carry = 0, chunk = 0, chunks = 0, qsum = 0;
    uint32_t bytesUsed = 0;
    size_t pcmFrame = 0;          // sample frames of audio already in sectors
    size_t lastAudio = SIZE_MAX;  // where the last audio sector starts, for its EOF bit
    std::string error;

    // The next frame, coded to fit its share of sectors.
    auto nextFrame = [&]() -> int {
        const int got = frames(planes, error);
        if (got <= 0) return got;
        const unsigned number = result.frames + 1;
        if (result.frames == 0) {
            result.width = planes.width;
            result.height = planes.height;
        } else if (planes.width != result.width || planes.height != result.height) {
            error = fmt::format("frame {} is {}x{} where the frames before are {}x{}", number, planes.width,
                                planes.height, result.width, result.height);
            return -1;
        }
        coefficients.resize(DCT::requiredCoefficientCount(planes.width, planes.height));
        const auto shape = encoder.submit(planes.frame(), coefficients).get();
        if (shape.failed) {
            error = fmt::format("frame {}: the DCT stage refused it", number);
            return -1;
        }
        carry += num;
        chunks = carry / den;
        carry %= den;
        int q = 1;
        for (; q <= 63; q++) {
            stream.clear();
            if (DCT::pack(coefficients, shape, settings.quant, q, stream).failed) {
                error = fmt::format("frame {}: the packing stage refused it", number);
                return -1;
            }
            const auto wrap = DCT::toContainer(stream, DCT::Container::Bs, bs);
            if (wrap.failed) {
                error = fmt::format("frame {}: {}", number, wrap.error);
                return -1;
            }
            // The BS header's first word counts the run-level's 32-bit words, which a player decodes into.
            const unsigned runLevel = 2u * (bs[0] | (bs[1] << 8));
            if (bs.size() <= chunks * 2016u && (!settings.rlHalfwords || runLevel <= settings.rlHalfwords)) break;
        }
        if (q > 63) {
            error = fmt::format(
                "frame {} does not fit its {} sectors and {} run-level halfwords even at q_scale 63: lower the size "
                "or the frame rate{}",
                number, chunks, settings.rlHalfwords, speed == 1 ? ", or stream it at double speed" : "");
            return -1;
        }
        result.mostRunLevel = std::max(result.mostRunLevel, 2u * (bs[0] | (bs[1] << 8)));
        qsum += q;
        result.worstQScale = std::max(result.worstQScale, q);
        result.mostSectorsAFrame = std::max(result.mostSectorsAFrame, chunks);
        bytesUsed = static_cast<uint32_t>((bs.size() + 3) & ~size_t(3));
        bs.resize(chunks * 2016u, 0);
        chunk = 0;
        result.frames = number;
        return 1;
    };

    static const uint8_t sync[12] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    for (unsigned lba = 0;; lba++) {
        if (chunk >= chunks) {
            const int got = nextFrame();
            if (got < 0) return fail(error);
            if (got == 0) break;
        }
        const size_t at = out.size();
        out.resize(at + 2352, 0);
        uint8_t *sector = out.data() + at;
        memcpy(sector, sync, 12);
        IEC60908b::MSF(lba + 150).toBCD(sector + 12);
        sector[15] = 2;
        IEC60908b::SubHeaders sub;
        sub.fileNumber = settings.fileNumber;
        sub.channelNumber = settings.channelNumber;
        if (interleave && lba % interleave == 0) {
            // Audio, Form 2, real time. Coding: stereo in bit 0, 18900 Hz in bit 2, 4 bits a sample.
            sub.subMode = 0x64;
            sub.codingInfo = static_cast<uint8_t>((channels == 2 ? 1 : 0) | (rate == 18900 ? 4 : 0));
            sub.toBuffer(sector + 16);
            int16_t group[224];
            for (unsigned g = 0; g < 18; g++) {
                for (unsigned s = 0; s < 224; s++) {
                    const size_t i = pcmFrame * channels + s;
                    group[s] = i < pcm.size() ? pcm[i] : 0;
                }
                pcmFrame += channels == 2 ? 112 : 224;
                xa.processXABlock(group, sector + 24 + g * 128, ADPCM::Encoder::XAMode::FourBits, channels);
            }
            lastAudio = at;
            result.audioSectors++;
        } else {
            // Data, real time.
            sub.subMode = 0x48;
            sub.codingInfo = 0;
            sub.toBuffer(sector + 16);
            putChunkHeader(sector + 24, chunk, chunks, result.frames, bytesUsed, result.width, result.height,
                           bs.data());
            memcpy(sector + 24 + 32, bs.data() + chunk * 2016u, 2016);
            chunk++;
        }
    }
    if (result.frames == 0) return fail("no frames");
    if (lastAudio != SIZE_MAX) {
        out[lastAudio + 18] |= 0x80;  // EOF, in both copies of the last audio sector's submode
        out[lastAudio + 22] |= 0x80;
    }
    for (size_t at = first; at < out.size(); at += 2352) IEC60908b::computeEDCECC(out.data() + at);
    result.sectors = static_cast<unsigned>((out.size() - first) / 2352);
    result.averageQScale = static_cast<double>(qsum) / result.frames;
    return result;
}

PCSX::STR::Result PCSX::STR::fromSectors(std::span<const uint8_t> in, std::vector<uint8_t> &out) {
    Result result;
    auto fail = [&](std::string error) {
        result.failed = true;
        result.error = std::move(error);
        return result;
    };
    static const uint8_t sync[12] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    static const uint8_t videoChunk[4] = {0x60, 0x01, 0x01, 0x80};  // 0x0160, 0x8001: an MDEC frame's chunk
    // The layout: `stride` bytes a sector, the subheader `skip` bytes in. Raw sectors open with the sync;
    // otherwise it is the layout whose first sectors carry STR chunks where it puts them, so a blank or
    // audio sector at the start does not decide it.
    size_t stride = 0, skip = 0;
    auto chunksAt = [&](size_t size, size_t at) {
        unsigned found = 0;
        for (size_t n = 0; n < std::min<size_t>(in.size() / size, 64); n++) {
            found += memcmp(in.data() + n * size + at, videoChunk, sizeof(videoChunk)) == 0;
        }
        return found;
    };
    if (in.size() >= 2352 && memcmp(in.data(), sync, sizeof(sync)) == 0) {
        stride = 2352;
        skip = 16;
    } else {
        const unsigned mode2 = chunksAt(2336, 8), video = chunksAt(2048, 0);
        if (!mode2 && !video) {
            return fail(
                "these are not STR sectors: no sync at the start, and no STR chunk where 2336- or "
                "2048-byte sectors would have one");
        }
        stride = mode2 >= video ? 2336 : 2048;
    }
    if (in.size() % stride != 0) {
        return fail(fmt::format("{} bytes is not a whole number of {}-byte sectors", in.size(), stride));
    }
    const size_t count = in.size() / stride;
    std::vector<uint8_t> sectors(count * 2352, 0);
    for (size_t n = 0; n < count; n++) {
        const uint8_t *from = in.data() + n * stride;
        uint8_t *sector = sectors.data() + n * 2352;
        if (stride == 2352 && from[15] != 2) return fail(fmt::format("sector {} is not Mode 2", n));
        memcpy(sector, sync, sizeof(sync));
        IEC60908b::MSF(static_cast<uint32_t>(n) + 150).toBCD(sector + 12);
        sector[15] = 2;
        if (stride == 2048) {
            IEC60908b::SubHeaders sub;
            sub.fileNumber = 0;
            sub.channelNumber = 0;
            sub.subMode = 0x48;
            sub.codingInfo = 0;
            sub.toBuffer(sector + 16);
            memcpy(sector + 24, from, 2048);
        } else {
            memcpy(sector + 16, from + skip, 2336);
        }
        const uint8_t *data = sector + 24;
        if (sector[18] & 0x04) {
            result.audioSectors++;
        } else if (memcmp(data, videoChunk, sizeof(videoChunk)) == 0) {
            if (result.width == 0) {
                result.width = data[16] | (data[17] << 8);
                result.height = data[18] | (data[19] << 8);
            }
            if ((data[4] | (data[5] << 8)) == 0) {
                result.frames++;
                result.mostRunLevel = std::max(result.mostRunLevel, 2u * (data[32] | (data[33] << 8)));
            }
            result.mostSectorsAFrame =
                std::max(result.mostSectorsAFrame, static_cast<unsigned>(data[6] | (data[7] << 8)));
        }
        IEC60908b::computeEDCECC(sector);
    }
    if (result.width == 0) return fail("there is no STR video in it");
    result.sectors = static_cast<unsigned>(count);
    out.insert(out.end(), sectors.begin(), sectors.end());
    return result;
}
