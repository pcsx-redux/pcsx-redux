/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include "cdrom/cdriso.h"
#include "core/cdrom.h"
#include "cueparser/cueparser.h"
#include "cueparser/disc.h"
#include "cueparser/fileabstract.h"
#include "cueparser/scheduler.h"
#include "support/ffmpeg-audio-file.h"
#include "support/uvfile.h"

// this function tries to get the .cue file of the given .bin
// the necessary data is put into the ti (trackinformation)-array
bool PCSX::CDRIso::parsecue(const char *isofileString) {
    std::filesystem::path isofile = MAKEU8(isofileString);
    std::filesystem::path cuename, filepath;
    IO<File> fi;

    m_numtracks = 0;

    // copy name of the iso and change extension from .bin to .cue
    cuename = isofile;
    cuename.replace_extension("cue");

    fi.setFile(new UvFile(cuename));
    if (fi->failed()) return false;
    if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
        fi.asA<UvFile>()->startCaching();
    }

    // Some stupid tutorials wrongly tell users to use cdrdao to rip a
    // "bin/cue" image, which is in fact a "bin/toc" image. So let's check
    // that...
    if (fi->gets() == "CD_ROM_XA") {
        // Don't proceed further, as this is actually a .toc file rather
        // than a .cue file.
        return parsetoc(isofileString);
    }
    fi->rSeek(0, SEEK_SET);

    // build a path for files referenced in .cue
    filepath = cuename.parent_path();

    CueScheduler scheduler;
    Scheduler_construct(&scheduler);
    struct Context {
        std::filesystem::path filepath;
        bool failed = false;
    } context;
    context.filepath = filepath;
    scheduler.opaque = &context;

    auto createFile = [](CueFile *file, CueScheduler *scheduler, const char *filename) -> CueFile * {
        Context *context = reinterpret_cast<Context *>(scheduler->opaque);
        // A FILE entry is relative to the cue sheet's directory; only fall back to the current directory.
        UvFile *fi = new UvFile(context->filepath / filename);
        if (fi->failed()) {
            delete fi;
            fi = new UvFile(filename);
        }
        if (!fi->failed()) {
            if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
                fi->startCaching();
            }
        }
        file->opaque = fi;
        file->destroy = [](CueFile *file) {
            File *fi = reinterpret_cast<File *>(file->opaque);
            fi->close();
            delete fi;
            file->opaque = nullptr;
        };
        file->close = [](CueFile *file, CueScheduler *scheduler, void (*cb)(CueFile *, CueScheduler *)) {
            File *fi = reinterpret_cast<File *>(file->opaque);
            fi->close();
            File_schedule_close(file, scheduler, cb);
        };
        file->size = [](CueFile *file, CueScheduler *scheduler, int compressed,
                        void (*cb)(CueFile *, CueScheduler *, uint64_t)) {
            File *fi = reinterpret_cast<File *>(file->opaque);
            if (compressed && dynamic_cast<UvFile *>(fi)) {
                FFmpegAudioFile *cfi =
                    new FFmpegAudioFile(fi, FFmpegAudioFile::Channels::Stereo, FFmpegAudioFile::Endianness::Little,
                                        FFmpegAudioFile::SampleFormat::S16, 44100);
                file->opaque = cfi;
                fi = cfi;
            }
            File_schedule_size(file, scheduler, fi->size(), cb);
        };
        file->read = [](CueFile *file, CueScheduler *scheduler, uint32_t amount, uint64_t cursor, uint8_t *buffer,
                        void (*cb)(CueFile *, CueScheduler *, int error, uint32_t amount, uint8_t *buffer)) {
            File *fi = reinterpret_cast<File *>(file->opaque);
            if (cursor >= fi->size()) {
                File_schedule_read(file, scheduler, 0, 0, nullptr, cb);
            } else {
                auto r = fi->readAt(buffer, amount, cursor);
                File_schedule_read(file, scheduler, r < 0 ? 1 : 0, r, buffer, cb);
            }
        };
        file->write = [](CueFile *file, CueScheduler *scheduler, uint32_t amount, uint64_t cursor,
                         const uint8_t *buffer, void (*cb)(CueFile *, CueScheduler *, int error, uint32_t amount)) {
            throw std::runtime_error("Writes not implemented");
        };
        file->cfilename = nullptr;
        file->filename = nullptr;
        file->references = 1;
        return !fi->failed() ? file : nullptr;
    };

    CueFile cue;
    CueParser parser;
    CueDisc disc;
    bool success = createFile(&cue, &scheduler, cuename.string().c_str());
    if (!success) {
        throw std::runtime_error("Couldn't open cue file twice...");
    }
    cue.cfilename = cuename.string().c_str();
    CueParser_construct(&parser, &disc);
    CueParser_parse(&parser, &cue, &scheduler, createFile,
                    [](CueParser *parser, CueScheduler *scheduler, const char *error) {
                        Context *context = reinterpret_cast<Context *>(scheduler->opaque);
                        if (error) {
                            context->failed = true;
                            PCSX::g_system->printf("Error parsing Cue File: %s", error);
                        }
                    });

    Scheduler_run(&scheduler);
    CueParser_close(&parser, &scheduler, [](CueParser *parser, CueScheduler *scheduler, const char *error) {
        Context *context = reinterpret_cast<Context *>(scheduler->opaque);
        if (error) {
            context->failed = true;
            PCSX::g_system->printf("Error closing cue parser: %s", error);
        }
        CueParser_destroy(parser);
    });
    Scheduler_run(&scheduler);

    File_schedule_close(&cue, &scheduler, [](CueFile *file, CueScheduler *scheduler) { file->destroy(file); });
    if (context.failed) {
        for (unsigned i = 1; i <= disc.trackCount; i++) {
            CueTrack *track = &disc.tracks[i];
            if (track->file) {
                if (track->file->references == 1) {
                    File_schedule_close(track->file, &scheduler, [](CueFile *file, CueScheduler *scheduler) {
                        file->destroy(file);
                        free(file);
                    });
                } else {
                    track->file->references--;
                }
                track->file = nullptr;
            }
        }
        Scheduler_run(&scheduler);
        return false;
    }
    Scheduler_run(&scheduler);

    m_cdHandle.setFile(reinterpret_cast<UvFile *>(disc.tracks[1].file->opaque));

    for (unsigned i = 1; i <= disc.trackCount; i++) {
        CueTrack *track = &disc.tracks[i];
        File *fi = reinterpret_cast<File *>(track->file->opaque);
        // track->size counts the sectors of a PREGAP directive, which aren't in the file, so stop the
        // track at the next track's INDEX 01 in the same file, or at the end of the file.
        uint32_t start = track->indices[1] - track->fileOffset;
        uint32_t end = fi->size() / 2352;
        if (i < disc.trackCount) {
            CueTrack *next = &disc.tracks[i + 1];
            if (next->file == track->file) end = next->indices[1] - next->fileOffset;
        }
        uint32_t size = end > start ? std::min(track->size, end - start) : 0;
        m_ti[i].handle.setFile(new SubFile(fi, start * 2352, size * 2352));
        m_ti[i].type = track->trackType == TRACK_TYPE_AUDIO ? TrackType::CDDA : TrackType::DATA;
        m_ti[i].cddatype = track->compressed ? trackinfo::CCDDA : trackinfo::BIN;
        m_ti[i].start = IEC60908b::MSF(track->indices[1] + 150);
        m_ti[i].pregap = IEC60908b::MSF(track->indices[1] - track->indices[0]);
        m_ti[i].length = IEC60908b::MSF(track->size);
        if (track->file->references == 1) {
            free(track->file);
        } else {
            track->file->references--;
        }
        track->file = nullptr;
    }

    m_numtracks = disc.trackCount;
    m_multifile = true;

    return true;
}
