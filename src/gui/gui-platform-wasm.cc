/*

MIT License

Copyright (c) 2024 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#ifdef __EMSCRIPTEN__

#include <GL/gl3w.h>
#include <SDL3/SDL.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5_webgl.h>

#include <deque>
#include <string>

#include "gui/gui-platform.h"

namespace PCSX {
namespace GUIPlatform {

// glClearDepth takes a double and is desktop-GL only; ES3 and WebGL2 have only
// the float form, glClearDepthf. gl3w resolves the desktop name to NULL there,
// which now lands in a typed thrower and surfaces as "gl function not loaded" -
// the two occurrences were exactly the two call sites in gui.cc that now go
// through GUIPlatform::clearDepth.
void clearDepth(double d) { glClearDepthf(static_cast<float>(d)); }

void presentFrame(SDL_Window* window) {
    SDL_GL_SwapWindow(window);
    // Nothing in the SDL or EGL path presents a frame in a browser: emscripten's
    // eglSwapBuffers is a no-op, and a WebGL canvas is normally composited only
    // when the owning thread's task ENDS.
    //
    // This call was written for a world that no longer exists: main() ran on a
    // proxied pthread whose loop never returned, so nothing ever presented -
    // measured with a four-arm oracle in which an identical program drew 208,800
    // frames to a canvas that stayed black, while the same program with one
    // `return` after 300 frames came up magenta. -sOFFSCREEN_FRAMEBUFFER plus an
    // explicit commit_frame was the escape hatch.
    //
    // main() is on the browser's main thread now and the frame body returns
    // every frame, so the ordinary composite path should present on its own and
    // this call may be redundant - or may be presenting twice. UNMEASURED. The
    // test is one build with this call and both -sOFFSCREEN_FRAMEBUFFER and
    // -sGL_SUPPORT_EXPLICIT_SWAP_CONTROL dropped, then look at the canvas.
    emscripten_webgl_commit_frame();
}

}  // namespace GUIPlatform
}  // namespace PCSX

// Called by the page shell once it has copied a picked or dropped file into the virtual
// filesystem. Posting it as a drop event hands it to the same place a desktop drop goes,
// GUI::magicOpen, on the next frame rather than from inside a browser callback.
extern "C" EMSCRIPTEN_KEEPALIVE void pcsxOpenFile(const char* path) {
    // A pushed event does not own the string it points to, and two loads can be queued before
    // the next frame polls them, so every path stays alive for the life of the page. That is
    // one short string per load.
    static std::deque<std::string> s_paths;
    SDL_Event event = {};
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.data = s_paths.emplace_back(path).c_str();
    SDL_PushEvent(&event);
}

#endif  // __EMSCRIPTEN__
