// Hand-written equivalent of the config.h that thorvg's meson build generates.
#pragma once

#define THORVG_VERSION_STRING "1.1.2"

#define THORVG_THREAD_SUPPORT 1
#define THORVG_CPU_ENGINE_SUPPORT 1
#define THORVG_GL_ENGINE_SUPPORT 1
#define THORVG_PARTIAL_RENDER_SUPPORT 1

#define THORVG_SVG_LOADER_SUPPORT 1
#define THORVG_PNG_LOADER_SUPPORT 1
#define THORVG_JPG_LOADER_SUPPORT 1
#define THORVG_LOTTIE_LOADER_SUPPORT 1
#define THORVG_SFNT_LOADER_SUPPORT 1
#define THORVG_TTF_LOADER_SUPPORT 1
#define THORVG_OTF_LOADER_SUPPORT 1

#define THORVG_CAPI_BINDING_SUPPORT 1
#define THORVG_FILE_IO_SUPPORT 1

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

// Route ThorVG's TVGERR into the emulator's log instead of stdio; TVGLOG
// (informational) is dropped. The sink is pcsxThorVGLog, in src/gui/gui.cc.
#define THORVG_LOG_ENABLED 1
void pcsxThorVGLog(bool error, const char* tag, const char* file, int line, const char* fmt, ...);
#define TVGERR(tag, fmt, ...) pcsxThorVGLog(true, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define TVGLOG(tag, fmt, ...) pcsxThorVGLog(false, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
