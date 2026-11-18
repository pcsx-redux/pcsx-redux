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

// No FFmpeg on wasm, nor in a build that defines PCSX_NO_FFMPEG: movies are refused, with an error saying why.
#if defined(__EMSCRIPTEN__) || defined(PCSX_NO_FFMPEG)

#include "support/ffmpeg-movie-file.h"

struct PCSX::FFmpegMovieFile::Private {};

PCSX::FFmpegMovieFile::FFmpegMovieFile(std::unique_ptr<Private> priv) : m_priv(std::move(priv)) {}
PCSX::FFmpegMovieFile::FFmpegMovieFile(FFmpegMovieFile&&) = default;
PCSX::FFmpegMovieFile& PCSX::FFmpegMovieFile::operator=(FFmpegMovieFile&&) = default;
PCSX::FFmpegMovieFile::~FFmpegMovieFile() = default;

std::optional<PCSX::FFmpegMovieFile> PCSX::FFmpegMovieFile::open(const std::string&, unsigned, unsigned, unsigned,
                                                                 std::string& error) {
    error = "this build has no ffmpeg, so it cannot read a movie";
    return std::nullopt;
}

std::optional<std::vector<int16_t>> PCSX::FFmpegMovieFile::readAudio(const std::string&, unsigned channels, unsigned,
                                                                     unsigned&, std::string& error) {
    if (!channels) return std::vector<int16_t>();
    error = "this build has no ffmpeg, so it cannot read a movie";
    return std::nullopt;
}

int PCSX::FFmpegMovieFile::nextFrame(const uint8_t*&, std::string& error) {
    error = "this build has no ffmpeg";
    return -1;
}

unsigned PCSX::FFmpegMovieFile::damaged() const { return 0; }

bool PCSX::FFmpegMovieFile::isStr() const { return false; }

#endif
