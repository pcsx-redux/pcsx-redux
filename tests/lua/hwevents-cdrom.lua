PCSX.HWEvents.arm('CDROM', 'IRQ')
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
CDEventsListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    local booted = false
    local pc = PCSX.getRegisters().pc
    if pc >= tAddr and pc < tAddr + tSize then
        booted = ffi.string(PCSX.getMemPtr() + bit.band(tAddr, 0x1fffff), tSize) == text
    end
    if not booted and frames < 6000 then return end
    local f = io.open(CDEventsOut, 'w')
    local arr, n = PCSX.HWEvents.read()
    local commands = 0
    for i = 0, n - 1 do
        if arr[i].type == PCSX.HWEvents.Types.CDCommand then commands = commands + 1 end
    end
    f:write(string.format('%d %d', PCSX.HWEvents.mask(), commands))
    f:close()
    PCSX.quit(booted and 0 or 1)
end)
