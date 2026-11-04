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

#pragma once

#include <GL/gl3w.h>
#include <stdarg.h>

#include <deque>
#include <functional>
#include <magic_enum/magic_enum_all.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/system.h"
#include "core/ui.h"
#include "fmt/printf.h"
#include "gui/widgets/alloc_tracker.h"
#include "gui/widgets/assembly.h"
#include "gui/widgets/breakpoints.h"
#include "gui/widgets/callstacks.h"
#include "gui/widgets/cdrom-viewer.h"
#include "gui/widgets/console.h"
#include "gui/widgets/cputrace.h"
#include "gui/widgets/dalos.h"
#include "gui/widgets/dynarec_disassembly.h"
#include "gui/widgets/events.h"
#include "gui/widgets/filedialog.h"
#include "gui/widgets/gpudump.h"
#include "gui/widgets/gpulogger.h"
#include "gui/widgets/handlers.h"
#include "gui/widgets/heap_viewer.h"
#include "gui/widgets/hwregs.h"
#include "gui/widgets/isobrowser.h"
#include "gui/widgets/kernellog.h"
#include "gui/widgets/log.h"
#include "gui/widgets/luaeditor.h"
#include "gui/widgets/luainspector.h"
#include "gui/widgets/memcard_manager.h"
#include "gui/widgets/msan_viewer.h"
#include "gui/widgets/named_savestates.h"
#include "gui/widgets/network.h"
#include "gui/widgets/patches.h"
#include "gui/widgets/pio-cart.h"
#include "gui/widgets/ram-viewer.h"
#include "gui/widgets/registers.h"
#include "gui/widgets/shader-editor.h"
#include "gui/widgets/sio1.h"
#include "gui/widgets/spulogger.h"
#include "gui/widgets/spusamples.h"
#include "gui/widgets/vram-viewer.h"
#include "imgui.h"
#include "imgui_md/imgui_md.h"
#include "imgui_memory_editor/imgui_memory_editor.h"
#include "support/eventbus.h"
#include "support/settings.h"
#include "support/version.h"
#include "widgets/memory_observer.h"
#include "widgets/typed_debugger.h"

#if defined(__APPLE__)
#define GL_SHADER_VERSION "#version 410\n"
#else
#define GL_SHADER_VERSION "#version 300 es\n"
#endif

struct SDL_Window;
struct SDL_GLContextState;
typedef SDL_GLContextState *SDL_GLContext;
struct ImGuiViewport;
namespace tvg {
struct GlCanvas;
struct Scene;
}  // namespace tvg

namespace PCSX {

class GUI;
extern GUI *g_gui;

enum class LogClass : unsigned;

class GUI final : public UI {
    typedef Setting<bool, TYPESTRING("Fullscreen"), false> Fullscreen;
    typedef Setting<bool, TYPESTRING("FullWindowRender"), true> FullWindowRender;
    typedef Setting<bool, TYPESTRING("ShowMenu")> ShowMenu;
    typedef Setting<bool, TYPESTRING("ShowLog")> ShowLog;
    typedef Setting<bool, TYPESTRING("ShowLuaConsole")> ShowLuaConsole;
    typedef Setting<bool, TYPESTRING("ShowLuaInspector")> ShowLuaInspector;
    typedef Setting<bool, TYPESTRING("ShowLuaEditor")> ShowLuaEditor;
    typedef Setting<bool, TYPESTRING("ShowMainVRAMViewer")> ShowMainVRAMViewer;
    typedef Setting<bool, TYPESTRING("ShowCLUTVRAMViewer")> ShowCLUTVRAMViewer;
    typedef Setting<bool, TYPESTRING("ShowVRAMViewer1")> ShowVRAMViewer1;
    typedef Setting<bool, TYPESTRING("ShowVRAMViewer2")> ShowVRAMViewer2;
    typedef Setting<bool, TYPESTRING("ShowVRAMViewer3")> ShowVRAMViewer3;
    typedef Setting<bool, TYPESTRING("ShowVRAMViewer4")> ShowVRAMViewer4;
    typedef Setting<bool, TYPESTRING("ShowMemoryObserver")> ShowMemoryObserver;
    typedef Setting<bool, TYPESTRING("ShowTypedDebugger")> ShowTypedDebugger;
    typedef Setting<bool, TYPESTRING("ShowPatches")> ShowPatches;
    typedef Setting<bool, TYPESTRING("ShowMemcardManager")> ShowMemcardManager;
    typedef Setting<bool, TYPESTRING("ShowRegisters")> ShowRegisters;
    typedef Setting<bool, TYPESTRING("ShowAssembly")> ShowAssembly;
    typedef Setting<bool, TYPESTRING("ShowDisassembly")> ShowDisassembly;
    typedef Setting<bool, TYPESTRING("ShowCpuTrace")> ShowCpuTrace;
    typedef Setting<bool, TYPESTRING("ShowBreakpoints")> ShowBreakpoints;
    typedef Setting<bool, TYPESTRING("ShowNamedSaveStates")> ShowNamedSaveStates;
    typedef Setting<bool, TYPESTRING("ShowEvents")> ShowEvents;
    typedef Setting<bool, TYPESTRING("ShowHandlers")> ShowHandlers;
    typedef Setting<bool, TYPESTRING("ShowKernelLog")> ShowKernelLog;
    typedef Setting<bool, TYPESTRING("ShowCallstacks")> ShowCallstacks;
    typedef Setting<bool, TYPESTRING("ShowSIO1")> ShowSIO1;
    typedef Setting<bool, TYPESTRING("ShowNetwork")> ShowNetwork;
    typedef Setting<bool, TYPESTRING("ShowIsoBrowser")> ShowIsoBrowser;
    typedef Setting<bool, TYPESTRING("ShowDalos")> ShowDalos;
    typedef Setting<bool, TYPESTRING("ShowGPULogger")> ShowGPULogger;
    typedef Setting<bool, TYPESTRING("ShowGPUDump")> ShowGPUDump;
    typedef Setting<bool, TYPESTRING("ShowSPULogger")> ShowSPULogger;
    typedef Setting<bool, TYPESTRING("SPULoggerEnabled")> SPULoggerEnabled;
    typedef Setting<bool, TYPESTRING("ShowSPUSamples")> ShowSPUSamples;
    typedef Setting<bool, TYPESTRING("ShowRAMViewer")> ShowRAMViewer;
    typedef Setting<bool, TYPESTRING("ShowCDRomViewer")> ShowCDRomViewer;
    typedef Setting<bool, TYPESTRING("ShowHeapViewer")> ShowHeapViewer;
    typedef Setting<bool, TYPESTRING("ShowAllocTracker")> ShowAllocTracker;
    typedef Setting<bool, TYPESTRING("ShowHWRegs")> ShowHWRegs;
    typedef Setting<bool, TYPESTRING("ShowMsanViewer")> ShowMsanViewer;
    typedef Setting<int, TYPESTRING("WindowPosX"), 0> WindowPosX;
    typedef Setting<int, TYPESTRING("WindowPosY"), 0> WindowPosY;
    typedef Setting<int, TYPESTRING("WindowSizeX"), 1280> WindowSizeX;
    typedef Setting<int, TYPESTRING("WindowSizeY"), 800> WindowSizeY;
    typedef Setting<bool, TYPESTRING("WindowMaximized"), false> WindowMaximized;
    typedef Setting<int, TYPESTRING("IdleSwapInterval"), 1> IdleSwapInterval;
    typedef Setting<int, TYPESTRING("MainFontSize"), 16> MainFontSize;
    typedef Setting<int, TYPESTRING("MonoFontSize"), 16> MonoFontSize;
    typedef Setting<int, TYPESTRING("GUITheme"), 0> GUITheme;
    typedef Setting<bool, TYPESTRING("AllowMouseCaptureToggle"), false> AllowMouseCaptureToggle;
    typedef Setting<bool, TYPESTRING("RawMouseMotion"), false> EnableRawMouseMotion;
    typedef Setting<bool, TYPESTRING("WidescreenRatio"), false> WidescreenRatio;
    typedef Setting<bool, TYPESTRING("ShowPIOCartConfig"), false> ShowPIOCartConfig;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor1")> ShowMemoryEditor1;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor2")> ShowMemoryEditor2;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor3")> ShowMemoryEditor3;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor4")> ShowMemoryEditor4;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor5")> ShowMemoryEditor5;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor6")> ShowMemoryEditor6;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor7")> ShowMemoryEditor7;
    typedef Setting<bool, TYPESTRING("ShowMemoryEditor8")> ShowMemoryEditor8;
    typedef Setting<bool, TYPESTRING("ShowParallelPortEditor")> ShowParallelPortEditor;
    typedef Setting<bool, TYPESTRING("ShowScratchpadEditor")> ShowScratchpadEditor;
    typedef Setting<bool, TYPESTRING("ShowHWRegsEditor")> ShowHWRegsEditor;
    typedef Setting<bool, TYPESTRING("ShowBiosEditor")> ShowBiosEditor;
    typedef Setting<bool, TYPESTRING("ShowVRAMEditor")> ShowVRAMEditor;
    typedef Setting<bool, TYPESTRING("ShowSPURAMEditor")> ShowSPURAMEditor;
    typedef Setting<size_t, TYPESTRING("MemoryEditor1Addr"), 0> MemoryEditor1Addr;
    typedef Setting<size_t, TYPESTRING("MemoryEditor2Addr"), 0> MemoryEditor2Addr;
    typedef Setting<size_t, TYPESTRING("MemoryEditor3Addr"), 0> MemoryEditor3Addr;
    typedef Setting<size_t, TYPESTRING("MemoryEditor4Addr"), 0> MemoryEditor4Addr;
    typedef Setting<size_t, TYPESTRING("MemoryEditor5Addr"), 0> MemoryEditor5Addr;
    typedef Setting<size_t, TYPESTRING("MemoryEditor6Addr"), 0> MemoryEditor6Addr;
    typedef Setting<size_t, TYPESTRING("MemoryEditor7Addr"), 0> MemoryEditor7Addr;
    typedef Setting<size_t, TYPESTRING("MemoryEditor8Addr"), 0> MemoryEditor8Addr;
    typedef Setting<size_t, TYPESTRING("ParallelPortEditorAddr"), 0> ParallelPortEditorAddr;
    typedef Setting<size_t, TYPESTRING("ScratchpadEditorAddr"), 0> ScratchpadEditorAddr;
    typedef Setting<size_t, TYPESTRING("HWRegsEditorAddr"), 0> HWRegsEditorAddr;
    typedef Setting<size_t, TYPESTRING("BiosEditorAddr"), 0> BiosEditorAddr;
    typedef Setting<size_t, TYPESTRING("VRAMEditorAddr"), 0> VRAMEditorAddr;
    typedef Setting<size_t, TYPESTRING("SPURAMEditorAddr"), 0> SPURAMEditorAddr;
    Settings<Fullscreen, FullWindowRender, ShowMenu, ShowLog, WindowPosX, WindowPosY, WindowSizeX, WindowSizeY,
             WindowMaximized, IdleSwapInterval, ShowLuaConsole, ShowLuaInspector, ShowLuaEditor, ShowMainVRAMViewer,
             ShowCLUTVRAMViewer, ShowVRAMViewer1, ShowVRAMViewer2, ShowVRAMViewer3, ShowVRAMViewer4, ShowMemoryObserver,
             ShowTypedDebugger, ShowPatches, ShowMemcardManager, ShowRegisters, ShowAssembly, ShowDisassembly,
             ShowCpuTrace, ShowBreakpoints, ShowNamedSaveStates, ShowEvents, ShowHandlers, ShowKernelLog,
             ShowCallstacks, ShowSIO1, ShowNetwork, ShowIsoBrowser, ShowGPULogger, ShowGPUDump, ShowRAMViewer,
             ShowCDRomViewer, ShowHeapViewer, ShowAllocTracker, ShowHWRegs, MainFontSize, MonoFontSize, GUITheme,
             AllowMouseCaptureToggle, EnableRawMouseMotion, WidescreenRatio, ShowPIOCartConfig, ShowMemoryEditor1,
             ShowMemoryEditor2, ShowMemoryEditor3, ShowMemoryEditor4, ShowMemoryEditor5, ShowMemoryEditor6,
             ShowMemoryEditor7, ShowMemoryEditor8, ShowParallelPortEditor, ShowScratchpadEditor, ShowHWRegsEditor,
             ShowBiosEditor, ShowVRAMEditor, MemoryEditor1Addr, MemoryEditor2Addr, MemoryEditor3Addr, MemoryEditor4Addr,
             MemoryEditor5Addr, MemoryEditor6Addr, MemoryEditor7Addr, MemoryEditor8Addr, ParallelPortEditorAddr,
             ScratchpadEditorAddr, HWRegsEditorAddr, BiosEditorAddr, VRAMEditorAddr, ShowSPURAMEditor, SPURAMEditorAddr,
             ShowSPULogger, SPULoggerEnabled, ShowSPUSamples, ShowDalos, ShowMsanViewer>
        settings;

    // imgui can't handle more than one "instance", so...
    void (*m_onChangedViewportOldCallback)(ImGuiViewport *viewport) = nullptr;
    void (*m_destroyWindowOldCallback)(ImGuiViewport *viewport) = nullptr;
    void glErrorCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message);
    void changeScale(float scale);
    bool m_onlyLogGLErrors = false;
    std::vector<std::string> m_glErrors;

  public:
    struct MarkDown : public imgui_md {
        static void newFrame() { m_id = 0; }
        MarkDown(GUI *gui);
        MarkDown(GUI *gui, std::map<std::string_view, std::function<void()>> &&customURLs);
        int print(const std::string_view text);
        void open_url() const override;
        bool get_image(image_info &nfo) const override;
        void BLOCK_CODE(const MD_BLOCK_CODE_DETAIL *d, bool e);
        void SPAN_CODE(bool e) override;

      private:
        std::map<std::string_view, std::function<void()>> m_customURLs;
        GUI *m_gui;
        static unsigned m_id;
    };
    static void openUrl(std::string_view url);
    void setOnlyLogGLErrors(bool value) { m_onlyLogGLErrors = value; }
    class ScopedOnlyLog {
      public:
        ScopedOnlyLog(GUI *gui) : m_gui(gui) { gui->setOnlyLogGLErrors(true); }
        ~ScopedOnlyLog() { m_gui->setOnlyLogGLErrors(false); }

      private:
        GUI *m_gui = nullptr;
    };
    std::vector<std::string> getGLerrors() { return std::move(m_glErrors); }
    GUI(std::vector<std::string> &favorites);
    ~GUI() {
        assert(g_gui == this);
        g_gui = nullptr;
    }
    void init(std::function<void()> applyArguments) override;
    void setLua(Lua L);
    void close();
    void update(bool vsync = false);
    void flip();
    void setViewport();
    void setFullscreen(bool fullscreen);
    void setRawMouseMotion();
    bool addLog(LogClass logClass, const std::string &msg) {
        return m_log.addLog(magic_enum::enum_integer(logClass), msg);
    }
    void addLuaLog(const std::string &msg, bool error) {
        if (error) {
            m_luaConsole.addError(msg);
        } else {
            m_luaConsole.addLog(msg);
        }
    }
    class Notifier {
      public:
        Notifier(std::function<const char *()> title) : m_title(title) {}
        void notify(const std::string &message) {
            m_message = message;
            m_toOpen = true;
        }
        bool draw() {
            if (m_toOpen) {
                ImGui::OpenPopup(m_title());
                m_toOpen = false;
            }
            bool done = false;
            if (ImGui::BeginPopupModal(m_title(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(m_message.c_str());
                if (ImGui::Button(_("Ok"), ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                    done = true;
                }
                ImGui::EndPopup();
            }
            return done;
        }

      private:
        const std::function<const char *()> m_title;
        bool m_toOpen = false;
        std::string m_message;
    };
    void addNotification(const std::string &notification) { m_notifier.notify(notification); }

    // Short-lived messages drawn at the bottom of the main viewport, which fade out on their own and never
    // take focus. Use them for anything the user doesn't have to act on, failures included: a state that
    // couldn't be saved is an Error notice. The kind only picks the colour of the bar on the left. If the
    // user has to do something about it, use addNotification instead.
    enum class NoticeKind { Info, Warning, Error };
    class PassiveNotifier {
      public:
        void notify(const std::string &message, NoticeKind kind) { m_pending.push_back({message, kind, 0.0}); }
        void draw();

      private:
        struct Notice {
            std::string message;
            NoticeKind kind;
            double shownAt;
        };
        std::deque<Notice> m_pending;
        std::deque<Notice> m_shown;
    };
    void addNotice(const std::string &notice, NoticeKind kind) { m_passiveNotifier.notify(notice, kind); }

    void magicOpen(const char *path);

    static const char *glErrorToString(GLenum error) {
        static const std::map<GLenum, const char *> glErrorMap = {
            {GL_NO_ERROR, "GL_NO_ERROR"},
            {GL_INVALID_ENUM, "GL_INVALID_ENUM"},
            {GL_INVALID_VALUE, "GL_INVALID_VALUE"},
            {GL_INVALID_OPERATION, "GL_INVALID_OPERATION"},
            {GL_INVALID_FRAMEBUFFER_OPERATION, "GL_INVALID_FRAMEBUFFER_OPERATION"},
            {GL_OUT_OF_MEMORY, "GL_OUT_OF_MEMORY"},
            {GL_STACK_UNDERFLOW, "GL_STACK_UNDERFLOW"},
            {GL_STACK_OVERFLOW, "GL_STACK_OVERFLOW"},
        };
        auto f = glErrorMap.find(error);
        if (f == glErrorMap.end()) return "Unknown error";
        return f->second;
    }

  private:
    void saveCfg();
    void resetSettings();
    bool m_settingsNuked = false;
    bool m_showResetSettings = false;

    void startFrame();
    void endFrame();

    bool configure();
    bool showThemes();  // Theme window : Allows for custom imgui themes
    bool about();
    void aboutFFmpegTab();
    void interruptsScaler();

  public:
    const ImVec2 &getRenderSize() { return m_renderSize; }

  private:
    SDL_Window *m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    bool m_hasCoreProfile = false;
    int &m_windowPosX = settings.get<WindowPosX>().value;
    int &m_windowPosY = settings.get<WindowPosY>().value;
    int &m_windowSizeX = settings.get<WindowSizeX>().value;
    int &m_windowSizeY = settings.get<WindowSizeY>().value;
    bool &m_windowMaximized = settings.get<WindowMaximized>().value;
    GLuint m_VRAMTexture = 0;
    struct TvgViewport {
        tvg::GlCanvas *canvas = nullptr;
        tvg::Scene *root = nullptr;
        tvg::Scene *transient = nullptr;
        void *glContext = nullptr;
        int width = 0;
        int height = 0;
    };
    std::map<unsigned, TvgViewport> m_tvgViewports;
    bool m_tvgInitialized = false;
    TvgViewport *getTvgViewport(unsigned viewportId);
    void renderTvgViewport(ImGuiViewport *viewport, SDL_Window *window);
    void destroyTvgViewport(unsigned viewportId, SDL_Window *window);

    unsigned int m_offscreenFrameBuffer = 0;
    unsigned int m_offscreenTextures[2] = {0, 0};
    unsigned int m_offscreenDepthBuffer = 0;
    int m_currentTexture = 0;

    ImVec4 m_backgroundColor = ImColor(114, 144, 154);
    ImVec2 m_framebufferSize = ImVec2(1, 1);  // Size of the SDL window framebuffer (in pixels)
    ImVec2 m_renderSize = ImVec2(1, 1);
    ImVec2 m_outputWindowSize = ImVec2(1, 1);

    bool &m_fullscreen = {settings.get<Fullscreen>().value};

    bool &m_fullWindowRender = {settings.get<FullWindowRender>().value};
    bool &m_showMenu = {settings.get<ShowMenu>().value};
    int &m_idleSwapInterval = {settings.get<IdleSwapInterval>().value};
    bool m_showThemes = false;
    bool m_showDemo = false;
    bool m_showImPlotDemo = false;
    bool m_showHandles = false;
    bool m_showAbout = false;
    bool m_showInterruptsScaler = false;
    bool m_turboEnabled = false;
    bool m_outputWindowFocused = false;
    Widgets::Log m_log = {settings.get<ShowLog>().value};
    struct MemoryEditorWrapper {
        MemoryEditorWrapper(GUI *gui, bool &show, size_t &offsetAddr, size_t baseAddr = 0x0000)
            : m_show(show), m_offsetAddr(offsetAddr), m_baseAddr(baseAddr) {
            editor.OptShowDataPreview = true;
            editor.OptUpperCaseHex = false;
            editor.PushMonoFont = [gui]() { gui->useMonoFont(); };
        }
        bool &m_show;
        size_t &m_offsetAddr;
        const size_t m_baseAddr;
        MemoryEditor editor{m_show, m_baseAddr, m_offsetAddr};
        std::function<const char *()> title;

        void MenuItem() { ImGui::MenuItem(title(), nullptr, &m_show); }
        void draw(void *mem, size_t size) {
            editor.ReadFn = [mem](size_t off) -> ImU8 { return ((ImU8 *)mem)[off]; };
            editor.WriteFn = [mem](size_t off, ImU8 d) { ((ImU8 *)mem)[off] = d; };
            editor.Cache.BulkReadFn = [mem](void *dest, size_t off, size_t len) {
                memcpy(dest, (ImU8 *)mem + off, len);
            };
            editor.DrawWindow(title(), size);
        }
        void draw(IO<File> file, size_t size) {
            IO<File> sub(new SubFile(file, m_baseAddr, size, FileOps::READWRITE));
            editor.ReadFn = [sub](size_t off) mutable -> ImU8 {
                ImU8 b;
                sub->readAt(&b, 1, off);
                return b;
            };
            editor.WriteFn = [sub](size_t off, ImU8 d) mutable { sub->writeAt(&d, 1, off); };
            editor.Cache.BulkReadFn = [sub](void *dest, size_t off, size_t len) mutable {
                sub->readAt(dest, len, off);
            };
            editor.DrawWindow(title(), size);
        }
    };
    std::string m_stringHolder;
    const size_t wramBaseAddr = 0x80000000;
    MemoryEditorWrapper m_mainMemEditors[8] = {
        {this, settings.get<ShowMemoryEditor1>().value, settings.get<MemoryEditor1Addr>().value, wramBaseAddr},
        {this, settings.get<ShowMemoryEditor2>().value, settings.get<MemoryEditor2Addr>().value, wramBaseAddr},
        {this, settings.get<ShowMemoryEditor3>().value, settings.get<MemoryEditor3Addr>().value, wramBaseAddr},
        {this, settings.get<ShowMemoryEditor4>().value, settings.get<MemoryEditor4Addr>().value, wramBaseAddr},
        {this, settings.get<ShowMemoryEditor5>().value, settings.get<MemoryEditor5Addr>().value, wramBaseAddr},
        {this, settings.get<ShowMemoryEditor6>().value, settings.get<MemoryEditor6Addr>().value, wramBaseAddr},
        {this, settings.get<ShowMemoryEditor7>().value, settings.get<MemoryEditor7Addr>().value, wramBaseAddr},
        {this, settings.get<ShowMemoryEditor8>().value, settings.get<MemoryEditor8Addr>().value, wramBaseAddr},
    };
    MemoryEditorWrapper m_parallelPortEditor = {this, settings.get<ShowParallelPortEditor>().value,
                                                settings.get<ParallelPortEditorAddr>().value, 0x1f000000};
    MemoryEditorWrapper m_scratchPadEditor = {this, settings.get<ShowScratchpadEditor>().value,
                                              settings.get<ScratchpadEditorAddr>().value, 0x1f800000};
    MemoryEditorWrapper m_hwrEditor = {this, settings.get<ShowHWRegsEditor>().value,
                                       settings.get<HWRegsEditorAddr>().value, 0x1f801000};
    MemoryEditorWrapper m_biosEditor = {this, settings.get<ShowBiosEditor>().value,
                                        settings.get<BiosEditorAddr>().value, 0xbfc00000};
    MemoryEditorWrapper m_vramEditor = {this, settings.get<ShowVRAMEditor>().value,
                                        settings.get<VRAMEditorAddr>().value};
    MemoryEditorWrapper m_spuRamEditor = {this, settings.get<ShowSPURAMEditor>().value,
                                          settings.get<SPURAMEditorAddr>().value};
    Widgets::MemoryObserver m_memoryObserver = {settings.get<ShowMemoryObserver>().value};
    Widgets::TypedDebugger m_typedDebugger;
    Widgets::Patches m_patches = {settings.get<ShowPatches>().value};
    Widgets::MemcardManager m_memcardManager;
    Widgets::Registers m_registers = {settings.get<ShowRegisters>().value};
    Widgets::Assembly m_assembly;
    Widgets::Disassembly m_disassembly = {settings.get<ShowDisassembly>().value};
    Widgets::CpuTrace m_cpuTrace;
    Widgets::FileDialog<> m_openIsoFileDialog;
    Widgets::FileDialog<> m_openBinaryDialog;
    Widgets::FileDialog<> m_openArchiveDialog;
    Widgets::FileDialog<> m_selectBiosDialog;
    Widgets::FileDialog<> m_selectEXP1Dialog;
    Widgets::NamedSaveStates m_namedSaveStates = {settings.get<ShowNamedSaveStates>().value};
    Widgets::Breakpoints m_breakpoints = {settings.get<ShowBreakpoints>().value};
    Widgets::IsoBrowser m_isoBrowser;
    Widgets::Network m_network = {settings.get<ShowNetwork>().value};
    Widgets::Dalos m_dalos = {settings.get<ShowDalos>().value};

    bool m_showCfg = false;
    bool m_showUiCfg = false;
    bool m_showSysCfg = false;

    Widgets::VRAMViewer m_mainVRAMviewer = {settings.get<ShowMainVRAMViewer>().value};
    Widgets::VRAMViewer m_clutVRAMviewer = {settings.get<ShowCLUTVRAMViewer>().value};
    Widgets::VRAMViewer m_VRAMviewers[4] = {{settings.get<ShowVRAMViewer1>().value},
                                            {settings.get<ShowVRAMViewer2>().value},
                                            {settings.get<ShowVRAMViewer3>().value},
                                            {settings.get<ShowVRAMViewer4>().value}};

    Widgets::RAMViewer m_ramViewer = {settings.get<ShowRAMViewer>().value};
    Widgets::CDRomViewer m_cdromViewer = {settings.get<ShowCDRomViewer>().value};
    Widgets::LuaEditor m_luaEditor = {settings.get<ShowLuaEditor>().value};

    Widgets::Events m_events = {settings.get<ShowEvents>().value};
    Widgets::Handlers m_handlers = {settings.get<ShowHandlers>().value};
    Widgets::KernelLog m_kernelLog = {settings.get<ShowKernelLog>().value};

    Widgets::CallStacks m_callstacks = {settings.get<ShowCallstacks>().value};

    Widgets::PIOCart m_pioCart;
    Widgets::GPUDump m_gpuDump;
    Widgets::SIO1 m_sio1 = {settings.get<ShowSIO1>().value};

    Widgets::GPULogger m_gpuLogger{settings.get<ShowGPULogger>().value};
    Widgets::SPULogger m_spuLoggerWidget;
    Widgets::SPUSamples m_spuSamplesWidget;
    Widgets::HeapViewer m_heapViewer{settings.get<ShowHeapViewer>().value};
    Widgets::AllocTrackerViewer m_allocTrackerViewer{settings.get<ShowAllocTracker>().value};
    Widgets::HWRegs m_hwRegs{settings.get<ShowHWRegs>().value};
    Widgets::MsanViewer m_msanViewer{settings.get<ShowMsanViewer>().value};

    EventBus::Listener m_listener;

  public:
    bool saveSaveState(std::filesystem::path filename);
    bool loadSaveState(std::filesystem::path filename);
    bool deleteSaveState(std::filesystem::path filename);
    bool saveSaveStateSlot(uint32_t slot);
    bool loadSaveStateSlot(uint32_t slot);
    bool deleteSaveStateSlot(uint32_t slot);
    std::string getSaveStatePrefix(bool includeSeparator);
    static std::string getSaveStatePostfix();
    bool getSaveStateExists(uint32_t slot);
    std::vector<std::pair<std::filesystem::path, std::string>> getNamedSaveStates();
    std::string buildSaveStateFilename(int i);
    std::string buildSaveStateFilename(std::string name);
    bool saveStateExists(std::filesystem::path filename);
    void saveScreenShot();

  private:
    void applyTheme(int theme);
    void cherryTheme();
    void monoTheme();
    void draculaTheme();
    void oliveTheme();

    Notifier m_notifier = {l_("Notification")};
    PassiveNotifier m_passiveNotifier;
    Widgets::Console m_luaConsole = {settings.get<ShowLuaConsole>().value};
    Widgets::LuaInspector m_luaInspector = {settings.get<ShowLuaInspector>().value};

    bool m_gotImguiUserError = false;
    bool m_reportGLErrors = false;
    std::string m_imguiUserError;

    std::map<float, ImFont *> m_mainFonts;
    std::map<float, ImFont *> m_monoFonts;
    ImFont *findClosestFont(const std::map<float, ImFont *> &fonts);
    std::set<float> m_allScales;
    bool m_hasJapanese = false;
    float m_currentScale = 1.0f;

    ImFont *loadFont(const PCSX::u8string &name, int size, ImGuiIO &io, bool combine);

    bool m_reloadFonts = true;
    Widgets::ShaderEditor m_outputShaderEditor = {"output"};

    static void byteRateToString(float rate, std::string &out);

    Update m_update;
    bool m_updateAvailable = false;
    bool m_updateDownloading = false;
    bool m_aboutSelectAuthors = false;
    bool m_enableSplashScreen = true;

    void setDefaultShaders();
    std::unique_ptr<uint32_t[]> getSplashScreen(uint32_t destWidth, uint32_t destHeight);

  public:
    bool hasJapanese() { return m_hasJapanese; }
    bool m_setupScreenSize = true;
    bool m_clearTextures = true;
    bool m_presentAfterHardReset = false;
    Widgets::ShaderEditor m_offscreenShaderEditor = {"offscreen"};
    ImFont *getMainFont() { return findClosestFont(m_mainFonts); }
    ImFont *getMonoFont() { return findClosestFont(m_monoFonts); }
    // ImGui v1.92's PushFont takes a size. We pass the closest-matching font's
    // LegacySize (the size that was passed to AddFontFromFileTTF) so the
    // per-scale font map continues to drive rendering size, matching pre-1.92
    // PushFont(font) semantics. Passing 0.0f instead would mean "keep current
    // size" which would render the larger DPI-baked font at the previous
    // smaller base size, defeating the per-scale map entirely.
    void useMainFont() {
        ImFont *f = getMainFont();
        ImGui::PushFont(f, f ? f->LegacySize : 0.0f);
    }
    void useMonoFont() {
        ImFont *f = getMonoFont();
        ImGui::PushFont(f, f ? f->LegacySize : 0.0f);
    }

    bool &allowMouseCaptureToggle() { return settings.get<AllowMouseCaptureToggle>().value; }
    bool &isRawMouseMotionEnabled() { return settings.get<EnableRawMouseMotion>().value; }

    // Returns the root scene drawn on top of the given viewport, in ImGui coordinates.
    tvg::Scene *getTvgViewportScene(unsigned viewportId);
    void drawBezierArrow(float width, ImVec2 start, ImVec2 c1, ImVec2 c2, ImVec2 end,
                         ImVec4 innerColor = {1.0f, 1.0f, 1.0f, 1.0f}, ImVec4 outerColor = {0.5f, 0.5f, 0.5f, 1.0f});
};  // namespace PCSX

}  // namespace PCSX
