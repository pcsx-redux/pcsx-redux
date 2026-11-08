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
        },
        {
            "path": "path/to/movie.str",
            "stream": true
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

All the file lookups will be relative to base directory, which is the same as the json file if the option `-basedir` isn't specified. The files will be stored into the archive in their order of appearance. The `executable` field is mandatory, and it should point to a valid PSX executable file. The `files` field is an array of objects, each containing a `path` field that points to a file to be included in the CD image. The attached `name` field for each object is optional, and will default to the `path` field. It represents the final string which will be hashed into the archive index, and can be virtually any string. The `compress` field is optional too: set it to `false` to store a file uncompressed, so its sectors can be read directly, for instance to stream it. The `stream` field is optional as well: set it to `true` to store a file made of raw CD-XA sectors, such as an STR movie with interleaved XA-ADPCM audio, as a `STREAM` entry. The file must be a whole number of 2336-byte sectors, each starting with its subheader, as `psxavenc -t str` or `-t xa` write them, or of 2352-byte sectors with sync and header, as `psxavenc -t strcd` or `-t xacd` write them. The sectors are stored as they are, Mode 2 Form 1 or Form 2 as their subheader says, with their header, EDC and ECC rebuilt for where they land on the disc. The entry's first 32-bit field holds the sector count, and such an entry can't be loaded with `readFile`: its sectors are meant to be streamed, for instance with `resolveMovie` and the `StrPlayer`. The `pvd` field is an optional object that contains the PVD (Primary Volume Descriptor) information for the CD image. All fields in the `pvd` object are optional. The `system_id` field defaults to "PLAYSTATION", and all other fields default to empty strings. The `volume_id` field is often used by emulators to identify the CD image, and it is recommended to set it to a unique value.

The tool accepts the following command line arguments:

- `-o`: The output file name for the CD image. Mandatory.
- `-license`: The license file to be used. Optional. Point to an existing valid PlayStation 1 disc, or an official license file. The tool will use the license file to generate the CD image. If not provided, the tool will generate an empty license on the CD image.
- `-basedir`: The base directory for the files. Optional. Default is the directory where the json file is located.
- `-threads`: The number of threads to use for compression. Optional. Default is the number of available CPU cores.
