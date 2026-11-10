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

#pragma once

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace PCSX {

// A movie file read through ffmpeg, for the tools that encode movies: its picture as 24-bit RGB frames at the rate
// and size asked for, and its sound as 16-bit PCM. Where ffmpeg is not there, on wasm or in a build that defines
// PCSX_NO_FFMPEG (the support mirror's, say), open() and readAudio() give std::nullopt and an error saying so
// (ffmpeg-movie-file-none.cc). The first open() or readAudio() turns ffmpeg's own logging down to errors: a
// tool's output is not the place for an mp4's dropped priming samples.
class FFmpegMovieFile {
  public:
    // The video of `path`: frames picked at `fps` from its timestamps and stretched to `width` by `height`.
    // std::nullopt, with `error` set, when ffmpeg cannot read it or it has no video ffmpeg can decode, or when
    // any of the three is 0. The caller keeps them in range (tools/authoring takes 1..60 fps and 16..640 by
    // 16..480).
    static std::optional<FFmpegMovieFile> open(const std::string& path, unsigned fps, unsigned width, unsigned height,
                                               std::string& error);
    // The sound of `path` as 16-bit PCM at `rate`, interleaved for `channels`: empty when it has none or
    // `channels` is 0, std::nullopt with `error` set when it cannot be read (sound ffmpeg has no decoder for among
    // them) or `rate` is 0. Packets ffmpeg finds damaged are left out and added to `damaged`.
    static std::optional<std::vector<int16_t>> readAudio(const std::string& path, unsigned channels, unsigned rate,
                                                         unsigned& damaged, std::string& error);

    FFmpegMovieFile(FFmpegMovieFile&&);
    FFmpegMovieFile& operator=(FFmpegMovieFile&&);
    ~FFmpegMovieFile();

    // The next frame, `width` * `height` * 3 bytes at `rgb`, valid until the next call: 1, 0 once there are no more,
    // -1 on an error with `error` set.
    int nextFrame(const uint8_t*& rgb, std::string& error);
    // Video packets ffmpeg found damaged and left out, as its own tools do.
    unsigned damaged() const;
    // The file is an STR movie already (ffmpeg's psxstr demuxer).
    bool isStr() const;

  private:
    struct Private;
    explicit FFmpegMovieFile(std::unique_ptr<Private>);
    std::unique_ptr<Private> m_priv;
};

}  // namespace PCSX
