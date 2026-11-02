# Redux basic API

## Settings
All of the settings are exposed to Lua via the `PCSX.settings` table. It contains pseudo-tables that are reflections of the internal objects, and can be used to read and write the settings. The exact list of settings can vary quickly over time, so making a full list here would be fruitless. It is possible however to traverse the settings using `pprint` for example. The semantic of the settings is the same as from within the GUI, with the same caveats. For example, disabling the dynamic recompiler requires a reboot of the emulator.

## ImGui interaction
PCSX-Redux will periodically try to call the Lua function `DrawImguiFrame` to allow the Lua code to draw some widgets on screen. The function will be called exactly once per actual UI frame draw, which, when the emulator is running, will correspond to the emulated GPU's vsync. If the function throws an exception however, it will be disabled until recompiled with new code.

## Events Engine interaction & Execution Contexts
LuaJIT C callbacks aren't called from a safe execution context that can allow for coroutine resuming, and luv's execution context doesn't have any error handling.

It is possible to defer executing code to the main loop of PCSX-Redux, which can (a) resume coroutines and (b) execute code in a safe context. The function `PCSX.nextTick(func)` will execute the given function in the next main loop iteration. Here's some examples of how to use it:

```lua
    local captures = {}
    captures.current = coroutine.running()
    captures.callback = function()
        PCSX.nextTick(function()
            captures.callback:free()
            coroutine.resume(captures.current)
        end)
    end
    captures.callback = ffi.cast('void (*)()', captures.callback)
    -- use the C callback somewhere...
```

```lua
function createClient(ip, port)
  client = luv.new_tcp()

  luv.tcp_connect(client, ip, port, function (err)
    PCSX.nextTick(function()
      assert(not err, err)

      luv.read_start(client, function (err, chunk)
        PCSX.nextTick(function()
          pprint("received at client", {err=err, chunk=chunk})
          assert(not err, err)
          if chunk then
            -- do something with the client
          else
            luv.close(client)
          end
        end)
      end)

      pprint("writing from client")
      luv.write(client, "Hello")
      luv.write(client, "World")

    end)
  end)
  return client
end
```

Of course, this can also delay processing significantly, as the main loop is usually bound to the speed of the UI, which can mean up to 20ms of delay.

## Constants
The table `PCSX.CONSTS` contains numerical constants used throughout the rest of the API. Keeping an up to date list here is too exhausting, and it's simpler to print them using `pprint(PCSX.CONSTS)`.

## Pads
You can access the pads API through `PCSX.SIO0.slots[s].pads[p]` where `s` is the slot number and `p` is the pad number, both indexed from 1, Lua-style. So `PCSX.SIO0.slots[1].pads[1]` accesses the first pad, and `PCSX.SIO0.slots[2].pads[1]` accesses the second pad.

Each Pad table has the following functions:

```lua
getButton(button)     -- Returns true if the specified button is pressed.
setOverride(button)   -- Overrides the specified button.
clearOverride(button) -- Clears the override for the specified button.
setAnalogMode(bool)   -- Sets or clears the analog mode of this pad.
map()                 -- Forces the pad to be remapped. Useful after changing pad settings.
```

The button constants can be found in `PCSX.CONSTS.PAD.BUTTON`.

You can for instance press the button Down on the first pad using the following code:

```lua
PCSX.SIO0.slots[1].pads[1].setOverride(PCSX.CONSTS.PAD.BUTTON.DOWN)
```

## Execution flow
The Lua code has the following API functions available to it in order to control the execution flow of the emulator:

 - `PCSX.pauseEmulator()`
 - `PCSX.resumeEmulator()`
 - `PCSX.softResetEmulator()`
 - `PCSX.hardResetEmulator()`

It's also possible to manipulate savestates using the following functions:

 - `PCSX.createSaveState()    -- returns a slice representing the savestate`
 - `PCSX.loadSaveState(slice)`
 - `PCSX.loadSaveState(file)`

When called while the emulated CPU is running, for instance from a `GPU::Vsync` listener or a breakpoint callback, `PCSX.loadSaveState` doesn't load the state right away. The current CPU step finishes against the old state, and the load happens as soon as the CPU loop returns, before any further instruction is executed. Listen to the `ExecutionFlow::SaveStateLoaded` event to know when it happened. When the emulator is paused, the load is immediate.

Additionally, the following function returns a string containing the .proto file used to serialize the savestate:

 - `PCSX.getSaveStateProtoSchema()`

Note that the actual savestates made from the UI are gzip-compressed, but the functions above don't compress or decompress the data, so if trying to reload a savestate made from the UI, it'll need to be decompressed first, possibly through the zReader File object.

Overall, this means the following is possible:

```lua
local compiler = require('protoc').new()
local pb = require('pb')

local state = PCSX.createSaveState()
compiler:load(PCSX.getSaveStateProtoSchema())

local decodedState = pb.decode('SaveState', Support.sliceToPBSlice(state))
print(string.format('%08x', decodedState.registers.pc))
```

### In-memory snapshots and rewind

Serializing a savestate to a slice is slow when it needs to be done thousands of times. For a harness which snapshots and rolls back repeatedly, there is a single in-memory snapshot slot:

 - `PCSX.captureState()` stores the whole machine in the slot, replacing what was there. It is an error to call it while a `restoreState()` is still waiting to be applied.
 - `PCSX.restoreState()` brings the slot back. It is an error to call it before `captureState()` has filled the slot. As with `PCSX.loadSaveState`, when called from a listener or a breakpoint callback, the restore is applied by the main loop once the CPU loop has returned; otherwise it is immediate.

The emulator also has a rewind ring, which keeps the most recent snapshots. It is filled automatically every N frames when the rewind interval setting is not zero, up to the configured number of snapshots, and by the following functions:

 - `PCSX.createRewindState()` pushes a snapshot onto the ring. When called from inside the emulation, the snapshot is taken at the end of the current CPU branch test, so restoring it later doesn't skip any of the work of that step.
 - `PCSX.rewindState()` restores the most recent snapshot, and removes it from the ring, so successive calls walk further into the past. It returns `false` if the ring is empty, and `true` otherwise. From inside the emulation, the rewind is queued and applied by the main loop, like `PCSX.restoreState()`.
 - `PCSX.getRewindStateCount()` returns how many snapshots can still be rewound to, not counting the ones already claimed by queued rewinds.

For calling a function of the running program and getting its result back without leaving a trace on the machine, see [Calling guest code](guest-calls.md).

## Messages
The globals `print` and `printError` are available, and will display logs in the Lua Console. You can also use `PCSX.log` to display a line in the general Log window. All three functions should behave the way you'd expect from the normal `print` function in mainstream Lua.

## GUI
You can move the cursor within the assembly window and the first memory view using the following functions:

- `PCSX.GUI.jumpToPC(pc)`
- `PCSX.GUI.jumpToMemory(address[, width])`

When drawing over an image with ImGui, `PCSX.Helpers.UI.imageCoordinates(x, y, w, h, imageW, imageH)` converts a point of a `w` by `h` source image, displayed at `imageW` by `imageH` pixels, into absolute screen coordinates. It reads the ImGui cursor position at the time of the call, so call it before drawing the image, while the cursor is still at the image's top left corner.

## GPU
You can take a screenshot of the current view of the emulated display using the following:

- `PCSX.GPU.takeScreenShot()`

This will return a struct that has the following fields:
```c
struct ScreenShot {
    Slice data;
    uint16_t width, height;
    enum { BPP_16, BPP_24 } bpp;
};
```

The `Slice` will contain the raw bytes of the screenshot data. It's meant to be written out using the `:writeMoveSlice()` method on a `File` object. The `width` and `height` will be the width and height of the screenshot, in pixels. The `bpp` will be either `BPP_16` or `BPP_24`, depending on the color depth of the screenshot. The size of the `data` Slice will be `height * width` multiplied by the number of bytes per pixel, depending on the `bpp`.

You can also get the framerate of the emulated game using the following:

- `PCSX.GPU.getGuestFPS()`

This will return a number, which is how many times the game moved the display start area per second of emulated vsyncs. As games flip their buffers by moving the display start area, this is the framerate of the game itself, as opposed to the framerate of the UI. The value is updated once every 50 or 60 emulated vsyncs, depending on the video mode. Games which keep displaying the same buffer, such as single buffered games, will read 0. The exception is 480i: when the display start doesn't move over the counting window, the value counts instead the vsyncs in which the game writes a drawing area or offset after sending primitives, at most one per vsync.

The GPU logger can be driven from Lua too:

- `PCSX.GPU.setLogging(enabled[, overdrawStats])` turns the logger on or off. `overdrawStats` defaults to false, and enables the per-frame overdraw statistics below, which cost a readback of the heatmaps once per frame.
- `PCSX.GPU.getOverdrawStats([read])` returns `nil` until a frame has produced statistics. Otherwise it returns a table for the last complete frame, about the pixels written (or, with `read` true, read) by drawing commands: `total` (sum of the touch counts), `pixels` (how many distinct pixels were touched), `max`, `mean`, and `buckets`, five counts of the pixels touched 1, 2, 3-4, 5-8, and 9 or more times.
- `PCSX.GPU.getOverdrawSum(read, x, y, w, h)` returns the sum of the touch counts over a VRAM rectangle, clamped to the 1024 by 1024 pixel area, from the same frame. It returns 0 when there are no statistics.
- `PCSX.GPU.getVRAM()` returns the emulated VRAM as a `Slice`. Dumping and replaying commands is covered in [Localizing bugs](playbook-localizing-bugs.md).

## SPU
You can play audio from memory through the emulator's audio output using the following:

- `PCSX.SPU.playAudio(source, descriptor)`

The `source` can be a string, a LuaBuffer, a [`File` object](file-api.md), or a `Slice`. A `File` is read entirely, without moving its read cursor. The data is copied or decoded before the function returns. The playback is independent of the emulated SPU, and happens whether the emulation is running or paused. The Mute setting also applies to it.

The `descriptor` is a table which describes the format of the data. Its `format` field is mandatory, and can be one of `'pcm'`, `'spu'`, or `'xa'`. All formats accept an optional `gain` field, which is a non-negative number defaulting to 1.0. Any other field that isn't listed below for the given format is an error.

- `'pcm'`: raw interleaved little endian samples. The data size needs to be a multiple of the frame size.
    - `bits`: mandatory, 8 or 16.
    - `channels`: mandatory, 1 or 2.
    - `rate`: mandatory, the sample rate in Hz, between 1 and 384000.
    - `signed`: optional boolean, defaults to true for 16 bits, and false for 8 bits. 16 bits samples have to be signed.
- `'spu'`: a sequence of 16-byte SPU-ADPCM blocks, mono. The data size needs to be a multiple of 16 bytes. Exactly one of these two fields is mandatory:
    - `rate`: the sample rate in Hz, between 1 and 176400.
    - `pitch`: the SPU pitch value, between 1 and 0x4000, where 0x1000 is 44100Hz.

    The loop flags of the blocks are followed: the playback stops at a block with the loop end flag without the repeat flag, and jumps back to the last loop start block at a block with both the loop end and repeat flags. A single silent block looping onto itself ends the playback, as it is the usual terminator of a one-shot sample.
- `'xa'`: XA-ADPCM data.
    - `sectors`: optional boolean, defaults to false.
    - When `sectors` is false, the data is a sequence of 128-byte sound groups, and these fields are mandatory:
        - `bits`: 4 or 8.
        - `channels`: 1 or 2.
        - `rate`: 37800 or 18900.
    - When `sectors` is true, the data is a sequence of 2352-byte raw sectors starting with the CD sync pattern, or of 2336-byte sectors starting at the subheader. Only the audio sectors are played, and the format is read from their subheaders, so `bits`, `channels`, and `rate` are an error. These fields are optional, and only valid in this mode:
        - `xaFile`: only play the sectors with this file number, between 0 and 255.
        - `xaChannel`: only play the sectors with this channel number, between 0 and 255.

        Without these fields, only the file and channel of the first audio sector are played, since XA files usually interleave several streams. All the played sectors need to have the same format.

The function will throw an error if the descriptor or the data are invalid, or if no audio device is available. Otherwise, it will return a sound object, which has the following methods:

- `:stop()` stops the playback.
- `:isPlaying()` returns true until the playback has finished or has been stopped.
- `:setGain(gain)` changes the gain, which is a non-negative number.

The sound object owns the playback: the sound stops when the object is garbage collected, so it needs to be kept around for as long as the sound should play.

```lua
-- One second of a 440Hz sine wave, as 16 bits mono samples.
local samples = ffi.new('int16_t[44100]')
for i = 0, 44099 do samples[i] = 8000 * math.sin(2 * math.pi * 440 * i / 44100) end
beep = PCSX.SPU.playAudio(ffi.string(samples, 44100 * 2), { format = 'pcm', bits = 16, channels = 1, rate = 44100 })

-- Playing an XA file, keeping its first stream only.
local file = Support.File.open('MUSIC.XA')
music = PCSX.SPU.playAudio(file, { format = 'xa', sectors = true, gain = 0.5 })
file:close()
```

### SPU logger

The SPU logger records the register writes made to the SPU, and can export them as an SPD file:

- `PCSX.SPU.Logger.enable()`, `.disable()`, `.isEnabled()`, `.clear()`
- `PCSX.SPU.Logger.getEventCount()` returns how many events are captured.
- `PCSX.SPU.Logger.exportSPD(file[, tickRate[, fullRam]])` writes the capture as an SPD dump. `file` is a `File` object or a filename; a filename is only opened, and truncated, once the capture is known to be exportable. `tickRate` is in 16.16 fixed point Hz, and defaults to 1/16th of the hblank rate. By default only the span of sound RAM that the keyed voices point into is stored, and `fullRam` set to true stores all of it. Returns `true`, or `false` and the reason, such as there being no register writes to export.

## CPU trace

`PCSX.CPU.Trace` gives access to the binary CPU trace. Instructions are only recorded by the interpreter, while tracing is enabled, and the store stops growing at its limit of 32 million records.

- `.enable()`, `.disable()`, `.enabled()`, `.clear()`, `.size()`
- `.read([start[, count]])` copies records into a new FFI array of `PCSX_TraceEntry`, and returns it with the number of records copied. It returns `nil, 0` when `start` is past the end.
- `.getLine(index)` returns the annotated disassembly of a record, the same text the trace viewer shows.
- `.exportText(path)` writes one annotated line per record to a text file, `.save(path)` writes the raw records, and `.load(path)` replaces the trace with a saved one. All three return a boolean.

A record is 32 bytes, and holds the operands as they were before the instruction executed, so a destination register still has its old value:

```c
typedef struct {
    uint32_t pc, code, regRs, regRt, regRd, hilo, cop, memValue;
} PCSX_TraceEntry;
```

`hilo` is `hi` or `lo`, and `cop` is the coprocessor register value, for the instructions that use them. `memValue` is the value read at the effective address, for loads and stores.

## Disc and lid

`PCSX.CDRom` swaps the disc and drives the lid of the emulated drive:

- `PCSX.CDRom.setIso(source)` mounts a disc image, and parses its table of contents, the same way as the File menu does. `source` is a path, a `File`, or an `Iso` object from `PCSX.openIso`.
- `PCSX.CDRom.ejectIso()` mounts an empty drive.
- `PCSX.CDRom.openLid()` and `PCSX.CDRom.closeLid()` change the lid state immediately.
- `PCSX.CDRom.scheduleCloseLid()` opens the lid now, and closes it one emulated second later.
- `PCSX.CDRom.isLidOpen()` returns the lid state.

## Loading and executing code

While the basic Lua functions `dofile` and `loadfile` exist, some alternative functions are available to load and execute code in a more flexible way.

- `Support.extra.addArchive(filename)` will load the given zip file, and will make it available to the `Support.extra.dofile` function. It is equivalent to the `-archive` command line flag. Note that if a file named `autoexec.lua` is found in the zip file, it will be executed automatically.
- `Support.extra.dofile(filename)` will load the given file, and execute it. It is equivalent to `dofile`, but will also search for the file next to the currently loaded Lua file which is calling this function, and will also search for the file in all of the loaded zip files, either through the command line, or through the `Support.extra.addArchive` function.
- `Support.extra.loadfile(filename)` will load the given file, and return a function that can be called to execute the file. It is equivalent to `loadfile`, but has the same file search algorithm as `Support.extra.dofile`.
- `Support.extra.open(filename)` will open the given file as read only, and return a `File` object. It is roughly equivalent to `Support.File.open`, but has the same file search algorithm as `Support.extra.dofile`.

If given the following directory structure:

```
.
└── bar.zip
    ├── test/baz.lua
    └── test2/qux.lua
```

If `test/baz.lua` contains the following code:

```lua
Support.extra.dofile('../test2/qux.lua')
```
Then running the following code:
```lua
Support.extra.addArchive('bar.zip')
Support.extra.dofile('test/baz.lua')
```
Will first load `test/baz.lua` from the zip file `bar.zip`, run it, which will in turn load `test2/qux.lua` from the zip file `bar.zip` again, and execute it.

This allows distributing complex "mods" as zip files, which can be loaded and executed from the command line or the console.

## Miscellaneous

- `PCSX.quit([code])` schedules the emulator to quit. It's not instantaneous, and will only quit after the current block of Lua code has finished executing, which will be before the next main loop iteration. The `code` parameter is optional, and will be the exit code of the emulator. If not specified, it'll default to 0.

- `PCSX.getCPUCycles()` returns an unsigned 64-bit number indicating how many CPU cycles have elapsed. This can be paired with the `PCSX.CONSTS.CPU.CLOCKSPEED` constant to determine how much emulated time has passed.

- `PCSX.Adpcm.NewEncoder` will return an Adpcm encoder object. The object has the following methods:
  - `:reset([mode])` will reset the encoder, and set the mode to the given mode. The mode can be `'Normal'`, `'XA'`, `'High'`, `'Low'`, `'FourBits'`. The default mode is `'Normal'`, which enables all the filters available in the SPU. The `'XA'` mode limits the encoder to the filters available in the XA ADPCM format. The `'High'` mode uses the high-pass filter, and the `'Low'` mode uses the low-pass filter. The `'FourBits'` mode forces plain 4-bit Adpcm encoding.
  - `:processBlock(inData, [outData], [channels])` will encode the given ffi input buffer, and write the result to the given ffi output buffer. The input buffer should be a buffer of 16-bit signed integers, and the output buffer should be a buffer of 16-bit signed integers. The channels parameter is optional, and will default to 1. The input buffer should contain exactly 28 samples per channel, interleaved when stereo, and so does the output buffer. If the output buffer is not given, the function will return a new buffer with the result. LuaBuffers are also accepted as input and output buffers. The function will return three values: the output buffer, the filter index used, and the shifting used. With 2 channels, it will return five values: the output buffer, then the filter index and shifting used for the first channel, then the filter index and shifting used for the second channel. The function is intended to be used as an intermediate computation step, and the output still needs to be processed into 4 bits or 8 bits samples.
  - `:processSPUBlock(inData, [outData], [blockAttribute])` will encode the given ffi input buffer, and write the result to the given ffi output buffer. The input buffer should be a buffer of 16-bit signed integers, and the output buffer should be a buffer which is at least 16 bytes large. The blockAttribute parameter is optional, and will default to `'OneShot'`. The input buffer should contain exactly 28 samples. If the output buffer is not given, the function will return a new buffer with the result. LuaBuffers are also accepted as input and output buffers. The function will return the encoded block, suitable for SPU usage. The `blockAttribute` parameter can be one of the following strings: `'OneShot'`, `'OneShotEnd'`, `'LoopStart'`, `'LoopBody'`, `'LoopEnd'`.
  - `:finishSPU([outData])` will write the opinionated end of sample looping block, as prescribed by the original Sony API. The output buffer should be a buffer which is at least 16 bytes large. If the output buffer is not given, the function will return a new buffer with the result. LuaBuffers are also accepted as output buffers. The function will return the encoded block, suitable for SPU usage.
  - `:processXABlock(inData, [outData], [xaMode], [channels])` will encode the given ffi input buffer, and write the result to the given ffi output buffer. The input buffer should be a buffer of 16-bit signed integers, and the output buffer should be a buffer which is at least 128 bytes large. Note that a MODE2 FORM2 XA sector requires subheaders and 18 of these blocks. The xaMode parameter is optional, and will default to `'XAFourBits'`. The other valid value is `'XAEightBits'`. It will defines the encoding output between either 4-bit and 8-bit. The channels parameter is optional, and will default to 1. If the output buffer is not given, the function will return a new buffer with the result. LuaBuffers are also accepted as input and output buffers. The function will return the encoded block, suitable for XA usage. The amount of required input samples varies depending of the number of channels and the encoding mode:
    - 4-bit mono: 224 samples aka 448 bytes
    - 4-bit stereo: 112 samples aka 448 bytes
    - 8-bit mono: 112 samples aka 224 bytes
    - 8-bit stereo: 56 samples aka 224 bytes

- `PCSX.Adpcm.NewDecoder` will return an Adpcm decoder object. The object has the following methods:
  - `:reset()` will reset the decoder history.
  - `:decodeSPUBlock(inData, [outData])` will decode a single 16-byte SPU-ADPCM block into 28 16-bit signed samples. The input can be a LuaBuffer, a string, or an ffi pointer, and needs to be at least 16 bytes large. The output can be a LuaBuffer or an ffi pointer, and needs to be at least 56 bytes large. If the output buffer is not given, the function will return a new LuaBuffer with the result. The function will return two values: the output buffer, and the flags byte of the block, which contains the loop flags.
  - `:decodeXASoundGroup(inData, [outData], [bitsPerSample], [channels])` will decode a single 128-byte XA-ADPCM sound group into 16-bit signed samples. The input can be a LuaBuffer, a string, or an ffi pointer, and needs to be at least 128 bytes large. The output can be a LuaBuffer or an ffi pointer. If the output buffer is not given, the function will return a new LuaBuffer with the result. The bitsPerSample parameter is optional, can be 4 or 8, and will default to 4. The channels parameter is optional, can be 1 or 2, and will default to 1. Stereo output is interleaved, left first. The function will return two values: the output buffer, and the number of samples written, which is 224 for 4-bit, and 112 for 8-bit, stereo included. An XA sector contains 18 of these sound groups.

Using the encoder to process an input audio file is as simple as:

```lua
function encodeAudioLoop(inputFile, outputFile)
  local closeInput = false
  if type(inputFile) == 'string' then
    inputFile = Support.File.open(inputFile)
    closeInput = true
  end
  local audio = Support.File.ffmpegAudioFile(inputFile, {
    channels = 'Mono',
    frequency = 22050
  })

  local closeOutput = false
  if type(outputFile) == 'string' then
    outputFile = Support.File.open(outputFile, 'TRUNCATE')
    closeOutput = true
  end
  local blockCount = math.floor(audio:size() / (2 * 28))
  local bufferIn = ffi.new('int16_t[28]')
  local bufferOut = Support.NewLuaBuffer(16)

  local encoder = PCSX.Adpcm.NewEncoder()
  encoder:reset 'Normal'

  for i = 1, blockCount do
    audio:read(bufferIn, 28 * 2)
    local blockType = 'LoopBody'
    if i == 1 then blockType = 'LoopStart' end
    if i == blockCount then blockType = 'LoopEnd' end
    encoder:processSPUBlock(bufferIn, bufferOut, blockType)
    outputFile:write(bufferOut)
  end

  if closeInput then
    inputFile:close()
  end
  if closeOutput then
    outputFile:close()
  end
  audio:close()
end
```
