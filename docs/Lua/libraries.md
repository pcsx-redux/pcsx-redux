# Loaded libraries

## Basic Lua
The [LuaJIT extensions](https://luajit.org/extensions.html) are fully loaded, and can be used globally. The [standard Lua libraries](https://www.lua.org/manual/5.1/manual.html#5) are loaded, and are usable. The `require` function exists, but isn't recommended as the loading of external DLLs might be difficult to properly accomplish. Loading pure Lua files is fine. The `ffi` table is loaded globally, there is no need to `require` it, but it'll work nonetheless. As a side-effect of Luv, [Lua-compat-5.3](https://github.com/keplerproject/lua-compat-5.3) is loaded.

## Dear ImGui
A good portion of [ImGui](https://github.com/ocornut/imgui) is bound to the Lua environment, and it's possible for the Lua code to emit arbitrary widgets through ImGui. It is advised to consult the [user manual](https://pthom.github.io/imgui_manual_online/manual/imgui_manual.html) of ImGui in order to properly understand how to make use of it. The list of current bindings can be found [within the source code](https://github.com/grumpycoders/pcsx-redux/blob/main/third_party/imgui_lua_bindings/imgui_iterator.inl). Some usage examples will be provided within the case studies. Additional features and interaction is documented in the [rendering](rendering.md#imgui) page.

## OpenGL
OpenGL is bound directly to the Lua API through FFI bindings, loosely inspired and adapted from [LuaJIT-OpenCL](https://github.com/malkia/luajit-opencl
). Some usage examples can be seen in [the CRT-Lottes shader configuration page](https://github.com/grumpycoders/pcsx-redux/blob/eadd59e764d526636d900fada6f3dd0057035690/src/gui/shaders/crt-lottes.cc#L141-L146).

## ThorVG
The [ThorVG](https://github.com/thorvg/thorvg) library is bound to the Lua API as the global `tvg` table, through FFI bindings of its C API, with some additional glue code. More explanation can be found in the [rendering](rendering.md#thorvg) page.

## Luv
For network access and interaction, PCSX-Redux uses [libuv](https://libuv.org/) internally, and is exposed to the Lua API through [Luv](https://github.com/luvit/luv), tho its loop is tied to the main thread one, meaning it'll run only once per frame. There is another layer of network API available through the File API, which is more convenient and faster for simple tasks.

## Zlib
The Zlib C-API is exposed through [FFI bindings](https://github.com/luapower/zlib). There is another layer of Zlib API available through the File API, which is more convenient and faster for simple tasks.

## FFI-Reflect
The [FFI-Reflect](https://github.com/corsix/ffi-reflect) library is loaded globally as the `reflect` symbol. It's able to generate reflection objects for the LuaJIT FFI module.

## PPrint
The [PPrint](https://github.com/jagt/pprint.lua) library is loaded globally as the `pprint` symbol. It's a more powerful `print` function than the one provided by Lua, and can be used to print tables in a more readable way.

## Lua-Protobuf
The [Lua-Protobuf](https://github.com/starwing/lua-protobuf) library is available, but not loaded by default. All of its documented API should be usable straight with no additional work. It has been slightly modified, but nothing that should be visible to the user. There is some limited glue between its API and PCSX's.

## luafilesystem
The [luafilesystem](https://github.com/lunarmodules/luafilesystem) library is loaded globally as the `lfs` symbol. It's a library that provides access to the filesystem.

## LPeg
The [LPeg](http://www.inf.puc-rio.br/~roberto/lpeg/) library is loaded globally as the `lpeg` symbol, which is also what `require 'lpeg'` returns. It's a library that provides a pattern-matching library for Lua, which can be useful to create ad-hoc arbitrary parsers.
