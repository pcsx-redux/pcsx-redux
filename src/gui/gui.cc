/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

// These need to go first
#include "support/windowswrapper.h"
#ifdef _WIN32
#include <shellapi.h>
#endif

// And only then we can load the rest
#define IMGUI_DEFINE_MATH_OPERATORS
#include <GL/gl3w.h>
#include <SDL3/SDL.h>
#include <assert.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <magic_enum/magic_enum_all.hpp>
#include <numbers>
#include <type_traits>
#include <unordered_set>

#include "clip/clip.h"
#include "core/callstacks.h"
#include "core/cdrom.h"
#include "core/cdromlogger.h"
#include "core/debug.h"
#include "core/gpu.h"
#include "core/gpulogger.h"
#include "core/pad.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/ramlogger.h"
#include "core/screenshot.h"
#include "core/sio1.h"
#include "core/sstate.h"
#include "fmt/chrono.h"
#include "gui/gui-platform.h"
#include "gui/gui.h"
#include "gui/luaimguiextra.h"
#include "gui/luaimplot.h"
#include "gui/luatvg.h"
#include "gui/resources.h"
#include "gui/shaders/crt-lottes.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "implot/implot.h"
#include "implot/implot_internal.h"
#include "json.hpp"
#include "lua/extra.h"
#include "lua/glffi.h"
#include "lua/luafile.h"
#include "lua/luawrapper.h"
#include "spu/interface.h"
#include "support/bezier.h"
#include "support/mem4g.h"
#include "support/uvfile.h"
#include "support/zfile.h"
#include "supportpsx/binloader.h"
#include "thorvg/inc/thorvg.h"
#include "tracy/Tracy.hpp"

unsigned PCSX::GUI::MarkDown::m_id = 0;

PCSX::GUI::MarkDown::MarkDown(GUI* gui) : m_gui(gui) {}

PCSX::GUI::MarkDown::MarkDown(GUI* gui, std::map<std::string_view, std::function<void()>>&& customURLs)
    : m_customURLs(std::move(customURLs)), m_gui(gui) {}

int PCSX::GUI::MarkDown::print(const std::string_view text) {
    const char* ptr = text.data();
    const char* end = ptr + text.size();
    return imgui_md::print(ptr, end);
}

void PCSX::GUI::MarkDown::open_url() const {
    if (m_href.starts_with("http")) {
        openUrl(m_href);
        return;
    }
    auto i = m_customURLs.find(m_href);
    if (i != m_customURLs.end()) i->second();
}

bool PCSX::GUI::MarkDown::get_image(image_info& nfo) const { return false; }

void PCSX::GUI::MarkDown::BLOCK_CODE(const MD_BLOCK_CODE_DETAIL* d, bool e) {
    imgui_md::BLOCK_CODE(d, e);
    if (e) {
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 5.0f);
        auto color = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushID(m_id++);
        ImGui::BeginChild("pre", ImVec2(-FLT_MIN, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        m_gui->useMonoFont();
    } else {
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
}

void PCSX::GUI::MarkDown::SPAN_CODE(bool e) {
    imgui_md::SPAN_CODE(e);
    if (e) {
        auto color = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        m_gui->useMonoFont();
    } else {
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
}

#ifdef _WIN32
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
}

void PCSX::GUI::openUrl(std::string_view url) {
    std::string storage = std::string(url);
    ShellExecuteA(0, 0, storage.c_str(), 0, 0, SW_SHOW);
}
#elif defined(__APPLE__) && defined(__MACH__)
#include <stdlib.h>
void PCSX::GUI::openUrl(std::string_view url) {
    auto cmd = fmt::format("open {}", url);
    system(cmd.c_str());
}
#else
#include <stdlib.h>
void PCSX::GUI::openUrl(std::string_view url) {
    auto cmd = fmt::format("xdg-open {}", url);
    system(cmd.c_str());
}
#endif

PCSX::GUI::GUI(std::vector<std::string>& favorites)
    : m_listener(g_system->m_eventBus),
      m_typedDebugger(settings.get<ShowTypedDebugger>().value, favorites),
      m_memcardManager(settings.get<ShowMemcardManager>().value, favorites),
      m_assembly(settings.get<ShowAssembly>().value, favorites),
      m_cpuTrace(settings.get<ShowCpuTrace>().value, favorites),
      m_openIsoFileDialog(l_("Open Disk Image"), favorites),
      m_openBinaryDialog(l_("Open Binary"), favorites),
      m_openArchiveDialog(l_("Open Archive"), favorites),
      m_selectBiosDialog(l_("Select BIOS"), favorites),
      m_selectEXP1Dialog(l_("Select EXP1"), favorites),
      m_isoBrowser(
          settings.get<ShowIsoBrowser>().value, favorites, [this]() { useMonoFont(); },
          [this](const std::string& name, IO<File> file) { m_dalos.addSource(name, file); }),
      m_pioCart(settings.get<ShowPIOCartConfig>().value, favorites),
      m_gpuDump(settings.get<ShowGPUDump>().value, favorites),
      m_spuLoggerWidget(settings.get<ShowSPULogger>().value, settings.get<SPULoggerEnabled>().value, favorites),
      m_spuSamplesWidget(settings.get<ShowSPUSamples>().value, favorites) {
    assert(g_gui == nullptr);
    g_gui = this;
}

using json = nlohmann::json;

static std::function<void(const char*)> s_imguiUserErrorFunctor = nullptr;
static void thrower(const char* msg) { throw std::runtime_error(msg); }
extern "C" void pcsxStaticImguiUserError(const char* msg) {
    if (s_imguiUserErrorFunctor) {
        s_imguiUserErrorFunctor(msg);
    } else {
        thrower(msg);
    }
}

extern "C" void pcsxStaticImguiAssert(int exp, const char* msg) {
    if (!exp) thrower(msg);
}

static SDL_Window* getSDLWindowFromImGuiViewport(ImGuiViewport* viewport) {
    // imgui_impl_sdl3 stores the SDL_Window* directly in PlatformHandle.
    return static_cast<SDL_Window*>(viewport->PlatformHandle);
}

PCSX::GUI* PCSX::g_gui = nullptr;

// Sink for ThorVG's TVGERR / TVGLOG, wired in third_party/thorvg-config/config.h.
void pcsxThorVGLog(bool error, const char* tag, const char* file, int line, const char* fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int len = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string buffer(len > 0 ? len : 0, '\0');
    if (len > 0) vsnprintf(buffer.data(), len + 1, fmt, args);
    va_end(args);
    if (!PCSX::g_system) {
        fprintf(stderr, "[ThorVG %s] %s (%s:%d): %s\n", error ? "error" : "log", tag, file, line, buffer.c_str());
        return;
    }
    auto base = std::filesystem::path(file).filename().string();
    PCSX::g_system->log(PCSX::LogClass::UI, "[ThorVG %s] %s (%s:%d): %s\n", error ? "error" : "log", tag, base, line,
                        buffer);
}

void PCSX::GUI::setFullscreen(bool fullscreen) {
    m_fullscreen = fullscreen;
    if (fullscreen) {
        // Snapshot windowed pos/size before the transition so we can restore them.
        SDL_GetWindowPosition(m_window, &m_windowPosX, &m_windowPosY);
        SDL_GetWindowSize(m_window, &m_windowSizeX, &m_windowSizeY);
        // No fullscreen mode set -> SDL gives us borderless desktop fullscreen,
        // which is the shape glfwSetWindowMonitor(..., GLFW_DONT_CARE) produced.
        SDL_SetWindowFullscreenMode(m_window, nullptr);
        SDL_SetWindowFullscreen(m_window, true);
    } else {
        SDL_SetWindowFullscreen(m_window, false);
        SDL_SetWindowPosition(m_window, m_windowPosX, m_windowPosY);
        SDL_SetWindowSize(m_window, m_windowSizeX, m_windowSizeY);
    }
}

void PCSX::GUI::setRawMouseMotion() {
    // SDL3 relative-mouse-mode covers cursor-hide + raw motion in one call,
    // collapsing GLFW's two-step (CURSOR_DISABLED + RAW_MOUSE_MOTION) flow.
    SDL_SetWindowRelativeMouseMode(m_window, isRawMouseMotionEnabled());
}

void LoadImguiBindings(lua_State* lState);

ImFont* PCSX::GUI::loadFont(const PCSX::u8string& name, int size, ImGuiIO& io, bool combine) {
    decltype(s_imguiUserErrorFunctor) backup = [](const char*) {};
    std::swap(backup, s_imguiUserErrorFunctor);
    ImFontConfig cfg;
    cfg.MergeMode = combine;
    cfg.Flags |= ImFontFlags_NoLoadError;
    ImFont* ret = nullptr;
    std::filesystem::path path = name;
    g_system->findResource(
        [&ret, fonts = io.Fonts, size, &cfg](const std::filesystem::path& filename) mutable {
            ret = fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(filename.u8string().c_str()), size, &cfg);
            return ret != nullptr;
        },
        path, "fonts", std::filesystem::path("third_party") / "noto");
    std::swap(backup, s_imguiUserErrorFunctor);
    return ret;
}

void PCSX::GUI::glErrorCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message) {
    const auto severityFilter = g_emulator->settings.get<Emulator::SettingGLErrorReportingSeverity>().value;
    switch (severity) {
        case GL_DEBUG_SEVERITY_MEDIUM:
            if (severityFilter >= 3) return;
            break;
        case GL_DEBUG_SEVERITY_LOW:
            if (severityFilter >= 2) return;
            break;
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            if (severityFilter >= 1) return;
            break;
    }
    static auto sourceToString = [](GLenum source) -> const char* {
        switch (source) {
            case GL_DEBUG_SOURCE_API:
                return "API";
            case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
                return "Window System";
            case GL_DEBUG_SOURCE_SHADER_COMPILER:
                return "Shader Compiler";
            case GL_DEBUG_SOURCE_THIRD_PARTY:
                return "Third Party";
            case GL_DEBUG_SOURCE_APPLICATION:
                return "Application";
            case GL_DEBUG_SOURCE_OTHER:
                return "Other";
            default:
                return "Unknown";
        }
    };

    static auto typeToString = [](GLenum type) -> const char* {
        switch (type) {
            case GL_DEBUG_TYPE_ERROR:
                return "Error";
            case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
                return "Deprecated Behavior";
            case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
                return "Undefined Behavior";
            case GL_DEBUG_TYPE_PORTABILITY:
                return "Portability";
            case GL_DEBUG_TYPE_PERFORMANCE:
                return "Performance";
            case GL_DEBUG_TYPE_MARKER:
                return "Marker";
            case GL_DEBUG_TYPE_PUSH_GROUP:
                return "Push Group";
            case GL_DEBUG_TYPE_POP_GROUP:
                return "Pop Group";
            case GL_DEBUG_TYPE_OTHER:
                return "Other";
            default:
                return "Unknown";
        }
    };
    static auto severityToString = [](GLenum severity) -> const char* {
        switch (severity) {
            case GL_DEBUG_SEVERITY_HIGH:
                return "High";
            case GL_DEBUG_SEVERITY_MEDIUM:
                return "Medium";
            case GL_DEBUG_SEVERITY_LOW:
                return "Low";
            case GL_DEBUG_SEVERITY_NOTIFICATION:
                return "Notification";
            default:
                return "Unknown";
        }
    };
    std::string fullmessage =
        fmt::format("Got OpenGL callback from \"{}\", type \"{}\", severity {}: {}\n", sourceToString(source),
                    typeToString(type), severityToString(severity), message);
    if (m_onlyLogGLErrors) {
        m_glErrors.push_back(std::move(fullmessage));
    } else {
#ifdef _WIN32
        if (IsDebuggerPresent()) __debugbreak();
#endif
        g_system->log(LogClass::UI, std::move(fullmessage));
        if (type == GL_DEBUG_TYPE_ERROR) throw std::runtime_error("Got an OpenGL error");
    }
}

void PCSX::GUI::setLua(Lua L) {
    setLuaCommon(L);
    LoadImguiBindings(L.getState());
    LuaFFI::open_imguiextra(this, L);
    LuaFFI::open_implot(L);
    LuaFFI::open_gl(L);
    LuaFFI::open_tvg(this, L);
    {
        static int lualoader = 1;
        static const char* guiextra = (
#include "gui/extra.lua"
        );
        L.load(guiextra, "src:gui/extra.lua");
    }
    // Optional Lua helpers shipped as resources. Missing or broken ones are
    // only logged; the widgets using them check they exist before calling.
    g_system->findResource(
        [&L](const std::filesystem::path& filename) {
            IO<File> file(new PosixFile(filename));
            if (file->failed()) return false;
            try {
                L.load(file->readString(file->size()), "resources:fileviewers.lua");
            } catch (...) {
                g_system->log(LogClass::UI, "Unable to load %s\n", filename.string());
            }
            return true;
        },
        "fileviewers.lua", "resources", "resources");
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.getfieldtable("settings");
    L.push("gui");
    settings.pushValue(L);
    L.settable();
    L.pop();
    auto setText = [this](Lua L, Widgets::ShaderEditor* editor,
                          void (Widgets::ShaderEditor::*setText)(std::string_view)) -> int {
        if (L.gettop() != 1) {
            return L.error(_("One argument needed to the setText* functions"));
        }
        std::optional<std::string> text;
        if (L.istable(-1)) {
            L.getfield("_type");
            if (L.tostring(-1) == "File") {
                L.pop();
                L.getfield("_wrapper");
                auto wrapper = *L.topointer<LuaFFI::LuaFile*>(-1);
                text = wrapper->file->readStringAt(wrapper->file->size(), 0);
            }
            L.pop();
        }
        if (!text.has_value()) {
            text = L.tostring(-1);
        }
        if (!text.has_value()) {
            return L.error(
                _("The argument to the setText* functions need to be convertible to a string, or be a File object"));
        }
        (editor->*setText)(text.value());
        auto status = editor->compile(this);
        if (!status.isOk()) {
            return L.error(fmt::format(f_("Error compiling new shader code: {}"), status.getError()));
        }
        return 0;
    };
    L.getfieldtable("GUI");
    L.getfieldtable("OffscreenShader");
    L.declareFunc(
        "setDefaults",
        [this](Lua L) -> int {
            m_offscreenShaderEditor.setDefaults();
            auto status = m_offscreenShaderEditor.compile(this);
            if (!status.isOk()) {
                m_offscreenShaderEditor.setFallbacks();
                m_offscreenShaderEditor.compile(this);
            }
            return 0;
        },
        -1);
    L.declareFunc(
        "setTextVS",
        [this, setText](Lua L) { return setText(L, &m_offscreenShaderEditor, &Widgets::ShaderEditor::setTextVS); }, -1);
    L.declareFunc(
        "setTextPS",
        [this, setText](Lua L) { return setText(L, &m_offscreenShaderEditor, &Widgets::ShaderEditor::setTextPS); }, -1);
    L.declareFunc(
        "setTextL",
        [this, setText](Lua L) { return setText(L, &m_offscreenShaderEditor, &Widgets::ShaderEditor::setTextL); }, -1);
    L.pop();
    L.getfieldtable("OutputShader");
    L.declareFunc(
        "setDefaults",
        [this](Lua L) -> int {
            m_outputShaderEditor.setDefaults();
            auto status = m_outputShaderEditor.compile(this);
            if (!status.isOk()) {
                m_outputShaderEditor.setFallbacks();
                m_outputShaderEditor.compile(this);
            }
            return 0;
        },
        -1);
    L.declareFunc(
        "setTextVS",
        [this, setText](Lua L) { return setText(L, &m_outputShaderEditor, &Widgets::ShaderEditor::setTextVS); }, -1);
    L.declareFunc(
        "setTextPS",
        [this, setText](Lua L) { return setText(L, &m_outputShaderEditor, &Widgets::ShaderEditor::setTextPS); }, -1);
    L.declareFunc(
        "setTextL",
        [this, setText](Lua L) { return setText(L, &m_outputShaderEditor, &Widgets::ShaderEditor::setTextL); }, -1);
    L.pop();
    L.pop();
    L.pop();
    auto offscreenStatus = m_offscreenShaderEditor.compile(this);
    auto outputStatus = m_outputShaderEditor.compile(this);
    if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
        m_offscreenShaderEditor.setFallbacks();
        m_outputShaderEditor.setFallbacks();
        offscreenStatus = m_offscreenShaderEditor.compile(this);
        outputStatus = m_outputShaderEditor.compile(this);
    }
    if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
        g_system->log(LogClass::UI, "Failed to compile output shaders. You won't be able to see anything");
        g_system->log(LogClass::UI, "%s", offscreenStatus.getError());
        g_system->log(LogClass::UI, "%s", outputStatus.getError());
    }
}

void PCSX::GUI::setDefaultShaders() {
    m_offscreenShaderEditor.setDefaults();
    auto offscreenStatus = m_offscreenShaderEditor.compile(this);
    m_outputShaderEditor.setDefaults();
    auto outputStatus = m_outputShaderEditor.compile(this);

    if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
        m_offscreenShaderEditor.setFallbacks();
        m_outputShaderEditor.setFallbacks();
        offscreenStatus = m_offscreenShaderEditor.compile(this);
        outputStatus = m_outputShaderEditor.compile(this);
    }
    if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
        g_system->log(LogClass::UI, "Failed to compile output shaders. You won't be able to see anything");
        g_system->log(LogClass::UI, "%s", offscreenStatus.getError());
        g_system->log(LogClass::UI, "%s", outputStatus.getError());
    }

    m_offscreenShaderEditor.reset(this);
    m_outputShaderEditor.reset(this);
}

void PCSX::GUI::init(std::function<void()> applyArguments) {
    int result;

    s_imguiUserErrorFunctor = [this](const char* msg) {
        m_gotImguiUserError = true;
        m_imguiUserError = msg;
    };
    bool luaStdout = g_system->getArgs().isLuaStdoutEnabled();
    m_luaConsole.setCmdExec([this, luaStdout](const std::string& cmd) {
        ScopedOnlyLog scopedOnlyLog(this);
        try {
            System::setCrashReportTag("user_lua", "console");
            g_emulator->m_lua->load(cmd, "console:", false);
            g_emulator->m_lua->pcall();
            for (const auto& error : m_glErrors) {
                m_luaConsole.addError(error);
                if (luaStdout) {
                    fputs(error.c_str(), stderr);
                    fputc('\n', stderr);
                }
            }
            m_glErrors.clear();
        } catch (std::exception& e) {
            m_luaConsole.addError(e.what());
            if (luaStdout) {
                fputs(e.what(), stderr);
                fputc('\n', stderr);
            }
        }
    });

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());
    }

    m_listener.listen<Events::Quitting>([this](const auto& event) { saveCfg(); });
    m_listener.listen<Events::ExecutionFlow::Pause>([this](const auto& event) {
        SDL_GL_SetSwapInterval(m_idleSwapInterval);
        SDL_SetWindowRelativeMouseMode(m_window, false);
    });
    // A hard reset clears VRAM, but while paused nothing presents it, so the last frame would linger.
    m_listener.listen<Events::ExecutionFlow::Reset>([this](const auto& event) {
        if (event.hard) m_presentAfterHardReset = true;
    });
    m_listener.listen<Events::ExecutionFlow::Run>([this](const auto& event) {
        m_enableSplashScreen = false;

        SDL_GL_SetSwapInterval(0);
        setRawMouseMotion();
    });

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    m_hasCoreProfile = true;

    // SDL_WINDOW_HIGH_PIXEL_DENSITY makes window-coords vs pixel-coords meaningful
    // on macOS Retina + Windows per-monitor DPI; required for the changeScale path.
    const SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    m_window = SDL_CreateWindow("PCSX-Redux", 1280, 800, windowFlags);
    if (m_window) {
        m_glContext = SDL_GL_CreateContext(m_window);
    }

    // SDL splits window and GL context creation, so the 3.0 fallback has to
    // cover both: a 3.3-core context can fail to materialize even after the
    // window itself succeeded. On platforms where the pixel format binds at
    // window creation (Win32 WGL is the strict case) a clean retry needs a
    // fresh window too, so we destroy and recreate both.
    if (!m_window || !m_glContext) {
        g_system->log(LogClass::UI, "SDL failed to create OpenGL 3.3 core context, retrying with any 3.0 profile\n");
        if (m_glContext) {
            SDL_GL_DestroyContext(m_glContext);
            m_glContext = nullptr;
        }
        if (m_window) {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }

        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        // No profile mask -> any profile.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        m_hasCoreProfile = false;

        m_window = SDL_CreateWindow("PCSX-Redux", 1280, 800, windowFlags);
        if (m_window) {
            m_glContext = SDL_GL_CreateContext(m_window);
        }
    }

    if (!m_window) {
        throw std::runtime_error(std::string("Unable to create main window: ") + SDL_GetError());
    }
    if (!m_glContext) {
        throw std::runtime_error(std::string("Unable to create GL context: ") + SDL_GetError());
    }
    SDL_GL_MakeCurrent(m_window, m_glContext);
    SDL_GL_SetSwapInterval(0);

    // Drop and resize events flow through SDL_PollEvent in startFrame() now.
    // ImGui_ImplSDL3_ProcessEvent gets first crack at every event for
    // mouse/keyboard/text dispatch.

    Resources::loadIcon([this](const uint8_t* data, uint32_t size) {
        clip::image img;
        if (!img.import_from_png(data, size)) return;
        int x = img.spec().width;
        int y = img.spec().height;
        // SDL_CreateSurfaceFrom needs a non-const pixel pointer; clip::image::data()
        // is non-const, so the cast below is benign.
        SDL_Surface* iconSurface = SDL_CreateSurfaceFrom(x, y, SDL_PIXELFORMAT_RGBA32, img.data(), x * 4);
        if (iconSurface) {
            SDL_SetWindowIcon(m_window, iconSurface);
            SDL_DestroySurface(iconSurface);
        }
    });

    result = gl3wInit();
    if (result) {
        throw std::runtime_error("Unable to initialize OpenGL layer. Check OpenGL drivers.");
    }
    gl3wFillCppThrowers();
    if (gl3wIsCppThrower(reinterpret_cast<GL3WglProc>(glDebugMessageCallback))) {
        glDebugMessageCallback = nullptr;
    }

    m_tvgInitialized = tvg::Initializer::init() == tvg::Result::Success;
    if (m_tvgInitialized) {
        g_system->findResource(
            [](auto path) -> bool {
                return tvg::Text::load((const char*)(path.u8string().c_str())) == tvg::Result::Success;
            },
            MAKEU8("NotoSans-Regular.ttf"), "fonts", std::filesystem::path("third_party") / "noto");
    } else {
        g_system->log(LogClass::UI, "Warning: Unable to initialize ThorVG.\n");
    }

    // Setup ImGui binding
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    {
        io.IniFilename = nullptr;
        auto& emuSettings = g_emulator->settings;
        if (loadSettings()) {
            if (!g_system->getArgs().isUIResetRequested() && (m_settingsJson.count("imgui") == 1) &&
                m_settingsJson["imgui"].is_string()) {
                std::string imguicfg = m_settingsJson["imgui"];
                ImGui::LoadIniSettingsFromMemory(imguicfg.c_str(), imguicfg.size());
            }
            if ((m_settingsJson.count("gui") == 1 && m_settingsJson["gui"].is_object())) {
                settings.deserialize(m_settingsJson["gui"]);
            }
            if ((m_settingsJson.count("loggers") == 1 && m_settingsJson["loggers"].is_object())) {
                m_log.deserialize(m_settingsJson["loggers"]);
            }
            auto& windowSizeX = settings.get<WindowSizeX>();
            auto& windowSizeY = settings.get<WindowSizeY>();
            if (windowSizeX <= 0 || g_system->getArgs().isUIResetRequested()) {
                windowSizeX.reset();
            }
            if (windowSizeY <= 0 || g_system->getArgs().isUIResetRequested()) {
                windowSizeY.reset();
            }
            if (g_system->getArgs().isUIResetRequested()) {
                settings.get<WindowPosX>().reset();
                settings.get<WindowPosY>().reset();
            }

            if ((settings.get<WindowPosX>().value > 0) && (settings.get<WindowPosY>().value > 0)) {
                SDL_SetWindowPosition(m_window, settings.get<WindowPosX>(), settings.get<WindowPosY>());
            }
            if (settings.get<WindowMaximized>().value) {
                SDL_MaximizeWindow(m_window);
            } else {
                SDL_SetWindowSize(m_window, windowSizeX, windowSizeY);
            }
        } else {
            saveCfg();
        }

        applyArguments();
        finishLoadSettings();

        if (!g_system->getArgs().isUpdateDisabled() && emuSettings.get<PCSX::Emulator::SettingAutoUpdate>() &&
            !g_system->getVersion().failed() && g_system->getVersion().hasUpdateInfo()) {
            m_update.downloadUpdateInfo(
                g_system->getVersion(),
                [this](bool success) {
                    if (success) {
                        m_updateAvailable = true;
                    }
                },
                g_system->getLoop());
        }

        setFullscreen(m_fullscreen);
        const auto currentTheme = emuSettings.get<Emulator::SettingGUITheme>().value;  // On boot: reload GUI theme
        applyTheme(currentTheme);
    }
    if (!g_system->running()) SDL_GL_SetSwapInterval(m_idleSwapInterval);

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (g_system->getArgs().isViewportsEnabled()) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;
    // io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;

    ImGui_ImplSDL3_InitForOpenGL(m_window, m_glContext);
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    io.SetClipboardTextFn = [](void*, const char* text) -> void { clip::set_text(text); };
    io.GetClipboardTextFn = [](void*) -> const char* {
        static std::string clipboard;
        clip::get_text(clipboard);
        return clipboard.c_str();
    };
    m_onChangedViewportOldCallback = platform_io.Platform_OnChangedViewport;
    platform_io.Platform_OnChangedViewport = [](ImGuiViewport* viewport) {
        if (g_gui->m_onChangedViewportOldCallback) g_gui->m_onChangedViewportOldCallback(viewport);
        g_gui->changeScale(viewport->DpiScale);
    };
    m_destroyWindowOldCallback = platform_io.Platform_DestroyWindow;
    platform_io.Platform_DestroyWindow = [](ImGuiViewport* viewport) {
        g_gui->destroyTvgViewport(viewport->ID, getSDLWindowFromImGuiViewport(viewport));
        if (g_gui->m_destroyWindowOldCallback) g_gui->m_destroyWindowOldCallback(viewport);
    };
    // Some bad GPU drivers (*cough* Intel) don't like mixed shaders versions,
    // and will silently fail to execute them.
    // This is just a bad heuristic to try and keep it the same version.
    ImGui_ImplOpenGL3_Init(m_hasCoreProfile ? GL_SHADER_VERSION : nullptr);

    if (glDebugMessageCallback &&
        (g_emulator->settings.get<Emulator::SettingGLErrorReporting>() || g_system->getArgs().isTestModeEnabled())) {
        m_reportGLErrors = true;
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glEnable(GL_DEBUG_OUTPUT);
        GLDEBUGPROC callback = [](GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                  const GLchar* message, const void* userParam) {
            GUI* self = reinterpret_cast<GUI*>(const_cast<void*>(userParam));
            self->glErrorCallback(source, type, id, severity, length, message);
        };
        glDebugMessageCallback(callback, this);
    } else {
        g_system->log(LogClass::UI,
                      _("Warning: OpenGL error reporting disabled. See About dialog for more information.\n"));
    }

    glDisable(GL_CULL_FACE);
    // The depth test isn't supposed to be enabled by default, but
    // it seems to be anyway on some drivers, so let's kill it there.
    glDisable(GL_DEPTH_TEST);
    // offscreen stuff
    glGenFramebuffers(1, &m_offscreenFrameBuffer);
    glGenTextures(2, m_offscreenTextures);
    glGenRenderbuffers(1, &m_offscreenDepthBuffer);

    m_mainVRAMviewer.setMain();
    m_mainVRAMviewer.setTitle(l_("Main VRAM Viewer"));
    m_clutVRAMviewer.setTitle(l_("CLUT VRAM selector"));
    m_memcardManager.initTextures();

    unsigned counter = 1;
    for (auto& viewer : m_VRAMviewers) {
        viewer.setTitle([counter]() { return _("Vram Viewer #") + std::to_string(counter); });
        counter++;
    }

    m_clutVRAMviewer.setClutDestination(&m_mainVRAMviewer);

    counter = 1;
    for (auto& editor : m_mainMemEditors) {
        editor.title = [counter, this]() {
            m_stringHolder = (_("Memory Editor #") + std::to_string(counter));
            return m_stringHolder.c_str();
        };
        counter++;
    }
    m_parallelPortEditor.title = l_("Parallel Port");
    m_scratchPadEditor.title = l_("Scratch Pad");
    m_hwrEditor.title = l_("Hardware Registers");
    m_biosEditor.title = []() { return "BIOS"; };
    m_vramEditor.title = l_("VRAM");
    m_spuRamEditor.title = l_("SPU RAM");
    auto makeExportFn = [this](MemoryEditorWrapper& wrapper, std::string postfixName) {
        return [this, &wrapper, postfixName](size_t len, size_t base_addr) {
            std::filesystem::path writeFilepath =
                g_system->getPersistentDir() / (getSaveStatePrefix(true) + "mem_" + postfixName + ".bin");
            IO<File> out(new PosixFile(writeFilepath.string(), FileOps::TRUNCATE));
            if (!out->failed()) {
                std::vector<uint8_t> buf(len);
                if (wrapper.editor.Cache.BulkReadFn) {
                    wrapper.editor.Cache.BulkReadFn(buf.data(), 0, len);
                }
                out->write(buf.data(), len);
                out->close();
                g_system->log(LogClass::UI, "Memory exported to: %s\n", writeFilepath.string().c_str());
            } else {
                g_system->log(LogClass::UI, "Failed to export memory to: %s\n", writeFilepath.string().c_str());
            }
        };
    };
    for (auto& editor : m_mainMemEditors) {
        editor.editor.ExportFn = makeExportFn(editor, "wram");
    }
    m_parallelPortEditor.editor.ExportFn = makeExportFn(m_parallelPortEditor, "parallel");
    m_scratchPadEditor.editor.ExportFn = makeExportFn(m_scratchPadEditor, "scratch");
    m_hwrEditor.editor.ExportFn = makeExportFn(m_hwrEditor, "hwr");
    m_biosEditor.editor.ExportFn = makeExportFn(m_biosEditor, "bios");
    m_vramEditor.editor.ExportFn = makeExportFn(m_vramEditor, "vram");
    m_spuRamEditor.editor.ExportFn = makeExportFn(m_spuRamEditor, "spuram");

    m_offscreenShaderEditor.init();
    m_outputShaderEditor.init();

    m_listener.listen<Events::GUI::JumpToMemory>([this](auto& event) {
        const uint32_t base = (event.address >> 20) & 0xff8;
        const uint32_t real = event.address & 0x7fffff;
        const uint32_t size = event.size;
        const uint32_t editorNum = event.editorNum;
        auto changeDataType = [](MemoryEditor* editor, int size) {
            bool isSigned = false;
            switch (editor->PreviewDataType) {
                case ImGuiDataType_S8:
                case ImGuiDataType_S16:
                case ImGuiDataType_S32:
                case ImGuiDataType_S64:
                    isSigned = true;
                    break;
            }
            switch (size) {
                case 1:
                    editor->PreviewDataType = isSigned ? ImGuiDataType_S8 : ImGuiDataType_U8;
                    break;
                case 2:
                    editor->PreviewDataType = isSigned ? ImGuiDataType_S16 : ImGuiDataType_U16;
                    break;
                case 4:
                    editor->PreviewDataType = isSigned ? ImGuiDataType_S32 : ImGuiDataType_U32;
                    break;
            }
        };
        if ((base == 0x000) || (base == 0x800) || (base == 0xa00)) {
            if (real < 0x00800000) {
                m_mainMemEditors[editorNum].m_show = true;
                m_mainMemEditors[editorNum].editor.GotoAddrAndHighlight(real, real + size);
                changeDataType(&m_mainMemEditors[editorNum].editor, size);
            }
        } else if (base == 0x1f8) {
            if (real >= 0x1000 && real < 0x3000) {
                m_hwrEditor.m_show = true;
                m_hwrEditor.editor.GotoAddrAndHighlight(real - 0x1000, real - 0x1000 + size);
                changeDataType(&m_hwrEditor.editor, size);
            }
        }
    });

    startFrame();
    m_currentTexture ^= 1;
    flip();
}

void PCSX::GUI::close() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    // Tear down all GL-backed resources (ThorVG canvases) BEFORE
    // dropping the GL context they live in.
    while (!m_tvgViewports.empty()) destroyTvgViewport(m_tvgViewports.begin()->first, m_window);
    if (m_tvgInitialized) tvg::Initializer::term();
    m_tvgInitialized = false;
    if (m_glContext) {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

void PCSX::GUI::saveCfg() {
    if (g_system->getArgs().isTestModeEnabled()) return;
    // The settings have been wiped, and we're on our way to a reboot. Writing them back out now would
    // simply undo the wipe: this gets called on quit, on layout changes, and whenever a config widget
    // reports a change, so all three would race the reset otherwise.
    if (m_settingsNuked) return;
    std::filesystem::path cfgTmpPath = g_system->getPersistentDir() / "pcsx.json.tmp";
    std::filesystem::path cfgPath = g_system->getPersistentDir() / "pcsx.json";
    {
        std::ofstream cfg(cfgTmpPath);
        json j;

        const SDL_WindowFlags flags = SDL_GetWindowFlags(m_window);
        if (m_fullscreen || (flags & SDL_WINDOW_MINIMIZED)) {
            m_windowPosX = settings.get<WindowPosX>();
            m_windowPosY = settings.get<WindowPosY>();
            m_windowSizeX = settings.get<WindowSizeX>();
            m_windowSizeY = settings.get<WindowSizeY>();
            m_windowMaximized = settings.get<WindowMaximized>();
        } else {
            SDL_GetWindowPosition(m_window, &m_windowPosX, &m_windowPosY);
            SDL_GetWindowSize(m_window, &m_windowSizeX, &m_windowSizeY);
            m_windowMaximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        }

        j["imgui"] = ImGui::SaveIniSettingsToMemory(nullptr);
        j["SPU"] = PCSX::g_emulator->m_spu->getCfg();
        j["emulator"] = PCSX::g_emulator->settings.serialize();
        j["gui"] = settings.serialize();
        j["loggers"] = m_log.serialize();
        j["pads"] = PCSX::g_emulator->m_pads->getCfg();
        cfg << std::setw(2) << j << std::endl;
    }
    if (std::filesystem::copy_file(cfgTmpPath, cfgPath, std::filesystem::copy_options::overwrite_existing)) {
        std::filesystem::remove(cfgTmpPath);
    }
}

void PCSX::GUI::resetSettings() {
    // Resetting the live settings objects in place isn't enough: the ImGui layout belongs to the current
    // context, and a fair amount of the emulator settings are only ever acted upon during startup. So the
    // wipe happens on disk, and the reboot below is what actually reloads everything from defaults, as it
    // tears down and recreates both the emulator and the UI.
    m_settingsNuked = true;
    std::filesystem::path cfgTmpPath = g_system->getPersistentDir() / "pcsx.json.tmp";
    std::filesystem::path cfgPath = g_system->getPersistentDir() / "pcsx.json";
    std::error_code ec;
    std::filesystem::remove(cfgTmpPath, ec);
    // An empty object rather than no file at all: every consumer already falls back to its defaults on a
    // missing key, and keeping the file around preserves the portable mode detection, which keys off of
    // the mere existence of pcsx.json in the current directory.
    {
        std::ofstream cfg(cfgPath);
        cfg << "{}" << std::endl;
    }
    // The shader editors keep their sources next to the settings, one set of files per base name. Rather
    // than hardcoding the list of editors, which would quietly go stale the moment another one is added,
    // key off of the vertex shaders actually present: every base has one, and nothing else uses that
    // extension, so the companion files can be derived from it.
    auto persistentDir = g_system->getPersistentDir();
    std::vector<std::filesystem::path> shaderBases;
    for (const auto& entry : std::filesystem::directory_iterator(persistentDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == ".vert") shaderBases.push_back(entry.path());
    }
    for (auto& base : shaderBases) {
        for (auto extension : {"vert", "frag", "lua", "json"}) {
            auto toRemove = std::filesystem::path(base).replace_extension(extension);
            // Guard against a stray pcsx.vert deriving the settings file we just wrote.
            if (toRemove == cfgPath) continue;
            std::filesystem::remove(toRemove, ec);
        }
    }
    g_system->quit(0x12eb007);
}

void PCSX::GUI::startFrame() {
    ZoneScoped;
    tick();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
            case SDL_EVENT_QUIT:
                g_system->quit();
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == SDL_GetWindowID(m_window)) g_system->quit();
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (event.window.windowID == SDL_GetWindowID(m_window)) m_setupScreenSize = true;
                break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                // Force a font/scale rebuild when the window crosses to a display
                // with different DPI. This is the case GLFW didn't fire and is the
                // recurring high-DPI bug we're fixing in this phase.
                if (event.window.windowID == SDL_GetWindowID(m_window)) {
                    changeScale(SDL_GetWindowDisplayScale(m_window));
                    m_setupScreenSize = true;
                }
                break;
            case SDL_EVENT_DROP_FILE:
                // SDL hands us a UTF-8 path that's owned by SDL and freed when
                // the event is consumed; magicOpen copies what it needs.
                if (event.drop.data) magicOpen(event.drop.data);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const int action = event.type == SDL_EVENT_KEY_UP ? 0 : event.key.repeat ? 2 : 1;
                g_system->m_eventBus->signal(Events::Keyboard{static_cast<int>(event.key.key),
                                                              static_cast<int>(event.key.scancode), action,
                                                              static_cast<int>(event.key.mod)});
                break;
            }
            default:
                break;
        }
    }

    if (m_setupScreenSize) {
        const float renderRatio = settings.get<WidescreenRatio>() ? 9.0f / 16.0f : 3.0f / 4.0f;
        int w, h;

        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        // Make width/height be 1 at minimum
        w = std::max<int>(w, 1);
        h = std::max<int>(h, 1);
        m_framebufferSize = ImVec2(w, h);
        m_renderSize = m_fullWindowRender ? ImVec2(w, h) : m_outputWindowSize;
        ImGuiHelpers::normalizeDimensions(m_renderSize, renderRatio);

        // Reset texture and framebuffer storage
        glBindTexture(GL_TEXTURE_2D, m_offscreenTextures[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_renderSize.x, m_renderSize.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindTexture(GL_TEXTURE_2D, m_offscreenTextures[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_renderSize.x, m_renderSize.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

        if (m_clearTextures || m_enableSplashScreen) {
            m_clearTextures = false;
            std::unique_ptr<uint32_t[]> splashImageData = getSplashScreen(m_renderSize.x, m_renderSize.y);

            // Upload to both textures
            for (int i = 0; i < 2; i++) {
                glBindTexture(GL_TEXTURE_2D, m_offscreenTextures[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_renderSize.x, m_renderSize.y, GL_RGBA, GL_UNSIGNED_BYTE,
                                splashImageData.get());
            }
        }

        glBindRenderbuffer(GL_RENDERBUFFER, m_offscreenDepthBuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_renderSize.x, m_renderSize.y);
        m_setupScreenSize = false;
    }

    auto& io = ImGui::GetIO();

    if (m_reloadFonts) {
        m_reloadFonts = false;
        auto scales = m_allScales;
        if (scales.empty()) scales.emplace(1.0f);

        // ImGui v1.92 retired ImGui_ImplOpenGL3_{Destroy,Create}FontsTexture and
        // ImFontAtlas::Build() in favour of the dynamic-texture protocol; the
        // backend pulls fresh glyphs from the atlas as it grows. We still
        // rebuild the per-scale font map below so each pinned scale has a
        // matching ImFont with the right LegacySize ready for PushFont; actual
        // per-size glyph data is allocated lazily inside ImFontBaked when those
        // fonts are first rendered.
        m_mainFonts.clear();
        m_monoFonts.clear();

        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        for (auto& scale : scales) {
            m_mainFonts[scale] = loadFont(MAKEU8("NotoSans-Regular.ttf"), settings.get<MainFontSize>().value * scale,
                                          io, false);
            for (auto& e : g_system->getLocaleExtra()) {
                loadFont(e, settings.get<MainFontSize>().value * scale, io, true);
            }
            // try loading the japanese font for memory card manager
            m_hasJapanese = loadFont(MAKEU8("NotoSansCJKjp-Regular.otf"), settings.get<MainFontSize>().value * scale,
                                     io, true);
            m_monoFonts[scale] = loadFont(MAKEU8("NotoMono-Regular.ttf"), settings.get<MonoFontSize>().value * scale,
                                          io, false);
            // NotoMono has no U+2713 (cester's pass mark) and no CJK; merge them in from the japanese font.
            loadFont(MAKEU8("NotoSansCJKjp-Regular.otf"), settings.get<MonoFontSize>().value * scale, io, true);
        }
        // Pick the font matching the current DPI scale as the default; in
        // v1.92 ImGui draws frames at Style.FontSizeBase unless overridden by
        // PushFont, so we have to push that size up to match or the default
        // frame text would render at the smallest pinned scale.
        ImFont* mainFont = getMainFont();
        io.FontDefault = mainFont;
        if (mainFont) ImGui::GetStyle().FontSizeBase = mainFont->LegacySize;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    MarkDown::newFrame();
    if (io.WantSaveIniSettings) {
        io.WantSaveIniSettings = false;
        saveCfg();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_offscreenFrameBuffer);
    GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};

    // this call seems to sometime fails when the window is minimized...?
    glDrawBuffers(1, DrawBuffers);  // "1" is the size of DrawBuffers

    // If kiosk mode is enabled, ignore hotkeys
    if (g_emulator->settings.get<Emulator::SettingKioskMode>().value) return;

    // Check hotkeys (TODO: Make configurable)
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_showMenu = !m_showMenu;
    }
    if (io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) setFullscreen(!m_fullscreen);
        if (io.KeyCtrl && allowMouseCaptureToggle()) {
            isRawMouseMotionEnabled() = !isRawMouseMotionEnabled();
            setRawMouseMotion();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1)) {  // Save to quick-save slot
        saveSaveState(buildSaveStateFilename(0));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {  // Load from quick-save slot
        const auto saveStateName = buildSaveStateFilename(0);
        loadSaveState(saveStateName);
    }

    if (!g_system->running()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F10)) {
            g_emulator->m_debug->stepOver();
        } else if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
            if (ImGui::GetIO().KeyShift) {
                g_emulator->m_debug->stepOut();
            } else {
                g_emulator->m_debug->stepIn();
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Pause) || ImGui::IsKeyPressed(ImGuiKey_F5)) {
            g_system->resume();
        }
    } else {
        if (ImGui::IsKeyPressed(ImGuiKey_Pause) || ImGui::IsKeyPressed(ImGuiKey_F6)) g_system->pause();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F8)) {
        if (ImGui::GetIO().KeyShift) {
            g_system->hardReset();
        } else {
            g_system->softReset();
        }
    }
    // Rewind: holding the key auto-repeats, stepping further back through the ring. Not while a
    // text field has focus, where Backspace is just Backspace.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        g_emulator->rewindState();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
        saveScreenShot();
    }

    // Tab is also a text and navigation key: leave it alone while a widget or editor (Lua console, assembler,
    // memory editor...) owns the keyboard. Keyboard navigation is enabled, so a focused Output window also
    // reports WantCaptureKeyboard; that one is where the user is playing, so let it through.
    const bool keyboardCaptured = io.WantCaptureKeyboard && !m_outputWindowFocused;
    if (!keyboardCaptured && ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        m_turboEnabled = !m_turboEnabled;
        g_emulator->setTurbo(m_turboEnabled);
    }
}

void PCSX::GUI::setViewport() { glViewport(0, 0, m_renderSize.x, m_renderSize.y); }

void PCSX::GUI::flip() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_offscreenFrameBuffer);
    glBindTexture(GL_TEXTURE_2D, m_offscreenTextures[m_currentTexture]);

    glBindRenderbuffer(GL_RENDERBUFFER, m_offscreenDepthBuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_offscreenDepthBuffer);
    GLuint texture = m_offscreenTextures[m_currentTexture];
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};

    // this call seems to sometime fails when the window is minimized...?
    glDrawBuffers(1, DrawBuffers);  // "1" is the size of DrawBuffers
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glClearColor(0, 0, 0, 0);
    GUIPlatform::clearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_CULL_FACE);
    m_currentTexture ^= 1;
}

void PCSX::GUI::endFrame() {
    constexpr float renderRatio = 3.0f / 4.0f;
    const int w = m_framebufferSize.x;
    const int h = m_framebufferSize.y;

    auto& io = ImGui::GetIO();
    if (ImGui::IsKeyDown(ImGuiKey_PrintScreen) && io.KeyCtrl) {
        auto screenshot = g_emulator->m_gpu->takeScreenShot();
        clip::image img = PCSX::ScreenShot::convertScreenshotToImage(std::move(screenshot));
        clip::set_image(img);
    }
    // bind back the output frame buffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    auto& emuSettings = PCSX::g_emulator->settings;
    auto& debugSettings = emuSettings.get<Emulator::SettingDebugSettings>();

    bool changed = false;

    m_offscreenShaderEditor.configure(this);
    m_outputShaderEditor.configure(this);

    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    ImGuiContext* context = ImGui::GetCurrentContext();
    ImGuiDockNode* dockspaceNode = ImGui::DockContextFindNodeByID(context, dockspaceId);
    if (m_fullWindowRender && !dockspaceNode->IsEmpty()) {
        m_fullWindowRender = false;
        ImGui::SetNextWindowDockID(dockspaceId);
    }
    m_outputWindowFocused = false;
    if (m_fullWindowRender) {
        ImTextureID texture = m_offscreenTextures[m_currentTexture];
        const auto basePos = ImGui::GetMainViewport()->Pos;
        const auto displayFramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        const auto logicalRenderSize =
            ImVec2(m_renderSize[0] / displayFramebufferScale[0], m_renderSize[1] / displayFramebufferScale[1]);
        ImGui::SetNextWindowPos(ImVec2((w - m_renderSize.x) / 2.0f / displayFramebufferScale[0] + basePos.x,
                                       (h - m_renderSize.y) / 2.0f / displayFramebufferScale[0] + basePos.y));
        ImGui::SetNextWindowSize(logicalRenderSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("FullWindowRender", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (g_system->getArgs().isShadersDisabled()) {
            ImGui::Image(texture, logicalRenderSize, ImVec2(0, 0), ImVec2(1, 1));
        } else {
            m_outputShaderEditor.renderWithImgui(this, texture, m_renderSize, logicalRenderSize);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    } else {
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
        bool outputWindowShown = true;
        if (ImGui::Begin(
                _("Output"), &outputWindowShown,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse)) {
            m_outputWindowFocused = ImGui::IsWindowFocused();
            ImGuiDockNode* outputNode = ImGui::GetWindowDockNode();
            if (outputNode && dockspaceNode->OnlyNodeWithWindows == outputNode &&
                (!outputNode->TabBar || outputNode->TabBar->Tabs.size() == 1)) {
                // if output is the only visible window in dockspace, switch to full window render mode automatically
                outputWindowShown = false;
            }
            ImVec2 contentRegion = ImGui::GetContentRegionAvail();
            ImVec2 textureSize = contentRegion;
            if ((m_outputWindowSize.x != textureSize.x) || (m_outputWindowSize.y != textureSize.y)) {
                m_outputWindowSize = textureSize;
                m_setupScreenSize = true;
            }
            ImGuiHelpers::normalizeDimensions(textureSize, renderRatio);
            ImGui::SetCursorPos(ImGui::GetCursorPos() + (contentRegion - textureSize) * 0.5f);
            ImTextureID texture = m_offscreenTextures[m_currentTexture];
            if (g_system->getArgs().isShadersDisabled()) {
                ImGui::Image(texture, textureSize, ImVec2(0, 0), ImVec2(1, 1));
            } else {
                m_outputShaderEditor.renderWithImgui(this, texture, m_renderSize, textureSize);
            }
        }
        ImGui::End();
        if (!outputWindowShown) {
            m_fullWindowRender = true;
            // full window render mode can't have anything docked in the dockspace
            ImGui::DockContextClearNodes(context, dockspaceId, true);
        }
    }

    bool showOpenIsoFileDialog = false;
    bool showOpenBinaryDialog = false;
    bool showOpenArchiveDialog = false;

    if (m_showMenu || !m_fullWindowRender || !PCSX::g_system->running()) {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu(_("File"))) {
                showOpenIsoFileDialog = ImGui::MenuItem(_("Open Disk Image"));
                auto currentIso = PCSX::g_emulator->m_cdrom->getIso();
                if (ImGui::MenuItem(_("Reload Disk Image"), nullptr, nullptr, currentIso && !currentIso->failed())) {
                    PCSX::g_emulator->m_cdrom->clearIso();
                    PCSX::g_emulator->m_cdrom->setIso(new CDRIso(currentIso->getIsoPath()));
                    PCSX::g_emulator->m_cdrom->parseIso();
                    g_system->hardReset();
                }
                if (ImGui::MenuItem(_("Close Disk Image"))) {
                    PCSX::g_emulator->m_cdrom->setIso(new CDRIso(new FailedFile));
                    PCSX::g_emulator->m_cdrom->parseIso();
                }
                if (ImGui::MenuItem(_("Load binary"))) {
                    showOpenBinaryDialog = true;
                }
                if (ImGui::MenuItem(_("Add Lua archive"))) {
                    showOpenArchiveDialog = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem(_("Dump save state proto schema"))) {
                    std::ofstream schema("sstate.proto");
                    SaveStates::ProtoFile::dumpSchema(schema);
                }

                const auto globalSaveStateName = fmt::format(f_("global{}"), getSaveStatePostfix());

                if (ImGui::BeginMenu(_("Save state slots"))) {
                    if (ImGui::MenuItem(_("Quick-save slot"), "F1")) {
                        saveSaveState(buildSaveStateFilename(0));
                    }

                    for (auto i = 1; i < 10; i++) {
                        const auto str = fmt::format(f_("Slot {}"), i);
                        if (ImGui::MenuItem(str.c_str())) {
                            saveSaveState(buildSaveStateFilename(i));
                        }
                    }

                    ImGui::Separator();
                    ImGui::MenuItem(_("Show named save states"), nullptr, &m_namedSaveStates.m_show);

                    ImGui::EndMenu();
                }

                if (ImGui::MenuItem(_("Save global state"))) saveSaveState(globalSaveStateName);

                if (ImGui::BeginMenu(_("Load state slots"))) {
                    auto saveFilename = buildSaveStateFilename(0);
                    ImGui::BeginDisabled(!saveStateExists(saveFilename));
                    if (ImGui::MenuItem(_("Quick-load slot"), "F2")) loadSaveState(saveFilename);
                    ImGui::EndDisabled();

                    for (auto i = 1; i < 10; i++) {
                        const auto str = fmt::format(f_("Slot {}"), i);
                        saveFilename = buildSaveStateFilename(i);
                        ImGui::BeginDisabled(!saveStateExists(saveFilename));
                        if (ImGui::MenuItem(str.c_str())) loadSaveState(saveFilename);
                        ImGui::EndDisabled();
                    }

                    auto namedSaves = m_namedSaveStates.getNamedSaveStates(this);
                    if (!namedSaves.empty()) {
                        ImGui::Separator();
                        for (auto filenamePair : namedSaves) {
                            if (ImGui::MenuItem(filenamePair.second.c_str())) {
                                loadSaveState(filenamePair.first);
                            }
                        }
                    }

                    ImGui::EndMenu();
                }

                ImGui::BeginDisabled(!saveStateExists(globalSaveStateName));
                if (ImGui::MenuItem(_("Load global state"))) loadSaveState(globalSaveStateName);
                ImGui::EndDisabled();

                ImGui::Separator();
                if (ImGui::MenuItem(_("Open LID"))) {
                    PCSX::g_emulator->m_cdrom->openLid();
                }
                if (ImGui::MenuItem(_("Close LID"))) {
                    PCSX::g_emulator->m_cdrom->closeLid();
                }
                if (ImGui::MenuItem(_("Open and close LID"))) {
                    PCSX::g_emulator->m_cdrom->scheduleCloseLid();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(_("Reset settings..."))) {
                    m_showResetSettings = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem(_("Reboot"))) {
                    g_system->quit(0x12eb007);
                }
                if (ImGui::MenuItem(_("Quit"))) {
                    g_system->quit();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Emulation"))) {
                if (ImGui::MenuItem(_("Start emulation"), "F5", nullptr, !g_system->running())) {
                    g_system->resume();
                }
                if (ImGui::MenuItem(_("Pause emulation"), "F6", nullptr, g_system->running())) {
                    g_system->pause();
                }
                if (ImGui::MenuItem(_("Soft Reset"), "F8")) {
                    g_system->softReset();
                }
                if (ImGui::MenuItem(_("Hard Reset"), "Shift+F8")) {
                    g_system->hardReset();
                }
                if (ImGui::MenuItem(_("Rewind"), "Backspace", nullptr, g_emulator->rewindStateCount() > 0)) {
                    g_emulator->rewindState();
                }
                if (ImGui::MenuItem(_("Take Screenshot"), "F12")) {
                    saveScreenShot();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Configuration"))) {
                if (ImGui::MenuItem(_("Fullscreen"), nullptr, &m_fullscreen)) {
                    setFullscreen(m_fullscreen);
                    m_setupScreenSize = true;
                }
                if (ImGui::MenuItem(_("Full window render"), nullptr, &m_fullWindowRender)) {
                    m_setupScreenSize = true;
                    // full window render mode can't have anything docked in the dockspace
                    ImGui::DockContextClearNodes(context, dockspaceId, true);
                }
                ImGui::Separator();
                if (ImGui::BeginMenu(_("Shader presets"))) {
                    if (ImGui::MenuItem(_("Default shader"))) {
                        setDefaultShaders();
                    }
                    if (ImGui::MenuItem(_("CRT-lottes shader"))) {
                        m_offscreenShaderEditor.setText(Shaders::CrtLottes::Offscreen::vert().data(),
                                                        Shaders::CrtLottes::Offscreen::frag().data(),
                                                        Shaders::CrtLottes::Offscreen::lua().data());
                        auto offscreenStatus = m_offscreenShaderEditor.compile(this);
                        m_outputShaderEditor.setText(Shaders::CrtLottes::Output::vert().data(),
                                                     Shaders::CrtLottes::Output::frag().data(),
                                                     Shaders::CrtLottes::Output::lua().data());
                        auto outputStatus = m_outputShaderEditor.compile(this);

                        if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
                            m_offscreenShaderEditor.setFallbacks();
                            m_outputShaderEditor.setFallbacks();
                            offscreenStatus = m_offscreenShaderEditor.compile(this);
                            outputStatus = m_outputShaderEditor.compile(this);
                        }
                        if (!offscreenStatus.isOk() || !outputStatus.isOk()) {
                            g_system->log(LogClass::UI,
                                          "Failed to compile output shaders. You won't be able to see anything");
                            g_system->log(LogClass::UI, "%s", offscreenStatus.getError());
                            g_system->log(LogClass::UI, "%s", outputStatus.getError());
                        }
                        m_offscreenShaderEditor.reset(this);
                        m_outputShaderEditor.reset(this);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(_("Configure Shaders"))) {
                    m_offscreenShaderEditor.setConfigure();
                    m_outputShaderEditor.setConfigure();
                }
                ImGui::Separator();
                ImGui::MenuItem(_("Controls"), nullptr, &g_emulator->m_pads->m_showCfg);
                if (ImGui::MenuItem(_("Manage Memory Cards"), nullptr, &m_memcardManager.m_show)) {
                    m_memcardManager.m_frameCount = 0;  // Reset frame count when memcard manager is toggled
                }
                ImGui::Separator();
                ImGui::MenuItem(_("Emulation"), nullptr, &m_showCfg);
                ImGui::MenuItem(_("GPU"), nullptr, &PCSX::g_emulator->m_gpu->m_showCfg);
                ImGui::MenuItem(_("SPU"), nullptr, &PCSX::g_emulator->m_spu->m_showCfg);
                ImGui::MenuItem(_("PIO Cartridge"), nullptr, &m_pioCart.m_show);
                ImGui::MenuItem(_("Network"), nullptr, &m_network.m_show);
                ImGui::Separator();
                ImGui::MenuItem(_("UI"), nullptr, &m_showUiCfg);
                ImGui::MenuItem(_("System"), nullptr, &m_showSysCfg);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Debug"))) {
                ImGui::MenuItem(_("Show Logs"), nullptr, &m_log.m_show);
                if (ImGui::BeginMenu("Lua")) {
                    ImGui::MenuItem(_("Show Lua Console"), nullptr, &m_luaConsole.m_show);
                    ImGui::MenuItem(_("Show Lua Inspector"), nullptr, &m_luaInspector.m_show);
                    ImGui::MenuItem(_("Show Lua editor"), nullptr, &m_luaEditor.m_show);
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::BeginMenu(_("CPU"))) {
                    ImGui::MenuItem(_("Show Registers"), nullptr, &m_registers.m_show);
                    ImGui::MenuItem(_("Show Assembly"), nullptr, &m_assembly.m_show);
                    if (PCSX::g_emulator->m_cpu->isDynarec()) {
                        ImGui::MenuItem(_("Show DynaRec Disassembly"), nullptr, &m_disassembly.m_show);
                    } else {
                        ImGui::MenuItem(_("Show DynaRec Disassembly"), nullptr, false, false);
                        ImGuiHelpers::ShowHelpMarker(_(
                            R"(DynaRec Disassembler is not available in Interpreted CPU mode. Try enabling [Dynarec CPU]
in Configuration->Emulation, restart PCSX-Redux, then try again.)"));
                    }
                    ImGui::MenuItem(_("Show CPU Trace"), nullptr, &m_cpuTrace.m_show);
                    ImGui::MenuItem(_("Show Breakpoints"), nullptr, &m_breakpoints.m_show);
                    ImGui::MenuItem(_("Show Callstacks"), nullptr, &m_callstacks.m_show);
                    if (ImGui::BeginMenu(_("Memory Editors"))) {
                        for (auto& editor : m_mainMemEditors) {
                            editor.MenuItem();
                        }
                        m_parallelPortEditor.MenuItem();
                        m_scratchPadEditor.MenuItem();
                        m_hwrEditor.MenuItem();
                        m_biosEditor.MenuItem();
                        m_vramEditor.MenuItem();
                        m_spuRamEditor.MenuItem();
                        ImGui::EndMenu();
                    }
                    ImGui::MenuItem(_("Show Memory Observer"), nullptr, &m_memoryObserver.m_show);
                    ImGui::MenuItem(_("Show RAM viewer"), nullptr, &m_ramViewer.m_show);
                    ImGui::MenuItem(_("Show MSAN Viewer"), nullptr, &m_msanViewer.m_show);
                    ImGui::MenuItem(_("Show Typed Debugger"), nullptr, &m_typedDebugger.m_show);
                    ImGui::MenuItem(_("Show Patches"), nullptr, &m_patches.m_show);
                    ImGui::MenuItem(_("Show Interrupts Scaler"), nullptr, &m_showInterruptsScaler);
                    if (ImGui::BeginMenu(_("First Chance Exceptions"))) {
                        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
                        constexpr auto& exceptions = magic_enum::enum_entries<PCSX::R3000Acpu::Exception>();
                        unsigned& s = debugSettings.get<Emulator::DebugSettings::FirstChanceException>().value;
                        for (auto& e : exceptions) {
                            unsigned f =
                                1 << static_cast<std::underlying_type<PCSX::R3000Acpu::Exception>::type>(e.first);
                            bool selected = s & f;
                            changed |= ImGui::MenuItem(std::string(e.second).c_str(), nullptr, &selected);
                            if (selected) {
                                s |= f;
                            } else {
                                s &= ~f;
                            }
                        }
                        ImGui::PopItemFlag();
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(_("GPU"))) {
                    if (ImGui::BeginMenu(_("VRAM viewers"))) {
                        ImGui::MenuItem(_("Show main VRAM viewer"), nullptr, &m_mainVRAMviewer.m_show);
                        ImGui::MenuItem(_("Show CLUT VRAM viewer"), nullptr, &m_clutVRAMviewer.m_show);
                        unsigned counter = 1;
                        for (auto& viewer : m_VRAMviewers) {
                            std::string title = _("Show VRAM viewer #") + std::to_string(counter);
                            ImGui::MenuItem(title.c_str(), nullptr, &viewer.m_show);
                            counter++;
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::MenuItem(_("Show GPU logger"), nullptr, &m_gpuLogger.m_show);
                    ImGui::MenuItem(_("Show GPU dump recorder / player"), nullptr, &m_gpuDump.m_show);
                    ImGui::MenuItem(_("Show GPU debug"), nullptr, &PCSX::g_emulator->m_gpu->m_showDebug);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(_("SPU"))) {
                    ImGui::MenuItem(_("Show SPU debug"), nullptr, &PCSX::g_emulator->m_spu->m_showDebug);
                    ImGui::MenuItem(_("Show SPU logger"), nullptr, &m_spuLoggerWidget.m_show);
                    ImGui::MenuItem(_("Show SPU samples"), nullptr, &m_spuSamplesWidget.m_show);
                    ImGui::EndMenu();
                }
                ImGui::MenuItem(_("Show Network"), nullptr, &m_network.m_show);
                if (ImGui::BeginMenu(_("CD-Rom"))) {
                    ImGui::MenuItem(_("Show Iso Browser"), nullptr, &m_isoBrowser.m_show);
                    ImGui::MenuItem(_("Show CD-ROM viewer"), nullptr, &m_cdromViewer.m_show);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(_("Misc hardware"))) {
                    ImGui::MenuItem(_("Show HW Registers"), nullptr, &m_hwRegs.m_show);
                    ImGui::MenuItem(_("Show SIO1 debug"), nullptr, &m_sio1.m_show);
                    ImGui::EndMenu();
                }
                ImGui::MenuItem(_("Show PSYQo heap viewer"), nullptr, &m_heapViewer.m_show);
                ImGui::MenuItem(_("Show allocator tracker"), nullptr, &m_allocTrackerViewer.m_show);
                ImGui::Separator();
                if (ImGui::BeginMenu(_("Kernel"))) {
                    ImGui::MenuItem(_("Kernel Events"), nullptr, &m_events.m_show);
                    ImGui::MenuItem(_("Kernel Handlers"), nullptr, &m_handlers.m_show);
                    ImGui::MenuItem(_("Kernel Calls"), nullptr, &m_kernelLog.m_show);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(_("Rendering"))) {
                    ImGui::MenuItem(_("Show Output Shader Editor"), nullptr, &m_outputShaderEditor.m_show);
                    ImGui::MenuItem(_("Show Offscreen Shader Editor"), nullptr, &m_offscreenShaderEditor.m_show);
                    if (ImGui::MenuItem(_("Reset shaders"), nullptr)) {
                        setDefaultShaders();
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(_("Dalos"))) {
                ImGui::MenuItem(_("Show Canvas"), nullptr, &m_dalos.m_show);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_("Help"))) {
                ImGui::MenuItem(_("Show ImGui Demo"), nullptr, &m_showDemo);
                ImGui::MenuItem(_("Show ImPlot Demo"), nullptr, &m_showImPlotDemo);
                ImGui::Separator();
                ImGui::MenuItem(_("Show UvFile information"), nullptr, &m_showHandles);
                ImGui::Separator();
                ImGui::MenuItem(_("About"), nullptr, &m_showAbout);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::Separator();
            ImGui::Text(_("CPU: %s"), g_emulator->m_cpu->getName().c_str());
            ImGui::Separator();
            ImGui::Text(_("GAME ID: %s"), g_emulator->m_cdrom->getCDRomID().c_str());
            ImGui::Separator();
            if (g_system->running()) {
                ImGui::Text(_("%.2f FPS (%.2f ms)"), ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
                ImGui::Separator();
                ImGui::Text(_("%.2f guest FPS"), g_emulator->m_gpuLogger->getGuestFPS());
                ImGui::Separator();
                uint32_t frameCount = g_emulator->m_spu->getFrameCount();
                ImGui::Text(_("%.2f ms audio buffer (%i frames)"), 1000.0f * frameCount / 44100.0f, frameCount);
            } else {
                ImGui::TextUnformatted(C_("Audio status", "Idle"));
            }
            if (m_turboEnabled) {
                ImGui::Separator();
                ImGui::TextUnformatted(_("Turbo"));
            }

            ImGui::EndMainMenuBar();
        }
    }

    auto& isoPath = g_emulator->settings.get<Emulator::SettingIsoPath>();

    if (showOpenIsoFileDialog) {
        if (!isoPath.empty()) {
            m_openIsoFileDialog.m_currentPath = isoPath.value;
        }
        m_openIsoFileDialog.openDialog();
    }
    if (m_openIsoFileDialog.draw()) {
        isoPath.value = m_openIsoFileDialog.m_currentPath;
        changed = true;
        std::vector<PCSX::u8string> fileToOpen = m_openIsoFileDialog.selected();
        if (!fileToOpen.empty()) {
            PCSX::g_emulator->m_cdrom->setIso(new CDRIso(reinterpret_cast<const char*>(fileToOpen[0].c_str())));
            PCSX::g_emulator->m_cdrom->parseIso();
        }
    }

    if (showOpenBinaryDialog) {
        if (!isoPath.empty()) {
            m_openBinaryDialog.m_currentPath = isoPath.value;
        }
        m_openBinaryDialog.openDialog();
    }
    if (m_openBinaryDialog.draw()) {
        isoPath.value = m_openBinaryDialog.m_currentPath;
        changed = true;
        std::vector<PCSX::u8string> fileToOpen = m_openBinaryDialog.selected();
        if (!fileToOpen.empty()) {
            m_exeToLoad.set(fileToOpen[0]);
            std::filesystem::path p = fileToOpen[0];
            g_system->log(LogClass::UI, "Scheduling to load %s and soft reseting.\n", p.string());
            g_system->softReset();
        }
    }

    if (showOpenArchiveDialog) {
        if (!isoPath.empty()) {
            m_openArchiveDialog.m_currentPath = isoPath.value;
        }
        m_openArchiveDialog.openDialog();
    }
    if (m_openArchiveDialog.draw()) {
        isoPath.value = m_openArchiveDialog.m_currentPath;
        changed = true;
        std::vector<PCSX::u8string> fileToOpen = m_openArchiveDialog.selected();
        if (!fileToOpen.empty()) {
            IO<File> file = new PosixFile(fileToOpen[0]);
            try {
                auto& archive = LuaFFI::addArchive(*g_emulator->m_lua, file);
                if (!archive.failed()) {
                    g_system->log(LogClass::UI, "Added %s to our list of loaded archives.\n",
                                  reinterpret_cast<const char*>(fileToOpen[0].c_str()));
                }
            } catch (...) {
            }
        }
    }

    if (m_showDemo) ImGui::ShowDemoWindow();
    if (m_showImPlotDemo) ImPlot::ShowDemoWindow();

    ImGui::SetNextWindowPos(ImVec2(10, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1024, 512), ImGuiCond_FirstUseEver);
    if (m_mainVRAMviewer.m_show) m_mainVRAMviewer.draw(this, g_emulator->m_gpu->getVRAMTexture());
    if (m_clutVRAMviewer.m_show) m_clutVRAMviewer.draw(this, g_emulator->m_gpu->getVRAMTexture());
    for (auto& viewer : m_VRAMviewers) {
        if (viewer.m_show) {
            viewer.draw(this, g_emulator->m_gpu->getVRAMTexture());
        }
    }

    if (m_ramViewer.m_show) {
        auto* ramLogger = g_emulator->m_ramLogger.get();
        if (!ramLogger->isEnabled()) ramLogger->enable();
        ramLogger->uploadRAM();
        ramLogger->uploadHeatmaps();
        m_ramViewer.draw(this);
    } else {
        auto* ramLogger = g_emulator->m_ramLogger.get();
        if (ramLogger->isEnabled()) ramLogger->disable();
    }

    if (m_cdromViewer.m_show) {
        auto* cdromLogger = g_emulator->m_cdromLogger.get();
        if (!cdromLogger->isEnabled()) cdromLogger->enable();
        cdromLogger->uploadHeatmaps();
        m_cdromViewer.draw(this);
    } else {
        auto* cdromLogger = g_emulator->m_cdromLogger.get();
        if (cdromLogger->isEnabled()) cdromLogger->disable();
    }

    if (m_log.m_show) {
        ImGui::SetNextWindowPos(ImVec2(10, 540), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(1200, 250), ImGuiCond_FirstUseEver);
        changed |= m_log.draw(this, _("Logs"));
    }

    if (m_luaConsole.m_show) {
        ImGui::SetNextWindowPos(ImVec2(15, 545), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(1200, 250), ImGuiCond_FirstUseEver);
        m_luaConsole.draw(_("Lua Console"), this);
    }

    if (m_luaInspector.m_show) {
        ImGui::SetNextWindowPos(ImVec2(20, 550), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(1200, 250), ImGuiCond_FirstUseEver);
        m_luaInspector.draw(_("Lua Inspector"), *g_emulator->m_lua, this);
    }
    if (m_luaEditor.m_show) {
        m_luaEditor.draw(_("Lua Editor"), this);
    }
    if (m_events.m_show) {
        m_events.draw(g_emulator->m_mem->getMemoryAsFile(), _("Kernel events"));
    }
    if (m_handlers.m_show) {
        m_handlers.draw(reinterpret_cast<const uint32_t*>(g_emulator->m_mem->m_wram), _("Kernel handlers"));
    }
    if (m_kernelLog.m_show) {
        changed |= m_kernelLog.draw(g_emulator->m_cpu.get(), _("Kernel Calls"));
    }
    if (m_callstacks.m_show) {
        m_callstacks.draw(_("Callstacks"), this);
    }

    {
        IO<File> memFile = g_emulator->m_mem->getMemoryAsFile();
        unsigned counter = 0;
        for (auto& editor : m_mainMemEditors) {
            if (editor.m_show) {
                ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
                editor.draw(memFile, 8 * 1024 * 1024);
            }
            counter++;
        }
        if (m_parallelPortEditor.m_show) {
            ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
            m_parallelPortEditor.draw(memFile, 512 * 1024);
        }
        counter++;
        if (m_scratchPadEditor.m_show) {
            ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
            m_scratchPadEditor.draw(memFile, 1024);
        }
        counter++;
        if (m_hwrEditor.m_show) {
            ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
            m_hwrEditor.draw(memFile, 8 * 1024);
        }
        counter++;
        if (m_biosEditor.m_show) {
            ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
            m_biosEditor.draw(memFile, 512 * 1024);
        }
        counter++;
        if (m_spuRamEditor.m_show) {
            ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);
            // Null until the SPU has been init'd; the editor would otherwise read off a wild pointer.
            auto* spuRam = g_emulator->m_spu->getSpuRam();
            if (spuRam) m_spuRamEditor.draw(spuRam, SPUInterface::c_spuRamSize);
        }
        counter++;
        if (m_vramEditor.m_show) {
            ImGui::SetNextWindowPos(ImVec2(520, 30 + 10 * counter), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(484, 480), ImGuiCond_FirstUseEver);

            const Slice vram = g_emulator->m_gpu->getVRAM();
            auto* vramData = (const ImU8*)vram.data();
            size_t vramSize = vram.size();
            m_vramEditor.editor.ReadFn = [vramData](size_t off) -> ImU8 { return vramData[off]; };
            m_vramEditor.editor.WriteFn = [vramData](size_t off, ImU8 writtenByte) {
                constexpr size_t vramWidth = 1024;
                constexpr size_t stride = vramWidth * sizeof(uint16_t);

                const auto x = (off % stride) / sizeof(uint16_t);
                const auto y = off / stride;
                const bool offsetIsOdd = (off & 1) == 1;
                const auto maskedOffset = off & ~(size_t)1;
                uint16_t newPixel;

                if (offsetIsOdd) {
                    newPixel = (writtenByte << 8) | vramData[maskedOffset];
                } else {
                    newPixel = writtenByte | (vramData[maskedOffset + 1] << 8);
                }

                g_emulator->m_gpu->partialUpdateVRAM(x, y, 1, 1, &newPixel);
            };
            m_vramEditor.editor.Cache.BulkReadFn = [vramData](void* dest, size_t off, size_t len) {
                memcpy(dest, vramData + off, len);
            };
            m_vramEditor.editor.DrawWindow(m_vramEditor.title(), vramSize);
        }
    }

    if (m_memcardManager.m_show) {
        changed |= m_memcardManager.draw(this, _("Memory Card Manager"));
    }

    if (m_registers.m_show) {
        m_registers.draw(this, &g_emulator->m_cpu->m_regs, _("Registers"));
    }

    if (m_assembly.m_show) {
        changed |= m_assembly.draw(this, &g_emulator->m_cpu->m_regs, g_emulator->m_mem.get(), _("Assembly"));
    }

    if (m_disassembly.m_show && g_emulator->m_cpu->isDynarec()) {
        m_disassembly.draw(this, _("DynaRec Disassembler"));
    }

    m_cpuTrace.draw(this, _("CPU Trace"));

    if (m_breakpoints.m_show) {
        m_breakpoints.draw(_("Breakpoints"));
    }

    if (m_patches.m_show) {
        m_patches.draw(_("Patches"));
    }

    if (m_namedSaveStates.m_show) {
        m_namedSaveStates.draw(this, _("Named Save States"));
    }

    if (m_memoryObserver.m_show) {
        m_memoryObserver.draw(_("Memory Observer"));
    }

    if (m_msanViewer.m_show) {
        m_msanViewer.draw(this, g_emulator->m_mem.get(), _("MSAN Viewer"));
    }

    if (m_typedDebugger.m_show) {
        m_typedDebugger.draw(_("Typed Debugger"), this);
    }

    if (m_showAbout) changed |= about();
    if (m_showInterruptsScaler) interruptsScaler();

    if (m_outputShaderEditor.m_show && m_outputShaderEditor.draw(this, _("Output Video"))) {
        // maybe throttle this?
        m_outputShaderEditor.compile(this);
    }

    if (m_offscreenShaderEditor.m_show && m_offscreenShaderEditor.draw(this, _("Offscreen Render"))) {
        // maybe throttle this?
        m_offscreenShaderEditor.compile(this);
    }

    if (m_pioCart.m_show) {
        changed |= m_pioCart.draw(_("PIO Cartridge Configuration"));
    }

    if (m_hwRegs.m_show) {
        m_hwRegs.draw(this, g_emulator->m_mem.get(), _("HW Registers"));
    }

    if (m_sio1.m_show) {
        m_sio1.draw(this, &PCSX::g_emulator->m_sio1->m_regs, _("SIO1 Debug"));
    }

    if (m_isoBrowser.m_show) {
        m_isoBrowser.draw(g_emulator->m_cdrom.get(), _("ISO Browser"));
    }
    m_isoBrowser.drawChildren();

    if (m_network.m_show) changed |= m_network.draw(this, _("Network"));
    if (m_dalos.m_show) {
        m_dalos.draw(_("Dalos Canvas"));
    }
    m_dalos.fileViewers().draw();

    if (m_showCfg) changed |= configure();
    if (g_emulator->m_spu->m_showCfg) changed |= g_emulator->m_spu->configure();
    g_emulator->m_spu->debug();
    changed |= g_emulator->m_pads->configure(this);

    if (g_emulator->m_gpu->m_showCfg) changed |= g_emulator->m_gpu->configure();
    if (g_emulator->m_gpu->m_showDebug) g_emulator->m_gpu->debug();
    if (m_gpuLogger.m_show) m_gpuLogger.draw(g_emulator->m_gpuLogger.get(), _("GPU Logger"), !g_system->running());
    if (m_gpuDump.m_show) m_gpuDump.draw(_("GPU Dump"));
    // Pushed unconditionally: capture follows the setting, not whether the window is open.
    m_spuLoggerWidget.syncEnabled(g_emulator->m_spuLogger.get());
    if (m_spuLoggerWidget.m_show) m_spuLoggerWidget.draw(g_emulator->m_spuLogger.get(), _("SPU Logger"));
    if (m_spuSamplesWidget.m_show) {
        m_spuSamplesWidget.draw(g_emulator->m_spuLogger.get(), g_emulator->m_spu->getSpuRam(), _("SPU Samples"));
    }
    if (m_heapViewer.m_show) m_heapViewer.draw(g_emulator->m_mem.get(), _("PSYQo Heap Viewer"));
    if (m_allocTrackerViewer.m_show) m_allocTrackerViewer.draw(_("Allocator Tracker"));

    if (m_showUiCfg) {
        if (ImGui::Begin(_("UI Configuration"), &m_showUiCfg)) {
            changed |= showThemes();
            bool needFontReload = false;
            {
                std::string currentLocale = g_system->localeName();
                if (currentLocale.length() == 0) currentLocale = "English";
                if (ImGui::BeginCombo(_("Locale"), currentLocale.c_str())) {
                    if (ImGui::Selectable("English", currentLocale == "English")) {
                        g_system->activateLocale("English");
                        g_emulator->settings.get<Emulator::SettingLocale>() = "English";
                        needFontReload = true;
                    }
                    for (auto& l : g_system->localesNames()) {
                        if (ImGui::Selectable(l.c_str(), currentLocale == l)) {
                            g_system->activateLocale(l);
                            g_emulator->settings.get<Emulator::SettingLocale>() = l;
                            needFontReload = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button(_("Reload locales"))) {
                    g_system->loadAllLocales();
                    g_system->activateLocale(currentLocale);
                }
            }
            needFontReload |= ImGui::SliderInt(_("Main Font Size"), &settings.get<MainFontSize>().value, 8, 48);
            needFontReload |= ImGui::SliderInt(_("Mono Font Size"), &settings.get<MonoFontSize>().value, 8, 48);
            bool ratioChanged =
                ImGui::Checkbox(_("Use Widescreen Aspect Ratio"), &settings.get<WidescreenRatio>().value);
            ImGuiHelpers::ShowHelpMarker(_(R"(Sets the output screen ratio to 16:9 instead of 4:3.

Note that this setting is NOT the popular hack
to force games into widescreen mode, but rather an
emulation of the fact users could change their
TV sets to be in 16:9 mode instead of 4:3.

Some games have a "wide screen" rendering setting,
which changes the rendering of the game accordingly,
and requires the user to change the settings of
their TV set to match the aspect ratio of the game.)"));
            changed |= needFontReload | ratioChanged;
            if (needFontReload) m_reloadFonts = true;
            if (ratioChanged) m_setupScreenSize = true;
        }
        ImGui::End();
    }

    if (m_showSysCfg) {
        if (ImGui::Begin(_("System Configuration"), &m_showSysCfg)) {
            changed |=
                ImGui::Checkbox(_("Preload Disk Image files"), &emuSettings.get<Emulator::SettingFullCaching>().value);
            changed |= ImGui::Checkbox(_("Enable Auto Update"), &emuSettings.get<Emulator::SettingAutoUpdate>().value);
        }
        ImGui::End();
    }

    if (!g_system->getArgs().isUpdateDisabled() && !g_system->getVersion().failed() &&
        g_system->getVersion().hasUpdateInfo() && !emuSettings.get<Emulator::SettingShownAutoUpdateConfig>().value) {
        if (ImGui::Begin(_("Update configuration"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted((_(R"(PCSX-Redux can automatically update itself.

If you enable the auto update option, it will check for new updates
on startup. No personal data nor identifiable token is sent for this
process. The update server is run by the PCSX-Redux project behind
Cloudflare, which can see your IP address like any CDN.

If an update is available, you will get prompted to download and
install it. You can still download versions of PCSX-Redux as usual
from its website.

If you want to change this setting later, you can go to the
Configuration -> System menu.)")));
            if (ImGui::Button(_("Enable auto update"))) {
                emuSettings.get<Emulator::SettingShownAutoUpdateConfig>().value = true;
                emuSettings.get<Emulator::SettingAutoUpdate>().value = true;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(_("No thanks"))) {
                emuSettings.get<Emulator::SettingShownAutoUpdateConfig>().value = true;
                changed = true;
            }
        }
        ImGui::End();
    }

    if (m_updateAvailable) {
        if (ImGui::Begin(_("Update available"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_update.canFullyApply()) {
                ImGui::TextUnformatted((_(R"(An update is available.
Click 'Update' to download and apply the update.
PCSX-Redux will automatically restart to apply it.

Click 'Download' to use your browser to download
the update and manually apply it.)")));
            } else {
                ImGui::TextUnformatted((_(R"(An update is available.
Click 'Update' to download it. While the update can be
downloaded, it won't be applied automatically. You will
have to install it manually, the way you previously did.
PCSX-Redux will quit once the update is downloaded.

Click 'Download' to use your browser to download
the update and manually apply it.)")));
            }
            ImGui::ProgressBar(m_update.progress());
            if (!m_updateDownloading) {
                if (ImGui::Button(C_("Update dialog button", "Update"))) {
                    m_updateDownloading = true;
                    bool started = m_update.downloadAndApplyUpdate(
                        g_system->getVersion(),
                        [this](bool success) {
                            m_updateAvailable = false;
                            m_updateDownloading = false;
                            if (success) {
                                m_update.applyUpdate(g_system->getBinDir());
                                g_system->quit();
                            } else {
                                addNotification(
                                    _("An error has occured while downloading\nand/or applying the update."));
                            }
                        },
                        g_system->getLoop());
                    if (!started) {
                        addNotification(_("An error has occured while downloading\nand/or applying the update."));
                        m_updateAvailable = false;
                        m_updateDownloading = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(C_("Update dialog button", "Download"))) {
                    m_updateDownloading = true;
                    bool started = m_update.getDownloadUrl(
                        g_system->getVersion(),
                        [this](std::string url) {
                            openUrl(url);
                            m_updateDownloading = false;
                        },
                        g_system->getLoop());
                    if (!started) {
                        addNotification(_("An error has occured while downloading the update."));
                        m_updateAvailable = false;
                        m_updateDownloading = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(_("Cancel"))) {
                    m_updateAvailable = false;
                }
            }
        }
        ImGui::End();
    }

    if (m_showHandles) {
        if (ImGui::Begin(_("UvFiles"), &m_showHandles)) {
            std::string rate;
            byteRateToString(UvFile::getReadRate(), rate);
            ImGui::Text(_("Read rate: %s"), rate.c_str());
            byteRateToString(UvFile::getWriteRate(), rate);
            ImGui::Text(_("Write rate: %s"), rate.c_str());
            byteRateToString(UvFile::getDownloadRate(), rate);
            ImGui::Text(_("Download rate: %s"), rate.c_str());
            if (ImGui::BeginTable("UvFiles", 2, ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn(_("Caching"));
                ImGui::TableSetupColumn(_("Filename"));
                ImGui::TableHeadersRow();
                UvThreadOp::iterateOverAllOps([](UvThreadOp* f) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (f->caching()) {
                        ImGui::ProgressBar(f->cacheProgress());
                    } else {
                        std::string label = fmt::format("Cache##{}", reinterpret_cast<void*>(f));
                        auto canCache = f->canCache();
                        if (!canCache) {
                            ImGui::BeginDisabled();
                        }
                        if (ImGui::Button(label.c_str()) && canCache) {
                            f->startCaching();
                        }
                        if (!canCache) {
                            ImGui::EndDisabled();
                        }
                    }
                    ImGui::TableSetColumnIndex(1);
                    File* actual = dynamic_cast<File*>(f);
                    ImGui::TextUnformatted(actual->filename().string().c_str());
                });
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    auto L = *g_emulator->m_lua;
    L.getfield("DrawImguiFrame", LUA_GLOBALSINDEX);
    if (!L.isnil()) {
        ScopedOnlyLog(this);
        try {
            L.pcall();
            bool gotGLerror = false;
            for (const auto& error : m_glErrors) {
                m_luaConsole.addError(error);
                if (g_system->getArgs().isLuaStdoutEnabled()) {
                    fputs(error.c_str(), stderr);
                    fputc('\n', stderr);
                }
                gotGLerror = true;
            }
            m_glErrors.clear();
            if (gotGLerror) throw("OpenGL error while running Lua code");
        } catch (...) {
            L.push("DrawImguiFrame");
            L.push();
            L.settable(LUA_GLOBALSINDEX);
        }
        // A script that errors (or forgets to close) between ImPlot Begin* and End* leaves ImPlot's
        // current plot/subplot set, and the next native BeginPlot would then throw its mismatch assert.
        // Calling End* here is unsafe since the enclosing ImGui window may already be gone, so just drop
        // the dangling state; ImGui's own error recovery takes care of the ID stack.
        ImPlotContext* gp = ImPlot::GetCurrentContext();
        if (gp && (gp->CurrentPlot || gp->CurrentSubplot || gp->CurrentAlignmentH || gp->CurrentAlignmentV)) {
            ImPlot::ResetCtxForNextPlot(gp);
            ImPlot::ResetCtxForNextSubplot(gp);
            ImPlot::ResetCtxForNextAlignedPlots(gp);
            gp->CurrentItems = nullptr;
        }
    } else {
        L.pop();
    }
    m_notifier.draw();
    m_passiveNotifier.draw();

    if (m_showResetSettings) {
        ImGui::OpenPopup(_("Reset settings"));
        m_showResetSettings = false;
    }
    if (ImGui::BeginPopupModal(_("Reset settings"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            _("This will restore every setting to its default value, including the window layout, the input "
              "bindings, the paths to the BIOS and the memory cards, and the contents of the shader "
              "editors.\n\nSave states and memory card contents are left alone.\n\nThe emulator will reboot to "
              "complete the operation."));
        ImGui::Separator();
        if (ImGui::Button(_("Reset and reboot"), ImVec2(160, 0))) {
            ImGui::CloseCurrentPopup();
            resetSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Render();
    glViewport(0, 0, w, h);
    if (m_fullWindowRender) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    } else {
        glClearColor(m_backgroundColor.x, m_backgroundColor.y, m_backgroundColor.z, m_backgroundColor.w);
    }
    GUIPlatform::clearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    renderTvgViewport(platform_io.Viewports[0], m_window);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        // Skip the main viewport (index 0), which is always fully handled by the application!
        for (int i = 1; i < platform_io.Viewports.Size; i++) {
            ImGuiViewport* viewport = platform_io.Viewports[i];
            if (viewport->Flags & ImGuiViewportFlags_IsMinimized) continue;
            if (platform_io.Platform_RenderWindow) platform_io.Platform_RenderWindow(viewport, nullptr);
            if (platform_io.Renderer_RenderWindow) platform_io.Renderer_RenderWindow(viewport, nullptr);
            renderTvgViewport(viewport, getSDLWindowFromImGuiViewport(viewport));
            if (platform_io.Platform_SwapBuffers) platform_io.Platform_SwapBuffers(viewport, nullptr);
            if (platform_io.Renderer_SwapBuffers) platform_io.Renderer_SwapBuffers(viewport, nullptr);
        }
        SDL_GL_MakeCurrent(m_window, m_glContext);
    }
    GUIPlatform::presentFrame(m_window);
    // Per-frame paints are dropped whether or not their viewport got rendered this frame.
    for (auto& [id, vp] : m_tvgViewports) vp.transient->remove();

    if (changed) saveCfg();
    if (m_gotImguiUserError) {
        g_system->log(LogClass::UI, "Got ImGui User Error: %s\n", m_imguiUserError.c_str());
        m_gotImguiUserError = false;
        L.push();
        L.setfield("DrawImguiFrame", LUA_GLOBALSINDEX);
    }

    FrameMark;
}

bool PCSX::GUI::configure() {
    bool changed = false;
    bool selectBiosDialog = false;
    bool selectEXP1Dialog = false;
    bool showDynarecDebugWarning = false;
    bool showDynarecWarning = false;
    bool show2MBVRAMHardwareWarning = false;
    auto& settings = g_emulator->settings;
    auto& debugSettings = settings.get<Emulator::SettingDebugSettings>();

    ImGui::SetNextWindowPos(ImVec2(50, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Emulation Configuration"), &m_showCfg)) {
        if (ImGui::SliderInt(_("Idle Swap Interval"), &m_idleSwapInterval, 0, 10)) {
            changed = true;
            if (!g_system->running()) SDL_GL_SetSwapInterval(m_idleSwapInterval);
        }
        ImGui::Separator();
        // Emulation speed now lives at the audio sink (SPU::Speed), set in the SPU configuration window.
        changed |= ImGui::Checkbox(_("Enable XA decoder"), &settings.get<Emulator::SettingXa>().value);
        changed |= ImGui::Checkbox(_("Always enable SPU IRQ"), &settings.get<Emulator::SettingSpuIrq>().value);
        changed |= ImGui::Checkbox(_("Decode MDEC videos in B&W"), &settings.get<Emulator::SettingBnWMdec>().value);
        if (ImGui::Checkbox(_("Dynarec CPU"), &settings.get<Emulator::SettingDynarec>().value)) {
            changed = true;
            showDynarecWarning = true;
            if (debugSettings.get<PCSX::Emulator::DebugSettings::Debug>() && settings.get<Emulator::SettingDynarec>()) {
                showDynarecDebugWarning = true;
            }
        }

        ImGuiHelpers::ShowHelpMarker(_(R"(Activates the dynamic recompiler CPU core.
It is significantly faster than the interpreted CPU,
however it doesn't play nicely with the debugger.
Changing this setting requires a reboot to take effect.
The dynarec core isn't available for all CPUs, so
this setting may not have any effect for you.)"));
        bool memChanged = ImGui::Checkbox(_("8MB"), &settings.get<Emulator::Setting8MB>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(Emulates an installed 8MB system,
instead of the normal 2MB. Useful for working
with development binaries and games.)"));
        changed |= ImGui::InputScalar(_("Memory Fill Value"), ImGuiDataType_U8,
                                      (void*)&settings.get<Emulator::SettingMemoryFillValue>().value, nullptr, nullptr,
                                      "%02x", 0);
        ImGuiHelpers::ShowHelpMarker(_(R"(The SDRAM of the console will be
filled with this value at startup.
On real hardware, the SDRAM decays over time,
and its contents may not be 0 on cold boot.
This setting is useful for debugging purposes,
to detect uninitialized memory reads that may
cause issues in some cases.
)"));
        if (ImGui::Checkbox(_("OpenGL GPU *ALPHA STATE*"), &settings.get<Emulator::SettingHardwareRenderer>().value)) {
            changed = true;
            if (settings.get<Emulator::SettingHardwareRenderer>() && settings.get<Emulator::Setting2MBVRAM>()) {
                show2MBVRAMHardwareWarning = true;
            }
        }
        ImGuiHelpers::ShowHelpMarker(_(R"(Enables the OpenGL GPU renderer.
This is not recommended for normal use at the moment,
as it is not fully implemented yet. It is recommended
to use the software renderer instead. Requires a restart
when changing this setting.)"));
        if (ImGui::Checkbox(_("2MB VRAM"), &settings.get<Emulator::Setting2MBVRAM>().value)) {
            changed = true;
            if (settings.get<Emulator::SettingHardwareRenderer>() && settings.get<Emulator::Setting2MBVRAM>()) {
                show2MBVRAMHardwareWarning = true;
            }
        }
        ImGuiHelpers::ShowHelpMarker(_(R"(Emulates a console fitted with 2MB of VRAM
instead of the stock 1MB, exposing the upper bank through
GP1(09h). Found on a handful of modded consoles and on
some arcade boards. Only the software renderer supports
it; the OpenGL GPU does not. Requires a restart when
changing this setting.)"));

        ImGui::Separator();
        {
            int interval = settings.get<Emulator::SettingRewindInterval>().value;
            if (ImGui::SliderInt(_("Rewind capture interval (frames; 0 = disabled)"), &interval, 0, 60)) {
                settings.get<Emulator::SettingRewindInterval>().value = interval < 0 ? 0 : interval;
                changed = true;
            }
            int count = settings.get<Emulator::SettingRewindCount>().value;
            if (ImGui::SliderInt(_("Rewind history depth (snapshots)"), &count, 1, 600)) {
                settings.get<Emulator::SettingRewindCount>().value = count < 1 ? 1 : count;
                changed = true;
            }
            ImGuiHelpers::ShowHelpMarker(_(R"(Captures an in-memory save state every N frames
and keeps the last M of them; press Backspace (or
Emulation -> Rewind) to step backwards through them.
Each snapshot is currently a full state (~18MB), so
depth x 18MB is roughly the memory cost. Set the
interval to 0 to disable.)"));
        }

        if (memChanged) {
            changed = true;
            g_emulator->m_mem->setLuts();
        }

        {
            static const std::function<const char*()> types[] = {l_("Auto"), l_("NTSC"), l_("PAL")};
            auto& autodetect = settings.get<Emulator::SettingAutoVideo>().value;
            auto& type = settings.get<Emulator::SettingVideo>().value;
            if (ImGui::BeginCombo(_("System Type"), types[autodetect ? 0 : (type + 1)]())) {
                if (ImGui::Selectable(types[0](), autodetect)) {
                    changed = true;
                    autodetect = true;
                }
                if (ImGui::Selectable(types[1](), !autodetect && (type == PCSX::Emulator::PSX_TYPE_NTSC))) {
                    changed = true;
                    type = PCSX::Emulator::PSX_TYPE_NTSC;
                    autodetect = false;
                }
                if (ImGui::Selectable(types[2](), !autodetect && (type == PCSX::Emulator::PSX_TYPE_PAL))) {
                    changed = true;
                    type = PCSX::Emulator::PSX_TYPE_PAL;
                    autodetect = false;
                }
                ImGui::EndCombo();
            }
        }

        changed |= ImGui::Checkbox(_("Fast boot"), &settings.get<Emulator::SettingFastBoot>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(This will cause the BIOS to skip the shell,
which may include additional checks.
Also will make the boot time substantially
faster by not displaying the logo.)"));
        changed |=
            ImGui::Checkbox(_("Randomize CD-ROM timings"), &settings.get<Emulator::SettingCDRomEntropySeed>().value);
        ImGuiHelpers::ShowHelpMarker(_(R"(The CD-ROM drive timings carry a small
amount of jitter, like a real drive. By default
it is the same on every run, so a session can be
replayed exactly. Enabling this picks a different
sequence at each power-on.)"));
        auto bios = settings.get<Emulator::SettingBios>().string();
        ImGui::InputText(_("BIOS file"), const_cast<char*>(reinterpret_cast<const char*>(bios.c_str())),
                         bios.length() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        selectBiosDialog = ImGui::Button("...");
        if (ImGui::Checkbox(_("Enable Debugger"), &debugSettings.get<Emulator::DebugSettings::Debug>().value)) {
            changed = true;
            if (debugSettings.get<PCSX::Emulator::DebugSettings::Debug>() && settings.get<Emulator::SettingDynarec>()) {
                showDynarecDebugWarning = true;
            }
        }

        ImGuiHelpers::ShowHelpMarker(_(R"(This will enable the usage of various breakpoints
throughout the execution of mips code. Enabling this
can slow down emulation to a noticeable extent.)"));
    }
    ImGui::End();

    {  // Select BIOS Dialog
        auto& biospath = settings.get<Emulator::SettingBiosBrowsePath>();
        if (selectBiosDialog) {
            if (!biospath.empty()) {
                m_selectBiosDialog.m_currentPath = biospath.value;
            }

            m_selectBiosDialog.openDialog();
        }
        if (m_selectBiosDialog.draw()) {
            biospath = m_selectBiosDialog.m_currentPath;
            std::vector<PCSX::u8string> fileToOpen = m_selectBiosDialog.selected();
            if (!fileToOpen.empty()) {
                settings.get<Emulator::SettingBios>().value = fileToOpen[0];
                changed = true;
            }
        }
    }
    if (showDynarecDebugWarning && showDynarecWarning) {
        addNotification(R"(Debugger and dynarec enabled at the same time.
Consider turning either one off, otherwise
debugging features may not work. Additionally,
changing the dynarec option requires a restart
of the emulator to take effect.)");
    } else if (showDynarecDebugWarning) {
        addNotification(R"(Debugger and dynarec enabled at the same time.
Consider turning either one off, otherwise
debugging features may not work.)");
    } else if (showDynarecWarning) {
        addNotification(R"(Toggling the Dynarec option requires a restart
of the emulator to take effect.)");
    }
    if (show2MBVRAMHardwareWarning) {
        addNotification(R"(2MB VRAM is enabled together with the OpenGL GPU.
The OpenGL renderer does not support the upper VRAM
bank yet - only the software renderer does. Switch to
the software renderer to make use of 2MB VRAM.)");
    }
    return changed;
}

void PCSX::GUI::interruptsScaler() {
    static const char* names[] = {
        "SIO",          "SIO1",    "CDR FIFO",    "CDR Command", "CDR Reads", "GPU DMA",
        "MDEC Out DMA", "SPU DMA", "MDEC In DMA", "GPU OTC DMA", "CDR DMA",
    };
    static_assert(std::size(names) == std::extent_v<decltype(R3000Acpu::m_scheduleScales)>);
    if (ImGui::Begin(_("Scheduler Scaler"), &m_showInterruptsScaler)) {
        if (ImGui::Button(_("Reset all"))) {
            for (auto& scale : g_emulator->m_cpu->m_scheduleScales) {
                scale = 1.0f;
            }
        }
        unsigned counter = 0;
        for (auto& scale : g_emulator->m_cpu->m_scheduleScales) {
            ImGui::SliderFloat(names[counter], &scale, 0.0f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            counter++;
        }
    }
    ImGui::End();
}

bool PCSX::GUI::showThemes() {
    static const std::function<const char*()> imgui_themes[] = {
        l_("Default theme##Theme name"), l_("Classic##Theme name"), l_("Light##Theme name"), l_("Cherry##Theme name"),
        l_("Mono##Theme name"),          l_("Dracula##Theme name"), l_("Olive##Theme name"),
    };
    auto changed = false;
    auto& currentTheme = g_emulator->settings.get<Emulator::SettingGUITheme>().value;

    if (ImGui::BeginCombo(_("Themes"), imgui_themes[currentTheme](), ImGuiComboFlags_HeightLarge)) {
        for (int n = 0; n < IM_ARRAYSIZE(imgui_themes); n++) {
            bool selected = (currentTheme == n);
            if (ImGui::Selectable(imgui_themes[n](), selected)) {
                currentTheme = n;
                changed = true;
                applyTheme(n);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    return changed;
}

bool PCSX::GUI::about() {
    bool changed = false;

    ImGui::SetNextWindowPos(ImVec2(200, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(880, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("About"), &m_showAbout)) {
        ImGui::TextUnformatted("PCSX-Redux");
        ImGui::Separator();
        auto someString = [](const char* str, GLenum index) {
            const char* value = (const char*)glGetString(index);
            ImGui::TextWrapped("%s: %s", str, value);
        };
        if (ImGui::BeginTabBar("AboutTabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem(_("Version"))) {
                auto& version = g_system->getVersion();
                if (version.failed()) {
                    ImGui::BeginDisabled();
                }
                if (version.failed()) {
                    ImGui::EndDisabled();
                    ImGui::TextUnformatted(_("No version information.\n\nProbably built from source."));
                } else {
                    if (ImGui::Button(_("Copy to clipboard"))) {
                        if (version.buildId.has_value()) {
                            clip::set_text(
                                fmt::format("Version: {}\nBuild: {}\nChangeset: {}\nDate & time: {:%Y-%m-%d %H:%M:%S}",
                                            version.version, version.buildId.value(), version.changeset,
                                            *std::localtime(&version.timestamp)));
                        } else {
                            clip::set_text(fmt::format("Version: {}\nChangeset: {}\nDate & time: {:%Y-%m-%d %H:%M:%S}",
                                                       version.version, version.changeset,
                                                       *std::localtime(&version.timestamp)));
                        }
                    }
                    ImGui::Text(_("Version: %s"), version.version.c_str());
                    if (version.buildId.has_value()) {
                        ImGui::Text(_("Build: %i"), version.buildId.value());
                    }
                    ImGui::TextUnformatted(_("Changeset:"));
                    ImGui::SameLine();
                    if (ImGui::SmallButton(version.changeset.c_str())) {
                        openUrl(fmt::format("https://github.com/pcsx-redux/pcsx-redux/commit/{}", version.changeset));
                    }
                    std::tm tm = *std::localtime(&version.timestamp);
                    std::string timestamp = fmt::format("{:%Y-%m-%d %H:%M:%S}", tm);
                    ImGui::Text(_("Date & time: %s"), timestamp.c_str());
                }
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags flag = 0;
            if (m_aboutSelectAuthors) {
                flag |= ImGuiTabItemFlags_SetSelected;
                m_aboutSelectAuthors = false;
            }
            if (ImGui::BeginTabItem(_("Authors"), nullptr, flag)) {
                ImGui::BeginChild("Authors", ImVec2(0, 0), true);
                ImGui::TextUnformatted(
#include "AUTHORS"
                );
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(_("Licenses"))) {
                ImGui::BeginChild("Licenses", ImVec2(0, 0), true);

                MarkDown md(this, {{"AUTHORS", [this]() { m_aboutSelectAuthors = true; }}});
                std::string_view text =
#include "LICENSES.md"
                    ;
                md.print(text);

                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(_("OpenGL information"))) {
                if (m_reportGLErrors) {
                    ImGui::TextUnformatted(_("OpenGL error reporting: enabled"));
                } else {
                    ImGui::TextUnformatted(_("OpenGL error reporting: disabled"));
                    if (!glDebugMessageCallback) {
                        ImGuiHelpers::ShowHelpMarker(
                            _("OpenGL error reporting has been disabled because your OpenGL driver is too old. Error "
                              "reporting requires at least OpenGL 4.3. Please update your graphics drivers, or "
                              "contact your GPU vendor for up to date OpenGL drivers. Disabled OpenGL error reporting "
                              "won't have a negative impact on the performance of this software, but user code such "
                              "as the shader editor won't be able to properly report problems accurately."));
                    }
                }

                changed |= ImGui::Checkbox(_("Enable OpenGL error reporting"),
                                           &g_emulator->settings.get<Emulator::SettingGLErrorReporting>().value);
                ImGuiHelpers::ShowHelpMarker(
                    _("OpenGL error reporting is necessary for properly reporting OpenGL problems. "
                      "However it requires OpenGL 4.3+ and might have performance repercussions on "
                      "some computers. (Requires a restart of the emulator)"));
                changed |= ImGui::SliderInt(
                    _("OpenGL error reporting severity"),
                    &g_emulator->settings.get<Emulator::SettingGLErrorReportingSeverity>().value, 0, 3);

                ImGui::Text(_("Core profile: %s"), m_hasCoreProfile ? _("yes") : _("no"));
                someString(_("Vendor"), GL_VENDOR);
                someString(_("Renderer"), GL_RENDERER);
                someString(_("Version"), GL_VERSION);
                someString(_("Shading language version"), GL_SHADING_LANGUAGE_VERSION);
                GLint textureUnits;
                glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textureUnits);
                ImGui::Text(_("Texture units: %d"), textureUnits);
                GLint n, i;
                glGetIntegerv(GL_NUM_EXTENSIONS, &n);
                ImGui::TextUnformatted(_("Extensions:"));
                ImGui::BeginChild("GLextensions", ImVec2(0, 0), true);
                for (i = 0; i < n; i++) {
                    const char* extension = (const char*)glGetStringi(GL_EXTENSIONS, i);
                    ImGui::Text("%s", extension);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            aboutFFmpegTab();
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    return changed;
}

void PCSX::GUI::update(bool vsync) {
    glDisable(GL_SCISSOR_TEST);
    endFrame();
    startFrame();
    if (m_presentAfterHardReset) {
        m_presentAfterHardReset = false;
        g_emulator->m_gpu->presentFromGui();
        flip();
    }
    // At all times, either the emulated GPU core or the GUI have full control of the host GPU & the GL context
    // We do this by having the emulated GPU have it most of the time, then let the GUI steal it when it needs it.
    // And in the line afterwards, the GUI gives the GL context back to the emulated GPU.
    g_emulator->m_gpu->setOpenGLContext();
}

void PCSX::GUI::magicOpen(const char* pathStr) {
    std::filesystem::path path = pathStr;
    bool success = false;
    IO<File> file = new PosixFile(path);

    // Is this something that looks like a binary we can load ?
    try {
        BinaryLoader::Info info;
        std::map<uint32_t, std::string> symbols;
        success = BinaryLoader::load(file, new Mem4G(), info, symbols);
        if (success) success = info.pc.has_value();
    } catch (...) {
        success = false;
    }

    if (success) {
        m_exeToLoad.set(path.u8string());
        g_system->log(LogClass::UI, "Scheduling to load %s and soft reseting.\n", path.string());
        g_system->softReset();
        return;
    }

    // Is this something that looks like a bios maybe ?
    if (file->size() == 512 * 1024) {
        // bit of a crude way to check if it's a bios, but it's good enough
        // we're assuming all bioses start with setting the timings for the
        // bios bus, which is basically these instructions:

        //     li    $t0, (19 << 16) | 0x243f
        //     sw    $t0, SBUS_DEV2_CTRL
        //     nop

        // this code translates to the following bytes:
        static constexpr uint8_t biosSignature[] = {0x13, 0x00, 0x08, 0x3c, 0x3f, 0x24, 0x08, 0x35, 0x80, 0x1f,
                                                    0x01, 0x3c, 0x10, 0x10, 0x28, 0xac, 0x00, 0x00, 0x00, 0x00};
        // we could potentially also check the rest, but I think some bioses
        // may differ there, so we'll just check the first 20 bytes for now

        uint8_t buffer[sizeof(biosSignature)];
        file->readAt(buffer, sizeof(buffer), 0);

        if (memcmp(buffer, biosSignature, sizeof(biosSignature)) == 0) {
            g_system->hardReset();
            g_system->log(LogClass::UI, "Temporary overriding bios with %s and hard resetting.\n", path.string());
            file->readAt(g_emulator->m_mem->m_bios, 512 * 1024, 0);
            return;
        }
    }

    // Maybe it's a zip file to add to our list of Lua archive ?
    {
        uint32_t signature = file->readAt<uint32_t>(0);
        try {
            switch (signature) {
                case 0x02014b50:
                case 0x04034b50:
                case 0x06054b50: {
                    auto& archive = LuaFFI::addArchive(*g_emulator->m_lua, file);
                    if (!archive.failed()) {
                        g_system->log(LogClass::UI, "Added %s to our list of loaded archives.\n", path.string());
                        return;
                    }
                    break;
                }
            }
        } catch (...) {
        }
    }

    // Iso loader is last because its detection is the most broken at the moment.
    g_emulator->m_cdrom->setIso(new CDRIso(path));
    g_emulator->m_cdrom->parseIso();
}

bool PCSX::GUI::getSaveStateExists(uint32_t slot) {
    if (slot >= 10) {
        return false;
    }
    auto saveFilename = buildSaveStateFilename(slot);
    return saveStateExists(saveFilename);
}

std::string PCSX::GUI::getSaveStatePrefix(bool includeSeparator) {
    // the ID of the game. Every savestate is marked with the ID of the game it's from.
    const auto gameID = g_emulator->m_cdrom->getCDRomID();
    // Check if the game has a non-NULL ID or a game hasn't been loaded. Some stuff like PS-X
    // EXEs don't have proper IDs
    if (!gameID.empty()) {
        // For games with an ID of SLUS00213 for example, this will generate a state named
        // SLUS00213.sstate
        return fmt::format("{}{}", gameID, includeSeparator ? "_" : "");
    } else {
        // For games without IDs, identify them via filename
        const auto& iso = g_emulator->m_cdrom->getIso()->getIsoPath().filename();
        const auto lastFile = iso.empty() ? "BIOS" : iso.string();
        return fmt::format("{}{}", lastFile, includeSeparator ? "_" : "");
    }
}

std::string PCSX::GUI::getSaveStatePostfix() { return ".sstate"; }

std::string PCSX::GUI::buildSaveStateFilename(int i) {
    return fmt::format("{}{}{}", getSaveStatePrefix(false), getSaveStatePostfix(), i);
}

std::string PCSX::GUI::buildSaveStateFilename(std::string saveStateName) {
    return fmt::format("{}{}{}", getSaveStatePrefix(true), saveStateName, getSaveStatePostfix());
}

bool PCSX::GUI::saveSaveState(std::filesystem::path filename) {
    if (filename.is_relative()) {
        filename = g_system->getPersistentDir() / filename;
    }
    // TODO: yeet this to libuv's threadpool.
    ZWriter save(new UvFile(filename, FileOps::TRUNCATE), ZWriter::GZIP);
    bool success = !save.failed();
    if (success) save.writeString(SaveStates::save());
    save.close();
    if (success) {
        addNotice(fmt::format(f_("State saved to {}"), filename.filename().string()), NoticeKind::Info);
    } else {
        addNotice(fmt::format(f_("Failed to save state to {}"), filename.filename().string()), NoticeKind::Error);
    }
    return success;
}

bool PCSX::GUI::loadSaveState(std::filesystem::path filename) {
    if (filename.is_relative()) {
        filename = g_system->getPersistentDir() / filename;
    }
    ZReader save(new PosixFile(filename));
    if (save.failed()) {
        addNotice(fmt::format(f_("Failed to load state from {}"), filename.filename().string()), NoticeKind::Error);
        return false;
    }
    std::ostringstream os;
    constexpr unsigned buff_size = 1 << 16;
    char* buff = new char[buff_size];
    bool error = false;

    while (!save.eof()) {
        auto cnt = save.read(buff, buff_size);
        if (cnt == 0) break;
        if (cnt < 0) {
            error = true;
            break;
        }
        os.write(buff, cnt);
    }

    save.close();
    delete[] buff;

    if (error || !SaveStates::loadSafe(os.str())) {
        addNotice(fmt::format(f_("Failed to load state from {}"), filename.filename().string()), NoticeKind::Error);
        return false;
    }
    addNotice(fmt::format(f_("State loaded from {}"), filename.filename().string()), NoticeKind::Info);
    return true;
}

void PCSX::GUI::PassiveNotifier::draw() {
    constexpr double c_duration = 3.0;
    constexpr double c_fadeTime = 0.5;
    constexpr size_t c_maxShown = 4;

    const double now = ImGui::GetTime();
    while (!m_shown.empty() && (now - m_shown.front().shownAt) >= c_duration) m_shown.pop_front();
    while (!m_pending.empty() && (m_shown.size() < c_maxShown)) {
        m_shown.push_back(std::move(m_pending.front()));
        m_shown.back().shownAt = now;
        m_pending.pop_front();
    }
    if (m_shown.empty()) return;

    // The foreground draw list sits above every window, including the full window render, and isn't a window
    // itself, so it can't grab focus or inputs.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* drawList = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(viewport));
    const ImGuiStyle& style = ImGui::GetStyle();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float wrapWidth = viewport->WorkSize.x * 0.8f;
    const ImVec2 padding = style.WindowPadding;
    const ImVec4 bgColor = style.Colors[ImGuiCol_PopupBg];
    const ImVec4 textColor = style.Colors[ImGuiCol_Text];
    // Indexed by NoticeKind: Info, Warning, Error.
    static const ImVec4 c_barColors[] = {
        ImVec4(0.26f, 0.59f, 0.98f, 1.0f),
        ImVec4(0.95f, 0.70f, 0.15f, 1.0f),
        ImVec4(0.90f, 0.25f, 0.25f, 1.0f),
    };
    const float barWidth = std::max(4.0f, fontSize * 0.25f);

    // Newest at the bottom, older ones stacked above it.
    float bottom = viewport->WorkPos.y + viewport->WorkSize.y - padding.y * 2.0f;
    for (auto it = m_shown.rbegin(); it != m_shown.rend(); it++) {
        const double remaining = c_duration - (now - it->shownAt);
        const float alpha = remaining < c_fadeTime ? float(remaining / c_fadeTime) : 1.0f;
        const char* text = it->message.c_str();
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, wrapWidth, text);
        const ImVec2 boxSize = ImVec2(barWidth + textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
        const ImVec2 boxMin =
            ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - boxSize.x) / 2.0f, bottom - boxSize.y);
        const ImVec2 boxMax = ImVec2(boxMin.x + boxSize.x, bottom);
        drawList->AddRectFilled(
            boxMin, boxMax, ImGui::ColorConvertFloat4ToU32(ImVec4(bgColor.x, bgColor.y, bgColor.z, bgColor.w * alpha)),
            style.WindowRounding);
        const ImVec4 barColor = c_barColors[static_cast<size_t>(it->kind)];
        drawList->AddRectFilled(boxMin, ImVec2(boxMin.x + barWidth, boxMax.y),
                                ImGui::ColorConvertFloat4ToU32(ImVec4(barColor.x, barColor.y, barColor.z, alpha)),
                                style.WindowRounding, ImDrawFlags_RoundCornersLeft);
        drawList->AddText(
            font, fontSize, ImVec2(boxMin.x + barWidth + padding.x, boxMin.y + padding.y),
            ImGui::ColorConvertFloat4ToU32(ImVec4(textColor.x, textColor.y, textColor.z, textColor.w * alpha)), text,
            nullptr, wrapWidth);
        bottom = boxMin.y - style.ItemSpacing.y;
    }
}

bool PCSX::GUI::deleteSaveState(std::filesystem::path filename) {
    if (filename.is_relative()) {
        filename = g_system->getPersistentDir() / filename;
    }
    return std::remove(filename.string().c_str()) == 0;
}

bool PCSX::GUI::saveSaveStateSlot(uint32_t slot) { return saveSaveState(buildSaveStateFilename(slot)); }

bool PCSX::GUI::loadSaveStateSlot(uint32_t slot) { return loadSaveState(buildSaveStateFilename(slot)); }

bool PCSX::GUI::deleteSaveStateSlot(uint32_t slot) { return deleteSaveState(buildSaveStateFilename(slot)); }

bool PCSX::GUI::saveStateExists(std::filesystem::path filename) {
    if (filename.is_relative()) {
        filename = g_system->getPersistentDir() / filename;
    }
    ZReader save(new PosixFile(filename));
    return !save.failed();
}

std::vector<std::pair<std::filesystem::path, std::string>> PCSX::GUI::getNamedSaveStates() {
    return m_namedSaveStates.getNamedSaveStates(this);
}

void PCSX::GUI::byteRateToString(float rate, std::string& str) {
    if (rate >= 1000000000) {
        str = fmt::format("{:.2f} GB/s", rate / 1000000000);
    } else if (rate >= 1000000) {
        str = fmt::format("{:.2f} MB/s", rate / 1000000);
    } else if (rate >= 1000) {
        str = fmt::format("{:.2f} KB/s", rate / 1000);
    } else {
        str = fmt::format("{:.2f} B/s", rate);
    }
}

PCSX::GUI::TvgViewport* PCSX::GUI::getTvgViewport(unsigned viewportId) {
    if (!m_tvgInitialized) return nullptr;
    auto [it, inserted] = m_tvgViewports.try_emplace(viewportId);
    auto& vp = it->second;
    if (inserted) {
        vp.root = tvg::Scene::gen();
        vp.root->ref();
        vp.transient = tvg::Scene::gen();
        vp.transient->ref();
    }
    return &vp;
}

tvg::Scene* PCSX::GUI::getTvgViewportScene(unsigned viewportId) {
    auto vp = getTvgViewport(viewportId);
    return vp ? vp->root : nullptr;
}

void PCSX::GUI::renderTvgViewport(ImGuiViewport* viewport, SDL_Window* window) {
    auto it = m_tvgViewports.find(viewport->ID);
    if (it == m_tvgViewports.end()) return;
    auto& vp = it->second;

    int winWidth, winHeight, fbWidth, fbHeight;
    SDL_GetWindowSize(window, &winWidth, &winHeight);
    SDL_GetWindowSizeInPixels(window, &fbWidth, &fbHeight);
    if ((winWidth <= 0) || (winHeight <= 0) || (fbWidth <= 0) || (fbHeight <= 0)) return;

    auto glContext = SDL_GL_GetCurrentContext();
    if (!vp.canvas) {
        vp.canvas = tvg::GlCanvas::gen();
        if (!vp.canvas) return;
        vp.canvas->add(vp.root);
        vp.canvas->add(vp.transient);
    }
    if ((vp.glContext != glContext) || (vp.width != fbWidth) || (vp.height != fbHeight)) {
        if (vp.canvas->target(nullptr, nullptr, glContext, 0, fbWidth, fbHeight, tvg::ColorSpace::ABGR8888S) !=
            tvg::Result::Success) {
            return;
        }
        vp.glContext = glContext;
        vp.width = fbWidth;
        vp.height = fbHeight;
    }

    // Paints are expressed in ImGui coordinates, which are absolute when multi-viewports are enabled.
    float scale = float(fbWidth) / float(winWidth);
    tvg::Matrix m = {scale, 0.0f, -viewport->Pos.x * scale, 0.0f, scale, -viewport->Pos.y * scale, 0.0f, 0.0f, 1.0f};
    vp.root->transform(m);
    vp.transient->transform(m);

    vp.canvas->update();
    if (vp.canvas->draw(false) == tvg::Result::Success) vp.canvas->sync();
}

void PCSX::GUI::destroyTvgViewport(unsigned viewportId, SDL_Window* window) {
    auto it = m_tvgViewports.find(viewportId);
    if (it == m_tvgViewports.end()) return;
    auto& vp = it->second;
    if (vp.canvas) {
        // The canvas releases its GL objects on destruction, so its context needs to be current.
        auto previousWindow = SDL_GL_GetCurrentWindow();
        auto previousContext = SDL_GL_GetCurrentContext();
        bool switchContext = window && vp.glContext && (previousContext != vp.glContext);
        if (switchContext) SDL_GL_MakeCurrent(window, static_cast<SDL_GLContext>(vp.glContext));
        delete vp.canvas;
        if (switchContext) SDL_GL_MakeCurrent(previousWindow, previousContext);
    }
    vp.root->unref();
    vp.transient->unref();
    m_tvgViewports.erase(it);
}

void PCSX::GUI::drawBezierArrow(float width, ImVec2 start, ImVec2 c1, ImVec2 c2, ImVec2 end, ImVec4 innerColor,
                                ImVec4 outerColor) {
    auto vp = getTvgViewport(ImGui::GetWindowViewport()->ID);
    if (!vp) return;
    float angle = Bezier::angle(start, c1, c2, end, 1.0f);

    auto drawArrow = [&](float w, ImVec4 color, float headLength, float headNotch, float headWidth) {
        uint8_t r = color.x * 255, g = color.y * 255, b = color.z * 255, a = color.w * 255;

        // The butt of the arrow.
        auto butt = tvg::Shape::gen();
        butt->appendCircle(start.x, start.y, w / 2.0f, w / 2.0f);
        butt->fill(r, g, b, a);
        vp->transient->add(butt);

        // The shaft of the arrow itself.
        auto shaft = tvg::Shape::gen();
        shaft->moveTo(start.x, start.y);
        shaft->cubicTo(c1.x, c1.y, c2.x, c2.y, end.x, end.y);
        shaft->strokeWidth(w);
        shaft->strokeFill(r, g, b, a);
        shaft->strokeCap(tvg::StrokeCap::Butt);
        vp->transient->add(shaft);

        // The arrowhead.
        float bump = w / 1.5f;
        auto head = tvg::Shape::gen();
        head->moveTo(0.0f, 0.0f);
        head->lineTo(-bump * headLength, bump * headWidth);
        head->lineTo(-bump * headNotch, 0.0f);
        head->lineTo(-bump * headLength, -bump * headWidth);
        head->close();
        head->strokeWidth(w);
        head->strokeFill(r, g, b, a);
        head->strokeCap(tvg::StrokeCap::Butt);
        head->rotate(angle * 180.0f / std::numbers::pi_v<float>);
        head->translate(end.x, end.y);
        vp->transient->add(head);
    };

    drawArrow(width * 2.0f, outerColor, 2.0f, 1.6f, 1.0f);
    drawArrow(width, innerColor, 3.55f, 2.85f, 1.65f);
}

ImFont* PCSX::GUI::findClosestFont(const std::map<float, ImFont*>& fonts) {
    if (fonts.empty()) return ImGui::GetIO().Fonts[0].Fonts[0];
    float scale = m_currentScale;
    auto it = fonts.find(scale);
    if (it != fonts.end()) return it->second;

    m_reloadFonts = true;
    float currentDistance = 9999.0f;
    ImFont* currentFont = nullptr;
    for (auto& font : fonts) {
        float d = fabs(font.first - scale);
        if (d < currentDistance) {
            currentDistance = d;
            currentFont = font.second;
        }
    }

    return currentFont;
}

void PCSX::GUI::changeScale(float scale) {
    if (scale <= 0.0f) return;
    m_currentScale = scale;
    // Track this scale so the font reload pass bakes glyphs pre-sized for it;
    // if it's a previously-unseen scale, request a reload so the per-scale font
    // map gets a matching entry. v1.92's dynamic font system also handles
    // unbaked sizes lazily via PushFont(font, size), but the pre-baked path
    // still avoids a same-frame atlas grow on viewport DPI flips.
    if (m_allScales.insert(scale).second) {
        m_reloadFonts = true;
        return;
    }
    // Already-pinned scale: no font reload needed, but the default frame font
    // and base size still have to track the new closest-scale match. v1.92
    // draws default-context frames at Style.FontSizeBase, so we have to keep
    // it in sync with the active main font's LegacySize on every DPI flip,
    // not only the first one.
    if (ImGui::GetCurrentContext()) {
        ImFont* mainFont = getMainFont();
        if (mainFont) {
            ImGui::GetIO().FontDefault = mainFont;
            ImGui::GetStyle().FontSizeBase = mainFont->LegacySize;
        }
    }
}

void PCSX::GUI::saveScreenShot() {
    std::filesystem::path path =
        g_system->getPersistentDir() / (getSaveStatePrefix(true) + PCSX::ScreenShot::getDateString() + ".png");
    auto screenshot = g_emulator->m_gpu->takeScreenShot();
    clip::image img = PCSX::ScreenShot::convertScreenshotToImage(std::move(screenshot));
    bool success = PCSX::ScreenShot::writeImagePNG(path.string(), std::move(img));

    if (success) {
        auto successMsg = fmt::format(f_("Screenshot saved to {}\n"), path.string());
        g_system->log(LogClass::UI, std::move(successMsg));
        addNotice(fmt::format(f_("Screenshot saved to {}"), path.filename().string()), NoticeKind::Info);
    } else {
        addNotice(_("Failed to save screenshot"), NoticeKind::Error);
    }
}
