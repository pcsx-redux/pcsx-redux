/*

MIT License

Copyright (c) 2019 PCSX-Redux authors

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

#include <stdint.h>
#include <stdio.h>
#include <zlib.h>

#include <atomic>
#include <bit>
#include <compare>
#include <concepts>
#include <filesystem>
#include <queue>
#include <string>
#include <string_view>
#include <type_traits>

#include "support/polyfills.h"
#include "support/slice.h"
#include "support/ssize_t.h"

namespace PCSX {

namespace FileOps {
enum Truncate { TRUNCATE };
enum Create { CREATE };
enum ReadWrite { READWRITE };
}  // namespace FileOps

class File;

template <class T>
concept FileDerived = std::is_base_of<File, T>::value;

class File {
  public:
    enum FileType { RO_STREAM, RW_STREAM, RO_SEEKABLE, RW_SEEKABLE };
    virtual ~File() {
        if (m_refCount.load() != 0) {
            abort();
        }
    }
    void close() {
        if (m_closed) return;
        closeInternal();
        m_closed = true;
    }
    virtual bool isClosed() { return false; }
    virtual ssize_t rSeek(ssize_t pos, int wheel = SEEK_SET) { throw std::runtime_error("Can't seek for reading"); }
    virtual ssize_t rTell() { throw std::runtime_error("Can't seek for reading"); }
    virtual ssize_t wSeek(ssize_t pos, int wheel = SEEK_SET) { throw std::runtime_error("Can't seek for writing"); }
    virtual ssize_t wTell() { throw std::runtime_error("Can't seek for writing"); }
    virtual size_t size() { throw std::runtime_error("Unable to determine file size"); }
    virtual ssize_t read(void* dest, size_t size) { throw std::runtime_error("File is not readable"); }
    virtual ssize_t write(const void* src, size_t size) { throw std::runtime_error("File is not writable"); }
    virtual void write(Slice&& slice) { write(slice.data(), slice.size()); }
    virtual ssize_t readAt(void* dest, size_t size, size_t ptr) {
        auto old = rTell();
        rSeek(ptr, SEEK_SET);
        auto ret = read(dest, size);
        rSeek(old, SEEK_SET);
        return ret;
    }
    virtual ssize_t writeAt(const void* src, size_t size, size_t ptr) {
        auto old = wTell();
        wSeek(ptr, SEEK_SET);
        auto ret = write(src, size);
        wSeek(old, SEEK_SET);
        return ret;
    }
    virtual void writeAt(Slice&& slice, size_t ptr) { writeAt(slice.data(), slice.size(), ptr); }
    virtual bool eof() { return rTell() == size(); }
    virtual std::filesystem::path filename() { return ""; }
    virtual File* dup() { throw std::runtime_error("Cannot duplicate file"); };
    virtual bool failed() { return false; }
    virtual int getc() {
        if (eof()) return -1;
        return byte();
    }

    File(const File&) = delete;
    File(File&&) = delete;
    File& operator=(const File&) = delete;
    File& operator=(File&&) = delete;

    bool writable() { return (m_filetype == RW_STREAM) || (m_filetype == RW_SEEKABLE); }
    bool seekable() { return (m_filetype == RO_SEEKABLE) || (m_filetype == RW_SEEKABLE); }

    char* gets(char* s, size_t size) {
        if (!size) return nullptr;
        char* ptr = s;
        size--;
        while (true) {
            if (!size) {
                *ptr = 0;
                return s;
            }
            int c = getc();
            if ((c == 0) || (c == -1) || (c == '\n') || (c == '\r')) {
                *ptr = 0;
                return (s == ptr) && (c == -1) ? nullptr : s;
            }
            *ptr++ = c;
            size--;
        }
    }

    template <bool splitOnEOL = true>
    std::string gets() {
        int c;
        std::string ret;
        while (true) {
            c = getc();
            if ((c == 0) || (c == -1)) {
                return ret;
            }
            if constexpr (splitOnEOL) {
                if ((c == '\n') || (c == '\r')) {
                    return ret;
                }
            }
            ret += c;
        }
    }

    // A failed or empty read gives an empty slice: read() returns -1 at the end of the file.
    Slice read(ssize_t size) {
        void* data = malloc(size);
        return acquireRead(data, read(data, size));
    }

    Slice readAt(ssize_t size, ssize_t pos) {
        void* data = malloc(size);
        return acquireRead(data, readAt(data, size, pos));
    }

    static Slice acquireRead(void* data, ssize_t got) {
        Slice slice;
        if (got <= 0) {
            free(data);
            return slice;
        }
        slice.acquire(realloc(data, got), got);
        return slice;
    }

    std::string readString(size_t size) {
        std::string r(size, '\0');
        read(r.data(), size);
        return r;
    }

    std::string readStringAt(size_t size, ssize_t pos) {
        std::string r(size, '\0');
        readAt(r.data(), size, pos);
        return r;
    }

    void writeString(const std::string_view& str) { write(str.data(), str.size()); }
    void writeStringAt(const std::string_view& str, ssize_t pos) { writeAt(str.data(), str.size(), pos); }

    template <PolyFill::IntegralConcept T, std::endian endianess = std::endian::little>
    T read() {
        T ret = T(0);
        read(&ret, sizeof(T));
        if constexpr (endianess != std::endian::native) {
            ret = PolyFill::byteSwap(ret);
        }
        return ret;
    }

    template <PolyFill::IntegralConcept T, std::endian endianess = std::endian::little>
    T peek() {
        T ret = T(0);
        readAt(&ret, sizeof(T), rTell());
        if constexpr (endianess != std::endian::native) {
            ret = PolyFill::byteSwap(ret);
        }
        return ret;
    }

    template <PolyFill::IntegralConcept T, std::endian endianess = std::endian::little>
    T readAt(size_t pos) {
        T ret = T(0);
        readAt(&ret, sizeof(T), pos);
        if constexpr (endianess != std::endian::native) {
            ret = PolyFill::byteSwap(ret);
        }
        return ret;
    }

    template <PolyFill::IntegralConcept T, std::endian endianess = std::endian::little>
    void write(T val) {
        if constexpr (endianess != std::endian::native) {
            val = PolyFill::byteSwap(val);
        }
        write(&val, sizeof(T));
    }

    template <PolyFill::IntegralConcept T, std::endian endianess = std::endian::little>
    void writeAt(T val, size_t pos) {
        if constexpr (endianess != std::endian::native) {
            val = PolyFill::byteSwap(val);
        }
        writeAt(&val, sizeof(T), pos);
    }

    uint8_t byte() {
        uint8_t r = 0;
        read(&r, 1);
        return r;
    }

    uint8_t byteAt(size_t pos) {
        uint8_t r = 0;
        readAt(&r, 1, pos);
        return r;
    }

    void skip(size_t amount) { rSeek(amount, SEEK_CUR); }

    template <PolyFill::IntegralConcept T>
    void skip() {
        skip(sizeof(T));
    }

  protected:
    File(FileType filetype) : m_filetype(filetype) {}
    const FileType m_filetype;

  private:
    void addRef() { ++m_refCount; }
    void delRef() {
        if (--m_refCount == 0) {
            close();
            delete this;
        }
    }
    virtual void closeInternal() {}
    friend class IOBase;
    template <FileDerived T>
    friend class IO;

    std::atomic<unsigned> m_refCount = 0;
    bool m_closed = false;
};

class IOBase {
  public:
    void setFile(File* f) {
        if (m_file) m_file->delRef();
        m_file = f;
        if (f) f->addRef();
    }
    void reset() {
        if (m_file) m_file->delRef();
        m_file = nullptr;
    }
    auto operator<=>(const IOBase&) const = default;
    operator bool() const { return !!m_file; }

  protected:
    IOBase() {}
    IOBase(File* f) : m_file(f) {
        if (f) f->addRef();
    }
    ~IOBase() { reset(); }
    File* m_file = nullptr;
};

template <FileDerived T>
class IO : public IOBase {
  public:
    IO() {}
    IO(T* f) : IOBase(f) {}
    IO(const IO<T>& io) : IOBase(io.m_file) {}
    IO(IO<T>&& io) {
        m_file = io.m_file;
        io.m_file = nullptr;
    }
    template <FileDerived U>
    friend class IO;
    template <FileDerived U>
    IO(const IO<U>& io) : IOBase(io.m_file) {}
    template <FileDerived U>
    IO(IO<U>&& io) {
        m_file = io.m_file;
        io.m_file = nullptr;
    }
    template <FileDerived U>
    bool isA() {
        return !!dynamic_cast<U*>(m_file);
    }
    template <FileDerived U>
    IO<U> asA() {
        IO<U> h(dynamic_cast<U*>(m_file));
        return h;
    }
    IO<T>& operator=(const IO<T>& io) {
        setFile(io.m_file);
        return *this;
    }
    T* operator->() {
        if (!m_file) throw std::runtime_error("nullptr in operator->");
        T* r = dynamic_cast<T*>(m_file);
        if (!r) throw std::runtime_error("operator-> used with incompatible type - shouldn't happen");
        return r;
    }
    bool isNull() const { return !dynamic_cast<T*>(m_file); }
    operator bool() const { return !isNull(); }
};

class FailedFile : public File {
  public:
    FailedFile() : File(RO_STREAM) {}
    virtual bool failed() final override { return true; }
};

class BufferFile : public File {
  public:
    enum Acquire { ACQUIRE };
    enum Borrow { BORROW };
    // Makes a read-only buffer in memory, referencing the memory
    // without acquiring it. Therefore, memory must remain allocated
    // for the lifespan of the File object. Any dup call will still
    // reference the same memory location without trying to acquire it.
    BufferFile(void* data, size_t size);
    // Make a read-write buffer in memory, copying the memory. Writing
    // past the end will enlarge the buffer. A call to dup will also
    // duplicate the buffer.
    BufferFile(void* data, size_t size, FileOps::ReadWrite);
    // Same as above, but acquires the memory instead of copying it.
    BufferFile(void* data, size_t size, Acquire);
    // Same as above, but borrows the memory instead of copying it.
    // The buffer will be written to if needed, but will not be
    // enlarged if the write goes past the end.
    BufferFile(void* data, size_t size, Borrow);
    // Makes a dummy read-only file of size 1.
    BufferFile();
    // Makes an empty read-write buffer.
    BufferFile(FileOps::ReadWrite);
    // Makes a read-only buffer out of the input slice.
    BufferFile(Slice&& slice);

    virtual ssize_t rSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t rTell() final override { return m_ptrR; }
    virtual ssize_t wSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t wTell() final override { return m_ptrW; }
    virtual size_t size() final override { return m_size; }
    virtual ssize_t read(void* dest, size_t size) final override;
    virtual ssize_t write(const void* dest, size_t size) final override;
    virtual bool eof() final override;
    virtual File* dup() final override;

    Slice borrow(size_t offset = 0);

  private:
    virtual void closeInternal() final override;
    static uint8_t m_internalBuffer;
    size_t m_ptrR = 0;
    size_t m_ptrW = 0;
    size_t m_size = 0;
    size_t m_allocSize = 0;
    uint8_t* m_data = nullptr;
    bool m_owned = false;
    Slice m_slice;
};

class PosixFile : public File {
  public:
    virtual ssize_t rSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t rTell() final override { return m_ptrR; }
    virtual ssize_t wSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t wTell() final override { return m_ptrW; }
    virtual size_t size() final override;
    virtual ssize_t read(void* dest, size_t size) final override;
    virtual ssize_t write(const void* dest, size_t size) final override;
    virtual bool eof() final override {
        if (failed()) throw std::runtime_error("Invalid file");
        return feof(m_handle);
    }
    virtual File* dup() final override {
        return writable() ? new PosixFile(m_filename, FileOps::READWRITE) : new PosixFile(m_filename);
    }
    virtual bool failed() final override { return m_handle == nullptr; }
    virtual std::filesystem::path filename() final override { return m_filename; }
    virtual int getc() final override {
        if (failed()) throw std::runtime_error("Invalid file");
        int r = fgetc(m_handle);
        if (r >= 0) m_ptrR++;
        return r;
    }

    // Open the file in read-only mode.
    PosixFile(const std::filesystem::path& filename) : PosixFile(filename.u8string()) {}
    // Open the file in write-only mode, creating it if needed, and truncate it otherwise.
    PosixFile(const std::filesystem::path& filename, FileOps::Truncate)
        : PosixFile(filename.u8string(), FileOps::TRUNCATE) {}
    // Open the file in write-only mode, creating it if needed, but won't truncate.
    PosixFile(const std::filesystem::path& filename, FileOps::Create)
        : PosixFile(filename.u8string(), FileOps::CREATE) {}
    // Open the existing file in read-write mode. Must exist.
    PosixFile(const std::filesystem::path& filename, FileOps::ReadWrite)
        : PosixFile(filename.u8string(), FileOps::READWRITE) {}
#if defined(__cpp_lib_char8_t)
    PosixFile(const std::u8string& filename) : PosixFile(reinterpret_cast<const char*>(filename.c_str())) {}
    PosixFile(const std::u8string& filename, FileOps::Truncate)
        : PosixFile(reinterpret_cast<const char*>(filename.c_str()), FileOps::TRUNCATE) {}
    PosixFile(const std::u8string& filename, FileOps::Create)
        : PosixFile(reinterpret_cast<const char*>(filename.c_str()), FileOps::CREATE) {}
    PosixFile(const std::u8string& filename, FileOps::ReadWrite)
        : PosixFile(reinterpret_cast<const char*>(filename.c_str()), FileOps::READWRITE) {}
#endif
    PosixFile(const std::string& filename) : PosixFile(filename.c_str()) {}
    PosixFile(const std::string& filename, FileOps::Truncate) : PosixFile(filename.c_str(), FileOps::TRUNCATE) {}
    PosixFile(const std::string& filename, FileOps::Create) : PosixFile(filename.c_str(), FileOps::CREATE) {}
    PosixFile(const std::string& filename, FileOps::ReadWrite) : PosixFile(filename.c_str(), FileOps::READWRITE) {}
    PosixFile(const char* filename);
    PosixFile(const char* filename, FileOps::Truncate);
    PosixFile(const char* filename, FileOps::Create);
    PosixFile(const char* filename, FileOps::ReadWrite);

    // Close the file. Returns 0, or -1 if it never opened, a write came up
    // short, or flushing and closing it failed.
    int closeChecked();

  private:
    virtual void closeInternal() final override;
    const std::filesystem::path m_filename;
    FILE* m_handle = nullptr;
    size_t m_ptrR = 0;
    size_t m_ptrW = 0;
    bool m_writeFailed = false;
};

class SubFile : public File {
  public:
    SubFile(IO<File> file, size_t start, ssize_t size = -1)
        : File(file->seekable() ? RO_SEEKABLE : RO_STREAM),
          m_file(file),
          m_start(start),
          m_size(size < 0 ? file->size() - start : size) {}
    SubFile(IO<File> file, size_t start, ssize_t size, FileOps::ReadWrite)
        : File(file->seekable() ? RW_SEEKABLE : RW_STREAM),
          m_file(file),
          m_start(start),
          m_size(size < 0 ? file->size() - start : size) {}
    virtual ssize_t rSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t rTell() final override { return m_ptrR; }
    virtual ssize_t wSeek(ssize_t pos, int wheel) final override;
    virtual ssize_t wTell() final override { return m_ptrW; }
    virtual size_t size() final override { return m_size; }
    virtual ssize_t read(void* dest, size_t size) final override;
    virtual ssize_t readAt(void* dest, size_t size, size_t ptr) final override;
    virtual ssize_t write(const void* src, size_t size) final override;
    virtual ssize_t writeAt(const void* src, size_t size, size_t ptr) final override;
    virtual bool eof() final override { return m_ptrR == m_size; }
    virtual File* dup() final override { return new SubFile(m_file, m_start, m_size); }
    virtual bool failed() final override { return m_file->failed(); }

  private:
    IO<File> m_file;
    size_t m_ptrR = 0;
    size_t m_ptrW = 0;
    const size_t m_start = 0;
    const size_t m_size = 0;
};

class Fifo : public File {
  public:
    Fifo() : File(RO_STREAM) {}
    void reset() {
        m_size = 0;
        m_ptrR = 0;
        while (!m_slices.empty()) m_slices.pop();
    }
    virtual size_t size() final override { return m_size; }
    virtual ssize_t read(void* dest, size_t size) final override;
    virtual bool eof() final override { return m_slices.empty(); }

    void pushSlice(Slice&& slice) {
        m_size += slice.size();
        m_slices.emplace(std::move(slice));
    }

  private:
    std::queue<Slice> m_slices;
    size_t m_ptrR = 0;
    size_t m_size = 0;
};

}  // namespace PCSX
