# Case studies

## Spyro: Year of the Dragon

By looking up some of the [gameshark codes](https://www.cheatcc.com/psx/codes/spyroyotd.html) for this game, we can determine the following memory addresses:

- `0x8007582c` is the number of lives.
- `0x80078bbc` is the health of Spyro.
- `0x80075860` is the number of unspent jewels available to the player.
- `0x80075750` is the number of dragons Spyro released so far.

With this, we can build a small UI to visualize and manipulate these values in real time:

```lua
-- Declare a helper function with the following arguments:
--   mem: the ffi object representing the base pointer into the main RAM
--   address: the address of the uint32_t to monitor and mutate
--   name: the label to display in the UI
--   min, max: the minimum and maximum values of the slider
--
-- This function is local as to not pollute the global namespace.
local function doSliderInt(mem, address, name, min, max)
  -- Clamping the address to the actual memory space, essentially
  -- removing the upper bank address using a bitmask. The result
  -- will still be a normal 32-bits value.
  address = bit.band(address, 0x1fffff)
  -- Computing the FFI pointer to the actual uint32_t location.
  -- The result will be a new FFI pointer, directly into the emulator's
  -- memory space, hopefully within normal accessible bounds. The
  -- resulting type will be a cdata[uint8_t*].
  local pointer = mem + address
  -- Casting this pointer to a proper uint32_t pointer.
  pointer = ffi.cast('uint32_t*', pointer)
  -- Reading the value in memory
  local value = pointer[0]
  -- Drawing the ImGui slider
  local changed
  changed, value = imgui.SliderInt(name, value, min, max, '%d')
  -- The ImGui Lua binding will first return a boolean indicating
  -- if the user moved the slider. The second return value will be
  -- the new value of the slider if it changed. Therefore we can
  -- reassign the pointer accordingly.
  if changed then pointer[0] = value end
end

-- Utilizing the DrawImguiFrame periodic function to draw our UI.
-- We are declaring this function global so the emulator can
-- properly call it periodically.
function DrawImguiFrame()
  -- This is typical ImGui paradigm to display a window using
  -- the safe mode. This will ensure that the window is properly
  -- closed even if an exception is thrown during the rendering
  -- of the window.
  imgui.safe.Begin('Spyro internals', function()
    -- Grabbing the pointer to the main RAM, to avoid calling
    -- the function for every pointer we want to change.
    -- Note: it's not a good idea to hold onto this value between
    -- calls to the Lua VM, as the memory can potentially move
    -- within the emulator's memory space.
    local mem = PCSX.getMemPtr()

    -- Now calling our helper function for each of our pointer.
    doSliderInt(mem, 0x8007582c, 'Lives', 0, 9)
    doSliderInt(mem, 0x80078bbc, 'Health', -1, 3)
    doSliderInt(mem, 0x80075860, 'Jewels', 0, 65000)
    doSliderInt(mem, 0x80075750, 'Dragons', 0, 70)
  end)
end
```

You can see this code in action [in this demo video](https://youtu.be/WeHXTLDy5rs).

## Crash Bandicoot

Using exactly the same as above, we can repeat the same sort of cheats for Crash Bandicoot. Note that when the CPU is being emulated, the `DrawImguiFrame` function will be called at least when the emulation is issuing a vsync event. This means that cheat codes that regularly write to memory during vsync can be applied naturally.

```lua
local function crash_Checkbox(mem, address, name, value, original)
  address = bit.band(address, 0x1fffff)
  local pointer = mem + address
  pointer = ffi.cast('uint32_t*', pointer)
  local changed
  local check
  local tempvalue = pointer[0]
  if tempvalue == original then check = false end
  if tempvalue == value then check = true else check = false end
  changed, check = imgui.Checkbox(name, check)
  if check then pointer[0] = value else pointer[0] = original end
end

function DrawImguiFrame()
  imgui.safe.Begin('Crash Bandicoot Mods', function()
    local mem = PCSX.getMemPtr()
    crash_Checkbox(mem, 0x80027f9a, 'Neon Crash', 0x2400, 0x100c00)
    crash_Checkbox(mem, 0x8001ed5a, 'Unlimited Time Aku', 0x0003, 0x3403)
    crash_Checkbox(mem, 0x8001dd0c, 'Walk Mid-Air', 0x0000, 0x8e0200c8)
    crash_Checkbox(mem, 0x800618ec, '99 Lives at Map', 0x6300, 0x0200)
    crash_Checkbox(mem, 0x80061949, 'Unlock all Levels', 0x0020, 0x00)
    crash_Checkbox(mem, 0x80019276, 'Disable Draw Level', 0x20212400, 0x20210c00)
  end)
end
```

## Crash Bandicoot - Using Conditional BreakPoints

This example will showcase using the BreakPoints and Assembly UI, as well as using the Lua console to manipulate breakpoints.

Crash Bandicoot 1 has several modes of execution. These modes tell the game what to do, such as which level to load into, or to load back into the map. These modes are passed to the main game loop routine as an argument. Due to this, manually manipulating memory at the right time with the correct value to can be tricky to ensure the desired result.

The game modes are [listed here](https://github.com/wurlyfox/crashutils/blob/da21a40a3e8928762eb58b551a54a6e6f8ed73e9/doc/crash/disasm_guide.txt#L131).

In Crash 1, there is a level that was included in the game but cut from the final level selection due to difficulty, 'Stormy Ascent'. This level can be accessed only by manipulating the game mode value that is passed to the main game routine. There is a gameshark code that points us to the memory location and value that needs to be written in order to set the game mode to the Story Ascent level.

- `30011DB0 0022` - This is telling us to write the value 0x0022 at memory location `0x8001db0` 0x0022 is the value of the Stormy Ascent level we want to play.

The issue is that GameShark uses a hook to achieve setting this value at the correct time. We will set up a breakpoint to see where the main game routine is.

Setting the breakpoint can be done through the Breakpoint UI or in the Lua console. There is a link to a video at the bottom of the article showing the entire procedure.

Breakpoints can alternatively be set through the Lua console. In PCSX-Redux top menu, click Debug → Lua → Show Lua Console

We are going to add a breakpoint to pause execution when memory address 0x8001db0 is read. This will show where the main game loop is located in memory.

In the Lua console, paste the following hit enter.

```lua
bp = PCSX.addBreakpoint(0x80011db0, 'Read', 1, 'Find main loop')
```

You should see where the breakpoint was added in the Lua console, as well as in the Breakpoints UI. Note that we need to assign the result of the function to a variable to avoid garbage collection.

Now open Debug → CPU → Show Assembly

Start the emulator with Crash Bandicoot 1 SCUS94900

Right before the BIOS screen ends, the emulator should pause. In the assembly window we can see a yellow arrow pointing to `0x80042068`. We can see this is a `lw` instruction that is reading a value from `0x8001db0`. This is the main game loop reading the game mode value from memory!

Now that we know where the main game loop is located in memory, we can set a conditional breakpoint to properly set the game mode value when the main game routine is executed.

This breakpoint will be triggered when the main game loop at `0x80042068` is executed, and ensure the value at `0x80011db0` is set to `0x0022`

In the Lua console, paste the following and hit enter.

```lua
bp = PCSX.addBreakpoint(0x80042068, 'Exec', 4, 'Stormy Ascent', function()
  PCSX.getMemPtr()[0x11db0] = 0x22
end)
```

We can now disable/remove our Read breakpoint using the Breakpoints UI, and restart the game. Emulation → Hard Reset

If the Emulator status shows Idle, click Emulation → Start emulation

Once the game starts, instead of loading into the main menu, you should load directly into the Stormy Ascent level.

You can see this in action [in this demo video](https://youtu.be/BczviiXUYOY).

## More references

Here's some projects using PCSX-Redux' Lua scripting capabilities, which can be used as references:

- [https://github.com/NDR008/TensorFlowPSX](https://github.com/NDR008/TensorFlowPSX)
- [https://github.com/Kuumba123/TeheManX4_Editor](https://github.com/Kuumba123/TeheManX4_Editor)
- [https://github.com/notdodgeball/vagrant-story-lua-script](https://github.com/notdodgeball/vagrant-story-lua-script)
- [https://github.com/johnbaumann/lua-pio-cart](https://github.com/johnbaumann/lua-pio-cart)
- [https://github.com/rixnobis/redux-ch376-cart](https://github.com/rixnobis/redux-ch376-cart)
- [https://github.com/FoxdieTeam/mgs_reversing](https://github.com/FoxdieTeam/mgs_reversing)
- [https://github.com/kiletic/tekken3-ai](https://github.com/kiletic/tekken3-ai)
