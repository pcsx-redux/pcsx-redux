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

-- Has to match the -webserver-port the runner passes.
local PORT = 34568

TestWebServer = {}

-- Sends one raw HTTP request and returns everything the server wrote back before closing.
local function exchange(raw)
    local co = coroutine.running()
    local client = luv.new_tcp()
    local reply = ''
    local done = false
    local function finish()
        if done then return end
        done = true
        client:read_stop()
        client:close(function() coroutine.resume(co) end)
    end
    local timer = luv.new_timer()
    timer:start(5000, 0, finish)
    client:connect('127.0.0.1', PORT, function(err)
        lu.assertNil(err)
        client:read_start(function(rerr, data)
            if rerr or not data then return finish() end
            reply = reply .. data
            if reply:find('\r\n\r\n', 1, true) and reply:find('END$') then finish() end
        end)
        client:write(raw)
    end)
    coroutine.yield()
    timer:stop()
    timer:close()
    return reply
end

-- The VRAM download must announce exactly as many body bytes as it sends.
function TestWebServer:test_vram_content_length_matches_body()
    local co = coroutine.running()
    local client = luv.new_tcp()
    local reply = {}
    local done = false
    local function finish()
        if done then return end
        done = true
        client:read_stop()
        client:close(function() coroutine.resume(co) end)
    end
    local timer = luv.new_timer()
    timer:start(5000, 0, finish)
    client:connect('127.0.0.1', PORT, function(err)
        lu.assertNil(err)
        client:read_start(function(rerr, data)
            if rerr or not data then return finish() end
            reply[#reply + 1] = data
        end)
        client:write('GET /api/v1/gpu/vram/raw HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n')
    end)
    coroutine.yield()
    timer:stop()
    timer:close()
    reply = table.concat(reply)
    local headerEnd = reply:find('\r\n\r\n', 1, true)
    lu.assertNotNil(headerEnd)
    local length = tonumber(reply:sub(1, headerEnd):match('[Cc]ontent%-[Ll]ength: (%d+)'))
    lu.assertNotNil(length)
    lu.assertEquals(length, #reply - (headerEnd + 3))
end

local function post(path, body)
    return exchange('POST /api/v1/lua/' .. path .. ' HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n' ..
                        'Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ' .. #body .. '\r\n\r\n' ..
                        body)
end

-- Query values that are not numbers are rejected with a 400 and must not take the emulator down.
function TestWebServer:test_non_numeric_query_is_bad_request()
    local function plain(method, path, body)
        body = body or ''
        return exchange(method .. ' ' .. path .. ' HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n' ..
                            'Content-Length: ' .. #body .. '\r\n\r\n' .. body)
    end
    local ram = plain('POST', '/api/v1/cpu/ram/raw?offset=abc&size=1', 'x')
    lu.assertEquals(ram:sub(1, 22), 'HTTP/1.1 400 Bad Reque')
    local vram = plain('POST', '/api/v1/gpu/vram/raw?x=a&y=0&width=1&height=1', 'xx')
    lu.assertEquals(vram:sub(1, 22), 'HTTP/1.1 400 Bad Reque')
    local after = plain('GET', '/api/v1/gpu/vram/raw')
    lu.assertEquals(after:sub(1, 15), 'HTTP/1.1 200 OK')
end

function TestWebServer:test_post_body_reaches_handler()
    local seen
    PCSX.WebServer.Handlers.echo = function(request)
        seen = request
        return request.method .. ':' .. request.body .. ':END'
    end
    local reply = post('echo', 'a=1&b=two%20words')
    PCSX.WebServer.Handlers.echo = nil
    lu.assertNotNil(seen)
    lu.assertEquals(seen.method, 'POST')
    lu.assertEquals(seen.body, 'a=1&b=two%20words')
    lu.assertNotNil(reply:find('POST:a=1&b=two%20words:END', 1, true))
end

-- A request with no body still gets the field, as an empty string.
function TestWebServer:test_get_has_empty_body()
    local seen
    PCSX.WebServer.Handlers.empty = function(request)
        seen = request
        return 'END'
    end
    exchange('GET /api/v1/lua/empty HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n')
    PCSX.WebServer.Handlers.empty = nil
    lu.assertNotNil(seen)
    lu.assertEquals(seen.body, '')
end

-- A request pipelined behind the first one in the same write must not be dispatched: the
-- connection closes after the first response, and the leftover request would see its body.
function TestWebServer:test_pipelined_request_not_dispatched()
    local first, second
    PCSX.WebServer.Handlers.first = function(request)
        first = request
        return 'END'
    end
    PCSX.WebServer.Handlers.second = function(request)
        second = request
        return 'END'
    end
    exchange('POST /api/v1/lua/first HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 6\r\n\r\nsecret' ..
                 'GET /api/v1/lua/second HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n')
    PCSX.WebServer.Handlers.first = nil
    PCSX.WebServer.Handlers.second = nil
    lu.assertNotNil(first)
    lu.assertEquals(first.body, 'secret')
    lu.assertNil(second and second.body)
    lu.assertNil(second)
end

-- A multipart/form-data handler gets the raw request body, boundaries and part headers included.
function TestWebServer:test_multipart_body_is_raw()
    local seen
    PCSX.WebServer.Handlers.multipart = function(request)
        seen = request
        return 'END'
    end
    local body = '--XyZ\r\nContent-Disposition: form-data; name="a"\r\n\r\n1\r\n--XyZ--\r\n'
    exchange('POST /api/v1/lua/multipart HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n' ..
                 'Content-Type: multipart/form-data; boundary=XyZ\r\nContent-Length: ' .. #body .. '\r\n\r\n' .. body)
    PCSX.WebServer.Handlers.multipart = nil
    lu.assertNotNil(seen)
    lu.assertEquals(seen.body, body)
end

-- A known endpoint that does not handle the method or function gets a 404, not a silent close.
function TestWebServer:test_unhandled_request_is_404()
    local function plain(method, path)
        return exchange(method .. ' ' .. path .. ' HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n')
    end
    lu.assertEquals(plain('DELETE', '/api/v1/cpu/cache'):sub(1, 12), 'HTTP/1.1 404')
    lu.assertEquals(plain('GET', '/api/v1/cd/nope'):sub(1, 12), 'HTTP/1.1 404')
    lu.assertEquals(plain('GET', '/api/v1/cpu/cache?function=flush'):sub(1, 12), 'HTTP/1.1 404')
    -- A handler that already answered must not get a second reply appended.
    local state = plain('GET', '/api/v1/state/usage')
    lu.assertNil(state:find('404', 1, true))
end
