# CD Authoring

This tool can be used to create a bootable CD image for the PlayStation, using a single input binary file, and a set of files to be archived into the CD image at a fixed location. See the [archive-manager.h](src/mips/psyqo-paths/archive-manager.h) file for more information about the archive format and API.

The generated CD image will have its archive file stored at the sector 23, and as such, the archive manager may be initialized using this LBA. The filenames will be hashed using DJB2. Each entry may be compressed using the UCL-NRV2E algorithm. The LZ4 algorithm will not be used. This means the archive manager only requires to have the UCL-NRV2E decompressor registered.

The tool takes a json file as input, which can have the following structure:

```json
{
    "executable": "executable.ps-exe",
    "files": [
        {
            "path": "path/to/file1.bin"
        },
        {
            "path": "path/to/file2.bin",
            "name": "custom_name.bin"
        }
    ],
    "pvd": {
        "system_id": "PLAYSTATION",
        "volume_id": "MY_CD",
        "volume_set_id": "MY_CD_SET",
        "publisher": "MY_PUBLISHER",
        "preparer": "MY_PREPARER",
        "application_id": "MY_APP_ID",
        "copyright": "MY_COPYRIGHT",
        "abstract": "MY_ABSTRACT",
        "bibliographic": "MY_BIBLIOGRAPHIC"
    }
}
```

Comments are allowed in the json file, and will be ignored.

All the file lookups will be relative to base directory, which is the same as the json file if the option `-basedir` isn't specified. The files will be stored into the archive in their order of appearance. The `executable` field is mandatory, and it should point to a valid PSX executable file. The `files` field is an array of objects, each containing a `path` field that points to a file to be included in the CD image. The attached `name` field for each object is optional, and will default to the `path` field. It represents the final string which will be hashed into the archive index, and can be virtually any string. The `compress` field is optional too: set it to `false` to store a file uncompressed, so its sectors can be read directly, for instance to stream it. The `pvd` field is an optional object that contains the PVD (Primary Volume Descriptor) information for the CD image. All fields in the `pvd` object are optional. The `system_id` field defaults to "PLAYSTATION", and all other fields default to empty strings. The `volume_id` field is often used by emulators to identify the CD image, and it is recommended to set it to a unique value.

A file with a `movie` field is a video instead, in anything ffmpeg reads (an mp4, say). It is encoded into an STR movie, BS v2 frames with 4-bit XA audio interleaved, and stored as a `STREAM` entry: its sectors are raw, Form 1 for video and Form 2 for audio, and the entry's first 32-bit field holds their count, so `psyqo::paths::resolveMovie` can hand the movie to the `StrPlayer` by its name:

```json
{
    "path": "intro.mp4",
    "name": "MOVIES/INTRO.STR",
    "movie": {
        "fps": 15,
        "speed": 2,
        "width": 320,
        "height": 240,
        "channels": 2,
        "rate": 37800,
        "rlHalfwords": 28672
    }
}
```

All the `movie` fields are optional, and `"movie": true` takes the defaults shown above. `speed` is the drive speed it is streamed at, 1 or 2; each frame gets the sectors that speed and the frame rate leave it, at the finest quantization whose frame fits them. The picture is scaled to `width` by `height` without keeping its aspect. `channels` is 2 for stereo, 1 for mono, or 0 for no sound, and `rate` is 37800 or 18900; a video without sound gets no audio sectors. `file` and `channel`, 0 by default, are the XA file and channel numbers written in every sector's subheader. A movie is never compressed, and its `compress` field is ignored. Nor is it held to the 1023 sectors of a stored file, as its count does not go in the index's 10-bit compressed size. Any other field in `movie` is an error. psxsplash, for one, looks its movies up as `MOVIES/<NAME>.STR`.

What plays it has to agree with it. The frames are the size given, rounded up to multiples of 16, as the MDEC decodes 16x16 macroblocks. The player reads at the movie's `speed` (the `StrPlayer`'s `doubleSpeed` option). And the `StrPlayer`, as it comes, drops a frame of more than 12 sectors (its `maxSectorsPerFrame`, which goes to 32 at most), one decoding to more than 28672 run-level halfwords (its `rlHalfwords`), or one larger than 320x240 (its display). The tool prints how many sectors the largest frame takes and warns of each. At double speed with the default sound, frame rates under 11 give frames over 12 sectors. `rlHalfwords` is the most run-level a frame may decode to, 0 for no limit and otherwise at least 64: the encoder coarsens a frame that would need more, as it does one that would need more sectors.

Some things a source may carry are not followed. The sound is taken to start with the picture. A rotated video, such as a phone's portrait clip, is not turned upright, and HDR is not tone mapped; ffmpeg can do either first. A YUV video that does not name its colour matrix is taken as BT.601, as ffmpeg's own scaler takes it. Packets ffmpeg finds damaged are left out, and the tool says how many. An STR movie given as a `movie` is decoded and encoded again, which the tool warns of.

Encoding movies needs ffmpeg's libavformat, libavcodec, libavutil and libswresample when the tool is built, and the tool links them, so it needs them to start at all, with movies or without. It reads them with `src/support/ffmpeg-movie-file.cc`. A build without those libraries, such as the support mirror's Makefile, or the xmake build when pkg-config does not find them, defines `PCSX_NO_FFMPEG` and gets `ffmpeg-movie-file-none.cc` in its place: the tool does everything else, and refuses movie entries.

The tool accepts the following command line arguments:

- `-o`: The output file name for the CD image. Mandatory.
- `-license`: The license file to be used. Optional. Point to an existing valid PlayStation 1 disc, or an official license file. The tool will use the license file to generate the CD image. If not provided, the tool will generate an empty license on the CD image.
- `-basedir`: The base directory for the files. Optional. Default is the directory where the json file is located.
- `-threads`: The number of files to compress or encode at once. Optional. Default is the number of available CPU cores.
