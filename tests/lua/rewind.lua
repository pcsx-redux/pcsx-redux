--   Copyright (C) 2026 PCSX-Redux authors
--
--   This program is free software; you can redistribute it and/or modify
--   it under the terms of the GNU General Public License as published by
--   the Free Software Foundation; either version 2 of the License, or
--   (at your option) any later version.
--
--   This program is distributed in the hope that it will be useful,
--   but WITHOUT ANY WARRANTY; without even the implied warranty of
--   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--   GNU General Public License for more details.
--
--   You should have received a copy of the GNU General Public License
--   along with this program; if not, write to the
--   Free Software Foundation, Inc.,
--   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

local lu = require 'luaunit'
local ffi = require 'ffi'

TestRewind = {}

local function ram32(offset)
    return ffi.cast('uint32_t*', PCSX.getMemPtr() + offset)
end

function TestRewind:test_empty()
    while PCSX.getRewindStateCount() > 0 do PCSX.rewindState() end
    lu.assertFalse(PCSX.rewindState())
end

function TestRewind:test_restores_ram_and_registers()
    while PCSX.getRewindStateCount() > 0 do PCSX.rewindState() end
    local word = ram32(0x1f0000)
    local regs = PCSX.getRegisters()
    word[0] = 0x11111111
    regs.GPR.n.t0 = 0x1234
    PCSX.createRewindState()
    word[0] = 0x22222222
    regs.GPR.n.t0 = 0x5678
    PCSX.createRewindState()
    word[0] = 0x33333333
    regs.GPR.n.t0 = 0x9abc
    lu.assertEquals(PCSX.getRewindStateCount(), 2)

    lu.assertTrue(PCSX.rewindState())
    lu.assertEquals(word[0], 0x22222222)
    lu.assertEquals(regs.GPR.n.t0, 0x5678)
    lu.assertEquals(PCSX.getRewindStateCount(), 1)

    lu.assertTrue(PCSX.rewindState())
    lu.assertEquals(word[0], 0x11111111)
    lu.assertEquals(regs.GPR.n.t0, 0x1234)
    lu.assertEquals(PCSX.getRewindStateCount(), 0)
end

function TestRewind:test_ring_is_bounded_and_reuses_slots()
    while PCSX.getRewindStateCount() > 0 do PCSX.rewindState() end
    local word = ram32(0x1f0004)
    for i = 1, 70 do
        word[0] = i
        PCSX.createRewindState()
    end
    lu.assertEquals(PCSX.getRewindStateCount(), 60)
    lu.assertTrue(PCSX.rewindState())
    lu.assertEquals(word[0], 70)
    for i = 2, 60 do PCSX.rewindState() end
    lu.assertEquals(word[0], 11)
    lu.assertFalse(PCSX.rewindState())
end

-- Hands control back to a test from the main loop, once the cpu has unwound out of Execute().
-- Resuming straight from a listener leaves the rest of the suite on the emulation stack, where a
-- rewind is only queued. A vsync polls the event loop too, so the first tick after a pause from
-- a vsync listener still runs on that stack; the cpu has unwound by the second one.
local function resumeFromMainLoop(co)
    local timer = luv.new_timer()
    local ticks = 0
    timer:start(0, 1, function()
        ticks = ticks + 1
        if ticks < 2 then return end
        timer:stop()
        timer:close()
        coroutine.resume(co)
    end)
end

-- Runs a few guest instructions that open a stack frame and spill ra, so the machine holds a
-- real call stack, then parks the cpu in a loop.
local function runIntoCallStack()
    local code = ram32(0x100000)
    code[0] = 0x27bdffe0 -- addiu sp, sp, -32
    code[1] = 0xafbf001c -- sw ra, 28(sp)
    code[2] = 0x08040002 -- j 0x80100008
    code[3] = 0x00000000 -- nop
    PCSX.invalidateCache()
    local regs = PCSX.getRegisters()
    regs.GPR.n.sp = 0x801fff00
    regs.GPR.n.ra = 0x80100100
    regs.pc = 0x80100000
    local co = coroutine.running()
    -- Pause on a vsync, which is certain to come after the guest ran: a nextTick queued from a
    -- test that was itself resumed by an event loop callback fires before the cpu starts.
    local vsync
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsync:remove()
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
end

-- Every capture into a recycled slot must replace what the slot held, not add to it. A slot
-- that accumulates feeds itself back through restore and doubles on every round trip.
function TestRewind:test_round_trips_on_recycled_slots_stay_flat()
    if PCSX.settings.emulator.Dynarec then lu.skip('only the interpreter records call stacks') end
    while PCSX.getRewindStateCount() > 0 do PCSX.rewindState() end
    local before = PCSX.createSaveState().size
    runIntoCallStack()
    local size = PCSX.createSaveState().size
    -- A few bytes of varint jitter between round trips are normal; a call stack is more.
    local slack = 16
    if size - before <= slack then lu.skip('no call stack was recorded, nothing could accumulate') end
    for i = 1, 32 do
        PCSX.createRewindState()
        lu.assertTrue(PCSX.rewindState())
        lu.assertTrue(math.abs(PCSX.createSaveState().size - size) <= slack, 'round trip ' .. i)
    end
end

-- Queues a rewind from a vsync listener and lets the main loop land it. restore() signals
-- SaveStateLoaded before rewindStateNow() returns, and the listener rewinds once more from there:
-- it is off the emulation stack, so that one runs straight away, while the outer step is still
-- in flight. Returns what the nested call returned.
local function rewindTwiceFromTheMainLoop()
    local co = coroutine.running()
    local r = {}
    -- One shot, and gone before it rewinds: the nested restore signals SaveStateLoaded too, and
    -- that must not call back into Lua from inside the rewindState() call.
    local loaded
    loaded = PCSX.Events.createEventListener('ExecutionFlow::SaveStateLoaded', function()
        loaded:remove()
        r.loads = true
        r.nested = PCSX.rewindState()
    end)
    local vsyncs = 0
    local vsync
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsyncs = vsyncs + 1
        if vsyncs == 1 then
            r.queued = PCSX.rewindState()
        elseif r.loads then
            vsync:remove()
            PCSX.pauseEmulator()
            resumeFromMainLoop(co)
        end
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
    lu.assertTrue(r.queued)
    return r.nested
end

-- The nested rewind has to step past the snapshot the outer one is landing on, not land on it
-- a second time.
function TestRewind:test_rewind_from_a_load_listener()
    while PCSX.getRewindStateCount() > 0 do PCSX.rewindState() end
    -- Park the cpu in a loop before taking the snapshots, so it runs on from there after a rewind.
    local code = ram32(0x100300)
    code[0] = 0x080400c0 -- j 0x80100300
    code[1] = 0x00000000 -- nop
    PCSX.invalidateCache()
    PCSX.getRegisters().pc = 0x80100300
    local word = ram32(0x1f0008)
    word[0] = 0x11111111
    PCSX.createRewindState()
    word[0] = 0x22222222
    PCSX.createRewindState()
    word[0] = 0x33333333
    lu.assertTrue(rewindTwiceFromTheMainLoop())
    lu.assertEquals(word[0], 0x11111111)
    lu.assertEquals(PCSX.getRewindStateCount(), 0)

    -- With the last snapshot in flight there is nothing left for the listener to step back to.
    word[0] = 0x44444444
    PCSX.createRewindState()
    word[0] = 0x55555555
    lu.assertFalse(rewindTwiceFromTheMainLoop())
    lu.assertEquals(word[0], 0x44444444)
    lu.assertEquals(PCSX.getRewindStateCount(), 0)
end

-- A rewind asked for from a vsync listener runs in the middle of Counters::update(). It has to
-- wait for the main loop, then land, and the machine has to keep going from the snapshot.
function TestRewind:test_vsync_rewind_waits_for_the_main_loop()
    while PCSX.getRewindStateCount() > 0 do PCSX.rewindState() end
    -- The guest counts in t1; registers come back with a rewind like the rest of the machine.
    local code = ram32(0x100200)
    code[0] = 0x25290001 -- addiu t1, t1, 1
    code[1] = 0x08040080 -- j 0x80100200
    code[2] = 0x00000000 -- nop
    PCSX.invalidateCache()
    local regs = PCSX.getRegisters()
    regs.GPR.n.t1 = 0
    regs.pc = 0x80100200
    local counter = function() return regs.GPR.n.t1 end

    local co = coroutine.running()
    local cycles = function() return tonumber(PCSX.getCPUCycles()) end
    local vsyncs, inListener = 0, false
    local r = { loadedInListener = false }
    local loaded = PCSX.Events.createEventListener('ExecutionFlow::SaveStateLoaded', function()
        r.loadedInListener = r.loadedInListener or inListener
        r.loadedCounter, r.loadedCycle = counter(), cycles()
    end)
    local vsync
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsyncs = vsyncs + 1
        if vsyncs == 2 then
            PCSX.createRewindState()
            r.firstCounter, r.firstCycle = counter(), cycles()
        elseif vsyncs == 3 then
            PCSX.createRewindState()
            r.frameCycles = cycles() - r.firstCycle
        elseif vsyncs == 5 then
            r.beforeCounter = counter()
            inListener = true
            r.rewinds = { PCSX.rewindState(), PCSX.rewindState(), PCSX.rewindState() }
            inListener = false
            r.counterAfterCall, r.countAfterCall = counter(), PCSX.getRewindStateCount()
        elseif vsyncs == 6 then
            r.nextCycle, r.finalCounter = cycles(), counter()
            vsync:remove()
            loaded:remove()
            PCSX.pauseEmulator()
            resumeFromMainLoop(co)
        end
    end)
    PCSX.resumeEmulator()
    coroutine.yield()

    -- Two snapshots, so the third rewind in the same window has nothing left to step back to.
    lu.assertEquals(r.rewinds, { true, true, false })
    lu.assertEquals(r.countAfterCall, 0)
    lu.assertFalse(r.loadedInListener, 'state replaced under the vsync listener')
    lu.assertEquals(r.counterAfterCall, r.beforeCounter)
    -- Both queued rewinds were applied, landing on the older snapshot.
    lu.assertEquals(r.loadedCounter, r.firstCounter)
    lu.assertEquals(r.loadedCycle, r.firstCycle)
    -- And the machine ran on from it, on its clock: the next vsync came a frame later.
    lu.assertTrue(r.finalCounter > r.firstCounter)
    lu.assertTrue(r.finalCounter < r.beforeCounter)
    lu.assertTrue(math.abs(r.nextCycle - r.firstCycle - r.frameCycles) < 64,
                  'frame after rewind: ' .. (r.nextCycle - r.firstCycle) .. ' vs ' .. r.frameCycles)
end

-- Runs guest code that points the SPU transfer address into sound RAM, then pauses on a vsync.
local function runIntoSpuTransferAddress()
    local code = ram32(0x100400)
    code[0] = 0x3c081f80 -- lui t0, 0x1f80
    code[1] = 0x34090100 -- ori t1, zero, 0x100
    code[2] = 0xa5091da6 -- sh t1, 0x1da6(t0)
    code[3] = 0x08040103 -- j 0x8010040c
    code[4] = 0x00000000 -- nop
    PCSX.invalidateCache()
    PCSX.getRegisters().pc = 0x80100400
    local co = coroutine.running()
    local vsync
    vsync = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsync:remove()
        PCSX.pauseEmulator()
        resumeFromMainLoop(co)
    end)
    PCSX.resumeEmulator()
    coroutine.yield()
end

-- On a paused machine a restored state has to save back byte for byte. The first load and the
-- first rewind may still normalize a few fields, so the comparison starts after one of each.
function TestRewind:test_paused_round_trips_are_a_fixed_point()
    while PCSX.getRewindStateCount() > 0 do PCSX.rewindState() end
    runIntoSpuTransferAddress()
    PCSX.loadSaveState(PCSX.createSaveState())
    PCSX.createRewindState()
    lu.assertTrue(PCSX.rewindState())
    local reference = tostring(PCSX.createSaveState())
    for i = 1, 3 do
        PCSX.loadSaveState(PCSX.createSaveState())
        lu.assertTrue(tostring(PCSX.createSaveState()) == reference, 'load round trip ' .. i)
        PCSX.createRewindState()
        lu.assertTrue(PCSX.rewindState())
        lu.assertTrue(tostring(PCSX.createSaveState()) == reference, 'rewind round trip ' .. i)
    end
end
