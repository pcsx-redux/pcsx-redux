# Expansion carts in Lua

## Introduction & Rationale

The PlayStation's parallel port, also known as EXP1, maps the cartridge connected to it at `0x1f000000` to `0x1f7fffff`. PCSX-Redux emulates one cartridge natively, the PIO cart found under Configuration > PIO Cartridge. Anything else, be it a development board, a USB adapter, or a cheat device behaving slightly differently, can be emulated from Lua, without touching the emulator's code, by combining the memory LUTs and the unmapped memory callbacks described in [Memory and registers](memory-and-registers.md).

The short version: a memory access whose page has no entry in the LUTs, and which isn't claimed by the emulator itself, is forwarded to the global Lua functions `UnknownMemoryRead` and `UnknownMemoryWrite`. A cart in Lua makes sure its pages have no LUT entry, and then answers these callbacks.

## How an access reaches Lua

For each read or write, the CPU looks up the 64kB page of the address, `address >> 16`, in the read or write LUT. In order:

- If the LUT entry is not `nil`, the access goes straight to the memory it points to, and Lua never sees it.
- Pages `0x1f80`, `0x9f80` and `0xbf80`, the scratchpad and the hardware registers, are always handled by the emulator.
- If the native PIO cart is connected, all of `0x1f000000` to `0x1f7fffff` goes to it. So the native PIO cart has to be disconnected for a Lua cart to work.
- Otherwise, the access is forwarded to Lua.

Each page is reachable through three addresses, one per memory segment: `0x1f00` for KUSEG, `0x9f00` for KSEG0, and `0xbf00` for KSEG1. The LUTs have one entry for each, so a cart needs to handle all three.

## Claiming a range

When the native PIO cart is disconnected, the EXP1 pages have no LUT entry, and every access already reaches Lua. However, another script may have mapped them, so a cart should clear the pages it wants to trap explicitly, through the pointers returned by `PCSX.getReadLUT()` and `PCSX.getWriteLUT()`:

```lua
local pages = { 0x1f00, 0x9f00, 0xbf00 }

local function claim()
    local readLUT = PCSX.getReadLUT()
    local writeLUT = PCSX.getWriteLUT()
    if readLUT == nil or writeLUT == nil then return end
    for _, page in ipairs(pages) do
        readLUT[page] = nil
        writeLUT[page] = nil
    end
end
```

The emulator signals the `Memory::SetLuts` event when it rebuilds its memory LUTs, so a cart should listen to it and claim its pages again, keeping the listener object alive as explained in [Events](events.md).

A cart which holds actual memory, such as a ROM, can do the opposite for its read pages: point them to a buffer allocated with `ffi.new`, and reads will then be served at full speed without going through Lua. The buffer must stay referenced from Lua for as long as the LUT points to it, otherwise the garbage collector will free memory the emulated CPU is still reading. Writes to a ROM or a flash chip usually still need to go through Lua, so their write pages stay `nil`.

## The callbacks

```lua
function UnknownMemoryRead(address, size) end
function UnknownMemoryWrite(address, size, value) end
```

- `address` is the full address used by the CPU, including its segment: a read from KSEG1 arrives as `0xbf000100`, not `0x1f000100`. Mask it with `bit.band(address, 0x1fffffff)` to get the physical address.
- `size` is 1, 2 or 4, depending on the instruction.
- For a read, returning a number makes it the value read by the CPU. It should fit in `size` bytes. Returning nothing at all leaves the read unhandled, and the emulator carries on as if the function didn't exist, which usually means the CPU reads `0xff`, `0xffff` or `0xffffffff`. Careful: returning `nil` explicitly still counts as a value, and the CPU will read 0.
- For a write, returning `true` marks it as handled. Anything else leaves it unhandled.
- If either function raises an error, the error is printed in the console, the access is left unhandled, and the emulator removes the global by setting it to `nil`. A buggy cart therefore disconnects itself on its first error.

These are two global names, so only one script at a time can own them. A cart which only handles its own addresses should keep the previous functions and forward everything else to them, as the example below does.

## Disconnecting

A cart which can be unplugged needs to undo what it did when it was connected: remove its `Memory::SetLuts` listener, put back the LUT entries it saved when connecting, and restore the previous `UnknownMemoryRead` and `UnknownMemoryWrite` functions. A cart which pointed LUT entries to its own buffers has to remove these before letting the buffers go.

## Packaging

A cart made of several files is easiest to distribute as a zip file. When a zip file is loaded, its `autoexec.lua` file, if present, is executed, and `Support.extra.dofile` will find the other Lua files of the archive. There are several ways to load it:

- From the command line, using the `-archive` flag, as in `pcsx-redux -archive mycart.zip`.
- From the File menu, using Add Lua archive.
- By dragging and dropping the zip file onto the main window.
- From Lua, using `Support.extra.addArchive(filename)`.

## Example

This cart exposes a single 32-bit status register at `0x1f000100`. Each read returns a counter which increments on every access, and writing to it sets the counter. All the other addresses are forwarded to whatever was handling them before.

```lua
CounterCart = {
    register = 0x1f000100,
    counter = 0,
    pages = { 0x1f00, 0x9f00, 0xbf00 },
    connected = false,
}

function CounterCart.claim()
    local readLUT = PCSX.getReadLUT()
    local writeLUT = PCSX.getWriteLUT()
    if readLUT == nil or writeLUT == nil then return end
    for _, page in ipairs(CounterCart.pages) do
        readLUT[page] = nil
        writeLUT[page] = nil
    end
end

function CounterCart.read(address, size)
    if bit.band(address, 0x1fffffff) == CounterCart.register then
        CounterCart.counter = (CounterCart.counter + 1) % 0x100000000
        return CounterCart.counter
    end
    if CounterCart.previousRead then return CounterCart.previousRead(address, size) end
end

function CounterCart.write(address, size, value)
    if bit.band(address, 0x1fffffff) == CounterCart.register then
        CounterCart.counter = value
        return true
    end
    if CounterCart.previousWrite then return CounterCart.previousWrite(address, size, value) end
    return false
end

function CounterCart.connect()
    if CounterCart.connected then return end
    local readLUT = PCSX.getReadLUT()
    local writeLUT = PCSX.getWriteLUT()
    CounterCart.saved = {}
    for _, page in ipairs(CounterCart.pages) do
        CounterCart.saved[page] = { readLUT[page], writeLUT[page] }
    end
    CounterCart.previousRead = UnknownMemoryRead
    CounterCart.previousWrite = UnknownMemoryWrite
    UnknownMemoryRead = CounterCart.read
    UnknownMemoryWrite = CounterCart.write
    CounterCart.claim()
    CounterCart.listener = PCSX.Events.createEventListener('Memory::SetLuts', CounterCart.claim)
    CounterCart.connected = true
end

function CounterCart.disconnect()
    if not CounterCart.connected then return end
    CounterCart.listener:remove()
    CounterCart.listener = nil
    local readLUT = PCSX.getReadLUT()
    local writeLUT = PCSX.getWriteLUT()
    for page, pointers in pairs(CounterCart.saved) do
        readLUT[page] = pointers[1]
        writeLUT[page] = pointers[2]
    end
    UnknownMemoryRead = CounterCart.previousRead
    UnknownMemoryWrite = CounterCart.previousWrite
    CounterCart.connected = false
end

CounterCart.connect()
```

Saved as `autoexec.lua` in a zip file and loaded with `-archive`, the cart is connected before the emulated machine starts. On the PlayStation side, reading the register is a plain load:

```c
volatile uint32_t *counter = (volatile uint32_t *)0xbf000100;
uint32_t first = *counter;  // 1
uint32_t second = *counter; // 2
*counter = 41;
uint32_t third = *counter;  // 42
```

A byte or halfword access to the same address goes through the same function, with `size` set to 1 or 2. The value returned should fit in that size.

## Complete examples

John Baumann's [lua-pio-cart](https://github.com/johnbaumann/lua-pio-cart) emulates a cheat cartridge with its PAL and SST29EE020 flash chip using these same mechanisms, with its ROM mapped into the read LUT and its control registers trapped through the callbacks. It is packaged as a zip file and is a good starting point for a more complex cart.

[redux-ch376-cart](https://github.com/rixnobis/redux-ch376-cart) emulates a CH376 USB host controller in parallel mode at `0x1f400000`, with a USB boot keyboard attached that follows the PC keyboard through the `Keyboard` event. It traps both of its registers through the callbacks and maps no memory.
