local f = assert(io.open('src/mips/monitor/hosts/retail/monitor-retail.ps-exe', 'rb'))
local exe = f:read('*a')
f:close()
local function u32(o)
    local a, b, c, d = exe:byte(o + 1, o + 4)
    return a + b * 0x100 + c * 0x10000 + d * 0x1000000
end
local tAddr, tSize = u32(0x18), u32(0x1c)
local text = exe:sub(0x801, 0x800 + tSize)
local frames = 0
DiscBootListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    local pc = PCSX.getRegisters().pc
    if pc >= tAddr and pc < tAddr + tSize then
        local ram = ffi.string(PCSX.getMemPtr() + bit.band(tAddr, 0x1fffff), tSize)
        if ram == text then PCSX.quit(0) end
    end
    if frames >= 6000 then PCSX.quit(1) end
end)
