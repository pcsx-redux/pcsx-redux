/*

MIT License

Copyright (c) 2026 PCSX-Redux authors

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

#pragma once

// The wasm build has no libuv, no curl and no worker thread, so uvfile.cc is not
// compiled. This declares the same names with synchronous implementations, so
// that every caller - the cdriso readers, isobrowser, the Lua file bindings,
// SIO1 - keeps compiling unchanged.
//
// The rule here is that a stub must be HONEST, not silent. A download does not
// pretend to succeed: it constructs a file that reports failed(). A fifo has no
// socket, so it is a failed file too. Caching is instantaneous rather than
// absent, because a PosixFile really is fully available the moment it opens.
// Nothing in here reports success for work it did not do.

#include <functional>
#include <string>
#include <string_view>

#include "support/file.h"

// libuv appears in these signatures only as an opaque handle, same as in
// core/system.h. Repeating the typedef is legal and keeps this header free of
// any dependency on <uv.h>.
struct uv_loop_s;
typedef struct uv_loop_s uv_loop_t;

namespace PCSX {

// The native UvThreadOp is an intrusive list node on the libuv worker thread.
// Here it carries no state; it exists so that call sites can still name the
// type, and so dynamic_cast<File*> on one keeps working.
class UvThreadOp {
  public:
    virtual ~UvThreadOp() = default;
    enum DownloadUrl { DOWNLOAD_URL };

    // An RAII guard around the worker thread. There is no thread, so the guard
    // is empty and ~UvThread has nothing to join.
    struct UvThread {
        UvThread() = default;
        ~UvThread() = default;
        void setEmergencyExit() {}
    };

    virtual bool canCache() const { return false; }
    void startCaching() { startCaching(nullptr, nullptr); }
    // Caching means pulling a file into memory off the worker thread. Synchronous
    // I/O is already there, so the work is done and the callback fires at once.
    virtual void startCaching(std::function<void()>&& completed, uv_loop_t* loop) {
        if (completed) completed();
    }
    bool caching() { return false; }
    float cacheProgress() { return 1.0f; }
    void waitCache() {}

    // No operation is ever in flight, so the walker has nothing to visit. The
    // UvFiles panel in gui.cc renders an empty table, which is accurate.
    static void iterateOverAllOps(std::function<void(UvThreadOp*)> walker) {}

    static float getReadRate() { return 0.0f; }
    static float getWriteRate() { return 0.0f; }
    static float getDownloadRate() { return 0.0f; }
};

class UvFile : public PosixFile, public UvThreadOp {
  public:
    using PosixFile::PosixFile;

    // PosixFile declares write(const void*, size_t) as a final override, which
    // name-hides File::write(Slice&&) and the write(T) template. Un-hide them.
    using File::write;
    using PosixFile::write;

    // There is no network. Both download constructors open nothing, so the file
    // reports failed() and the caller takes its error path.
    UvFile(const std::string_view& url, DownloadUrl) : PosixFile(std::filesystem::path()) {}
    UvFile(const std::string_view& url, std::function<void()>&& completed, uv_loop_t* other, DownloadUrl)
        : PosixFile(std::filesystem::path()) {}

    virtual bool canCache() const override { return true; }

    // Writes here are synchronous, so there is nothing queued to wait for.
    int closeAndWait() { return closeChecked(); }
};

// SIO1 over TCP needs a socket. Without one the fifo is a file that failed to
// open, and it is never mid-connect.
class UvFifo : public FailedFile, public UvThreadOp {
  public:
    UvFifo(const std::string_view address, unsigned port) {}
    bool isConnecting() { return false; }
};

class UvFifoListener : public UvThreadOp {
  public:
    UvFifoListener() {}
    void start(unsigned port, uv_loop_t* loop, void* async, std::function<void(UvFifo*)>&& cb) {}
    void stop() {}
    const char* lastError() const { return ""; }
};

}  // namespace PCSX
