PCSX.HWEvents.arm('IRQ', 'DMA')
HWEventsSaveQuit = PCSX.Events.createEventListener('Quitting', function()
    local before, n = PCSX.HWEvents.read()
    local bytes = ffi.string(before, n * ffi.sizeof('PCSX_HWEvent'))
    local wrote = PCSX.HWEvents.save(saved)
    PCSX.HWEvents.clear()
    local loaded = PCSX.HWEvents.load(saved)
    local after, m = PCSX.HWEvents.read()
    local same = m == n and ffi.string(after, m * ffi.sizeof('PCSX_HWEvent')) == bytes
    local f = io.open(saved, 'rb')
    local file = f:read('*a')
    f:close()
    f = io.open(bad, 'wb')
    f:write('PCSXTRC' .. file:sub(8))
    f:close()
    f = io.open(cut, 'wb')
    f:write(file:sub(1, #file - 16))
    f:close()
    local badRefused = not PCSX.HWEvents.load(bad) and PCSX.HWEvents.size() == 0
    PCSX.HWEvents.load(saved)
    local cutRefused = not PCSX.HWEvents.load(cut) and PCSX.HWEvents.size() == 0
    -- A header that stops before the count field.
    f = io.open(cut, 'wb')
    f:write(file:sub(1, 16))
    f:close()
    PCSX.HWEvents.load(saved)
    cutRefused = cutRefused and not PCSX.HWEvents.load(cut) and PCSX.HWEvents.size() == 0
    f = io.open(out, 'w')
    f:write(string.format('%d %d %s %s %s %s %s', n, #file, tostring(wrote), tostring(loaded), tostring(same),
        tostring(badRefused), tostring(cutRefused)))
    f:close()
end)
