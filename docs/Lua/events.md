# Events

The Lua code can listen for events broadcasted from within the emulator. The following function is available to register a callback to be called when certain events happen:

```lua
PCSX.Events.createEventListener(eventName, callback)
```

**Important**: the return value of this function will be an object that represents the listener itself. If this object gets garbage collected, the corresponding listener will be removed. Thus it is important to store it somewhere that won't get garbage collected right away. The listener object has a `:remove` method to stop the listener before its garbage collection time.

The callback function will be called from an unsecured environment, and it is advised to delegate anything complex or risky enough to `PCSX.nextTick`.

The `eventName` argument is a string that can have the following values:

 - `Quitting`: The emulator is about to quit. The callback will be called with an empty table. This is where you'd need to close libuv objects held by Lua through luv in order to allow the emulator to quit gracefully. Otherwise you may soft lock the application where it'll wait for libuv objects to close.
 - `IsoMounted`: A new ISO file has been mounted into the virtual CDRom drive. The callback will be called with an empty table.
 - `GPU::Vsync`: The emulated GPU has just completed a vertical blanking interval. The callback will be called with an empty table.
 - `ExecutionFlow::ShellReached`: The emulation execution has reached the beginning of the BIOS' shell. The callback will be called with an empty table. This is the moment where the kernel is properly set up and ready to execute any arbitrary binary. The emulator may use this event to side load binaries, or signal gdb that the kernel is ready.
 - `ExecutionFlow::Run`: The emulator resumed execution. The callback will be called with an empty table. This event will fire when calling `PCSX.resumeEmulator()`, when the user presses Start, or other potential interactions.
 - `ExecutionFlow::Pause`: The emulator paused execution. The callback will be called with a table that contains a boolean named `exception`, indicating if the pause is the result of an execution exception within the emulated CPU. This event will fire on breakpoints too, so if breakpoints have Lua callbacks attached on them, they will be executed too.
 - `ExecutionFlow::Reset`: The emulator is resetting the emulated machine. The callback will be called with a table that contains a boolean named `hard`, indicating if the reset is a hard reset or a soft reset. This event will fire when calling `PCSX.softResetEmulator()` or `PCSX.hardResetEmulator()`, when the user presses Reset, or other potential interactions.
 - `ExecutionFlow::SaveStateLoaded`: The emulator just loaded a savestate. The callback will be called with an empty table. This event will fire when calling `PCSX.loadSaveState()`, when the user loads a savestate, or other potential interactions. This is useful to listen to in case some internal state needs to be reset within the Lua logic.
 - `GUI::JumpToPC`: The UI is being asked to move the assembly view cursor to the specified address. The callback will be called with a table that contains a number named `pc`, indicating the address to jump to.
 - `GUI::JumpToMemory`: The UI is being asked to move the memory view cursor to the specified address. The callback will be called with a table that contains a number named `address`, indicating the address to jump to, and `size`, indicating the number of bytes to highlight.
 - `Keyboard`: The emulator is dispatching keyboard events. The callback will be called with a table containing four numbers: `key`, `scancode`, `action`, and `mods`. `key` is the SDL keycode, `scancode` the SDL scancode, `action` is 1 for a key press and 0 for a release, and `mods` holds the SDL modifier flags. SDL scancodes use the USB HID keyboard usage numbers. Key repeats arrive as further presses.
 - `Memory::SetLuts`: The emulator has updated the memory LUTs. The callback will be called with an empty table.