# Playbook: scripting a game session

Most reverse engineering work ends up needing the same thing over and over: get the game to a given screen, poke at it, look at the result, and do it all again after changing something. Doing this by hand is slow, and it's never quite the same run twice. This page shows how to turn it into a script, using nothing but the Lua API, so the emulator boots the game, presses the buttons, takes the screenshots and dumps the memory by itself, on the same frames every time.

The example uses Crash Bandicoot (USA), which reaches its title menu after about 3600 frames with no input at all, but nothing here is specific to that game.

## The clock: counting vsyncs

A script needs a notion of time that doesn't depend on how fast the host is. The `GPU::Vsync` [event](events.md) fires once per emulated vertical blank, so counting those gives a frame number that means the same thing on every machine:

```lua
local frame = 0
vsyncListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frame = frame + 1
end)
```

The listener is stored in a global on purpose: if it gets garbage collected, it stops firing.

## A schedule

With a frame counter, the rest is a table of things to do at given frames. Here's a complete script, `session.lua`, which takes a screenshot and a RAM dump of the title menu, presses Start, then takes another screenshot once the game has moved on, and quits:

```lua
local outdir = os.getenv('SESSION_OUT') or '.'
local pad = PCSX.SIO0.slots[1].pads[1]
local B = PCSX.CONSTS.PAD.BUTTON

local frame = 0
local schedule = {}

local function at(f, action)
    schedule[f] = schedule[f] or {}
    table.insert(schedule[f], action)
end

-- Press a button, and release it 6 frames later. A press lasting a single
-- frame can fall between two reads of the pad by the game.
local function tap(f, button)
    at(f, function() pad.setOverride(button) end)
    at(f + 6, function() pad.clearOverride(button) end)
end

-- Save the current display as a binary PPM file, which most image tools
-- can open or convert. This handles the usual 16bpp display mode; the
-- 24bpp mode used by movies needs a different conversion.
local function shot(f, name)
    at(f, function()
        local ss = PCSX.GPU.takeScreenShot()
        local w, h = ss.width, ss.height
        if ss.data.size ~= w * h * 2 then
            print('shot ' .. name .. ': not a 16bpp display, skipped')
            return
        end
        -- 5 bits per component, red in the low bits.
        local p = ffi.cast('const uint16_t*', ss.data.data)
        local px = {}
        for i = 0, w * h - 1 do
            local c = p[i]
            local r = bit.band(c, 31) * 8
            local g = bit.band(bit.rshift(c, 5), 31) * 8
            local b = bit.band(bit.rshift(c, 10), 31) * 8
            px[#px + 1] = string.char(r, g, b)
        end
        local f = io.open(outdir .. '/' .. name .. '.ppm', 'wb')
        f:write(string.format('P6\n%d %d\n255\n', w, h))
        f:write(table.concat(px))
        f:close()
        print(string.format('shot %s %dx%d at frame %d', name, w, h, frame))
    end)
end

-- Write the 2MB of main RAM to a file.
local function dumpRAM(f, name)
    at(f, function()
        local f = io.open(outdir .. '/' .. name .. '.bin', 'wb')
        f:write(ffi.string(PCSX.getMemPtr(), 0x200000))
        f:close()
        print(string.format('ram %s at frame %d', name, frame))
    end)
end

shot(3600, 'title')
dumpRAM(3600, 'title')
tap(3700, B.START)
shot(4300, 'after-start')
at(4310, function() PCSX.quit(0) end)

vsyncListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frame = frame + 1
    local actions = schedule[frame]
    if actions then
        for _, action in ipairs(actions) do action() end
    end
end)
```

A few notes about the pieces:

- The screenshot is the display area as the GPU sees it, so its width is the one the game programmed (512 pixels here), without any aspect ratio correction. See [the GPU section](redux-basics.md#gpu) for the structure it returns.
- `setOverride` holds a button down until `clearOverride` releases it, regardless of what the keyboard or a gamepad are doing. The button names are in `PCSX.CONSTS.PAD.BUTTON`, see [Pads](redux-basics.md#pads).
- The RAM dump is a plain 2MB file, where offset `n` is address `0x80000000 + n`. Two of them taken a few frames apart are the cheapest way to find a variable: diff them, and look for the words that changed the way you expect. For instance, a word that goes up by exactly one on every frame is a frame counter.

## Running it

The script is loaded with `-dofile`, and the run is made independent from your own settings with `-testmode`, which neither loads nor saves them:

```
pcsx-redux -testmode -stdout -lua_stdout -bios scph5501.bin -iso "Crash Bandicoot (USA).cue" -run -dofile session.lua
```

`-stdout` and `-lua_stdout` send the logs and the output of `print` to the terminal, which is where the `shot` and `ram` lines above end up. Passing `-bios` explicitly is a good habit for scripted runs, so they don't depend on whichever BIOS the settings happen to point at.

On a machine without a display, such as a build server, the same command runs under a virtual X server:

```
xvfb-run -a pcsx-redux -testmode -stdout -lua_stdout -bios scph5501.bin -iso "Crash Bandicoot (USA).cue" -run -dofile session.lua
```

Since the schedule is keyed to emulated frames, the same build, BIOS and command line give the same frames on every run: two runs of a script like this one produce byte-identical RAM dumps at frame 3600. This is what makes a script a good regression test: once a screenshot shows the problem at frame N, it shows it at frame N every time, and a fix can be checked the same way.

## Savestates

Booting the game every time is fine when the interesting part is near the start. When it's an hour in, play there once, save a state, and start the script from it instead:

```lua
local state = PCSX.createSaveState()        -- a Slice holding the state
-- ...
PCSX.loadSaveState(state)
```

A state can also be written to a file and loaded back in a later run:

```lua
local out = Support.File.open('/path/to/level3.state', 'TRUNCATE')
out:writeMoveSlice(PCSX.createSaveState())
out:close()

-- later, in another session
local file = Support.File.open('/path/to/level3.state')
PCSX.loadSaveState(file)
file:close()
```

Savestates made from the UI are gzip-compressed, while these aren't. See [Execution flow](redux-basics.md#execution-flow) for the details, including what happens when loading a state from a vsync listener.

## Driving it from outside, through the web server

Sometimes the schedule isn't known in advance, and something outside the emulator decides what to do next by looking at the screen: a test harness, a script in another language, a person at a terminal. The [web server](../web_server.md) already covers screenshots, pausing and resuming, and anything else can be added as a Lua endpoint. Here's a script that adds two of them: one returning the frame counter, and one tapping a button.

```lua
local pad = PCSX.SIO0.slots[1].pads[1]
local frame = 0
local releases = {}

vsyncListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frame = frame + 1
    local buttons = releases[frame]
    if buttons then
        for _, button in ipairs(buttons) do pad.clearOverride(button) end
        releases[frame] = nil
    end
end)

PCSX.WebServer = PCSX.WebServer or {}
PCSX.WebServer.Handlers = PCSX.WebServer.Handlers or {}

PCSX.WebServer.Handlers.frame = function(request)
    return tostring(frame)
end

PCSX.WebServer.Handlers.tap = function(request)
    local name = string.match(request.urlData.query or '', 'button=(%w+)')
    local button = name and PCSX.CONSTS.PAD.BUTTON[name]
    if not button then return 'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n' end
    pad.setOverride(button)
    local release = frame + 6
    releases[release] = releases[release] or {}
    table.insert(releases[release], button)
    return 'pressed ' .. name .. ' at frame ' .. frame
end
```

Start the emulator with `-webserver -webserver-port 8080 -dofile remote.lua`, and the session can be driven with nothing more than `curl`:

```
$ curl http://localhost:8080/api/v1/lua/frame
3720
$ curl -o title.png http://localhost:8080/api/v1/screen/still
$ curl "http://localhost:8080/api/v1/lua/tap?button=START"
pressed START at frame 3726
$ curl -X POST "http://localhost:8080/api/v1/execution-flow?function=pause"
$ curl -o paused.png http://localhost:8080/api/v1/screen/still
$ curl -X POST "http://localhost:8080/api/v1/execution-flow?function=resume"
```

While paused, the frame counter stays put, so a screenshot taken then is exactly the frame the counter says. Keep in mind the web server listens on all network interfaces, so anything that can reach the machine can reach these endpoints too.

The [next playbook](playbook-localizing-bugs.md) builds on this to find out where a bug lives.
