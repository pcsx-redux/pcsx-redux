# Calling guest code

`PCSX.callGuest(call)` runs a function of the emulated program from Lua, with arguments, and returns its result. It is meant for harnesses: feeding a decompressor deliberately broken streams, measuring how many cycles a routine takes, or checking which memory a routine writes.

It needs the interpreter, since the recompilers can't be stopped on an arbitrary address. Start with `-interpreter`, or turn the dynarec off in the Emulation settings and reboot. It can't be nested, which means it can't be called from a breakpoint invoker that fires during another guest call; an `ExecutionFlow` event listener is a fine place to call it from.

The argument is a single table:

| Field | Meaning |
|---|---|
| `pc` | Mandatory. The address to call, 4-byte aligned. |
| `args` | Table of numbers. The first four go in `$a0` to `$a3`, the rest on the stack, following the o32 convention with its 16-byte argument save area. |
| `sp` | Stack pointer before the argument frame is reserved, 8-byte aligned. Defaults to the top of RAM, minus 16. The callee gets `sp` lowered by the frame, which is 16 bytes, or 4 bytes per argument if that is more: with the default, `$sp` is `0x801fffe0` on entry to a function of the 2MB RAM. The frame must be in RAM or the scratchpad if there are more than four arguments. |
| `gp` | If present, loaded into `$gp`. |
| `ra` | Sentinel return address, 4-byte aligned, defaults to `0x8f000000`. The call ends when the program counter reaches it, so it should be somewhere the callee never legitimately jumps to. |
| `cycles` | Cycle budget, defaults to 100000000. |
| `stage` | Array of `{ addr = ..., data = '...' }`. The strings are copied into RAM before the call, bypassing the guest's cache isolation state. |
| `fetch` | Array of `{ addr = ..., size = ... }`. The ranges are read out of RAM after the call, before any rollback. |
| `isolate` | `false` (default), `true` or `'full'`, or `'ram'`. See below. |
| `dirty` | Boolean. Report which 64kB pages of RAM the callee changed. |

In every mode, the general purpose registers, the coprocessor 0 and 2 registers, the program counter and the cycle counter come back as they were. What differs is the rest of the machine:

- `isolate = false`: whatever the callee did to memory stays.
- `isolate = 'ram'`: RAM and the scratchpad are rolled back, along with the staged inputs and the stack arguments. It is cheaper than `'full'`, and blind to anything that isn't RAM, which `nonRamAccesses` in the result tells you about.
- `isolate = true` or `'full'`: the whole machine is rolled back, through a savestate taken before the inputs go in.

The function returns a table:

| Field | Meaning |
|---|---|
| `status` | `'returned'` when the program counter reached `ra`, `'cycles'` when the budget ran out, `'exception'` when the callee raised one, `'unknown'` otherwise. |
| `v0`, `v1` | The values of `$v0` and `$v1` when the call ended. |
| `cycles` | Cycles spent. |
| `out` | The strings read by `fetch`, in order. |
| `dirty` | Addresses of the changed 64kB pages (`0x80000000` or'd with the page offset), when `isolate = 'ram'` or `dirty` was requested. |
| `nonRamAccesses` | How many bus accesses outside of RAM happened. Zero means the callee touched nothing but RAM, so `'ram'` isolation was sound for it. |
| `depth` | Change in the depth of the call stack monitor. A leaf function never opens a frame, so it stays 0; non-zero after a clean return means the callee unwound badly. |
| `storesDropped` | True when the RAM write LUT is empty, which is the case while the caches are isolated, including on a cold boot: every guest store then vanishes. Worth checking before trusting `dirty`, and the arguments past the fourth, which are stored through the same bus, are dropped as well. `stage` isn't affected. |
| `pc` | Only with status `'cycles'`: where it stopped. |
| `exceptionCode`, `epc`, `badVAddr` | Only with status `'exception'`. |

An invalid argument, an unaligned address, a stage or fetch running off the end of RAM, or a missing interpreter raises a Lua error before anything is modified.

```lua
-- Call a decompressor at 0x80012340 on a staged buffer, and read the result back.
local r = PCSX.callGuest {
    pc = 0x80012340,
    args = { 0x80100000, 0x80200000 },
    stage = { { addr = 0x80100000, data = compressed } },
    fetch = { { addr = 0x80200000, size = 0x800 } },
    isolate = 'ram',
    cycles = 5000000,
}
if r.status == 'returned' and r.nonRamAccesses == 0 then
    print(('v0 = %08x in %d cycles'):format(r.v0, r.cycles))
    print(#r.out[1], 'bytes read back,', #r.dirty, 'pages touched')
end
```
