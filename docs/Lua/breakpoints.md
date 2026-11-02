# Breakpoints
If the debugger is activated, and while using the interpreter, the Lua code can insert powerful breakpoints using the following API:

Breakpoints added while the debugger is off are accepted, but won't trigger until it is turned on, either with the `-debugger` command line flag or from the configuration menu. `PCSX.addBreakpoint` prints a warning in that case.

```lua
PCSX.addBreakpoint(address, type, width, cause, invoker, label)
```

**Important**: the return value of this function will be an object that represents the breakpoint itself. If this object gets garbage collected, the corresponding breakpoint will be removed. Thus it is important to store it somewhere that won't get garbage collected right away.

The only mandatory argument is `address`, which will by default place an execution breakpoint at the corresponding address. The second argument `type` is an enum which can be represented by one of the 3 following strings: `'Exec'`, `'Read'`, `'Write'`, and will set the breakpoint type accordingly. The third argument `width` is the width of the breakpoint, which indicates how many bytes should intersect from the base address with operations done by the emulated CPU in order to actually trigger the breakpoint. The fourth argument `cause` is a string that will be displayed in the logs about why the breakpoint triggered. It will also be displayed in the Breakpoint Debug UI. And the fifth and final argument `invoker` is a Lua function that will be called whenever the breakpoint is triggered. By default, this will simply call `PCSX.pauseEmulator()`. The optional sixth argument `label` is a string naming the breakpoint in the Breakpoint Debug UI; when absent or empty, the `cause` is used. If `cause` is a function and `invoker` is omitted, the function is taken as the invoker. If the invoker returns `false`, the breakpoint will be permanently removed, permitting temporary breakpoints for example. The signature of the invoker callback is:

```lua
function(address, width, cause)
    -- body
end
```

The `address` parameter will contain the address that triggered the breakpoint. For `'Exec'` breakpoints, this is going to be the same as the current `pc`, but for `'Read'` and `'Write'`, it's going to be the actual accessed address. The `width` parameter will contain the width of the access that triggered the breakpoint, which can be different from what the breakpoint is monitoring. And the `cause` parameter will contain a string describing the reason for the breakpoint; the latter may or may not be the same as what was passed to the `addBreakpoint` function. Note that you don't need to strictly adhere to the signature, and have zero, one, two, or three arguments for your invoker callback. The return value of the invoker callback is also optional.

For example, these two examples are well formed and perfectly valid:

```lua
bp1 = PCSX.addBreakpoint(0x80000000, 'Write', 0x80000, 'Write tracing', function(address, width, cause)
    local regs = PCSX.getRegisters()
    local pc = regs.pc
    print('Writing at ' .. address .. ' from ' .. pc .. ' with width ' .. width .. ' and cause ' .. cause)
end)

bp2 = PCSX.addBreakpoint(0x80030000, 'Exec', 4, 'Shell reached - pausing', function()
    PCSX.pauseEmulator()
    return false
end)
```

The returned breakpoint object will have a few methods attached to it:

- `:disable()`
- `:enable()`
- `:isEnabled()`
- `:remove()`

A removed breakpoint will no longer have any effect whatsoever, and none of its methods will do anything. Remember it is possible for the user to still manually remove a breakpoint from the UI.

A Lua error raised by the invoker is caught, reported in the Lua console, and the breakpoint is deleted, as if the invoker had returned `false`. The invoker can still catch its own errors with `pcall` to keep the breakpoint alive. For example:

```lua
local someActualFunction = function(address, width, cause)
    -- body
end
bp = PCSX.addBreakpoint(0x80030000, 'Write', 4, 'Shell write tracing', function(address, width, cause)
    local success, msg = pcall(function()
        someActualFunction(address, width, cause)
    end)
    if not success then
        print('Error while running Lua breakpoint callback: ' .. msg)
    end
end)
```

This will ensure that the breakpoint will never crash the emulator, and will instead display the error in the logs, but it will also slow down the execution of the breakpoint. It's up to the user to decide whether or not this is acceptable.

It is safe to add or remove breakpoints from within a breakpoint callback, but it's not safe to remove the breakpoint that is currently being executed. For this specific case, simply return `false` from the invoker callback, and the breakpoint will be removed after the callback returns.
