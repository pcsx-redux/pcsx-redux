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

// No FFmpeg on wasm, and none in a build that defines PCSX_NO_FFMPEG: ffmpeg-movie-file-none.cc is the one there.
#if !defined(__EMSCRIPTEN__) && !defined(PCSX_NO_FFMPEG)

#include "support/ffmpeg-movie-file.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "fmt/format.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

namespace {

// ffmpeg's warnings (an mp4's dropped priming samples, say) are not a tool's to print.
void quietFFmpeg() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

std::string averror(int error) {
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
            error = m_stream == AVERROR_DECODER_NOT_FOUND ? "ffmpeg has no decoder for its video" : "it has no video";
            return false;
        }
        const AVStream* stream = m_format->streams[m_stream];
        if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            // av_find_best_stream passes over a video it has no decoder for, to the cover if there is one.
            bool video = false;
            for (unsigned i = 0; i < m_format->nb_streams; i++) {
                const AVStream* other = m_format->streams[i];
                video |= other->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                         !(other->disposition & AV_DISPOSITION_ATTACHED_PIC);
            }
            error = video ? "ffmpeg has no decoder for its video" : "its only picture is a cover image, not a video";
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

    // The picture on screen at the next frame's time, as 24-bit RGB of the movie's size, valid until the next
    // call: 1, 0 once there are no more, -1 on an error with `error` set.
    int next(const uint8_t*& rgb, std::string& error) {
        const AVRational frameTime = {1, static_cast<int>(m_fps)};
        while (!m_ended) {
            if (!m_hasPending) {
                const int got = decode(m_pending);
                if (got < 0) {
                    error = m_readError ? "ffmpeg failed reading it: " + averror(m_readError)
                                        : "ffmpeg failed decoding its video";
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
        rgb = m_scaled.data();
        m_index++;
        return 1;
    }

    // Packets ffmpeg found damaged and left out, as its own tools do.
    unsigned damaged() const { return m_damaged; }
    // The file is an STR movie (ffmpeg's psxstr demuxer).
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
            const int read = av_read_frame(m_format, m_packet);
            if (read == AVERROR_EOF) {
                if (avcodec_send_packet(m_codec, nullptr) < 0) return 0;
                continue;
            }
            if (read < 0) {
                m_readError = read;
                return -1;
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
    int m_readError = 0;
    bool m_hasCurrent = false, m_hasPending = false, m_ended = false;
    unsigned m_fps = 0, m_width = 0, m_height = 0;
    int64_t m_index = 0;
    std::vector<uint8_t> m_rgb, m_scaled;
};

// The sound of a file as 16-bit PCM at `rate`, interleaved for `channels`. A file
// without sound leaves `pcm` empty and is not an error.
bool readMovieAudio(const std::string& path, unsigned channels, unsigned rate, std::vector<int16_t>& pcm,
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
    // Sound ffmpeg has no decoder for is an error: taken as no sound, the movie would be written silent, unsaid.
    if (stream == AVERROR_DECODER_NOT_FOUND) {
        error = "ffmpeg has no decoder for its sound (\"channels\": 0 makes the movie without it)";
        return done(false);
    }
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
        const int read = av_read_frame(format, packet);
        if (read == AVERROR_EOF) {
            draining = true;
            avcodec_send_packet(codec, nullptr);
            continue;
        }
        if (read < 0) {
            error = "ffmpeg failed reading it: " + averror(read);
            return done(false);
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

}  // namespace

struct PCSX::FFmpegMovieFile::Private {
    MovieVideo video;
};

PCSX::FFmpegMovieFile::FFmpegMovieFile(std::unique_ptr<Private> priv) : m_priv(std::move(priv)) {}
PCSX::FFmpegMovieFile::FFmpegMovieFile(FFmpegMovieFile&&) = default;
PCSX::FFmpegMovieFile& PCSX::FFmpegMovieFile::operator=(FFmpegMovieFile&&) = default;
PCSX::FFmpegMovieFile::~FFmpegMovieFile() = default;

std::optional<PCSX::FFmpegMovieFile> PCSX::FFmpegMovieFile::open(const std::string& path, unsigned fps, unsigned width,
                                                                 unsigned height, std::string& error) {
    if (!fps || !width || !height) {
        error = "a movie needs a frame rate and a size";
        return std::nullopt;
    }
    quietFFmpeg();
    auto priv = std::make_unique<Private>();
    if (!priv->video.open(path, fps, width, height, error)) return std::nullopt;
    return FFmpegMovieFile(std::move(priv));
}

std::optional<std::vector<int16_t>> PCSX::FFmpegMovieFile::readAudio(const std::string& path, unsigned channels,
                                                                     unsigned rate, unsigned& damaged,
                                                                     std::string& error) {
    std::vector<int16_t> pcm;
    if (!channels) return pcm;
    if (!rate) {
        error = "a movie's sound needs a rate";
        return std::nullopt;
    }
    quietFFmpeg();
    if (!readMovieAudio(path, channels, rate, pcm, damaged, error)) return std::nullopt;
    return pcm;
}

int PCSX::FFmpegMovieFile::nextFrame(const uint8_t*& rgb, std::string& error) { return m_priv->video.next(rgb, error); }

unsigned PCSX::FFmpegMovieFile::damaged() const { return m_priv->video.damaged(); }

bool PCSX::FFmpegMovieFile::isStr() const { return m_priv->video.isStr(); }

#endif
