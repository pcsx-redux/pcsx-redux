/***************************************************************************
 *   Copyright (C) 2025 PCSX-Redux authors                                 *
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

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>

#include "args/args.hxx"
#include "fmt/format.h"
#include "json.hpp"
#include "mips-common/util/bitfield.hh"
#include "support/container-file.h"
#include "support/djbhash.h"
#include "support/file.h"
#include "support/mem4g.h"
#include "support/polyfills.h"
#include "support/tool-args.h"
#include "supportpsx/binloader.h"
#include "supportpsx/iec-60908b.h"
#include "supportpsx/iso9660-builder.h"
#include "supportpsx/iso9660-lowlevel.h"
#include "supportpsx/ps1-packer.h"
#include "supportpsx/str.h"
#include "supportpsx/ucl-utils.h"
#include "ucl/ucl.h"

// Movies are read through ffmpeg. Built with AUTHORING_NO_FFMPEG defined, as the
// support mirror's Makefile does, the tool does everything else and refuses them.
#ifndef AUTHORING_NO_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}
#endif

template <PCSX::PolyFill::IntegralConcept T, std::endian endianess = std::endian::little>
void writeToBuffer(void* buffer_, T v) {
    if constexpr (endianess != std::endian::native) {
        v = PCSX::PolyFill::byteSwap(v);
    }
    uint8_t* buffer = reinterpret_cast<uint8_t*>(buffer_);
    for (unsigned i = 0; i < sizeof(T); i++) {
        buffer[i] = v & 0xff;
        v >>= 8;
    }
}

static constexpr unsigned c_maximumSectorCount = (99 * 60 + 59) * 75 + 74 - 150;

union IndexEntry {
    enum class Method : uint32_t {
        NONE = 0,
        UCL_NRV2E = 1,
        LZ4 = 2,
        // Raw sectors, such as a movie, streamed rather than loaded. The
        // entry's first 32-bit field holds the sector count.
        STREAM = 3,
        COUNT = 4,
    };
    typedef Utilities::BitSpan<uint32_t, 21> DecompSizeField;
    typedef Utilities::BitSpan<uint32_t, 11> PaddingField;
    typedef Utilities::BitSpan<uint32_t, 19> SectorOffsetField;
    typedef Utilities::BitSpan<uint32_t, 10> CompressedSizeField;
    typedef Utilities::BitSpan<Method, 3> MethodField;
    typedef Utilities::BitField<DecompSizeField, PaddingField, SectorOffsetField, CompressedSizeField, MethodField>
        CompressedEntry;
    uint32_t getDecompSize() const { return entry.get<DecompSizeField>(); }
    uint32_t getPadding() const { return entry.get<PaddingField>(); }
    uint32_t getSectorOffset() const { return entry.get<SectorOffsetField>(); }
    uint32_t getCompressedSize() const { return entry.get<CompressedSizeField>(); }
    Method getCompressionMethod() const { return entry.get<MethodField>(); }
    void setDecompSize(uint32_t v) { entry.set<DecompSizeField>(v); }
    void setPadding(uint32_t v) { entry.set<PaddingField>(v); }
    void setSectorOffset(uint32_t v) { entry.set<SectorOffsetField>(v); }
    void setCompressedSize(uint32_t v) { entry.set<CompressedSizeField>(v); }
    void setMethod(Method v) { entry.set<MethodField>(v); }
    uint32_t asArray[4];
    struct {
        uint64_t hash;
        CompressedEntry entry;
    };
};

static_assert(sizeof(IndexEntry) == 16);

#ifndef AUTHORING_NO_FFMPEG
static std::string averror(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return text;
}

// The video of a file, through ffmpeg: frames picked at the movie's rate, scaled
// to its size, and handed over as 24-bit RGB.
class MovieVideo {
  public:
    ~MovieVideo() {
        av_frame_free(&m_current);
        av_frame_free(&m_pending);
        av_packet_free(&m_packet);
        avcodec_free_context(&m_codec);
        avformat_close_input(&m_format);
    }

    bool open(const std::string& path, unsigned fps, unsigned width, unsigned height, std::string& error) {
        m_fps = fps;
        m_width = width;
        m_height = height;
        int ret = avformat_open_input(&m_format, path.c_str(), nullptr, nullptr);
        if (ret < 0 || (ret = avformat_find_stream_info(m_format, nullptr)) < 0) {
            error = "ffmpeg cannot read it: " + averror(ret);
            return false;
        }
        const AVCodec* codec = nullptr;
        m_stream = av_find_best_stream(m_format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (m_stream < 0) {
            error = "it has no video";
            return false;
        }
        const AVStream* stream = m_format->streams[m_stream];
        if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            error = "its only picture is a cover image, not a video";
            return false;
        }
        m_codec = avcodec_alloc_context3(codec);
        if (m_codec) m_codec->thread_count = 0;  // as many decoding threads as ffmpeg sees fit
        if (!m_codec || avcodec_parameters_to_context(m_codec, stream->codecpar) < 0 ||
            avcodec_open2(m_codec, codec, nullptr) < 0) {
            error = "ffmpeg cannot decode its video";
            return false;
        }
        m_timeBase = stream->time_base;
        AVRational rate = stream->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = stream->r_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = AVRational{static_cast<int>(fps), 1};
        m_frameDuration = std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), m_timeBase));
        m_packet = av_packet_alloc();
        m_current = av_frame_alloc();
        m_pending = av_frame_alloc();
        if (!m_packet || !m_current || !m_pending) {
            error = "out of memory";
            return false;
        }
        return true;
    }

    // An STR::FrameSource: the picture on screen at the next frame's time.
    int next(PCSX::STR::Planes& planes, std::string& error) {
        const AVRational frameTime = {1, static_cast<int>(m_fps)};
        while (!m_ended) {
            if (!m_hasPending) {
                const int got = decode(m_pending);
                if (got < 0) {
                    error = "ffmpeg failed decoding its video";
                    return -1;
                }
                if (got == 0) {
                    m_ended = true;
                    break;
                }
                const int64_t ts = m_pending->best_effort_timestamp;
                if (ts == AV_NOPTS_VALUE) {
                    m_pendingTime = m_hasCurrent ? m_currentTime + m_frameDuration : 0;
                } else {
                    if (m_start == AV_NOPTS_VALUE) m_start = ts;
                    m_pendingTime = ts - m_start;
                }
                m_hasPending = true;
            }
            if (m_hasCurrent && av_compare_ts(m_pendingTime, m_timeBase, m_index, frameTime) > 0) break;
            std::swap(m_current, m_pending);
            m_currentTime = m_pendingTime;
            m_hasCurrent = true;
            m_hasPending = false;
        }
        if (!m_hasCurrent) return 0;
        if (m_ended && av_compare_ts(m_currentTime + m_frameDuration, m_timeBase, m_index, frameTime) <= 0) return 0;
        if (!toRGB(m_current, error)) return -1;
        planes.fromRGB(m_scaled.data(), m_width, m_height, m_width * 3);
        m_index++;
        return 1;
    }

    // Packets ffmpeg found damaged and left out, as its own tools do.
    unsigned damaged() const { return m_damaged; }
    // An STR movie read back is decoded and encoded again.
    bool isStr() const { return m_format && m_format->iformat && strcmp(m_format->iformat->name, "psxstr") == 0; }

  private:
    int decode(AVFrame* frame) {
        while (true) {
            int ret = avcodec_receive_frame(m_codec, frame);
            if (ret == 0) return 1;
            if (ret == AVERROR_EOF) return 0;
            if (ret == AVERROR_INVALIDDATA) {
                m_damaged++;
                continue;
            }
            if (ret != AVERROR(EAGAIN)) return -1;
            if (av_read_frame(m_format, m_packet) < 0) {
                if (avcodec_send_packet(m_codec, nullptr) < 0) return 0;
                continue;
            }
            ret = m_packet->stream_index == m_stream ? avcodec_send_packet(m_codec, m_packet) : 0;
            av_packet_unref(m_packet);
            if (ret == AVERROR_INVALIDDATA) {
                m_damaged++;
                continue;
            }
            if (ret < 0) return -1;
        }
    }

    // Any RGB, YUV or gray layout ffmpeg describes with 4 to 16 bits a component,
    // in either byte order, or 8-bit palette indices, converted and then
    // area-averaged to the movie's size; the picture is stretched to fit it.
    bool toRGB(const AVFrame* frame, std::string& error) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        constexpr uint64_t unsupported = AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT;
        const bool palette = desc && (desc->flags & AV_PIX_FMT_FLAG_PAL);
        const bool gray = desc && !palette && desc->nb_components <= 2;
        bool usable = desc && !(desc->flags & unsupported) && desc->nb_components >= 1;
        for (unsigned c = 0; usable && !palette && c < (gray ? 1u : 3u); c++) {
            usable = desc->comp[c].depth >= 4 && desc->comp[c].depth <= 16;
        }
        if (!usable) {
            error = fmt::format("its pixel format {} is not one this tool converts", desc ? desc->name : "?");
            return false;
        }
        const bool rgb = desc->flags & AV_PIX_FMT_FLAG_RGB;
        const bool bigEndian = desc->flags & AV_PIX_FMT_FLAG_BE;
        auto sample = [&](unsigned c, unsigned x, unsigned y) -> int {
            const AVComponentDescriptor& comp = desc->comp[c];
            if (!rgb && c > 0) {
                x >>= desc->log2_chroma_w;
                y >>= desc->log2_chroma_h;
            }
            const uint8_t* p = frame->data[comp.plane] + static_cast<ptrdiff_t>(y) * frame->linesize[comp.plane] +
                               static_cast<ptrdiff_t>(x) * comp.step + comp.offset;
            const int bytes = std::min(4, (comp.shift + comp.depth + 7) / 8);
            uint32_t raw = 0;
            for (int i = 0; i < bytes; i++) raw |= uint32_t(p[bigEndian ? bytes - 1 - i : i]) << (8 * i);
            const uint32_t max = (1u << comp.depth) - 1;
            return static_cast<int>((((raw >> comp.shift) & max) * 255 + max / 2) / max);
        };
        double kr = 0.299, kb = 0.114;
        if (frame->colorspace == AVCOL_SPC_BT709) {
            kr = 0.2126;
            kb = 0.0722;
        } else if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL) {
            kr = 0.2627;
            kb = 0.0593;
        }
        const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG || strncmp(desc->name, "yuvj", 4) == 0;
        const unsigned w = frame->width, h = frame->height;
        m_rgb.resize(static_cast<size_t>(w) * h * 3);
        for (unsigned y = 0; y < h; y++) {
            for (unsigned x = 0; x < w; x++) {
                uint8_t* out = m_rgb.data() + (static_cast<size_t>(y) * w + x) * 3;
                if (palette) {
                    // The palette, in the second plane, is 32-bit ARGB in native order.
                    const uint8_t index = frame->data[0][static_cast<ptrdiff_t>(y) * frame->linesize[0] + x];
                    uint32_t entry;
                    memcpy(&entry, frame->data[1] + 4 * index, 4);
                    for (unsigned c = 0; c < 3; c++) out[c] = static_cast<uint8_t>(entry >> (16 - 8 * c));
                    continue;
                }
                if (gray) {
                    // Gray pictures are full range unless they say otherwise.
                    double luma = sample(0, x, y);
                    if (frame->color_range == AVCOL_RANGE_MPEG) luma = (luma - 16.0) * 255.0 / 219.0;
                    out[0] = out[1] = out[2] = static_cast<uint8_t>(std::clamp(std::lround(luma), 0L, 255L));
                    continue;
                }
                if (rgb) {
                    for (unsigned c = 0; c < 3; c++) out[c] = static_cast<uint8_t>(sample(c, x, y));
                    continue;
                }
                double luma = sample(0, x, y), cb = sample(1, x, y) - 128.0, cr = sample(2, x, y) - 128.0;
                if (!fullRange) {
                    luma = (luma - 16.0) * 255.0 / 219.0;
                    cb *= 255.0 / 224.0;
                    cr *= 255.0 / 224.0;
                }
                const double r = luma + 2.0 * (1.0 - kr) * cr;
                const double b = luma + 2.0 * (1.0 - kb) * cb;
                const double g = (luma - kr * r - kb * b) / (1.0 - kr - kb);
                out[0] = static_cast<uint8_t>(std::clamp(std::lround(r), 0L, 255L));
                out[1] = static_cast<uint8_t>(std::clamp(std::lround(g), 0L, 255L));
                out[2] = static_cast<uint8_t>(std::clamp(std::lround(b), 0L, 255L));
            }
        }
        m_scaled.resize(static_cast<size_t>(m_width) * m_height * 3);
        for (unsigned j = 0; j < m_height; j++) {
            const unsigned y0 = j * h / m_height, y1 = std::max(y0 + 1, (j + 1) * h / m_height);
            for (unsigned i = 0; i < m_width; i++) {
                const unsigned x0 = i * w / m_width, x1 = std::max(x0 + 1, (i + 1) * w / m_width);
                unsigned sum[3] = {0, 0, 0};
                for (unsigned y = y0; y < y1; y++) {
                    const uint8_t* in = m_rgb.data() + (static_cast<size_t>(y) * w + x0) * 3;
                    for (unsigned x = x0; x < x1; x++, in += 3) {
                        for (unsigned c = 0; c < 3; c++) sum[c] += in[c];
                    }
                }
                const unsigned count = (y1 - y0) * (x1 - x0);
                uint8_t* out = m_scaled.data() + (static_cast<size_t>(j) * m_width + i) * 3;
                for (unsigned c = 0; c < 3; c++) out[c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
            }
        }
        return true;
    }

    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_current = nullptr;
    AVFrame* m_pending = nullptr;
    AVRational m_timeBase = {1, 1};
    int64_t m_start = AV_NOPTS_VALUE;
    int64_t m_currentTime = 0, m_pendingTime = 0, m_frameDuration = 1;
    int m_stream = -1;
    unsigned m_damaged = 0;
    bool m_hasCurrent = false, m_hasPending = false, m_ended = false;
    unsigned m_fps = 0, m_width = 0, m_height = 0;
    int64_t m_index = 0;
    std::vector<uint8_t> m_rgb, m_scaled;
};

// The sound of a file as 16-bit PCM at `rate`, interleaved for `channels`. A file
// without sound leaves `pcm` empty and is not an error.
static bool readMovieAudio(const std::string& path, unsigned channels, unsigned rate, std::vector<int16_t>& pcm,
                           unsigned& damaged, std::string& error) {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto done = [&](bool ok) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&resampler);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return ok;
    };
    int opened = packet && frame ? avformat_open_input(&format, path.c_str(), nullptr, nullptr) : AVERROR(ENOMEM);
    if (opened < 0 || (opened = avformat_find_stream_info(format, nullptr)) < 0) {
        error = "ffmpeg cannot read it: " + averror(opened);
        return done(false);
    }
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream < 0) return done(true);
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
        error = "ffmpeg cannot decode its sound";
        return done(false);
    }
    // pcm_* decoders report an unspecified channel order, which the resampler
    // normalizes, so every frame needs it normalized the same way.
    auto normalize = [](AVChannelLayout& layout) {
        if (layout.order == AV_CHANNEL_ORDER_UNSPEC && layout.nb_channels > 0) {
            av_channel_layout_default(&layout, layout.nb_channels);
        }
    };
    normalize(codec->ch_layout);
    AVChannelLayout layout;
    av_channel_layout_default(&layout, static_cast<int>(channels));
    if (swr_alloc_set_opts2(&resampler, &layout, AV_SAMPLE_FMT_S16, static_cast<int>(rate), &codec->ch_layout,
                            codec->sample_fmt, codec->sample_rate, 0, nullptr) < 0 ||
        swr_init(resampler) < 0) {
        error = "ffmpeg cannot resample its sound";
        return done(false);
    }
    // swr_convert takes `const uint8_t**` before ffmpeg 7 and `const uint8_t* const*` since, and the
    // first converts to the second, so this builds against both.
    auto convert = [&](const uint8_t** in, int inCount) {
        const int room = swr_get_out_samples(resampler, inCount);
        if (room <= 0) return 0;
        const size_t at = pcm.size();
        pcm.resize(at + static_cast<size_t>(room) * channels);
        uint8_t* out = reinterpret_cast<uint8_t*>(pcm.data() + at);
        const int got = swr_convert(resampler, &out, room, in, inCount);
        pcm.resize(at + static_cast<size_t>(std::max(got, 0)) * channels);
        return got;
    };
    bool draining = false;
    while (true) {
        int ret = avcodec_receive_frame(codec, frame);
        if (ret == 0) {
            normalize(frame->ch_layout);
            if (convert(const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples) < 0) {
                error = "ffmpeg failed resampling its sound";
                return done(false);
            }
            continue;
        }
        if (ret == AVERROR_EOF) break;
        if (ret == AVERROR_INVALIDDATA) {
            damaged++;
            continue;
        }
        if (ret != AVERROR(EAGAIN) || draining) {
            error = "ffmpeg failed decoding its sound";
            return done(false);
        }
        if (av_read_frame(format, packet) < 0) {
            draining = true;
            avcodec_send_packet(codec, nullptr);
            continue;
        }
        ret = packet->stream_index == stream ? avcodec_send_packet(codec, packet) : 0;
        av_packet_unref(packet);
        if (ret == AVERROR_INVALIDDATA) {
            damaged++;
            continue;
        }
        if (ret < 0) {
            error = "ffmpeg failed decoding its sound";
            return done(false);
        }
    }
    while (convert(nullptr, 0) > 0) {
    }
    return done(true);
}
#endif

// What psyqo-paths' StrPlayer, as it comes, does not play of a movie: frames of more
// sectors than its maxSectorsPerFrame, or larger than its display.
static std::string playerWarning(const PCSX::STR::Result& result) {
    std::string warning;
    auto add = [&warning](std::string text) {
        if (!warning.empty()) warning += "; ";
        warning += text;
    };
    if (result.mostSectorsAFrame > 32) {
        add(fmt::format(
            "frames take up to {} sectors, more than StrPlayer holds at all (32); a higher frame rate, or speed 1, "
            "gives each frame fewer",
            result.mostSectorsAFrame));
    } else if (result.mostSectorsAFrame > 12) {
        add(fmt::format(
            "frames take up to {} sectors, and StrPlayer drops those over its maxSectorsPerFrame, 12 unless raised "
            "(to 32 at most); a higher frame rate, or speed 1, gives each frame fewer",
            result.mostSectorsAFrame));
    }
    if (result.mostRunLevel > 28672) {
        add(fmt::format(
            "a frame decodes to {} run-level halfwords, and StrPlayer drops those over its rlHalfwords, 28672 "
            "unless raised",
            result.mostRunLevel));
    }
    if (result.width % 16 || result.height % 16) {
        add(fmt::format("its frames are {}x{}, and StrPlayer plays only whole 16x16 macroblocks", result.width,
                        result.height));
    }
    if (result.width > 320 || result.height > 240) {
        add(
            fmt::format("its {}x{} frames are larger than the 320x240 display StrPlayer plays into unless "
                        "given a larger one",
                        result.width, result.height));
    }
    return warning;
}

// A movie entry: the file encoded to STR sectors by the options of its "movie"
// key. `summary` says what came out, or what went wrong, and `warning` what a
// player may not play.
static bool encodeMovie(const std::string& path, const nlohmann::json& options, std::vector<uint8_t>& sectors,
                        std::string& summary, std::string& warning) {
    if (!options.is_object() && !options.is_boolean()) {
        summary = "\"movie\" is true or an object of options";
        return false;
    }
    if (options.is_object()) {
        static const char* const known[] = {"fps",    "speed", "channels", "rate",       "width",
                                            "height", "file",  "channel",  "rlHalfwords"};
        for (const auto& [key, value] : options.items()) {
            if (std::find(std::begin(known), std::end(known), key) == std::end(known)) {
                summary = fmt::format("\"{}\" is not a movie option", key);
                return false;
            }
        }
    }
    // A whole number, written as one or as a float (15.0), up to StrPlayer's largest rlHalfwords.
    const char* wrongKey = nullptr;
    auto get = [&](const char* key, unsigned fallback) -> unsigned {
        if (!options.is_object() || !options.contains(key)) return fallback;
        const auto& value = options[key];
        constexpr double largest = 1 << 22;
        if (value.is_number_unsigned() && value.get<uint64_t>() <= largest) return value.get<unsigned>();
        if (value.is_number_float()) {
            const double d = value.get<double>();
            if (d >= 0 && d <= largest && d == std::floor(d)) return static_cast<unsigned>(d);
        }
        if (!wrongKey) wrongKey = key;
        return fallback;
    };
    PCSX::STR::Settings settings;
    settings.fps = get("fps", 15);
    settings.speed = get("speed", 2);
    settings.channels = get("channels", 2);
    settings.rate = get("rate", 37800);
    const unsigned width = get("width", 320), height = get("height", 240);
    const unsigned fileNumber = get("file", 0), channelNumber = get("channel", 0);
    settings.rlHalfwords = get("rlHalfwords", settings.rlHalfwords);
    if (wrongKey) {
        summary =
            fmt::format("the movie's \"{}\" is {}, where a whole number is wanted", wrongKey, options[wrongKey].dump());
        return false;
    }
    if (width < 16 || width > 640 || height < 16 || height > 480 || fileNumber > 255 || channelNumber > 31) {
        summary = "a movie is 16..640 wide and 16..480 high, with a file 0..255 and a channel 0..31";
        return false;
    }
    settings.fileNumber = static_cast<uint8_t>(fileNumber);
    settings.channelNumber = static_cast<uint8_t>(channelNumber);
    if (auto error = PCSX::STR::check(settings); !error.empty()) {
        summary = std::move(error);
        return false;
    }
#ifdef AUTHORING_NO_FFMPEG
    summary =
        "this authoring was built without ffmpeg, so it cannot encode a movie (\"stream\" stores one already encoded)";
    return false;
#else
    std::vector<int16_t> pcm;
    unsigned damaged = 0;
    if (settings.channels && !readMovieAudio(path, settings.channels, settings.rate, pcm, damaged, summary)) {
        return false;
    }
    if (pcm.empty()) settings.channels = 0;
    MovieVideo video;
    if (!video.open(path, settings.fps, width, height, summary)) return false;
    const auto result = PCSX::STR::encode(
        settings, [&video](PCSX::STR::Planes& planes, std::string& error) { return video.next(planes, error); }, pcm,
        sectors);
    if (result.failed) {
        summary = result.error;
        return false;
    }
    damaged += video.damaged();
    summary = fmt::format(
        "{} frames of {}x{} at {} fps, {} sectors ({:.2f} s at speed {}), {} of them audio; up to {} sectors a "
        "frame; q_scale {:.1f} on average, {} at worst{}",
        result.frames, result.width, result.height, settings.fps, result.sectors,
        result.sectors / (75.0 * settings.speed), settings.speed, result.audioSectors, result.mostSectorsAFrame,
        result.averageQScale, result.worstQScale,
        damaged ? fmt::format("; {} damaged packet{} of the source left out", damaged, damaged == 1 ? "" : "s") : "");
    warning = playerWarning(result);
    if (video.isStr()) {
        if (!warning.empty()) warning += "; ";
        warning += "it is an STR movie already, encoded again here: \"stream\": true stores it as it is";
    }
    return true;
#endif
}

// A movie encoded elsewhere, stored as it is: its sectors, in any layout STR::fromSectors takes.
static bool storeStream(const std::filesystem::path& path, std::vector<uint8_t>& sectors, std::string& summary,
                        std::string& warning) {
    PCSX::IO<PCSX::File> file(new PCSX::PosixFile(path));
    if (file->failed()) {
        summary = "cannot open it";
        return false;
    }
    std::vector<uint8_t> data(file->size());
    if (file->read(data.data(), data.size()) != static_cast<ssize_t>(data.size())) {
        summary = "cannot read it";
        return false;
    }
    const auto result = PCSX::STR::fromSectors(data, sectors);
    if (result.failed) {
        summary = result.error;
        return false;
    }
    summary = fmt::format(
        "{} frames of {}x{}, {} sectors stored as they were, {} of them audio; up to {} sectors a frame", result.frames,
        result.width, result.height, result.sectors, result.audioSectors, result.mostSectorsAFrame);
    warning = playerWarning(result);
    return true;
}

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> basedirFlag(parser, "basedir", "", {"basedir"});
    args::ValueFlag<std::string> licenseFlag(parser, "license", "", {"license"});
    args::ValueFlag<unsigned> threadsFlag(parser, "threads", "", {"threads"});
    args::Flag quietFlag(parser, "quiet", "", {"q"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
#ifndef AUTHORING_NO_FFMPEG
    // ffmpeg's warnings (an mp4's dropped priming samples, say) are not this tool's to print.
    av_log_set_level(AV_LOG_ERROR);
#endif
    const auto output = PCSX::ToolArgs::get(outputFlag);
    const auto inputs = args::get(inputsList);
    const auto license = PCSX::ToolArgs::get(licenseFlag);
    const bool asksForHelp = args::get(helpFlag);
    const bool quiet = args::get(quietFlag);
    const bool hasOutput = output.has_value();
    const bool hasExactlyOneInput = inputs.size() == 1;

    if (asksForHelp || !hasExactlyOneInput || !hasOutput) {
        fmt::print(R"(
Usage: {} input.json [-h] -o output.bin
  input.json        mandatory: specify the input JSON file.
  -o output.bin     mandatory: name of the output file.
  -basedir path     optional: base directory for the input files.
  -license file     optional: use this license file.
  -threads count    optional: number of threads to use for compression.
  -q                optional: only print errors.
  -h                displays this help information and exit.
)",
                   argv[0]);
        return -1;
    }

    auto input = inputs[0];
    const std::filesystem::path basePath =
        PCSX::ToolArgs::get(basedirFlag).value_or(std::filesystem::path(input).parent_path().string());
    PCSX::IO<PCSX::File> indexFile(new PCSX::PosixFile(input));
    if (indexFile->failed()) {
        fmt::print("Unable to open file: {}\n", input);
        return -1;
    }
    PCSX::FileAsContainer container(indexFile);
    auto indexData = nlohmann::json::parse(container.begin(), container.end(), nullptr, false, true);
    if (indexData.is_discarded()) {
        fmt::print("Unable to parse JSON file: {}\n", input);
        return -1;
    }
    if (indexData.is_null()) {
        fmt::print("Unable to parse JSON file: {}\n", input);
        return -1;
    }

    if (!indexData.is_object()) {
        fmt::print("Invalid JSON file: {}\n", input);
        return -1;
    }

    if (!indexData.contains("executable") || !indexData["executable"].is_string()) {
        fmt::print("Invalid JSON file: {}\n", input);
        return -1;
    }

    if (!indexData.contains("files") || !indexData["files"].is_array()) {
        fmt::print("Invalid JSON file: {}\n", input);
        return -1;
    }

    PCSX::IO<PCSX::File> out(new PCSX::PosixFile(output.value(), PCSX::FileOps::TRUNCATE));
    if (out->failed()) {
        fmt::print("Error opening output file {}\n", output.value());
        return -1;
    }
    PCSX::ISO9660Builder builder(out);

    PCSX::IO<PCSX::File> licenseFile(new PCSX::FailedFile);
    if (license.has_value()) {
        licenseFile.setFile(new PCSX::PosixFile(license.value()));
        if (licenseFile->failed()) {
            fmt::print("Error opening license file {}\n", license.value());
            return -1;
        }
    }

    const unsigned threadCount = PCSX::ToolArgs::get(threadsFlag).value_or(std::thread::hardware_concurrency());

    nlohmann::json pvdData = nlohmann::json::object();
    if (indexData.contains("pvd") && indexData["pvd"].is_object()) {
        pvdData = indexData["pvd"];
    }

    auto executablePath = indexData["executable"].get<std::string>();
    PCSX::IO<PCSX::File> executableFile(new PCSX::PosixFile(basePath / executablePath));
    if (executableFile->failed()) {
        fmt::print("Unable to open file: {}\n", executablePath);
        return -1;
    }

    builder.writeLicense(licenseFile);

    PCSX::BinaryLoader::Info info;
    PCSX::IO<PCSX::Mem4G> memory(new PCSX::Mem4G());
    std::map<uint32_t, std::string> symbols;
    bool success = PCSX::BinaryLoader::load(executableFile, memory, info, symbols);
    if (!success) {
        fmt::print("Unable to load file: {}\n", executablePath);
        return -1;
    }
    if (!info.pc.has_value()) {
        fmt::print("File {} is invalid.\n", executablePath);
        return -1;
    }

    const unsigned filesCount = indexData["files"].size();
    const unsigned indexSectorsCount = ((filesCount + 1) * sizeof(IndexEntry) + 2047) / 2048;

    if (filesCount > c_maximumSectorCount) {
        fmt::print("Too many files specified ({}), max allowed is {}\n", filesCount, c_maximumSectorCount);
        return -1;
    }
    if (!quiet) {
        fmt::print("Index size: {}\n", indexSectorsCount * 2048);
    }

    PCSX::PS1Packer::Options options;
    options.booty = false;
    options.raw = false;
    options.rom = false;
    options.cpe = false;
    options.shell = false;
    options.nokernel = true;
    options.tload = false;
    options.nopad = false;
    PCSX::IO<PCSX::File> compressedExecutable(new PCSX::BufferFile(PCSX::FileOps::READWRITE));
    PCSX::PS1Packer::pack(new PCSX::SubFile(memory, memory->lowestAddress(), memory->actualSize()),
                          compressedExecutable, memory->lowestAddress(), info.pc.value_or(0), info.gp.value_or(0),
                          info.sp.value_or(0), options);

    if (compressedExecutable->size() % 2048 != 0) {
        fmt::print("Executable size is not a multiple of 2048\n");
        return -1;
    }
    if (!quiet) {
        fmt::print("Executable size: {}\n", compressedExecutable->size());
        fmt::print("Executable location: {}\n", 23 + indexSectorsCount);
    }

    const unsigned executableSectorsCount = compressedExecutable->size() / 2048;
    unsigned currentSector = 23 + indexSectorsCount;
    std::atomic<uint32_t> maxMargin;

    for (unsigned i = 0; i < executableSectorsCount; i++) {
        auto sector = compressedExecutable.asA<PCSX::BufferFile>()->borrow(i * 2048);
        builder.writeSectorAt(sector.data<uint8_t>(), PCSX::IEC60908b::MSF{150 + currentSector++},
                              PCSX::IEC60908b::SectorMode::M2_FORM1);
    }

    std::unique_ptr<uint8_t[]> indexEntryDataBuffer(new uint8_t[indexSectorsCount * 2048]);
    memset(indexEntryDataBuffer.get(), 0, indexSectorsCount * 2048);
    std::span<IndexEntry> indexEntryData = {reinterpret_cast<IndexEntry*>(indexEntryDataBuffer.get()) + 1, filesCount};

    struct WorkUnit {
        WorkUnit() : semaphore(0), failed(false) {}
        std::binary_semaphore semaphore;
        std::vector<uint8_t> sectorData;
        nlohmann::json fileInfo;
        std::string summary, warning;
        uint32_t margin = 0;
        bool failed;
    };
    static WorkUnit work[c_maximumSectorCount];
    for (unsigned i = 0; i < filesCount; i++) {
        auto& fileInfo = indexData["files"][i];
        if (!fileInfo.is_object()) {
            fmt::print("Invalid JSON file: {}\n", input);
            return -1;
        }
        if (!fileInfo.contains("path") || !fileInfo["path"].is_string()) {
            fmt::print("Invalid JSON file: {}\n", input);
            return -1;
        }
        work[i].fileInfo = fileInfo;
    }
    auto createSectorHeader = [](uint8_t sector[2352]) {
        memset(sector + 1, 0xff, 10);
        sector[15] = 2;
        sector[18] = sector[22] = 8;
    };

    std::atomic<unsigned> currentWorkUnit = 0;
    for (unsigned i = 0; i < threadCount; i++) {
        std::thread t([&]() {
            while (1) {
                std::atomic_thread_fence(std::memory_order_acq_rel);
                unsigned workUnitIndex = currentWorkUnit.fetch_add(1);
                if (workUnitIndex >= filesCount) return;
                auto& workUnit = work[workUnitIndex];
                auto filePath = workUnit.fileInfo["path"].get<std::string>();
                // A movie is a STREAM entry: never compressed, and with its sector count in the entry's
                // first 32-bit field, so neither the 2 MB nor the 1023-sector limit of a stored file applies.
                // "movie" encodes one; "stream": true takes one already encoded, psxavenc's say, as it is.
                const bool movie = workUnit.fileInfo.contains("movie") && workUnit.fileInfo["movie"] != false;
                const bool stream = workUnit.fileInfo.contains("stream") && workUnit.fileInfo["stream"] != false;
                if (movie || stream) {
                    IndexEntry* entry = &indexEntryData[workUnitIndex];
                    entry->hash = PCSX::djb::hash(workUnit.fileInfo["name"].is_string()
                                                      ? workUnit.fileInfo["name"].get<std::string>()
                                                      : filePath);
                    workUnit.margin = 0;
                    if (movie && stream) {
                        workUnit.summary = "a file is a movie to encode or a stream to store, not both";
                        workUnit.failed = true;
                    } else if (stream && workUnit.fileInfo["stream"] != true) {
                        workUnit.summary = "\"stream\" is true or false";
                        workUnit.failed = true;
                    } else if (movie) {
                        workUnit.failed = !encodeMovie((basePath / filePath).string(), workUnit.fileInfo["movie"],
                                                       workUnit.sectorData, workUnit.summary, workUnit.warning);
                    } else {
                        workUnit.failed =
                            !storeStream(basePath / filePath, workUnit.sectorData, workUnit.summary, workUnit.warning);
                    }
                    if (!workUnit.failed) {
                        entry->asArray[2] = static_cast<uint32_t>(workUnit.sectorData.size() / 2352);
                        entry->setMethod(IndexEntry::Method::STREAM);
                    }
                    workUnit.semaphore.release();
                    continue;
                }
                PCSX::IO<PCSX::File> file(new PCSX::PosixFile(basePath / filePath));
                if (file->failed()) {
                    workUnit.summary = "cannot open it";
                    workUnit.failed = true;
                    workUnit.semaphore.release();
                    continue;
                }
                unsigned size = file->size();
                if (size >= 2 * 1024 * 1024) {
                    workUnit.summary =
                        "it is 2 MB or more, more than an archive file holds (a video needs \"movie\", a movie "
                        "already encoded \"stream\")";
                    workUnit.failed = true;
                    workUnit.semaphore.release();
                    continue;
                }
                unsigned originalSectorsCount = (size + 2047) / 2048;
                std::vector<uint8_t> dataIn;
                dataIn.resize(originalSectorsCount * 2048);
                file->read(dataIn.data(), dataIn.size());

                // "compress": false keeps a file stored, so it can be read sector by sector.
                const bool store = workUnit.fileInfo.contains("compress") &&
                                   workUnit.fileInfo["compress"].is_boolean() &&
                                   !workUnit.fileInfo["compress"].get<bool>();

                std::vector<uint8_t> dataOut;
                ucl_uint outSize = 0;
                unsigned compressedSectorsCount = originalSectorsCount;
                if (!store) {
                    dataOut.resize(dataIn.size() * 1.2 + 2064 + 2048);
                    int r = ucl_nrv2e_99_compress(dataIn.data(), size, dataOut.data() + 2048, &outSize, nullptr, 10,
                                                  nullptr, nullptr);
                    if (r != UCL_E_OK) {
                        workUnit.failed = true;
                        workUnit.semaphore.release();
                        continue;
                    }
                    compressedSectorsCount = (outSize + 2047) / 2048;
                }
                const bool compressed = compressedSectorsCount < originalSectorsCount;
                // The index holds a size of at most 1023 sectors; a stored file of 1024 would read as 0.
                if (!compressed && originalSectorsCount > 1023) {
                    workUnit.summary = fmt::format("stored, it is {} sectors, more than the index counts (1023)",
                                                   originalSectorsCount);
                    workUnit.failed = true;
                    workUnit.semaphore.release();
                    continue;
                }

                // Only a compressed entry is decompressed in place, so only it needs a margin.
                if (compressed) {
                    ssize_t margin = PCSX::UCLUtils::inPlaceOverlapMargin(dataOut.data() + 2048, outSize, size);
                    ssize_t relMargin = margin - size + outSize;
                    workUnit.margin = relMargin > 0 ? static_cast<uint32_t>(relMargin) : 0;
                    do {
                        auto oldOverlap = maxMargin.load();
                        if (workUnit.margin <= oldOverlap) break;
                        if (maxMargin.compare_exchange_weak(oldOverlap, workUnit.margin)) break;
                    } while (1);
                }

                IndexEntry* entry = &indexEntryData[workUnitIndex];

                if (workUnit.fileInfo["name"].is_string()) {
                    entry->hash = PCSX::djb::hash(workUnit.fileInfo["name"].get<std::string>());
                } else {
                    entry->hash = PCSX::djb::hash(filePath);
                }
                entry->setDecompSize(size);
                std::span<uint8_t> source;
                unsigned sectorCount = 0;
                if (compressed) {
                    entry->setCompressedSize(compressedSectorsCount);
                    entry->setMethod(IndexEntry::Method::UCL_NRV2E);
                    unsigned padding = outSize % 2048;
                    if (padding > 0) {
                        padding = 2048 - padding;
                    }
                    entry->setPadding(padding);
                    sectorCount = compressedSectorsCount;
                    source = {reinterpret_cast<uint8_t*>(dataOut.data()) - padding + 2048, sectorCount * 2048};
                } else {
                    entry->setCompressedSize(originalSectorsCount);
                    entry->setMethod(IndexEntry::Method::NONE);
                    entry->setPadding(0);
                    sectorCount = originalSectorsCount;
                    source = {reinterpret_cast<uint8_t*>(dataIn.data()), sectorCount * 2048};
                }
                workUnit.sectorData.resize(sectorCount * 2352);
                for (unsigned sector = 0; sector < sectorCount; sector++) {
                    uint8_t* dest = workUnit.sectorData.data() + sector * 2352;
                    createSectorHeader(dest);
                    memcpy(dest + 24, source.data() + sector * 2048, 2048);
                    PCSX::IEC60908b::computeEDCECC(dest);
                }
                workUnit.semaphore.release();
            }
        });
        t.detach();
    }

    auto putSectorLBA = [](uint8_t sector[2352], uint32_t lba) {
        PCSX::IEC60908b::MSF time(lba + 150);
        time.toBCD(sector + 12);
    };

    for (unsigned workUnitIndex = 0; workUnitIndex < filesCount; workUnitIndex++) {
        auto& workUnit = work[workUnitIndex];
        workUnit.semaphore.acquire();
        std::atomic_thread_fence(std::memory_order_acq_rel);
        const auto path = workUnit.fileInfo["path"].get<std::string>();
        if (workUnit.failed) {
            fmt::print("Error processing file: {}{}\n", path, workUnit.summary.empty() ? "" : ": " + workUnit.summary);
            return -1;
        }
        IndexEntry* entry = &indexEntryData[workUnitIndex];
        const bool stream = entry->getCompressionMethod() == IndexEntry::Method::STREAM;
        if (!workUnit.warning.empty()) fmt::print("Warning: {}: {}\n", path, workUnit.warning);
        if (!quiet && stream) {
            fmt::print("Processed movie: {}\n", path);
            fmt::print("  {}\n", workUnit.summary);
            fmt::print("  Sector offset: {}\n", currentSector);
        } else if (!quiet) {
            fmt::print("Processed file: {}\n", workUnit.fileInfo["path"].get<std::string>());
            fmt::print("  Original size: {}\n", entry->getDecompSize());
            fmt::print("  Compressed size: {}\n", entry->getCompressedSize() * 2048);
            fmt::print("  Compression method: {}\n", static_cast<uint32_t>(entry->getCompressionMethod()));
            fmt::print("  Sector offset: {}\n", currentSector);
            fmt::print("  Margin: {}\n", workUnit.margin);
        }
        entry->setSectorOffset(currentSector);
        unsigned sectorCount = stream ? entry->asArray[2] : entry->getCompressedSize();
        // MSF addresses end at 99:59:74, before the index's 19-bit sector offsets do; 9000 empty sectors end the disc.
        if (currentSector + sectorCount + 9000 - 1 > c_maximumSectorCount) {
            fmt::print("Error processing file: {}: the disc would be {} sectors, more than its addresses reach ({})\n",
                       path, currentSector + sectorCount + 9000, c_maximumSectorCount + 1);
            return -1;
        }
        for (unsigned sector = 0; sector < sectorCount; sector++) {
            uint8_t* dest = workUnit.sectorData.data() + sector * 2352;
            putSectorLBA(dest, currentSector);
            builder.writeSectorAt(dest, PCSX::IEC60908b::MSF{150 + currentSector++}, PCSX::IEC60908b::SectorMode::RAW);
        }
        // Written, so let it go: a movie is not held to 2 MB, and a disc of them would otherwise stay in memory whole.
        std::vector<uint8_t>().swap(workUnit.sectorData);
    }

    if (!quiet) {
        fmt::print("Processed {} files.\n", filesCount);
        fmt::print("Maximum margin: {}\n", maxMargin.load());
    }

    uint8_t empty[2048] = {0};
    for (unsigned i = 0; i < 9000; i++) {
        builder.writeSectorAt(empty, PCSX::IEC60908b::MSF{150 + currentSector++},
                              PCSX::IEC60908b::SectorMode::M2_FORM1);
    }

    const unsigned totalSectorCount = currentSector;
    // The disc's first two seconds are the pregap before LBA 0.
    if (totalSectorCount + 150 > 80 * 60 * 75) {
        fmt::print("Warning: the disc is {:.1f} minutes long, more than an 80-minute CD-R holds.\n",
                   (totalSectorCount + 150) / (60.0 * 75));
    }

    indexEntryDataBuffer[0] = 'P';
    indexEntryDataBuffer[1] = 'S';
    indexEntryDataBuffer[2] = 'X';
    indexEntryDataBuffer[3] = '-';
    indexEntryDataBuffer[4] = 'A';
    indexEntryDataBuffer[5] = 'R';
    indexEntryDataBuffer[6] = 'C';
    indexEntryDataBuffer[7] = '1';
    // The reader allows (byte + 1) * 16 bytes, so 4095 is the largest margin the byte holds.
    if (maxMargin.load() >= 4096) {
        fmt::print("Decompression margin of {} bytes does not fit the archive header (4095 max).\n",
                   maxMargin.load());
        return -1;
    }
    uint32_t maxMarginValue = maxMargin.load() >> 4;
    writeToBuffer(indexEntryDataBuffer.get() + 8, filesCount);
    writeToBuffer<uint8_t>(indexEntryDataBuffer.get() + 11, maxMarginValue);
    writeToBuffer(indexEntryDataBuffer.get() + 12, totalSectorCount);
    std::sort(indexEntryData.begin(), indexEntryData.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.hash < b.hash; });

    for (unsigned i = 0; i < indexSectorsCount; i++) {
        auto sector = indexEntryDataBuffer.get() + i * 2048;
        builder.writeSectorAt(sector, PCSX::IEC60908b::MSF{150 + i + 23}, PCSX::IEC60908b::SectorMode::M2_FORM1);
    }

    PCSX::IO<PCSX::File> pvdSector(new PCSX::BufferFile(PCSX::FileOps::READWRITE));
    PCSX::ISO9660LowLevel::PVD pvd;
    pvd.reset();
    pvd.get<PCSX::ISO9660LowLevel::PVD_TypeCode>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_StdIdent>().set("CD001");
    pvd.get<PCSX::ISO9660LowLevel::PVD_Version>().value = 1;
    auto systemIdent = pvdData["system_id"].is_string() ? pvdData["system_id"].get<std::string>() : "PLAYSTATION";
    pvd.get<PCSX::ISO9660LowLevel::PVD_SystemIdent>().set(systemIdent, ' ');
    auto volumeIdent = pvdData["volume_id"].is_string() ? pvdData["volume_id"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeIdent>().set(volumeIdent, ' ');
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSpaceSize>().value = totalSectorCount;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSpaceSizeBE>().value = totalSectorCount;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSetSize>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSetSizeBE>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSequenceNumber>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSequenceNumberBE>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LogicalBlockSize>().value = 2048;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LogicalBlockSizeBE>().value = 2048;
    pvd.get<PCSX::ISO9660LowLevel::PVD_PathTableSize>().value = 10;
    pvd.get<PCSX::ISO9660LowLevel::PVD_PathTableSizeBE>().value = 10;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LPathTableLocation>().value = 18;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LPathTableOptLocation>().value = 19;
    pvd.get<PCSX::ISO9660LowLevel::PVD_MPathTableLocation>().value = 20;
    pvd.get<PCSX::ISO9660LowLevel::PVD_MPathTableOptLocation>().value = 21;
    auto& root = pvd.get<PCSX::ISO9660LowLevel::PVD_RootDir>();
    root.get<PCSX::ISO9660LowLevel::DirEntry_Length>().value = 34;
    root.get<PCSX::ISO9660LowLevel::DirEntry_ExtLength>().value = 0;
    root.get<PCSX::ISO9660LowLevel::DirEntry_LBA>().value = 22;
    root.get<PCSX::ISO9660LowLevel::DirEntry_LBABE>().value = 22;
    root.get<PCSX::ISO9660LowLevel::DirEntry_Size>().value = 2048;
    root.get<PCSX::ISO9660LowLevel::DirEntry_SizeBE>().value = 2048;
    root.get<PCSX::ISO9660LowLevel::DirEntry_Flags>().value = 2;
    root.get<PCSX::ISO9660LowLevel::DirEntry_VolSeqNo>().value = 1;
    root.get<PCSX::ISO9660LowLevel::DirEntry_VolSeqNoBE>().value = 1;
    root.get<PCSX::ISO9660LowLevel::DirEntry_Filename>().value.resize(1);
    auto volumeSetIdent = pvdData["volume_set_id"].is_string() ? pvdData["volume_set_id"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolSetIdent>().set(volumeSetIdent, ' ');
    auto publisherIdent = pvdData["publisher"].is_string() ? pvdData["publisher"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_PublisherIdent>().set(publisherIdent, ' ');
    auto dataPreparerIdent = pvdData["preparer"].is_string() ? pvdData["preparer"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_DataPreparerIdent>().set(dataPreparerIdent, ' ');
    auto applicationIdent = pvdData["application_id"].is_string() ? pvdData["application_id"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_ApplicationIdent>().set(applicationIdent, ' ');
    auto copyrightFileIdent = pvdData["copyright"].is_string() ? pvdData["copyright"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_CopyrightFileIdent>().set(copyrightFileIdent, ' ');
    auto abstractFileIdent = pvdData["abstract"].is_string() ? pvdData["abstract"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_AbstractFileIdent>().set(abstractFileIdent, ' ');
    auto bibliographicFileIdent =
        pvdData["bibliographic"].is_string() ? pvdData["bibliographic"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_BibliographicFileIdent>().set(bibliographicFileIdent, ' ');
    pvd.get<PCSX::ISO9660LowLevel::PVD_FileStructureVersion>().value = 1;

    pvd.serialize(pvdSector);
    while (pvdSector->size() < 2048) {
        pvdSector->write<uint8_t>(0);
    }
    builder.writeSectorAt(pvdSector.asA<PCSX::BufferFile>()->borrow(0).data<uint8_t>(), {0, 2, 16},
                          PCSX::IEC60908b::SectorMode::M2_FORM1);

    uint8_t sector[2048];
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xff;
    sector[1] = 'C';
    sector[2] = 'D';
    sector[3] = '0';
    sector[4] = '0';
    sector[5] = '1';
    builder.writeSectorAt(sector, {0, 2, 17}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    memset(sector, 0, sizeof(sector));
    sector[0] = 1;
    sector[2] = 22;
    sector[6] = 1;
    builder.writeSectorAt(sector, {0, 2, 18}, PCSX::IEC60908b::SectorMode::M2_FORM1);
    builder.writeSectorAt(sector, {0, 2, 19}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    memset(sector, 0, sizeof(sector));
    sector[0] = 1;
    sector[5] = 22;
    sector[7] = 1;
    builder.writeSectorAt(sector, {0, 2, 20}, PCSX::IEC60908b::SectorMode::M2_FORM1);
    builder.writeSectorAt(sector, {0, 2, 21}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    uint8_t rootSector[2048] = {
        0x22, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x01, 0x00, 0x22, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x08, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x01, 0x01, 0x01, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x01, 0x09, 0x50, 0x53, 0x58, 0x2e, 0x45, 0x58, 0x45, 0x3b, 0x31,
    };
    writeToBuffer<uint32_t, std::endian::little>(rootSector + 70, indexSectorsCount + 23);
    writeToBuffer<uint32_t, std::endian::big>(rootSector + 74, indexSectorsCount + 23);
    writeToBuffer<uint32_t, std::endian::little>(rootSector + 78, executableSectorsCount * 2048);
    writeToBuffer<uint32_t, std::endian::big>(rootSector + 82, executableSectorsCount * 2048);
    builder.writeSectorAt(rootSector, {0, 2, 22}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    return 0;
}
