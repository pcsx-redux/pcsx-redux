local support_root = path.join(os.scriptdir(), "..", "..")

includes(path.join(support_root, "tools", "xmake"))

common_rules()

-- Movie entries are read through ffmpeg, with src/support's ffmpeg-movie-file.cc:
-- the same four libraries, by the same pkg-config names, as the emulator's own
-- build asks for. Without them the tool builds ffmpeg-movie-file-none.cc in its
-- place, with PCSX_NO_FFMPEG, and does everything but encode movies.
local ffmpeg = {"pkgconfig::libavcodec", "pkgconfig::libavformat",
                "pkgconfig::libavutil", "pkgconfig::libswresample"}
add_requires(ffmpeg, {optional = true})

target("pcsx.authoring", function ()
    common_tool()
    add_includedirs(path.join(support_root, "third_party", "ucl", "include"))
    add_packages(ffmpeg)
    on_config(function (target)
        local found = true
        for _, name in ipairs(ffmpeg) do
            found = found and target:pkg(name) ~= nil
        end
        local support = path.join(support_root, "src", "support")
        if found then
            target:add("files", path.join(support, "ffmpeg-movie-file.cc"))
        else
            target:add("files", path.join(support, "ffmpeg-movie-file-none.cc"))
            target:add("defines", "PCSX_NO_FFMPEG")
        end
    end)
end)

rule("pcsx.authoring.image", function()
    authoring_image("pcsx.authoring", ".bin", "authoring")
end)
