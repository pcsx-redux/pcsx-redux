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

// The browser equivalent of complain.mm's NSAlert and mainthunk.cc's X11 window.
// Whole-file guarded and picked up by the src glob, so no build wiring.
#ifdef __EMSCRIPTEN__

#include <emscripten/em_asm.h>

extern "C" void Complain(const char *message) {
    // An overlay rather than alert(): alert() is blocked in some embedding
    // contexts and cannot be styled or scrolled, and a fatal message here is
    // often a multi-line what() string. Built with textContent, never innerHTML,
    // so the message cannot inject markup into the page - it is an exception
    // string that may contain a filename or user input.
    EM_ASM(
        {
            var msg = UTF8ToString($0);
            console.error(msg);
            // PROXY_TO_PTHREAD runs main() on a WORKER, and a worker has no
            // document. Touching it there threw "ReferenceError: document is
            // not defined" from inside the crash reporter, which replaced the
            // real error with a second one - a reporter that fails louder than
            // what it was reporting. console.error above has already run, so
            // bailing out here still surfaces the message.
            // Plain == on purpose: clang-format reads this block as C++ and splits a
            // === into '== =', which is a JavaScript syntax error at link time.
            if (typeof document == 'undefined') return;
            var d = document.createElement('div');
            d.setAttribute('style',
                           'position:fixed;inset:0;z-index:2147483647;background:rgba(0,0,0,.85);' +
                               'color:#fff;font:13px/1.45 monospace;padding:2em;overflow:auto;white-space:pre-wrap');
            var h = document.createElement('div');
            h.setAttribute('style', 'font-weight:bold;font-size:1.3em;margin-bottom:.75em;color:#ff6b6b');
            h.textContent = 'PCSX-Redux: fatal error';
            var b = document.createElement('div');
            b.textContent = msg;
            d.appendChild(h);
            d.appendChild(b);
            document.body.appendChild(d);
        },
        message);
}

#endif
