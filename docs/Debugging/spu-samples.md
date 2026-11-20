# SPU samples and replacement packs

The SPU samples window lists the ADPCM samples a game has keyed on, exports them as a SoundFont, and can play a SoundFont of replacements in place of the originals. The file format is described in the [SPU sample replacement standard](https://github.com/ps1dev/standards/blob/main/SPUSAMPLE.md), so a pack made here can be read by other tools.

## Listing the samples

Open `Debug -> SPU -> Show SPU samples`. The window finds samples from the key-on events of the SPU logger, so the logger has to be running: open `Debug -> SPU -> Show SPU logger` and check `Enable`. Then play the game to the point where the sounds you want are used. `Refresh` rebuilds the list from the logged key-ons. With `Auto refresh` checked, the list is rebuilt whenever the logger records new events.

Each row is one sample:

- `Start` and `End` are its addresses in sound RAM, from the start address to the end of the block carrying the loop end flag. An end with a `+` means the sample ran off the end of sound RAM without an end flag; such a sample has no signature and cannot be exported or replaced.
- `Samples` is its length in decoded samples, 28 per block.
- `Loop` is the address of its loop block, or `one shot`. Many games end a sample on a repeat flag with no loop-start flag and set the loop address in the voice's register instead; for those the column shows, of the addresses inside the sample that the game used at its key-ons, the most frequent one, marked `(register)`. It shows `repeat` when the game used no address inside the sample.
- `Key ons` counts the key-ons logged for it, and `Voices` lists the voices that played it.
- `Signature` is the 16 hexadecimal digit MD5-based signature that identifies the sample by its content. The same sample loaded at another address has the same signature.
- `Root` is the note the sample sounds at pitch 1000h, from its measured fundamental frequency, with note 69 = 440 Hz = A4. Unpitched samples, such as noise and most percussion, have none.
- `Check` compares that note with the pitches the game keyed the sample on with. A game tuned to A440 plays a correctly measured sample on whole notes, and `agrees` says it does. `disagrees` means the sample is worth checking by ear; a whole octave error is not caught by this check.
- `Replaced` shows whether a loaded pack replaces the sample.

Clicking a row plots the selected sample's decoded waveform, with its loop marked.

## Exporting a SoundFont

`Export SoundFont...` writes every complete sample in the list to a SoundFont 2 file, one preset per sample. Each sample is named by its signature, decoded at 44100 Hz, which is what pitch 1000h plays, and carries its loop when it has one. Its root key and pitch correction describe the note it sounds; unpitched samples get root key 255. The file's comment repeats these rules, records each sample's size and the note it sounds in one `spu-sample` line per sample, and lists the samples whose pitch disagrees with the game, the samples with no key-on to check against, and the unpitched ones.

## Editing the pack

Open the exported file in any SoundFont editor, such as [Polyphone](https://www.polyphone.io/). To replace a sample, change its audio and keep its name: the name is the signature, and it is the only thing that ties a replacement to the original. For each replacement:

- set the sample rate to the rate of the new recording;
- set the root key and pitch correction to the note the new recording sounds, so it is played at the same pitch as the original;
- to make it loop, set the loop points on the sample and the loop mode of the instrument zone that plays it to loop.

Leave the `spu-sample` lines in the file's comment: a replacement is tuned against the note its line records for the original, and the recorded sizes let the emulator skip most samples at key-on without hashing them. A sample whose line is missing is not used as a replacement.

The new recording does not need to be the same length or the same rate as the original. Samples you do not want to replace can be deleted from the pack, and samples whose name is not a signature are ignored, so the file can carry other material. A SoundFont 3 file, with Ogg Vorbis samples, works too.

## Playing the replacements

`Load replacements...` loads a pack, and the line below the buttons says how many replacements it found and how many samples it skipped. From then on, each key-on of a sample whose signature is in the pack plays the replacement. A voice that is already playing keeps its original until its next key-on. `Clear replacements` unloads the pack. The pack is not remembered across restarts, so load it again each session.

Only the sound changes. The original sample keeps being decoded underneath, so the end flags, the loop addresses, sound RAM IRQs and the capture buffers behave as they did without the pack. The voice's envelope and volume still apply to the replacement, and the replacement feeds the reverb. A replacement that does not loop and runs out before the note ends leaves the voice silent for the rest of the note.

After loading a save state, replacements start again at each voice's next key-on.
