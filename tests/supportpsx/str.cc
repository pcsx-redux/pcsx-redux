/*
 * Tests for supportpsx/str: the STR movie a frame source and PCM become.
 *
 * Every count asserted below follows from the settings by the format's own
 * arithmetic (75 sectors a second per speed, 18 sound groups an audio sector),
 * so a scheduling change shows up as a wrong number rather than passing quietly.
 */

#include "supportpsx/str.h"

#include <stdint.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "supportpsx/iec-60908b.h"

namespace {

// Frames with structure, each a little different from the one before, so no
// two frames pack to the same stream.
PCSX::STR::FrameSource frames(unsigned count, unsigned w = 32, unsigned h = 32) {
    auto made = std::make_shared<unsigned>(0);
    return [=](PCSX::STR::Planes &planes, std::string &) {
        if (*made == count) return 0;
        std::vector<uint8_t> rgb(w * h * 3);
        for (unsigned j = 0; j < h; j++) {
            for (unsigned i = 0; i < w; i++) {
                for (unsigned c = 0; c < 3; c++) {
                    rgb[(j * w + i) * 3 + c] = static_cast<uint8_t>((i * 7 + j * 3 + c * 50 + *made * 11) ^ (i * j));
                }
            }
        }
        planes.fromRGB(rgb.data(), w, h, w * 3);
        (*made)++;
        return 1;
    };
}

// Noise the encoder cannot squeeze, for the frame that must not fit.
PCSX::STR::FrameSource noise(unsigned w, unsigned h) {
    return [=](PCSX::STR::Planes &planes, std::string &) {
        std::vector<uint8_t> rgb(w * h * 3);
        uint32_t x = 12345;
        for (auto &v : rgb) {
            x = x * 1103515245 + 12345;
            v = static_cast<uint8_t>(x >> 16);
        }
        planes.fromRGB(rgb.data(), w, h, w * 3);
        return 1;
    };
}

uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }

bool isAudio(const uint8_t *sector) { return sector[18] & 0x04; }

}  // namespace

TEST(Str, videoOnlyGivesEachFrameItsShareOfSectors) {
    PCSX::STR::Settings settings;  // 15 fps at double speed: 150 / 15 = 10 sectors a frame
    std::vector<uint8_t> out;
    const auto r = PCSX::STR::encode(settings, frames(3), {}, out);
    ASSERT_FALSE(r.failed) << r.error;
    EXPECT_EQ(r.frames, 3u);
    EXPECT_EQ(r.sectors, 30u);
    EXPECT_EQ(r.audioSectors, 0u);
    EXPECT_EQ(r.mostSectorsAFrame, 10u);
    EXPECT_EQ(r.width, 32u);
    EXPECT_EQ(r.height, 32u);
    ASSERT_EQ(out.size(), 30u * 2352);
    for (unsigned n = 0; n < 30; n++) {
        const uint8_t *sector = out.data() + n * 2352;
        const uint8_t *chunk = sector + 24;
        EXPECT_EQ(sector[15], 2u);  // Mode 2
        EXPECT_EQ(sector[18], 0x48u);
        EXPECT_EQ(sector[22], 0x48u);
        EXPECT_EQ(le16(chunk), 0x0160u);
        EXPECT_EQ(le16(chunk + 2), 0x8001u);
        EXPECT_EQ(le16(chunk + 4), n % 10);
        EXPECT_EQ(le16(chunk + 6), 10u);
        EXPECT_EQ(le32(chunk + 8), n / 10 + 1);
        EXPECT_EQ(le16(chunk + 16), 32u);
        EXPECT_EQ(le16(chunk + 18), 32u);
    }
}

// A movie goes into an archive as a STREAM entry, which keeps its own 32-bit
// sector count rather than the 10 bits (1023 sectors) a stored file is held to.
TEST(Str, aMovieRunsPastTheSectorCountOfAStoredFile) {
    PCSX::STR::Settings settings;  // 10 sectors a frame
    std::vector<uint8_t> out;
    const auto r = PCSX::STR::encode(settings, frames(103, 16, 16), {}, out);
    ASSERT_FALSE(r.failed) << r.error;
    EXPECT_EQ(r.frames, 103u);
    EXPECT_EQ(r.sectors, 1030u);
    ASSERT_EQ(out.size(), 1030u * 2352);
    const uint8_t *last = out.data() + 1029 * 2352;
    uint8_t msf[3];
    PCSX::IEC60908b::MSF(1029 + 150).toBCD(msf);
    EXPECT_EQ(memcmp(last + 12, msf, 3), 0);
    EXPECT_EQ(le16(last + 24 + 4), 9u);
    EXPECT_EQ(le16(last + 24 + 6), 10u);
    EXPECT_EQ(le32(last + 24 + 8), 103u);
}

// A frame decodes to the run-level its BS header counts, and the player drops one needing more than its buffer
// holds, so the encoder trades detail for it as it does for sectors.
TEST(Str, framesStayWithinTheRunLevelAPlayerHolds) {
    PCSX::STR::Settings settings;
    settings.rlHalfwords = 0;  // no limit
    std::vector<uint8_t> loose;
    const auto unlimited = PCSX::STR::encode(settings, frames(3, 64, 64), {}, loose);
    ASSERT_FALSE(unlimited.failed) << unlimited.error;
    ASSERT_GT(unlimited.mostRunLevel, 800u);  // else this tests nothing
    settings.rlHalfwords = 800;
    std::vector<uint8_t> out;
    const auto r = PCSX::STR::encode(settings, frames(3, 64, 64), {}, out);
    ASSERT_FALSE(r.failed) << r.error;
    EXPECT_LE(r.mostRunLevel, 800u);
    EXPECT_GT(r.averageQScale, unlimited.averageQScale);
    for (unsigned frame = 0; frame < 3; frame++) {
        const uint8_t *bs = out.data() + frame * 10 * 2352 + 24 + 32;
        EXPECT_LE(2u * le16(bs), 800u) << "frame " << frame;
    }
    // A limit no frame can meet is an error, as a frame too big for its sectors is.
    settings.rlHalfwords = 2;
    std::vector<uint8_t> none;
    EXPECT_TRUE(PCSX::STR::encode(settings, frames(1, 64, 64), {}, none).failed);
}

TEST(Str, theChunkHeaderRepeatsTheFramesBsHeader) {
    PCSX::STR::Settings settings;
    std::vector<uint8_t> out;
    ASSERT_FALSE(PCSX::STR::encode(settings, frames(2), {}, out).failed);
    for (unsigned frame = 0; frame < 2; frame++) {
        const uint8_t *chunk = out.data() + frame * 10 * 2352 + 24;
        const uint32_t used = le32(chunk + 12);
        EXPECT_GT(used, 8u);
        EXPECT_LE(used, 10u * 2016);
        EXPECT_EQ(used % 4, 0u);
        EXPECT_EQ(memcmp(chunk + 0x14, chunk + 32, 8), 0);
        EXPECT_EQ(le32(chunk + 0x1c), 0u);
    }
}

TEST(Str, sectorsAreAddressedFromTwoSecondsWithTheirChecksums) {
    PCSX::STR::Settings settings;
    settings.channels = 2;
    std::vector<int16_t> pcm(37800 * 2, 1000);
    std::vector<uint8_t> out;
    const auto r = PCSX::STR::encode(settings, frames(4), pcm, out);
    ASSERT_FALSE(r.failed) << r.error;
    for (unsigned n = 0; n < r.sectors; n++) {
        const uint8_t *sector = out.data() + n * 2352;
        uint8_t msf[3];
        PCSX::IEC60908b::MSF(n + 150).toBCD(msf);
        EXPECT_EQ(memcmp(sector + 12, msf, 3), 0) << "sector " << n;
        uint8_t copy[2352];
        memcpy(copy, sector, sizeof(copy));
        // Clobber the checksums: Form 1's EDC and ECC from 2072, Form 2's EDC in the last four bytes.
        const size_t checksums = isAudio(sector) ? 2348 : 2072;
        memset(copy + checksums, 0xaa, sizeof(copy) - checksums);
        PCSX::IEC60908b::computeEDCECC(copy);
        EXPECT_EQ(memcmp(copy, sector, sizeof(copy)), 0) << "sector " << n;
    }
}

TEST(Str, stereoAudioTakesOneSectorInEightAndTheLastCarriesEof) {
    PCSX::STR::Settings settings;
    settings.channels = 2;  // 37800 Hz stereo at double speed: 150 * 2016 / 37800 = 8
    std::vector<int16_t> pcm(37800 * 2, 1000);
    std::vector<uint8_t> out;
    const auto r = PCSX::STR::encode(settings, frames(6), pcm, out);
    ASSERT_FALSE(r.failed) << r.error;
    // The video's share is 150 * 7 / 8 sectors a second, 8.75 a frame, given out
    // whole with the remainder carried: 8, 9, 9, 9, 8, 9.
    unsigned audio = 0, eof = 0, last = 0;
    for (unsigned n = 0; n < r.sectors; n++) {
        const uint8_t *sector = out.data() + n * 2352;
        EXPECT_EQ(isAudio(sector), n % 8 == 0) << "sector " << n;
        if (!isAudio(sector)) continue;
        audio++;
        last = n;
        EXPECT_EQ(sector[18] & 0x7f, 0x64);
        EXPECT_EQ(sector[19], 0x01);  // stereo, 37800 Hz
        EXPECT_EQ(sector[18], sector[22]);
        if (sector[18] & 0x80) eof++;
    }
    EXPECT_EQ(audio, r.audioSectors);
    EXPECT_EQ(r.sectors - audio, 52u);
    EXPECT_EQ(r.mostSectorsAFrame, 9u);
    EXPECT_EQ(eof, 1u);
    EXPECT_TRUE(out[last * 2352 + 18] & 0x80);
}

TEST(Str, monoAtLowRateSetsTheCodingAndSpacing) {
    PCSX::STR::Settings settings;
    settings.channels = 1;
    settings.rate = 18900;  // 150 * 4032 / 18900 = 32
    std::vector<int16_t> pcm(18900, 1000);
    std::vector<uint8_t> out;
    const auto r = PCSX::STR::encode(settings, frames(8), pcm, out);
    ASSERT_FALSE(r.failed) << r.error;
    for (unsigned n = 0; n < r.sectors; n++) {
        const uint8_t *sector = out.data() + n * 2352;
        EXPECT_EQ(isAudio(sector), n % 32 == 0) << "sector " << n;
        if (isAudio(sector)) EXPECT_EQ(sector[19], 0x04);  // mono, 18900 Hz
    }
}

TEST(Str, aFrameRateTheDriveCannotFeedIsRefused) {
    PCSX::STR::Settings settings;
    settings.fps = 60;
    settings.speed = 1;  // 75 sectors a second: 60 frames get one each
    std::vector<uint8_t> out;
    EXPECT_FALSE(PCSX::STR::encode(settings, frames(2), {}, out).failed);
    settings.channels = 2;  // and with audio taking one in four, fewer than one
    std::vector<int16_t> pcm(1000);
    out.clear();
    const auto r = PCSX::STR::encode(settings, frames(2), pcm, out);
    EXPECT_TRUE(r.failed);
    EXPECT_TRUE(out.empty());
}

TEST(Str, aFrameTooBigForItsShareFailsAndLeavesTheOutputAlone) {
    PCSX::STR::Settings settings;
    settings.fps = 60;
    settings.speed = 1;
    std::vector<uint8_t> out(100, 0x5a);
    const auto r = PCSX::STR::encode(settings, noise(320, 240), {}, out);
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.error.find("does not fit"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("and 28672 run-level halfwords"), std::string::npos) << r.error;
    ASSERT_EQ(out.size(), 100u);
    EXPECT_EQ(out[99], 0x5a);
    settings.rlHalfwords = 0;  // no run-level limit: the message names the sectors alone
    const auto unlimited = PCSX::STR::encode(settings, noise(320, 240), {}, out);
    EXPECT_TRUE(unlimited.failed);
    EXPECT_NE(unlimited.error.find("does not fit"), std::string::npos) << unlimited.error;
    EXPECT_EQ(unlimited.error.find("run-level"), std::string::npos) << unlimited.error;
    EXPECT_EQ(out.size(), 100u);
}

TEST(Str, framesOfDifferentSizesAreRefused) {
    PCSX::STR::Settings settings;
    unsigned n = 0;
    auto source = [&](PCSX::STR::Planes &planes, std::string &) {
        if (n == 2) return 0;
        std::vector<uint8_t> rgb(64 * 32 * 3, 100);
        planes.fromRGB(rgb.data(), n ? 64 : 32, 32, 64 * 3);
        n++;
        return 1;
    };
    std::vector<uint8_t> out;
    const auto r = PCSX::STR::encode(settings, source, {}, out);
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.error.find("64x32"), std::string::npos) << r.error;
}

TEST(Str, noFramesAndOutOfRangeSettingsAreErrors) {
    std::vector<uint8_t> out;
    PCSX::STR::Settings settings;
    EXPECT_TRUE(PCSX::STR::encode(settings, frames(0), {}, out).failed);
    EXPECT_FALSE(PCSX::STR::encode(settings, frames(1), {}, out).failed);
    EXPECT_TRUE(PCSX::STR::check(settings).empty());
    for (auto change : {+[](PCSX::STR::Settings &s) { s.fps = 0; }, +[](PCSX::STR::Settings &s) { s.fps = 61; },
                        +[](PCSX::STR::Settings &s) { s.speed = 3; }, +[](PCSX::STR::Settings &s) { s.channels = 3; },
                        +[](PCSX::STR::Settings &s) {
                            s.channels = 2;
                            s.rate = 44100;
                        }}) {
        PCSX::STR::Settings bad;
        change(bad);
        EXPECT_FALSE(PCSX::STR::check(bad).empty());
        EXPECT_TRUE(PCSX::STR::encode(bad, frames(1), {}, out).failed);
    }
}

// A movie encoded elsewhere comes back as encode() would have written it, from any of its three layouts.
TEST(StrSectors, rawAndMode2SectorsComeBackByteForByte) {
    PCSX::STR::Settings settings;
    settings.channels = 2;
    std::vector<int16_t> pcm(37800 * 2, 1000);
    std::vector<uint8_t> movie;
    const auto encoded = PCSX::STR::encode(settings, frames(4), pcm, movie);
    ASSERT_FALSE(encoded.failed) << encoded.error;

    // Raw sectors from elsewhere on a disc: readdressed from 00:02:00.
    std::vector<uint8_t> raw = movie;
    for (unsigned n = 0; n < encoded.sectors; n++) PCSX::IEC60908b::MSF(n + 5000).toBCD(raw.data() + n * 2352 + 12);
    std::vector<uint8_t> out;
    auto r = PCSX::STR::fromSectors(raw, out);
    ASSERT_FALSE(r.failed) << r.error;
    EXPECT_EQ(out, movie);
    EXPECT_EQ(r.sectors, encoded.sectors);
    EXPECT_EQ(r.audioSectors, encoded.audioSectors);
    EXPECT_EQ(r.mostRunLevel, encoded.mostRunLevel);
    EXPECT_EQ(r.mostSectorsAFrame, encoded.mostSectorsAFrame);
    EXPECT_EQ(r.frames, 4u);
    EXPECT_EQ(r.width, 32u);
    EXPECT_EQ(r.height, 32u);

    // 2336 bytes a sector, from the subheader on, with the checksums left out.
    std::vector<uint8_t> mode2;
    for (unsigned n = 0; n < encoded.sectors; n++) {
        const uint8_t *sector = movie.data() + n * 2352;
        mode2.insert(mode2.end(), sector + 16, sector + 2352);
        // Form 1's EDC and ECC from 2072, Form 2's EDC in the last four bytes.
        const size_t checksums = isAudio(sector) ? 2348 : 2072;
        memset(mode2.data() + mode2.size() - (2352 - checksums), 0, 2352 - checksums);
    }
    out.clear();
    r = PCSX::STR::fromSectors(mode2, out);
    ASSERT_FALSE(r.failed) << r.error;
    EXPECT_EQ(out, movie);
}

TEST(StrSectors, videoAloneGetsTheSubheaderEncodeGivesVideo) {
    PCSX::STR::Settings settings;
    std::vector<uint8_t> movie;
    const auto encoded = PCSX::STR::encode(settings, frames(3), {}, movie);
    ASSERT_FALSE(encoded.failed) << encoded.error;
    std::vector<uint8_t> video;
    for (unsigned n = 0; n < encoded.sectors; n++) {
        const uint8_t *sector = movie.data() + n * 2352;
        video.insert(video.end(), sector + 24, sector + 24 + 2048);
    }
    std::vector<uint8_t> out = {1, 2, 3};
    const auto r = PCSX::STR::fromSectors(video, out);
    ASSERT_FALSE(r.failed) << r.error;
    ASSERT_EQ(out.size(), 3 + movie.size());
    EXPECT_EQ(memcmp(out.data() + 3, movie.data(), movie.size()), 0);
    EXPECT_EQ(r.sectors, 30u);
    EXPECT_EQ(r.audioSectors, 0u);
    EXPECT_EQ(r.frames, 3u);

    // A blank sector first does not make it look like another layout.
    std::vector<uint8_t> blankFirst = video;
    memset(blankFirst.data(), 0, 2048);
    out.clear();
    const auto b = PCSX::STR::fromSectors(blankFirst, out);
    ASSERT_FALSE(b.failed) << b.error;
    EXPECT_EQ(b.sectors, 30u);
    EXPECT_EQ(memcmp(out.data() + 2352, movie.data() + 2352, movie.size() - 2352), 0);
}

TEST(StrSectors, whatIsNotAnStrMovieIsRefusedAndLeavesTheOutputAlone) {
    PCSX::STR::Settings settings;
    settings.channels = 2;
    std::vector<int16_t> pcm(37800 * 2, 1000);
    std::vector<uint8_t> movie;
    ASSERT_FALSE(PCSX::STR::encode(settings, frames(2), pcm, movie).failed);
    std::vector<uint8_t> out = {7};
    auto refused = [&](std::vector<uint8_t> in) {
        const auto r = PCSX::STR::fromSectors(in, out);
        EXPECT_TRUE(r.failed);
        EXPECT_FALSE(r.error.empty());
        EXPECT_EQ(out, std::vector<uint8_t>{7});
    };
    refused({});
    refused(std::vector<uint8_t>(4096, 0x55));
    refused(std::vector<uint8_t>(movie.begin(), movie.end() - 100));  // not whole sectors
    std::vector<uint8_t> mode1 = movie;
    mode1[2352 * 3 + 15] = 1;
    refused(mode1);
    std::vector<uint8_t> audio;  // its audio sectors alone
    for (size_t at = 0; at < movie.size(); at += 2352) {
        if (movie[at + 18] & 0x04) audio.insert(audio.end(), movie.begin() + at, movie.begin() + at + 2352);
    }
    ASSERT_FALSE(audio.empty());
    refused(audio);
}

TEST(StrPlanes, paddingRepeatsTheLastRowAndColumn) {
    // 17x9 grey at 100 with its last column at 200: padded to 32x16.
    std::vector<uint8_t> rgb(17 * 9 * 3, 100);
    for (unsigned j = 0; j < 9; j++) {
        for (unsigned c = 0; c < 3; c++) rgb[(j * 17 + 16) * 3 + c] = 200;
    }
    PCSX::STR::Planes planes;
    planes.fromRGB(rgb.data(), 17, 9, 17 * 3);
    ASSERT_EQ(planes.width, 32u);
    ASSERT_EQ(planes.height, 16u);
    EXPECT_EQ(planes.y[0], 100);
    EXPECT_EQ(planes.y[16], 200);
    EXPECT_EQ(planes.y[31], 200);                // past the right edge
    EXPECT_EQ(planes.y[15 * 32 + 3], 100);       // past the bottom edge
    EXPECT_EQ(planes.y[15 * 32 + 31], 200);      // past both
    for (auto v : planes.cb) EXPECT_EQ(v, 128);  // grey has no colour
    for (auto v : planes.cr) EXPECT_EQ(v, 128);
}
