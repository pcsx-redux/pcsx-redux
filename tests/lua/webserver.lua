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

local function post(path, body)
    return exchange(
        'POST /api/v1/lua/' .. path .. ' HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n' ..
        'Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ' .. #body .. '\r\n\r\n' .. body)
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
