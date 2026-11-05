-- LuaJIT's build is host-arch-gated (its minilua/buildvm targets are guarded on
-- x64/arm64/mips64) and its VM is per-arch assembly, so there is nothing for it
-- to do on wasm. The wasm arm uses PUC-Lua 5.4 through third_party/puc-lua-compat
-- instead - a SECOND Lua backend, not a migration off LuaJIT.
if not is_plat("wasm") then
    includes("third_party/luajit")
end

add_rules("mode.debug", "mode.release")

-- WASM PROBE (local, uncommitted): libuv is the ONLY package xmake reports as
-- unsupported on wasm/wasm32, and libav is pkgconfig-on-the-host. Both are
-- already scoped OUT of v1. Dropped here purely to let configure proceed far
-- enough to enumerate the NEXT layer of blockers.
if is_plat("wasm") then
    -- The packages have to be built with the SAME threading model as the target.
    -- -pthread implies -matomics -mbulk-memory, and wasm-ld refuses to mix:
    -- "--shared-memory is disallowed by sfnt.c.o because it was not compiled
    -- with 'atomics' or 'bulk-memory' features." freetype and SDL3 come from
    -- xmake packages built in a separate compilation that does not see the
    -- target's flags, so they must be told separately. Same shape as the
    -- exception-model constraint above: the packages pin the ABI, not us.
    -- NOT -fwasm-exceptions here, even though the target uses it. SDL3's cmake
    -- configure fails its try-compiles with that flag in CFLAGS. What the
    -- packages actually have to agree on is the setjmp/longjmp LOWERING:
    -- -fwasm-exceptions implies SUPPORT_LONGJMP=wasm, and freetype's ftbase.c /
    -- sfnt.c use setjmp, so they must be lowered the same way or the link dies
    -- on an undefined emscripten_longjmp. None of these packages throws across
    -- our boundary, so they do not need the EH model itself - only the longjmp
    -- one, which is the narrower flag.
    -- LISTS, not space-separated strings: xmake passes a string through as ONE
    -- argument and clang reports `unknown argument: '-pthread -sSUPPORT_LONGJMP=wasm'`.
    add_requireconfs("*", {configs = {
        cxflags = {"-pthread", "-sSUPPORT_LONGJMP=wasm"},
        cflags  = {"-pthread", "-sSUPPORT_LONGJMP=wasm"},
        ldflags = {"-pthread", "-sSUPPORT_LONGJMP=wasm"}}})
    add_requires("capstone", "fmt", "freetype", "libsdl3", "zlib")
else
    add_requires("capstone", "fmt", "freetype", "libcurl", "libsdl3", "libuv", "zlib")
end

-- Only four of ffmpeg's libraries are used, and asking for "ffmpeg" wholesale
-- never resolves to the system copy: that package requires all eight pkg-config
-- modules, and libpostproc is GPL-only and so isn't packaged on Ubuntu at all.
-- The result is ffmpeg getting built from source on every clean checkout. These
-- are the same four names the Makefile's PACKAGES already asks for.
if not is_plat("wasm") then
add_requires("pkgconfig::libavcodec", "pkgconfig::libavformat",
             "pkgconfig::libavutil", "pkgconfig::libswresample")
end

set_languages("c++26")

target("thorvg", function()
    set_kind("static")
    local dir = "third_party/thorvg/src/"
    add_files(
        dir .. "common/*.cpp",
        dir .. "renderer/*.cpp",
        dir .. "renderer/cpu_engine/*.cpp",
        dir .. "renderer/gpu_engine/*.cpp",
        dir .. "renderer/gpu_engine/gl/*.cpp",
        dir .. "loaders/svg/*.cpp",
        dir .. "loaders/png/*.cpp",
        dir .. "loaders/jpg/*.cpp",
        dir .. "loaders/lottie/*.cpp",
        dir .. "loaders/sfnt/*.cpp",
        dir .. "loaders/raw/*.cpp",
        dir .. "bindings/capi/tvgCapi.cpp",
        "third_party/thorvg-config/tvgGlLoader.cpp",
        nil
    )
    -- Replaced by third_party/thorvg-config/tvgGlLoader.cpp, which includes it.
    remove_files(dir .. "renderer/gpu_engine/gl/tvgGl.cpp")
    add_includedirs("third_party/thorvg-config", "third_party/thorvg/inc")
    for _, sub in ipairs({ "common", "renderer", "renderer/cpu_engine", "renderer/gpu_engine",
                           "renderer/gpu_engine/gl", "loaders/svg", "loaders/png", "loaders/jpg",
                           "loaders/lottie", "loaders/sfnt", "loaders/raw", "bindings/capi" }) do
        add_includedirs(dir .. sub)
    end
    -- WebGL2 is GLES 3.0: thorvg keys its "#version 300 es" shader header off this.
    add_defines(is_plat("wasm") and "THORVG_GL_TARGET_GLES=1" or "THORVG_GL_TARGET_GL=1")
    if is_plat("wasm") then
        -- Same flags as the pcsx-redux target: wasm-ld refuses to link
        -- non-atomics objects into a --shared-memory module.
        add_cxflags("-fwasm-exceptions", "-pthread", {force = true})
    end
    add_defines("TVG_STATIC=1", { public = true })
end)

-- FTXUI: compiled from its sources, no CMake. The *_test.cpp / *_fuzzer.cpp
-- files need gtest / libFuzzer and are not part of the library. component/loop.cpp
-- is a leftover upstream no longer builds: Loop now lives in component/app.cpp.
target("ftxui", function()
    set_kind("static")
    add_files("third_party/ftxui/src/**.cpp")
    remove_files("third_party/ftxui/src/**_test.cpp", "third_party/ftxui/src/**_fuzzer.cpp",
                 "third_party/ftxui/src/ftxui/component/loop.cpp")
    add_includedirs("third_party/ftxui/src")
    add_includedirs("third_party/ftxui/include", { public = true })
    if is_plat("windows") then
        add_defines("UNICODE", "_UNICODE")
    end
    if is_plat("wasm") then
        -- Same flags as the pcsx-redux target: wasm-ld refuses to link
        -- non-atomics objects into a --shared-memory module.
        add_cxflags("-fwasm-exceptions", "-pthread", {force = true})
    end
end)

target("pcsx-redux", function()
    add_includedirs(
        ".",
        "src",
        "third_party",
        "third_party/ELFIO",
        "third_party/gl3w",
        "third_party/imgui",
        "third_party/imgui/backends",
        "third_party/imgui/misc/cpp",
        "third_party/llhttp",
        -- WASM PROBE (local): PUC-Lua headers + a luajit.h shim must come FIRST
        -- so "lua.hpp" resolves to the compat one rather than LuaJIT's.
        (is_plat("wasm") and "third_party/puc-lua-compat" or "third_party/luajit/src"),
        "third_party/luajit/src",
        "third_party/luv/src",
        "third_party/luv/deps/lua-compat-5.3/c-api",
        "third_party/magic_enum/include",
        "third_party/md4c/src",
        "third_party/multipart-parser-c",
        "third_party/PEGTL/include",
        "third_party/tracy/public",
        "third_party/ucl",
        "third_party/ucl/include",
        "third_party/uriparser/include",
        "third_party/xbyak/xbyak",
        "third_party/zep/extensions",
        "third_party/zep/include",
        nil
    )

    -- NOT folded into the list above: a nil in the middle of a vararg call
    -- truncates it, so `(cond and dir or nil)` inline would silently drop every
    -- includedir after it on the non-wasm arm.
    if is_plat("wasm") then
        -- uv-wasm-stub declares exactly uv_loop_t / uv_run / uv_loop_init /
        -- uv_loop_close and #errors outside __EMSCRIPTEN__. That is the whole of
        -- the core's libuv surface once the three servers and uvfile are dropped.
        -- Anything wanting a real handle, timer, poll or async MUST fail to
        -- compile against it rather than get a silent no-op: a no-op in an async
        -- layer is a runtime-only failure on a build with no debugger.
        add_includedirs("third_party/uv-wasm-stub")
    end

    add_files("third_party/imgui/*.cpp", { cxxflags = "-include src/forced-includes/imgui.h" })
    add_files("third_party/implot/*.cpp", { cxxflags = "-include src/forced-includes/implot.h" })

    if is_plat("wasm") then
        add_deps("thorvg", "ftxui")
        add_packages("capstone", "fmt", "freetype", "libsdl3", "zlib")

        -- ===== THE SECOND LUA BACKEND =====
        -- PUC-Lua 5.4.7 + luaffifb + libffi, replacing LuaJIT on wasm only.
        -- Redux's own Lua corpus and its C++ host layer are unchanged; this is a
        -- build-time backend choice, not a migration off LuaJIT.
        --
        -- Four PUC-Lua sources are overridden rather than compiled from the
        -- submodule: linit.c auto-opens ffi so `require 'ffi'` works, llex.c
        -- ignores LL/ULL suffixes, and lobject.c + lvm.c make (void*)NULL == nil
        -- compare true. onelua/luac/ltests are drivers we do not want, and lua.c
        -- would give us a second main().
        local LUA_SKIP = {["linit.c"] = true, ["llex.c"] = true, ["lobject.c"] = true,
                          ["lvm.c"] = true, ["onelua.c"] = true, ["luac.c"] = true,
                          ["ltests.c"] = true, ["lua.c"] = true}
        for _, f in ipairs(os.files("third_party/lua/*.c")) do
            if not LUA_SKIP[path.filename(f)] then add_files(f) end
        end
        add_files("third_party/lua-wasm-patch/*.c")

        add_files("third_party/luaffifb/call.c", "third_party/luaffifb/ctype.c",
                  "third_party/luaffifb/ffi.c", "third_party/luaffifb/ffi_complex.c",
                  "third_party/luaffifb/lua.c", "third_party/luaffifb/parser.c")

        -- libffi's configured output is not relocatable, which is why lua-ffi-wasm
        -- compiles these eight directly rather than linking a built library. There
        -- is no wasm arm in xmake-repo's libffi package either (checked: aui, botan
        -- and chipmunk2d do carry wasm arms, so that absence is real).
        for _, f in ipairs({"prep_cif", "types", "raw_api", "java_raw_api",
                            "closures", "tramp", "debug", "wasm/ffi"}) do
            add_files("third_party/libffi/src/" .. f .. ".c")
        end

        add_includedirs(
            "third_party/lua",                  -- pristine PUC-Lua 5.4.7 headers
            "third_party/luaffifb",             -- so "luaffifb/ffi.h" resolves
            "third_party/libffi-wasm-config",   -- fficonfig.h, configure-generated
                                                -- and absent upstream; MUST precede
                                                -- the submodule's own include dir
            "third_party/libffi/src/wasm",      -- the wasm ffitarget.h, which is
                                                -- pristine - the x86 one is the
                                                -- wrongly-autodetected header, so
                                                -- ORDER is the whole correction
            "third_party/libffi/include",
            "third_party/libffi/src")
        -- CALL_WITH_LIBFFI swaps luaffifb's DynASM call path, which cannot target
        -- wasm, for libffi's signature-keyed dispatch.
        add_defines("CALL_WITH_LIBFFI", "LUA_COMPAT_5_3", "PCSX_GL3W_TRACE_MISSING")
        -- The link had NO memory settings at all, so it took emscripten's
        -- defaults: a 16 MB heap with growth OFF. Redux is past that before it
        -- draws a frame - imgui plus freetype's atlas plus the 2 MB PSX RAM, the
        -- 8 MB expansion and the read/write LUTs - so it aborted at startup.
        -- MSAN's 1.5 GB is NOT part of this: it is lazily calloc'd in msanInit
        -- and never touched unless the user turns it on.
        --
        -- INITIAL_MEMORY is deliberately generous rather than minimal: with
        -- growth enabled every expansion copies the whole heap, and there is no
        -- reason to pay that repeatedly during startup.
        -- C++ EXCEPTIONS ARE REQUIRED, and this is NOT what v1 scoped out.
        -- The v1 note says "drop C++ exception support", but that was about
        -- Support.extra.safeFFI - the pcall net catching C++ exceptions crossing
        -- the LUA barrier. Redux's own C++ throws internally regardless;
        -- gl3w-throwers.cc exists to do exactly that.
        --
        -- NATIVE WASM EH, which only became available once the packages were
        -- being rebuilt anyway. The first attempt used the JS-based model,
        -- because -fwasm-exceptions forces SUPPORT_LONGJMP=wasm and freetype's
        -- ftbase.c/sfnt.c came prebuilt with JS-lowered setjmp, so the link died
        -- on an undefined emscripten_longjmp. -pthread forced those packages to
        -- be rebuilt regardless, so the constraint that pinned the exception
        -- model is gone. Native EH drops the invoke_* JS trampolines around
        -- every try block.
        add_cxflags("-fwasm-exceptions", "-pthread", {force = true})
        add_ldflags("-fwasm-exceptions",
                    "-pthread", "-sPTHREAD_POOL_SIZE=8", {force = true})
    -- PROXY_TO_PTHREAD is deliberately GONE. It moved main() onto a worker so
    -- that the emulation loop could block, and that bought a loop which never
    -- returns to the browser - so nothing paints or handles input between
    -- frames, SDL_GL_SetSwapInterval logs "a main loop does not exist" because
    -- there genuinely is none, and emscripten_webgl_commit_frame has to exist
    -- to present at all.
    --
    -- It is also incompatible with the fix: emscripten's main loop runner does
    -- per-frame GL bookkeeping against GL.currentContext on the thread it runs
    -- on, so with the context parked on a worker it dies in
    -- GL.newRenderingFrameStarted. The two designs want opposite things.
    --
    -- What actually blocked was ONE thing, and it was not the joins the old
    -- comment here claimed: SPU::waitForGoal, called from psxcounters inside
    -- Execute(). The joins are all in shutdown paths, which a browser tab never
    -- runs. -pthread and the worker pool stay; only main() moves.
        add_ldflags("-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=512MB",
                    "-sMAXIMUM_MEMORY=4GB",
                    -- 64 kB by default in current emscripten, which nothing in a
                    -- C++ UI with recursive parsers survives.
                    "-sSTACK_SIZE=8MB",
                    -- ASSERTIONS is what turned the first abort from "OOM, build
                    -- with -sASSERTIONS" into something readable. Keep it until
                    -- the thing renders; it is a debug build either way.
                    "-sASSERTIONS=1",
                    -- -g2 = wasm NAME SECTION only, so a RuntimeError stack
                    -- shows function names instead of wasm-function[6585].
                    -- NOT -g: full DWARF crashes wasm-opt at -O3 with an
                    -- assertion in wasm-debug.cpp AddrExprMap::add, and it fails
                    -- AFTER writing a zero-byte .wasm, so an mtime check alone
                    -- reads it as a successful build.
                    "-g2",
                    "-sEXIT_RUNTIME=0",
                    -- gl3w and imgui's GL3 backend both want real ES3/WebGL2.
                    "-sMIN_WEBGL_VERSION=2", "-sMAX_WEBGL_VERSION=2", "-sFULL_ES3=1",
                    -- HISTORY, and the premise under it is GONE as of the commit
                    -- that moved main() back to the browser's main thread. Both
                    -- flags below were added to survive PROXY_TO_PTHREAD, and
                    -- whether either is still needed is UNMEASURED - the test is
                    -- one build with both dropped, then look at the canvas.
                    -- Left in place because they are known to work, not because
                    -- they are known to be required.
                    --
                    -- PROXY_TO_PTHREAD put main() on a worker, and a worker has
                    -- no DOM: SDL_GL_CreateContext failed with "Could not create
                    -- webgl context" because the canvas lives on the browser
                    -- main thread. Emscripten offers two ways out, and the
                    -- obvious one does not work here.
                    --
                    -- NOT -sOFFSCREENCANVAS_SUPPORT. It does fix context
                    -- creation - the canvas is transferred to the pthread and
                    -- SDL comes up clean - but a transferred OffscreenCanvas is
                    -- composited only when the owning worker's TASK ENDS, and
                    -- our main loop never returns. Measured with a four-arm
                    -- oracle: 208,800 glClear(magenta) frames left the canvas
                    -- BLACK, while the same program with one `return` after 300
                    -- frames came up magenta. emscripten_webgl_commit_frame()
                    -- cannot rescue it either - it returns -3 in implicit-swap
                    -- mode, and libhtml5_webgl.js says outright that it is a
                    -- no-op because browsers removed OffscreenCanvas.commit().
                    --
                    -- OFFSCREEN_FRAMEBUFFER is the arm that works: the canvas
                    -- stays on the browser main thread, the pthread's GL calls
                    -- are proxied to it, and commit_frame blits an offscreen
                    -- backbuffer to the real canvas - on a thread whose event
                    -- loop IS turning. Same blocking loop, canvas comes up.
                    -- GL_SUPPORT_EXPLICIT_SWAP_CONTROL is what compiles
                    -- commit_frame in; without it the call is inert.
                    -- endFrame still calls it explicitly. That call was written
                    -- when there was no main loop for emscripten to hang its
                    -- automatic swap hook on; there is one now, so the explicit
                    -- call may be redundant or may be presenting twice. Neither
                    -- has been measured. Same experiment as above.
                    "-sOFFSCREEN_FRAMEBUFFER=1",
                    "-sGL_SUPPORT_EXPLICIT_SWAP_CONTROL=1",
                    -- Our own shell rather than emscripten's, for exactly one
                    -- reason today: it loads enable-threads.js first. Editing
                    -- the generated pcsx-redux.html instead would work until
                    -- the next build overwrote it.
                    "--shell-file", "src/gui/shell-wasm.html",
                    "-Wl,--error-limit=0", {force = true})
    -- The virtual filesystem. findResource() walks seven candidate paths off
    -- m_binDir, and on wasm getExecutablePath() returns an empty string, so
    -- every one of them is relative to the MEMFS root. The second probe is
    -- <releasePath>/<name>, which is why laying the bundle out as /resources
    -- and /fonts needs no change to findResource itself.
    --
    -- --preload-file rather than --embed-file: embedding bakes the bytes into
    -- the .wasm, which is the one file the browser has to compile before
    -- anything runs. A preload lands in a sibling .data that fetches in
    -- parallel with it.
    --
    -- The CJK faces are deliberately NOT here. NotoSansCJKjp-Regular.otf is
    -- 16 MB by itself, more than the emulator, and gui.cc already treats a
    -- failed load as "no Japanese in the memory card manager" rather than an
    -- error. It belongs behind an on-demand fetch, not in the boot path.
    --
    -- The `=` form is load-bearing: xmake deduplicates identical ldflag
    -- tokens, so four bare `--preload-file` strings collapse into one and the
    -- three orphaned paths get handed to em++ as input files. That fails with
    -- "expected to be an input file", naming the second path, which reads
    -- like a missing file rather than a build-system quirk.
    add_ldflags("--preload-file=src/mips/openbios/openbios.bin@/resources/openbios.bin",
                "--preload-file=resources/pcsx-redux.ico@/resources/pcsx-redux.ico",
                "--preload-file=third_party/noto/NotoSans-Regular.ttf@/fonts/NotoSans-Regular.ttf",
                "--preload-file=third_party/noto/NotoMono-Regular.ttf@/fonts/NotoMono-Regular.ttf",
                "--preload-file=$(buildir)/version.json@/version.json",
                {force = true})
    else
    add_deps("luajit", "thorvg", "ftxui")
    add_packages("capstone", "fmt", "freetype", "libcurl", "libsdl3", "libuv", "zlib",
                 "pkgconfig::libavcodec", "pkgconfig::libavformat",
                 "pkgconfig::libavutil", "pkgconfig::libswresample")
    end
    add_files(
        "src/**/*.cc",
        "third_party/cq/reclaimer.cc",
        "third_party/clip/clip.cpp",
        "third_party/clip/image.cpp",
        "third_party/cueparser/*.c",
        "third_party/gl3w/GL/gl3w.c",
        "third_party/gl3w/GL/gl3w-throwers.cc",
        "third_party/iec-60908b/*.c",
        "third_party/ImFileDialog/ImFileDialog.cpp",
        "third_party/imgui/backends/imgui_impl_opengl3.cpp",
        "third_party/imgui/backends/imgui_impl_sdl3.cpp",
        "third_party/imgui/misc/cpp/imgui_stdlib.cpp",
        "third_party/imgui/misc/freetype/imgui_freetype.cpp",
        "third_party/imgui_lua_bindings/imgui_lua_bindings.cpp",
        "third_party/imgui_md/imgui_md.cpp",
        "third_party/imgui_memory_editor/imgui_memory_editor.cpp",
        "third_party/llhttp/*.c",
        "third_party/lpeg/*.c",
        "third_party/lua-protobuf/pb.c",
        "third_party/luafilesystem/src/lfs.c",
        "third_party/md4c/src/md4c.c",
        "third_party/multipart-parser-c/multipart_parser.c",
        "third_party/ucl/src/n2e_99.c",
        "third_party/ucl/src/n2e_ds.c",
        "third_party/ucl/src/alloc.c",
        "third_party/uriparser/src/*.c",
        "third_party/zep/extensions/repl/mode_repl.cpp",
        "third_party/zep/src/*.cpp",
        "third_party/zep/src/mcommon/animation/timer.cpp",
        "third_party/zep/src/mcommon/file/path.cpp",
        "third_party/zep/src/mcommon/string/stringutils.cpp",
        nil
    )
    -- src/mips is the nugget submodule, built by its own toolchain.
    remove_files("src/mips/**")
    -- The PocketStation core's own tests and benchmarks are standalone programs.
    remove_files("src/pocketstation/tests/**")

    -- luv is the Lua libuv binding and v1 wasm has no libuv. It is the ONE
    -- exception to putting the guard in the source: luvit/luv is an upstream
    -- submodule we do not control, so a whole-file #ifdef there would mean
    -- forking a third repository.
    --
    -- ITS OWN BLOCK, NOT AN INLINE CONDITIONAL IN THE LIST ABOVE. I put
    -- `(not is_plat("wasm")) and "..." or nil` in that list and it evaluated to a
    -- bare nil on wasm, which TRUNCATES a vararg call - silently dropping zep,
    -- nanovg, ucl, uriparser and everything else after it. 159 undefined symbols
    -- and 3661 errors, from a footgun documented in a comment forty lines up in
    -- this same file.
    if not is_plat("wasm") then
        add_files("third_party/luv/src/luv.c")
    end

    -- v1 wasm scope is expressed in the SOURCES, not here. Every file below that
    -- belongs to a dropped feature wraps its whole body in one #ifdef, the way
    -- sharedmem-unix.cc / sharedmem-windows.cc and version-linux.cc already do,
    -- and the specialised wasm implementations sit beside them: binpath-wasm.cc,
    -- version-wasm.cc, DynaRec_none/recompiler.cc, third_party/clip/clip_wasm.cpp.
    -- So src/**/*.cc stays a full glob on every platform and nothing is removed.

    add_defines(
        "IMGUI_IMPL_OPENGL_LOADER_GL3W",
        "IMGUI_ENABLE_FREETYPE",
        "PB_STATIC_API",
        "ZEP_FEATURE_CPP_FILE_SYSTEM",
        nil
    )

    if is_plat("macosx") then
        add_files("src/main/complain.mm", "third_party/clip/clip_osx.mm")
        add_frameworks("GLUT", "OpenGL", "CoreFoundation", "Cocoa")
    elseif is_plat("wasm") then
        -- A browser has no X11 and no synchronous host clipboard, so clip_wasm
        -- is a process-local board: copy and paste work inside the app, nothing
        -- crosses to the host. GL comes from emscripten's WebGL2 shim at link.
        add_files("third_party/clip/clip_wasm.cpp")
    else
        add_files("third_party/clip/clip_x11.cpp")
        add_ldflags("-lstdc++fs", "-lGL", "-lX11", "-lxcb")
    end

    -- enable-threads.js has to be a real same-origin file next to the page: a
    -- service worker cannot be inlined, preloaded into the virtual filesystem,
    -- or served cross-origin. So the deployable set is five files - .html,
    -- .js, .wasm, .data and this one - and this is the one that decides
    -- whether any of the others run at all.
    if is_plat("wasm") then
        -- OpenBIOS is a freestanding MIPS binary with its own toolchain and
        -- its own make chain, and it is not getting an xmake target. Build it
        -- here anyway so that a single `xmake -p wasm` yields a complete,
        -- deployable artifact instead of one that silently falls back to the
        -- built-in BIOS stub because nobody ran a second command.
        --
        -- No guard on the toolchain being present: if mipsel-none-elf-gcc is
        -- missing this must fail the build loudly. A missing openbios.bin
        -- would otherwise surface as emcc failing to package a path, or worse,
        -- as a working page with no BIOS.
        before_build(function(target)
            import("core.project.config")
            os.execv("make", {"-C", "src/mips/openbios", "-j" .. tostring(os.default_njob()), "all"})
            -- setBinDir() reads version.json off the MEMFS root, which is what
            -- the About box shows. Same fields as the CI-generated one, minus
            -- buildId and updateInfo: the page updates by being redeployed.
            local changeset = try { function() return os.iorunv("git", {"rev-parse", "HEAD"}) end }
            changeset = changeset and changeset:trim() or "unknown"
            io.writefile(path.join(config.buildir(), "version.json"), format(
                '{\n  "version": "%s",\n  "changeset": "%s",\n  "timestamp": %d,\n  "channel": "web"\n}\n',
                changeset:sub(1, 8), changeset, os.time()))
        end)
        after_build(function(target)
            os.cp("third_party/coi-serviceworker/enable-threads.js",
                  path.directory(target:targetfile()))
        end)
    end

end)
