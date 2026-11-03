/***************************************************************************
 *   Copyright (C) 2020 PCSX-Redux authors                                 *
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

#include <stdio.h>
#include <string.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "main/main.h"

#if (defined(_WIN32) || defined(_WIN64)) && defined(_M_AMD64)
#define USE_SENTRY
#include <filesystem>

#include "core/system.h"
#include "sentry.h"
#include "support/binpath.h"
#include "support/file.h"
#include "support/version-info.h"
#endif

static void setupSentry() {
#ifdef USE_SENTRY
    PCSX::VersionInfo version;
    try {
        std::filesystem::path binDir = std::filesystem::absolute(PCSX::BinPath::getExecutablePath()).parent_path();
        version.loadFromFile(new PCSX::PosixFile(binDir / "version.json"));
    } catch (...) {
        version.clear();
    }
    std::string release = "pcsx-redux@" + (version.failed() ? std::string("head") : version.version);

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options,
                           "https://6c203a041de14d57a454889f50151f0f@o502319.ingest.sentry.io/4504971395858432");
    sentry_options_set_database_path(options, ".sentry-native");
    sentry_options_set_release(options, release.c_str());
    sentry_options_set_debug(options, 0);
    sentry_init(options);
    if (!version.changeset.empty()) sentry_set_tag("changeset", version.changeset.c_str());
    PCSX::System::s_crashReportTagSetter = sentry_set_tag;
#endif
}

static void closeSentry() {
#ifdef USE_SENTRY
    PCSX::System::s_crashReportTagSetter = nullptr;
    sentry_close();
#endif
}

static std::pair<int, std::string> loopMain(int argc, char **argv) {
    int r = 0;
    std::string errorMsg;
    do {
        try {
            r = pcsxMain(argc, argv);
        } catch (std::exception &e) {
            errorMsg = e.what();
            r = -1;
        } catch (...) {
            errorMsg = "An unknown exception occured.";
            r = -1;
        }
    } while (r == 0x12eb007);

    return {r, errorMsg};
}

// A headless run has nobody to dismiss a dialog, and the error is already on stderr.
static bool isHeadless(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        const char *arg = argv[i];
        // Only flags: "-logfile cli" names a file, it doesn't ask for the CLI.
        if (*arg != '-') continue;
        while (*arg == '-') arg++;
        if (!strcmp(arg, "testmode") || !strcmp(arg, "no-ui") || !strcmp(arg, "cli")) return true;
    }
    return false;
}

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Please, clang-format, stop being stubborn and let this windows.h
// header first. Yes, I know it's wrong, blame Microsoft.
#include <shellapi.h>

static void Complain(const char *msg, bool headless) {
#ifdef USE_SENTRY
    sentry_capture_event(sentry_value_new_message_event(SENTRY_LEVEL_FATAL, "exception", msg));
#endif
    if (!headless) MessageBoxA(nullptr, msg, "Error", MB_ICONERROR | MB_OK);
}

#ifndef PCSX_CLI
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    LPWSTR *argvw;
    char **argv;
    int argc;

    setupSentry();

    argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argvw) return -1;
    argv = (char **)calloc(argc + 1, sizeof(*argv));
    if (!argv) return -1;
    for (int i = 0; i < argc; i++) {
        int needed;
        char *str;

        needed = WideCharToMultiByte(CP_UTF8, 0, argvw[i], -1, NULL, 0, NULL, NULL);
        if (needed <= 0) {
            argv[i] = nullptr;
            continue;
        }
        str = (char *)calloc(needed, sizeof(char));
        WideCharToMultiByte(CP_UTF8, 0, argvw[i], -1, reinterpret_cast<LPSTR>(str), needed, NULL, NULL);
        argv[i] = str;
    }
    argv[argc] = nullptr;
    LocalFree(argvw);

    auto [r, errorMsg] = loopMain(argc, argv);

    if (!errorMsg.empty()) {
        r = -1;
        fprintf(stderr, "%s\n", errorMsg.c_str());
        Complain(errorMsg.c_str(), isHeadless(argc, argv));
    }

    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    closeSentry();
    return r;
}
#endif
#else
#ifndef PCSX_CLI
#if defined(__APPLE__) && defined(__MACH__)
extern "C" void Complain(const char *msg);
#elif defined(__EMSCRIPTEN__)
// Same shape as the Apple arm above: the implementation lives in its own file,
// src/main/complain-wasm.cc, because it needs EM_ASM and the DOM.
extern "C" void Complain(const char *msg);
#else
#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>

static void Complain(const char *msg) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) return;

    int s = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, s), 10, 10, 600, 100, 1, BlackPixel(d, s), WhitePixel(d, s));
    XSelectInput(d, w, ExposureMask | KeyPressMask);
    XMapWindow(d, w);

    XEvent e;
    while (1) {
        XNextEvent(d, &e);
        if (e.type == Expose) {
            XDrawString(d, w, DefaultGC(d, s), 10, 10, msg, strlen(msg));
        }
        if (e.type == KeyPress) break;
    }

    XCloseDisplay(d);
    return;
}
#endif
#endif
#endif

#if (!defined(_WIN32) && !defined(_WIN64)) || defined(PCSX_CLI)
int main(int argc, char **argv) {
    auto [r, errorMsg] = loopMain(argc, argv);

    if (errorMsg.empty()) return r;

    fprintf(stderr, "%s\n", errorMsg.c_str());

#if defined(_WIN32) || defined(_WIN64)
    Complain(errorMsg.c_str(), isHeadless(argc, argv));
#else
    if (!isHeadless(argc, argv)) Complain(errorMsg.c_str());
#endif

    return -1;
}
#endif
