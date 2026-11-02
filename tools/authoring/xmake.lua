local support_root = path.join(os.scriptdir(), "..", "..")

includes(path.join(support_root, "tools", "xmake"))

common_rules()

-- Movie entries are read through ffmpeg: the same four libraries, by the same
-- pkg-config names, as the emulator's own build asks for. Without them the tool
-- is built with AUTHORING_NO_FFMPEG, and does everything but encode movies.
local ffmpeg = {"pkgconfig::libavcodec", "pkgconfig::libavformat",
                "pkgconfig::libavutil", "pkgconfig::libswresample"}
add_requires(ffmpeg, {optional = true})

target("pcsx.authoring", function ()
    common_tool()
    add_includedirs(path.join(support_root, "third_party", "ucl", "include"))
    add_packages(ffmpeg)
    on_config(function (target)
        for _, name in ipairs(ffmpeg) do
            if not target:pkg(name) then
                target:add("defines", "AUTHORING_NO_FFMPEG")
                return
            end
        end
    end)
end)

rule("pcsx.authoring.image", function()
    authoring_image("pcsx.authoring", ".bin", "authoring")
end)
