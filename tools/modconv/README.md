# MODCONV
This folder contains a modern recreation of the MODCONV.EXE software, which is used to convert MOD files to HIT files. The original software was [provided by Hitmen](http://hitmen.c02.at/html/psx_tools.html), without source code.

This version has been written from scratch, without reverse engineering, as the file format is fairly simple to understand. This means that the output files will be slightly different from the original software.

Its purpose is to convert [MOD files](https://en.wikipedia.org/wiki/Module_file) to HIT files, which can then be played by the [modplayer library available](https://github.com/pcsx-redux/nugget/tree/main/modplayer) in the PCSX-Redux project.

## Usage
```sh
modconv input.mod [-s output.smp] [-a amp] [--stretch list] -o output.hit
```

## Arguments
| Argument | Type | Description |
|-|-|-|
| input.mod | mandatory | Specify the input mod file. |
| -o output.hit | mandatory | Name of the output file. |
| -s output.smp | optional | Name of the output samples data file. |
| -a amp | optional | Amplification factor. Default is 175. |
| --stretch list | optional | Comma separated sample numbers, or `all`, whose loops are resampled instead of rounded. See below. |
| -h | optional | Show help. |

If the `-s` argument is not provided, the sample data will be written to the .hit file itself, and can be loaded with the `MOD_Load` function from the modplayer library or the old Hitmen implementation. If the `-s` argument is provided, the sample data will be written into a separate file. Both will need to be loaded with the `MOD_LoadEx` function from the modplayer library, and is not backwards compatible with the old Hitmen implementation. This allows the user to have a simple way to unload the samples data from the main ram after the call to `MOD_LoadEx`, only keeping the .hit file in memory.

The SPU can only loop on blocks of 28 samples. By default, a loop whose length isn't a multiple of 28 is rounded to the nearest block, which changes the pitch of the looped part of the sample; modconv prints how much for each loop it rounds. Short loops are the worst: a 182 samples loop rounded to 196 plays 128 cents flat. The samples listed with `--stretch` are resampled instead, so that their loop is a whole number of blocks, and the song is rewritten to play them at the right pitch: their notes are transposed, their finetune adjusted, and the portamento, vibrato, sample offset and finetune effects playing them are scaled to match. modconv prints the result for each stretched sample, and warns about anything it couldn't rewrite exactly, such as an arpeggio reaching past the end of the period table. A listed sample is rounded instead, with a message saying why, when its notes would be transposed past the end of the period table, when it would grow past 64 kB, or when stretching wouldn't come closer to the original pitch than rounding.
