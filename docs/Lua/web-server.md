# Webserver Lua API

When the [webserver](../web_server.md) is enabled, it will expose the `/api/v1/lua/` prefix, which can be used to execute Lua code on the emulator. When an endpoint with this prefix is called, the Lua table `PCSX.WebServer.Handlers` will be inspected to find a handler for the rest of the path in the endpoint. If a handler is found, it will be called with a request object representing the query, and it has to return a string, which will be sent back to the client as the response. If no handler is found, a 404 error will be returned. If an error occurs while executing the handler, a 500 error will be returned.

The request object has the following fields:

- `body` is the raw request body as a string, empty when the request has none. A form POST with `application/x-www-form-urlencoded` arrives here, since `form` only covers `multipart/form-data`.
- `form` is a table of the part headers found in the request body. It is only filled if the content type of the request is `multipart/form-data`, and is empty otherwise. Each header name maps to an array of its values.
- `headers` is a table of the headers in the request. Each header name maps to an array of its values.
- `method` is the HTTP method of the request, as an uppercase string such as `POST`. Note that GET requests report `HTTP_GET`.
- `urlData` is a table with more information about the URL. It has the following string fields:
    - `fragment`
    - `host`
    - `path`
    - `port`
    - `query`
    - `schema`
    - `userInfo`

If the returned string starts with the characters "HTTP/", then the web server will consider the response string is a full HTTP response with headers, and will send it as-is to the client. Otherwise, the response string will be sent as the body of a normal 200 response.
