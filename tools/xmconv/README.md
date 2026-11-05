# XMCONV

Converts FastTracker 2 modules (.xm, version 0104) and 31-sample ProTracker modules (.mod, played with FastTracker 2 effect semantics) for the [xmplayer library](https://github.com/pcsx-redux/nugget/tree/main/xmplayer). It writes two files: the song (patterns, instruments and sample headers, in the format described in `xmplayer.h`), and the samples as SPU ADPCM, which the player uploads to SPU RAM.

## Usage
```sh
xmconv input.xm|input.mod [-a amp] [-f] -o output.xmp -s output.smp
```

## Arguments
| Argument | Type | Description |
|-|-|-|
| input.xm | mandatory | The input module. |
| -o output.xmp | mandatory | The song file. |
| -s output.smp | mandatory | The sample file. |
| -a amp | optional | Sample amplification in 1/256ths. Default is 224, which leaves headroom for ADPCM overshoot on full-scale samples. |
| -f | optional | If the samples do not fit in SPU RAM, halve the sample rate of the largest ones until they do. |
| -h | optional | Show help. |

Channels that are never used are dropped. A song that still uses more than the SPU's 24 voices is refused. Ping-pong loops are unfolded into forward loops, and loops are laid out the same way as modconv's, so their period stays exact. A sample that a song plays above the SPU's pitch ceiling is resampled down by octaves at conversion. The SPU RAM used is printed per sample and in total.
