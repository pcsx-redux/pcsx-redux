local support_root = path.join(os.scriptdir(), '..', '..')

add_rules('mode.debug', 'mode.release')

add_requires('fmt', 'zlib')
set_languages('c++26')

target('pcsx.support', function()
    set_kind('static')

    if is_plat('windows') then add_cxflags('/bigobj') end

    add_packages('fmt', 'zlib')
    add_includedirs(path.join(support_root, 'src'), path.join(support_root, 'third_party'),
                    path.join(support_root, 'third_party', 'PEGTL', 'include'))

    add_files('*.cc')
    remove_files('ffmpeg-audio-file.cc', 'network.cc', 'uvfile.cc', 'version*.cc')
end)
