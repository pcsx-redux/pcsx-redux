# Playbook: finding where a bug lives

When a game misbehaves in the emulator, the first question is where the problem is: in the game's own code doing something unexpected, in the CPU emulation, in the GPU, or somewhere else entirely. This page goes through the tools that answer that question from Lua: breakpoints to see what the game's code is doing, and GPU dumps to take the game out of the equation altogether. It builds on the scripted session from the [previous playbook](playbook-scripted-session.md), and the examples were run against Crash Bandicoot (USA) on its title menu.

## Turning the debugger on

[Breakpoints](breakpoints.md) are only evaluated when the debugger is enabled, and the ones triggered by the CPU only work with the interpreter: the dynarec doesn't check them. So any run using them needs both flags:

```
pcsx-redux -testmode -stdout -lua_stdout -bios scph5501.bin -iso "Crash Bandicoot (USA).cue" -run -debugger -interpreter -dofile debug.lua
```

The interpreter is a lot slower than the dynarec, but a scripted session doesn't care, since its schedule is counted in emulated frames.

## Who writes this variable?

Diffing two RAM dumps, as shown in the previous playbook, gives candidate addresses. On Crash's title menu, the word at `0x800549f0` goes up by one on every frame. The next question is which code writes it, and a write breakpoint answers it. Here it's set from the vsync listener once the game has reached the menu, so that it doesn't fire during boot:

```lua
local frame = 0
vsyncListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frame = frame + 1
    if frame == 3600 then
        writerBp = PCSX.addBreakpoint(0x800549f0, 'Write', 4, 'who writes the counter', function(address, width)
            local r = PCSX.getRegisters()
            print(string.format('write to %08x (%d bytes) by pc=%08x ra=%08x', address, width, r.pc, r.GPR.n.ra))
            return false
        end)
    end
end)
```

Which prints:

```
write to 800549f0 (4 bytes) by pc=8003eed8 ra=8004b844
```

`pc` is the store instruction, and `ra` usually points back into the function's caller, which is the next place to look in the assembly view or in a disassembler. Returning `false` from the callback removes the breakpoint after its first hit; returning `true` keeps it, and the callback then runs on every write. The callback doesn't pause the emulator unless it calls `PCSX.pauseEmulator()` itself, so a script keeps running through the hits.

## What is it asking the BIOS?

Games call the BIOS through three entry points, at `0xa0`, `0xb0` and `0xc0`, with the function number in register `t1`. Execution breakpoints on these three addresses log every BIOS call, along with who made it and its first arguments:

```lua
local kernelBps = {}

local function logKernelCalls()
    for _, entry in ipairs({ 0xa0, 0xb0, 0xc0 }) do
        kernelBps[#kernelBps + 1] = PCSX.addBreakpoint(entry, 'Exec', 4, 'kernel call', function()
            local r = PCSX.getRegisters().GPR.n
            print(string.format('%02X(%02x) from %08x a0=%08x a1=%08x', entry, bit.band(r.t1, 0xff), r.ra, r.a0, r.a1))
            return true
        end)
    end
end

local function stopKernelCalls()
    for _, bp in ipairs(kernelBps) do bp:remove() end
    kernelBps = {}
end
```

Calling `logKernelCalls()` at frame 3500 and `stopKernelCalls()` ten frames later gives lines such as:

```
B0(17) from 8003eb2c a0=00000040 a1=1f801070
A0(2a) from 80040e98 a0=80054ad8 a1=8005a49c
B0(16) from 8003e470 a0=00000000 a1=00000000
```

The function numbers can be looked up in the BIOS function list of [psx-spx](https://psx-spx.consoledev.net/). Keep the window short, as every call prints a line.

## Following the code

An execution breakpoint can also cover a whole range. One spanning the 2MB of RAM fires on every instruction, which makes a trace of everything the CPU runs. It's slow, so arm it at the point of interest, and remove it as soon as it has seen enough:

```lua
local count = 0
traceBp = PCSX.addBreakpoint(0x80000000, 'Exec', 0x200000, 'trace', function(address)
    local r = PCSX.getRegisters()
    print(string.format('%08x ra=%08x v0=%08x', address, r.GPR.n.ra, r.GPR.n.v0))
    count = count + 1
    return count < 1000
end)
```

This is the tool for questions such as "which path does the code take after this write", or "what runs between this exception and the return from it".

## Taking the game out of the picture: GPU dumps

When the problem is on the screen, the question becomes whether the game sent the GPU the wrong commands, or whether the GPU emulation drew the right commands wrong. A GPU dump settles it. It records everything the GPU receives, starting from a snapshot of its state, so the same frames can later be replayed without the game, the CPU or the rest of the machine:

```lua
-- in the vsync listener, at the frames of interest
if frame == 3600 then PCSX.GPU.startDump('title.gpd') end
if frame == 3604 then
    PCSX.GPU.stopDump()
    print('dumped frames: ' .. PCSX.GPU.getDumpFrames())
end
```

The recording begins at the next vsync after `startDump`, so the example above records 3 complete frames. The dump can then be replayed by the dump player, which runs its own GPU, separate from the emulator's:

```lua
local player = PCSX.GPU.DumpPlayer
player.load('title.gpd')
while player.step() do end      -- one frame per step, false at the end of the dump
print('replayed up to frame ' .. player.getFrame())

local out = Support.File.open('replay.vram', 'TRUNCATE')
out:writeMoveSlice(player.getVRAM())   -- 16bpp, 1024 pixels per row
out:close()
player.unload()
```

`PCSX.GPU.getVRAM()` returns the emulator's own VRAM in the same layout. On the Crash title menu, replaying the dump gives back the displayed frame byte for byte. Other areas of VRAM, such as textures, can differ, depending on what was uploaded around the moment the dump stopped, so comparing the display area is what tells whether the replay reproduces the picture.

From there, the dump is a self-contained test case:

- If the replay shows the problem, the GPU received commands that produce it. Replaying the same file through a build with a candidate fix, and comparing the VRAM, tells whether the fix works, without ever booting the game again.
- If the replay looks right while the game looks wrong, the problem is elsewhere, on the CPU side or in timing.
- The commands themselves are in the file, so the geometry the game sent can be drawn separately, to check whether it was already broken before reaching the GPU.

### A worked example

This is how the torn spider and web in MediEvil's graveyard ([issue #2012](https://github.com/pcsx-redux/pcsx-redux/issues/2012)) were tracked down. A scripted session played the game to the right frames and dumped them. Drawing the polygons from the dump as a wireframe showed intact geometry, so the game was sending correct data. Replaying the same dump in the emulator reproduced the shards, which put the problem in the GPU's command parsing. The log was also printing `Got an invalid line command`, and the culprit turned out to be a polyline whose very first word looked like the end-of-polyline marker. With the parsing fixed, the same dump replayed cleanly, and both the interpreter and the dynarec had torn identically before, which ruled the CPU out.

That log line is a cheap first check when geometry looks torn: any `Got an invalid line command` message means the emulated GPU desynchronized from the command stream, and the vertices themselves may well be fine.
