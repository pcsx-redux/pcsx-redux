/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "gtest/gtest.h"
#include "main/main.h"

// The drawing environment set by GP0(E1h)..GP0(E6h) must survive a save state round trip.
// The script plants a few instructions that write GP0(E1h) and GP0(E6h), saves, loads,
// and saves again. GPUSTAT bits 0-12 and 15 (texpage, dither, draw-to-display, mask)
// must read the same in both states. A flat rectangle drawn after the load must land
// where the same rectangle landed before it, which covers the drawing area, offset and
// mask state that GPUSTAT does not report.
//
// plant() writes each GP0 word through a stub at 0x80100000 that then spins; like
// tests/lua/pcwrite.lua, it pauses and sets the pc from the main loop, not mid-Execute(). The
// environment is texpage (5,1) 8-bit with dither and draw-to-display, a texture window,
// drawing area (16,8)-(400,256), offset (16,2), and mask set + check. The rectangle is
// flat blue 16x16 at (0,0): it lands at (16,2), clipped to rows 8 and below. The script
// stays under 4KB because a longer -exec argument fails to start.
static const char c_gpustatSaveState[] = R"(
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local code = 0x100000 / 4

local function varint(s, pos)
    local v, mul = 0, 1
    while true do
        local b = s:byte(pos)
        pos = pos + 1
        v = v + bit.band(b, 0x7f) * mul
        if b < 0x80 then return v, pos end
        mul = mul * 128
    end
end

local function field(s, wanted)
    local pos = 1
    while pos <= #s do
        local key
        key, pos = varint(s, pos)
        local num, wt = math.floor(key / 8), key % 8
        local payload
        if wt == 0 then
            payload, pos = varint(s, pos)
        elseif wt == 2 then
            local len
            len, pos = varint(s, pos)
            payload = s:sub(pos, pos + len - 1)
            pos = pos + len
        elseif wt == 1 then
            pos = pos + 8
        elseif wt == 5 then
            pos = pos + 4
        else
            error('wire type ' .. wt)
        end
        if num == wanted then return payload end
    end
end

local function gpustat(s) return field(field(s, 5), 1) end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local function plant(words)
    local i = code
    ram[i] = 0x3c081f80 i = i + 1 -- lui $t0, 0x1f80
    for _, w in ipairs(words) do
        ram[i] = 0x3c090000 + bit.rshift(w, 16) i = i + 1 -- lui $t1, hi
        ram[i] = 0x35290000 + bit.band(w, 0xffff) i = i + 1 -- ori $t1, $t1, lo
        ram[i] = 0xad091810 i = i + 1 -- sw $t1, 0x1810($t0)
    end
    ram[i] = 0x1000ffff i = i + 1 -- b .
    ram[i] = 0 i = i + 1
    PCSX.invalidateCache()
    PCSX.pauseEmulator()
    local timer = luv.new_timer()
    timer:start(1, 0, function()
        timer:close()
        PCSX.getRegisters().pc = 0x80100000
        PCSX.resumeEmulator()
    end)
end

local env = { 0xe10006b5, 0xe2012345, 0xe3002010, 0xe4040190, 0xe5001010, 0xe6000003 }
local rect = { 0x60ff0000, 0x00000000, 0x00100010 }
local probes = { { 20, 5 }, { 20, 10 }, { 40, 10 }, { 31, 17 } }

local function pixels()
    local vram = tostring(PCSX.GPU.getVRAM())
    local out = {}
    for i, p in ipairs(probes) do
        local o = (p[2] * 1024 + p[1]) * 2 + 1
        out[i] = vram:byte(o) + vram:byte(o + 1) * 256
    end
    return out
end

local frames = 0
local first, control
GpustatSaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        plant(env)
    elseif frames == 32 then
        first = tostring(PCSX.createSaveState())
        local stat = gpustat(first)
        if bit.band(stat, 0x9fff) ~= 0x1eb5 then
            print(string.format('GP0 writes did not land: GPUSTAT %08x', stat))
            PCSX.quit(1)
            return
        end
        plant(rect)
    elseif frames == 34 then
        control = pixels()
        if control[2] ~= 0xfc00 then
            print(string.format('control rectangle did not draw as expected: %04x', control[2]))
            PCSX.quit(2)
            return
        end
        PCSX.loadSaveState(toSlice(first))
    elseif frames == 36 then
        local second = tostring(PCSX.createSaveState())
        local a, b = bit.band(gpustat(first), 0x9fff), bit.band(gpustat(second), 0x9fff)
        if a ~= b then
            print(string.format('GPUSTAT draw-mode bits changed across save/load: %04x -> %04x', a, b))
            PCSX.quit(3)
            return
        end
        plant(rect)
    elseif frames == 38 then
        local after = pixels()
        for i, p in ipairs(probes) do
            if after[i] ~= control[i] then
                print(string.format('drawing environment lost across save/load: (%d,%d) %04x, expected %04x',
                                    p[1], p[2], after[i], control[i]))
                PCSX.quit(4)
                return
            end
        end
        PCSX.quit(0)
    end
end)
)";

TEST(SaveStateGPU, DrawEnvironmentRoundTrips) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-exec", c_gpustatSaveState);
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// GP1(08h) is replayed after GP1(00h) on load, and GP1(00h) resets the display mode: a 480i mode set
// before the save has to be the one a second save records.
static const char c_displayModeSaveState[] = R"(
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())

local function varint(s, pos)
    local v, mul = 0, 1
    while true do
        local b = s:byte(pos)
        pos = pos + 1
        v = v + bit.band(b, 0x7f) * mul
        if b < 0x80 then return v, pos end
        mul = mul * 128
    end
end

local function field(s, wanted)
    local pos = 1
    while pos <= #s do
        local key
        key, pos = varint(s, pos)
        local num, wt = math.floor(key / 8), key % 8
        local payload
        if wt == 0 then
            payload, pos = varint(s, pos)
        elseif wt == 2 then
            local len
            len, pos = varint(s, pos)
            payload = s:sub(pos, pos + len - 1)
            pos = pos + len
        elseif wt == 1 then
            pos = pos + 8
        elseif wt == 5 then
            pos = pos + 4
        else
            error('wire type ' .. wt)
        end
        if num == wanted then return payload end
    end
end

local function displayMode(s)
    local control = field(field(s, 5), 2)
    local b1, b2, b3, b4 = control:byte(8 * 4 + 1, 8 * 4 + 4)
    return b1 + b2 * 0x100 + b3 * 0x10000 + b4 * 0x1000000
end

local function toSlice(s)
    local slice = Support.File.createEmptySlice()
    slice:resize(#s)
    ffi.copy(slice.mutable, s, #s)
    return slice
end

local mode = 0x08000027
local frames = 0
local first
DisplayModeSaveStateListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        local i = 0x100000 / 4
        ram[i] = 0x3c081f80 -- lui $t0, 0x1f80
        ram[i + 1] = 0x3c090800 -- lui $t1, 0x0800
        ram[i + 2] = 0x35290027 -- ori $t1, $t1, 0x0027
        ram[i + 3] = 0xad091814 -- sw $t1, 0x1814($t0)
        ram[i + 4] = 0x1000ffff -- b .
        ram[i + 5] = 0
        PCSX.invalidateCache()
        PCSX.pauseEmulator()
        local timer = luv.new_timer()
        timer:start(1, 0, function()
            timer:close()
            PCSX.getRegisters().pc = 0x80100000
            PCSX.resumeEmulator()
        end)
    elseif frames == 32 then
        first = tostring(PCSX.createSaveState())
        if displayMode(first) ~= mode then
            print(string.format('GP1(08h) write did not land: %08x', displayMode(first)))
            PCSX.quit(1)
            return
        end
        PCSX.loadSaveState(toSlice(first))
    elseif frames == 34 then
        local after = displayMode(tostring(PCSX.createSaveState()))
        if after ~= mode then
            print(string.format('display mode lost across save/load: %08x, expected %08x', after, mode))
            PCSX.quit(2)
            return
        end
        PCSX.quit(0)
    end
end)
)";

TEST(SaveStateGPU, DisplayModeRoundTrips) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-softgpu", "-exec", c_displayModeSaveState);
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}
