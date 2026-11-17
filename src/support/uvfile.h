/*

MIT License

Copyright (c) 2022 PCSX-Redux authors

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

// The wasm build has no libuv and drops uvfile.cc entirely. Dispatch here so the
// ~30 v1-path call sites keep saying `new UvFile(path)` unchanged.
#ifdef __EMSCRIPTEN__
#include "support/uvfile-wasm.h"
#else

#include <curl/curl.h>
#include <uv.h>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cq/concurrent_queue.h"
#include "support/file.h"
#include "support/list.h"

namespace PCSX {

class UvThreadOp;
typedef Intrusive::List<UvThreadOp> UvThreadOpListType;

class UvThreadOp : public UvThreadOpListType::Node {
  public:
    enum DownloadUrl { DOWNLOAD_URL };
    struct UvThread {
        void setEmergencyExit() { m_emergencyExit = true; }
        UvThread() { PCSX::UvThreadOp::startThread(); }
        ~UvThread() {
            if (!m_emergencyExit) {
                PCSX::UvThreadOp::stopThread();
            } else {
                // Not waiting for the loop is the point of an emergency exit, but a
                // still-joinable static std::thread calls std::terminate when it is
                // destroyed at process exit.
                PCSX::UvThreadOp::s_uvThread.detach();
            }
        }

      private:
        bool m_emergencyExit = false;
    };

  private:
    static void startThread();
    static void stopThread();

  public:
    virtual bool canCache() const = 0;
    void startCaching() { startCaching(nullptr, nullptr); }
    virtual void startCaching(std::function<void()>&& completed, uv_loop_t* loop) {
        throw std::runtime_error("Not cachable");
    }
    bool caching() { return m_cache; }
    float cacheProgress() { return m_cacheProgress.load(std::memory_order_relaxed); }
    void waitCache() { m_cacheBarrier.get_future().get(); }

    static void iterateOverAllOps(std::function<void(UvThreadOp*)> walker) {
        for (auto& f : s_allOps) walker(&f);
    }

    static float getReadRate() {
        return 1000.0f * float(s_dataReadLastTick.load(std::memory_order_relaxed)) / float(c_tick);
    }
    static float getWriteRate() {
        return 1000.0f * float(s_dataWrittenLastTick.load(std::memory_order_relaxed)) / float(c_tick);
    }
    static float getDownloadRate() {
        return 1000.0f * float(s_dataDownloadLastTick.load(std::memory_order_relaxed)) / float(c_tick);
    }

  private:
    virtual void downloadDone(CURLMsg* message) { throw std::runtime_error("Not cachable"); }

  protected:
    static uv_loop_t s_uvLoop;
    uint8_t* m_cache = nullptr;
    std::atomic<float> m_cacheProgress = 0.0;
    std::promise<void> m_cacheBarrier;
    struct UvRequest {
        std::function<void(uv_loop_t*)> functor;
        uint64_t sequence;
    };
    static void request(std::function<void(uv_loop_t*)>&& functor) {
        UvRequest req;
        req.functor = std::move(functor);
        req.sequence = s_writeSequence++;
        s_queue.Enqueue(std::move(req));
        uv_async_send(&s_kicker);
    }
    static CURLM* s_curlMulti;
    static uv_timer_t s_curlTimeout;

    static int curlSocketFunction(CURL* easy, curl_socket_t s, int action, void* userp, void* socketp);
    static int curlTimerFunction(CURLM* multi, long timeout_ms, void* userp);
    static void processCurlMultiInfo();

    // This isn't really safe, but it's not meant to.
    static bool s_threadRunning;
    static std::thread s_uvThread;
    static uv_async_t s_kicker;
    static uv_timer_t s_timer;
    static size_t s_dataReadTotal;
    static size_t s_dataWrittenTotal;
    static size_t s_dataDownloadTotal;
    static size_t s_dataReadSinceLastTick;
    static size_t s_dataWrittenSinceLastTick;
    static size_t s_dataDownloadSinceLastTick;
    static std::atomic<size_t> s_dataReadLastTick;
    static std::atomic<size_t> s_dataWrittenLastTick;
    static std::atomic<size_t> s_dataDownloadLastTick;
    static constexpr uint64_t c_tick = 500;

    static UvThreadOpListType s_allOps;

  private:
    static ConcurrentQueue<UvRequest> s_queue;
    static uint64_t s_writeSequence;
    static uint64_t s_readSequence;
};

class UvFile : public File, public UvThreadOp {
  public:
    virtual ssize_t rSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t rTell() final override { return m_ptrR; }
    virtual ssize_t wSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t wTell() final override { return m_ptrW; }
    virtual size_t size() final override { return m_size; }
    virtual ssize_t read(void* dest, size_t size) final override;
    virtual ssize_t write(const void* dest, size_t size) final override;
    virtual void write(Slice&& slice) final override;
    virtual ssize_t readAt(void* dest, size_t size, size_t ptr) final override;
    virtual ssize_t writeAt(const void* src, size_t size, size_t ptr) final override;
    virtual void writeAt(Slice&& slice, size_t ptr) final override;
    virtual bool failed() final override { return m_failed; }
    virtual bool eof() final override;
    virtual std::filesystem::path filename() final override { return m_filename; }
    virtual File* dup() final override {
        return m_download   ? new UvFile(m_filename.string(), DOWNLOAD_URL)
               : writable() ? new UvFile(m_filename, FileOps::READWRITE)
                            : new UvFile(m_filename);
    }

    // Open the file in read-only mode.
    UvFile(const std::filesystem::path& filename) : UvFile(filename.u8string()) {}
    // Open the file in write-only mode, creating it if needed, and truncate it otherwise.
    UvFile(const std::filesystem::path& filename, FileOps::Truncate) : UvFile(filename.u8string(), FileOps::TRUNCATE) {}
    // Open the file in write-only mode, creating it if needed, but won't truncate.
    UvFile(const std::filesystem::path& filename, FileOps::Create) : UvFile(filename.u8string(), FileOps::CREATE) {}
    // Open the existing file in read-write mode. Must exist.
    UvFile(const std::filesystem::path& filename, FileOps::ReadWrite)
        : UvFile(filename.u8string(), FileOps::READWRITE) {}
    // Download a URL
    UvFile(const std::string_view& url, DownloadUrl) : UvFile(url, nullptr, nullptr, DOWNLOAD_URL) {}
    UvFile(const std::string_view& url, std::function<void()>&& completed, uv_loop_t* other, DownloadUrl);
#if defined(__cpp_lib_char8_t)
    UvFile(const std::u8string& filename) : UvFile(reinterpret_cast<const char*>(filename.c_str())) {}
    UvFile(const std::u8string& filename, FileOps::Truncate)
        : UvFile(reinterpret_cast<const char*>(filename.c_str()), FileOps::TRUNCATE) {}
    UvFile(const std::u8string& filename, FileOps::Create)
        : UvFile(reinterpret_cast<const char*>(filename.c_str()), FileOps::CREATE) {}
    UvFile(const std::u8string& filename, FileOps::ReadWrite)
        : UvFile(reinterpret_cast<const char*>(filename.c_str()), FileOps::READWRITE) {}
#endif
    UvFile(const std::string& filename) : UvFile(filename.c_str()) {}
    UvFile(const std::string& filename, FileOps::Truncate) : UvFile(filename.c_str(), FileOps::TRUNCATE) {}
    UvFile(const std::string& filename, FileOps::Create) : UvFile(filename.c_str(), FileOps::CREATE) {}
    UvFile(const std::string& filename, FileOps::ReadWrite) : UvFile(filename.c_str(), FileOps::READWRITE) {}
    UvFile(const char* filename);
    UvFile(const char* filename, FileOps::Truncate);
    UvFile(const char* filename, FileOps::Create);
    UvFile(const char* filename, FileOps::ReadWrite);

    void startCaching() { startCaching(nullptr, nullptr); }
    virtual void startCaching(std::function<void()>&& completed, uv_loop_t* loop) override;

    // Close the file and block until every queued write and the close itself
    // have completed. Returns 0 on success, or the first libuv error code.
    // close() alone returns before the writes have landed.
    int closeAndWait();

  private:
    virtual void closeInternal() final override;
    virtual bool canCache() const override { return true; }
    void openwrapper(const char* filename, int flags);
    void readCacheChunk(uv_loop_t* loop);
    void readCacheChunkResult();
    virtual void downloadDone(CURLMsg* message) override;
    size_t curlWriteFunction(char* ptr, size_t size);
    int curlXferInfoFunction(curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

    static size_t curlWriteFunctionTrampoline(char* ptr, size_t size, size_t nmemb, void* userdata);
    static int curlXferInfoFunctionTrampoline(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                                              curl_off_t ulnow);
    void cacheCallbackSetup(std::function<void()>&& callbackDone, uv_loop_t* otherLoop);

    bool m_failed = true;
    bool m_download = false;
    std::atomic<bool> m_cancelDownload = false;
    std::function<void()> m_cachingDoneCB = nullptr;
    uv_async_t* m_cbAsync = nullptr;
    const std::filesystem::path m_filename;
    size_t m_ptrR = 0;
    size_t m_ptrW = 0;
    size_t m_size = 0;
    uv_file m_handle = -1;  // ugh
    CURL* m_curlHandle = nullptr;
    uv_buf_t m_cacheBuf;
    uv_fs_t m_cacheReq;
    size_t m_cachePtr = 0;
    struct PendingCloseInfo {
        unsigned pendingWrites = 0;
        bool closePending = false;
        // First error from a queued write, or 0.
        int error = 0;
        // Set by closeAndWait(), fulfilled once the handle is closed.
        std::promise<int>* closeDone = nullptr;
    };
    struct WriteInfo {
        uv_buf_t buf;
        uv_fs_t req;
        Slice slice;
        uv_file handle;
        PendingCloseInfo* pendingCloseInfo;
    };
    static void queueWrite(uv_loop_s* loop, WriteInfo* info, size_t offset);
    static void writeDone(WriteInfo* info, uv_loop_s* loop, ssize_t result);
    static void closeUVHandle(uv_file handle, uv_loop_s* loop, PendingCloseInfo* pendingCloseInfo);
    PendingCloseInfo* m_pendingCloseInfo = nullptr;
    std::promise<int>* m_closeWaiter = nullptr;
};

class UvFifo : public File, public UvThreadOp {
  public:
    UvFifo(const std::string_view address, unsigned port);
    virtual bool isClosed() final override { return m_closed.load(); }
    virtual ssize_t read(void* dest, size_t size) final override;
    virtual ssize_t write(const void* src, size_t size) final override;
    virtual void write(Slice&& slice) final override;
    virtual size_t size() final override { return m_size.load(); }
    virtual bool eof() final override { return m_closed.load() && (m_size.load() == 0); }
    virtual bool failed() final override { return m_control->m_failed.test(); }
    bool isConnecting() { return m_control->m_connecting.test(); }
    // uv error code of a failed connect, or 0. Meaningful once failed() is set.
    // Without this a failed outgoing connection is a bare bool, which is enough
    // to colour a status bullet red and not enough to say why.
    int connectErrorCode() const { return m_connectErrorCode.load(std::memory_order_acquire); }
    const char* connectError() const {
        int code = connectErrorCode();
        return code == 0 ? "" : uv_strerror(code);
    }

    // Opt-in readable notification.
    //
    // Reads land in a lock-free queue on the uv worker thread, so without this
    // the only way to notice traffic is to poll size() from somewhere - which
    // is what SIO1 and ATCons do off the counters, and which is fine for them.
    // It is not fine for anything latency-sensitive or idle-heavy, so a
    // consumer can hand over an async living on ITS OWN loop and get woken
    // instead. `cb` runs on that loop, never on the worker thread.
    //
    // Fires on data arrival and on EOF/error. Wake-ups are coalesced by libuv,
    // so treat it as "something changed, go look", not as one call per chunk.
    // Safe to install after data has already arrived: it fires once up front if
    // the fifo is already readable or already closed, so no wake-up is lost in
    // the gap between accept and setNotifier.
    //
    // The caller owns `async` and must keep it alive until it closes the fifo,
    // then uv_close it, exactly like UvFifoListener's.
    void setNotifier(uv_loop_t* loop, uv_async_t* async, std::function<void()>&& cb);
    void clearNotifier() {
        std::lock_guard lock(m_control->m_lock);
        m_notifyAsync.store(nullptr, std::memory_order_release);
    }

    // Writes are queued to the uv worker thread, so "I called write()" and "the
    // bytes left the machine" are different moments. Closing between the two
    // cancels the pending uv_writes and truncates whatever was in flight, which
    // for an HTTP response is a silently short reply. close() is therefore
    // graceful: the socket is torn down by the last write to complete, not by
    // the caller. Exposed for consumers that want to know, e.g. to hold a
    // connection open until a response has drained.
    size_t pendingWrites() const { return m_control->m_pending.load(std::memory_order_acquire); }

  private:
    virtual void closeInternal() final override;
    UvFifo(uv_tcp_t*);
    struct Control;
    static void startRead(Control*);
    // Worker-thread side of the notification. A no-op when nobody opted in.
    // Worker callers hold m_control->m_lock, so clearNotifier() returning means
    // no uv_async_send on the old async is still in progress.
    void notify() {
        auto async = m_notifyAsync.load(std::memory_order_acquire);
        if (async) uv_async_send(async);
    }
    virtual bool canCache() const override { return false; }
    uv_tcp_t* m_tcp = nullptr;
    std::atomic<bool> m_closed = false;
    static constexpr size_t c_chunkSize = 4096;
    ConcurrentQueue<Slice> m_queue;
    std::atomic<size_t> m_size = 0;
    std::atomic<int> m_connectErrorCode = 0;
    Slice m_slice;
    size_t m_currentPtr = 0;
    std::atomic<uv_async_t*> m_notifyAsync = nullptr;

    // The socket and the graceful-close bookkeeping outlive the UvFifo on
    // purpose. An in-flight uv_write holds a reference, so a fifo destroyed
    // while writes are still queued cannot pull the state out from under the
    // worker thread - which is why the write path must never capture `this`.
    //
    // The read and connect paths do need the fifo, so they reach it through
    // m_fifo under m_lock. closeInternal() clears it before File::delRef()
    // deletes the fifo, and the socket's data pointer is this block rather than
    // the fifo, so a read landing after that finds nullptr instead of freed
    // memory.
    struct Control {
        std::atomic<size_t> m_pending = 0;
        bool m_closeRequested = false;  // worker thread only
        uv_tcp_t* m_tcp = nullptr;      // worker thread only, after construction
        std::mutex m_lock;
        UvFifo* m_fifo = nullptr;  // guarded by m_lock
        // In here rather than in the fifo so a write that errors after the
        // fifo is gone has somewhere to land.
        std::atomic_flag m_failed;
        std::atomic_flag m_connecting;
        // Armed by closeInternal(), so a peer that stops reading, or never
        // hangs up, cannot hold the socket open forever. The block keeps itself
        // alive through m_lingerSelf until the timer is closed.
        static constexpr uint64_t c_lingerTimeoutMs = 30000;
        uv_timer_t m_linger;                    // worker thread only
        std::shared_ptr<Control> m_lingerSelf;  // worker thread only
        uv_shutdown_t m_shutdown;               // worker thread only
        bool m_shutdownStarted = false;         // worker thread only
        bool m_shutdownDone = false;            // worker thread only
        bool m_peerDone = false;                // worker thread only
        void shutdown();
        void closeNow();
        void writeCompleted();
        void writeFailed();
    };
    std::shared_ptr<Control> m_control = std::make_shared<Control>();
    std::function<void()> m_notifyCb;
    friend class UvFifoListener;
};

class UvFifoListener : public UvThreadOp {
  public:
    // The bind and the listen both happen asynchronously on the uv worker
    // thread, so a failure cannot be returned from start(). It lands here
    // instead, and the callback is invoked with nullptr the same way an
    // orderly shutdown does, so consumers have exactly one path for "this
    // listener is finished, clean up".
    enum class Status { Stopped, Starting, Listening, Failed };

    UvFifoListener() {}
    void start(unsigned port, uv_loop_t* loop, uv_async_t* async, std::function<void(UvFifo*)>&& cb);
    void stop();

    Status status() const { return m_status.load(std::memory_order_acquire); }
    bool isListening() const { return status() == Status::Listening; }
    // uv error code of whichever step failed, or 0. Meaningful once status() is Failed.
    int lastErrorCode() const { return m_lastErrorCode.load(std::memory_order_acquire); }
    const char* lastError() const {
        int code = lastErrorCode();
        return code == 0 ? "" : uv_strerror(code);
    }

  private:
    virtual bool canCache() const override { return false; }
    // Worker-thread only. Records the error, tears down the half-built server
    // handle, and wakes the consumer with a nullptr.
    void failed(int code);
    uv_async_t* m_async = nullptr;
    uv_tcp_t m_server = {};
    std::function<void(UvFifo*)> m_cb;
    ConcurrentQueue<UvFifo*> m_pending;
    std::atomic<Status> m_status = Status::Stopped;
    std::atomic<int> m_lastErrorCode = 0;
};

}  // namespace PCSX

#endif  // __EMSCRIPTEN__
