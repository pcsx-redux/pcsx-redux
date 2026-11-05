# Memory and registers

## FFI access
The Lua code can access the emulated memory and registers directly through some FFI bindings:

- `PCSX.getMemPtr()` will return a `cdata[uint8_t*]` representing up to 8MB of emulated memory. This can be written to, but careful about the emulated i-cache in case code is being written to.
- `PCSX.getParPtr()` will return a `cdata[uint8_t*]` representing up to 512kB of the EXP1/Parallel port memory space. This can be written to.
- `PCSX.getRomPtr()` will return a `cdata[uint8_t*]` representing up to 512kB of the BIOS memory space. This can be written to.
- `PCSX.getScratchPtr()` will return a `cdata[uint8_t*]` representing up to 1kB for the scratchpad memory space.
- `PCSX.getRegisters()` will return a structured cdata representing all the registers present in the CPU:
- `PCSX.setGPR(index, value)` writes the general purpose register `index`, from 0 to 33 (`lo` and `hi` are 32 and 33). Unlike a store into `getRegisters().GPR`, it also drops a load still pending for that register. Assigning `pc` through `getRegisters()` likewise drops a branch still pending in a delay slot.
- `PCSX.getReadLUT()` will return a `cdata[uint8_t**]` representing the read LUT for the CPU.
- `PCSX.getWriteLUT()` will return a `cdata[uint8_t**]` representing the write LUT for the CPU.

```c
typedef union {
    struct {
        uint32_t r0, at, v0, v1, a0, a1, a2, a3;
        uint32_t t0, t1, t2, t3, t4, t5, t6, t7;
        uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
        uint32_t t8, t9, k0, k1, gp, sp, s8, ra;
        uint32_t lo, hi;
    } n;
    uint32_t r[34];
} psxGPRRegs;

typedef union {
    uint32_t r[32];
} psxCP0Regs;

typedef union {
    uint32_t r[32];
} psxCP2Data;

typedef union {
    uint32_t r[32];
} psxCP2Ctrl;

typedef struct {
    psxGPRRegs GPR;
    psxCP0Regs CP0;
    psxCP2Data CP2D;
    psxCP2Ctrl CP2C;
    uint32_t pc;
} psxRegisters;
```

## Safer access

The above methods will return direct pointers into the emulated memory, so it's easy to crash the emulator if you're not careful. The `getMemoryAsFile()` method is safer, but will be slower:

- `PCSX.getMemoryAsFile()` will return a `File` object representing the full 4GB of accessible memory. All operations on this file will be translated to the emulated memory space. This is slower than the direct access methods, but safer. Any read or write operation will be clamped to the emulated memory space, and will not crash the emulator.

## Memory mapping

PCSX-Redux will attempt to forward reads and writes for memory not mapped in the LUTs. This is useful for debugging, but will be slower than the direct access methods.

- `UnknownMemoryRead(address, size)` will be called when a read is attempted to an unmapped memory address. The function should return an 8, 16, or 32-bit value to be returned to the CPU.
- `UnknownMemoryWrite(address, size, value)` will be called when a write is attempted to an unmapped memory address. The function should return `true` or `false` indicating whether the write was handled.

See [Expansion carts in Lua](expansion-carts.md) for how to use these, together with the LUTs, to emulate a parallel port cartridge.