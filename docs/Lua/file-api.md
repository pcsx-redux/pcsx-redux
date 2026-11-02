# File API

## Introduction & Rationale
While the normal Lua io API is loaded, there's a more powerful API that's more tightly integrated with the rest of the PCSX-Redux File handling code. It's an abstraction class that allows seamless manipulation of various objects using a common API.

The File objects have different properties depending on how they are created and their intention. But generally speaking, the following rules apply:

- Files are reference counted. They will be deleted when the reference count reaches zero. The Lua garbage collector will only decrease the reference count.
- Whenever possible, writes are deferred to an asynchronous thread, making writes return basically instantly. This speed up comes at the trade off of data integrity, which means writes aren't guaranteed to be flushed to the disk yet when the function returns. Data will always have integrity internally within PCSX-Redux however, and when exiting normally, all data will be flushed to the disk.
- Some File objects can be cached. When caching, reads and writes will be done transparently, and the cache will be used instead of the actual file. This will make reads return basically instantly too.
- The Read and Write APIs can haul LuaBuffer objects. These are Lua objects that can be used to read and write data to the file. You can construct one using the `Support.NewLuaBuffer(size)` function. They can be cast to strings, and can be used as a table for reading and writing bytes off of it, in a 0-based fashion. The length operator will return the size of the buffer. The methods `:maxsize()` and `:resize(size)` are available. They also have a `.pbSlice` property that implicitly converts them to a Lua-Protobuf's `pb.slice`, which can then be passed to `pb.decode`.
- The Read and Write APIs can also function using Lua-Protobuf's buffers and slices respectively.
- If the file isn't closed when the file object is destroyed, it'll be closed then, but letting the garbage collector do the closing is not recommended. This is because the garbage collector will only run when the memory pressure is high enough, and the file handle will be held for a long time.
- When using streamed functions, unlike POSIX files handles, there's two distinct seeking pointers: one for reading and one for writing.

## Common API for all File objects

All File objects have the following API attached to them as methods:

Closes and frees any associated resources. Better to call this manually than letting the garbage collector do it:
```lua
:close()
```

Reads from the File object and advances the read pointer accordingly. The return value depends on the variant used.
```lua
:read(size)            -- returns a LuaBuffer
:read(ptr, size)       -- returns the number of bytes read, ptr has to be a cdata of pointer type
:read(buffer)          -- returns the number of bytes read, and adjusts the buffer's size
:read(pb_buffer, size) -- returns the number of bytes read, while appending to the pb_buffer's existing data
:gets()                -- returns a string, up to the next newline character
```

Reads from the File object at the specified position. No pointers are modified. The return value depends on the variant used, just like the non-At variants above.
```lua
:readAt(size, pos)
:readAt(ptr, size, pos)
:readAt(buffer, pos)
:readAt(pb_buffer, size, pos)
```

Writes to the File object. The non-At variants will advances the write pointer accordingly. The At variants will not modify the write pointer, and simply write at the requested location. Returns the number of bytes written. The `string` variants will in fact take any object that can be transformed to a string using `tostring()`.
```lua
:write(string)
:write(buffer)
:write(slice)
:write(pb_slice)
:write(ptr, size)
:writeAt(string, pos)
:writeAt(buffer, pos)
:writeAt(slice, pos)
:writeAt(pb_slice, pos)
:writeAt(ptr, size, pos)
```

Note that in this context, `pb_slice` and `pb_buffer` refer to Lua-Protobuf's `pb.slice` and `pb.buffer` objects respectively.

Some APIs may return a `Slice` object, which is an opaque buffer coming from C++. The `write` and `writeAt` methods can take a `Slice`. It is possible to write a slice to a file in a zero-copy manner, which will be more efficient:

```lua
:writeMoveSlice(slice)
:writeAtMoveSlice(slice, pos)
```

After which, the slice will be consumed and not reusable. The `Slice` object is convertible to a string using `tostring()`, and also has, among others, the members `data`, which is a `const void*`, and `size`. Once consumed by the `MoveSlice` variants, the size of a slice will go down to zero.

Finally, it is possible to convert a `Slice` object to a `pb.slice` one using the `Support.sliceToPBSlice` function. However, the same caveats as for normal `pb.slice` objects apply: it is fragile, and will be invalidated if the underlying Slice is moved or destroyed, so it is recommended to use it as a temporary object, such as an argument to `pb.decode`. Still, it is a much faster alternative to calling `tostring()` which will make a copy of the underlying slice.

The following methods manipulate the read and write pointers. All of them return their corresponding pointer. The `wheel` argument can be of the values `'SEEK_SET'`, `'SEEK_CUR'`, and `'SEEK_END'`, and will default to `'SEEK_SET'`.
```lua
:rSeek(pos[, wheel])
:rTell()
:wSeek(pos[, wheel])
:wTell()
```

These will query the corresponding File object.
```lua
:size()      -- Returns the size in bytes, if possible. If the file is not seekable, will throw an error.
:seekable()  -- Returns true if the file is seekable.
:writable()  -- Returns true if the file is writable.
:eof()       -- Returns true if the read pointer is at the end of file.
:failed()    -- Returns true if the file failed in some ways. The File object is defunct if this is true.
:cacheable() -- Returns true if the file is cacheable.
:caching()   -- Returns true if caching is in progress or completed.
:cacheProgress() -- Returns a value between 0 and 1 indicating the progress of the caching operation.
```

If applicable, this will start caching the corresponding file in memory.
```lua
:startCaching()
```

Same as above, but will suspend the current coroutine until the caching is done. Cannot be used with the main thread.
```lua
:startCachingAndWait()
```

Duplicates the File object. This will re-open the file, and possibly duplicate all ressources associated with it.
```lua
:dup()
```

Creates a read-only view of the file starting at the specified position, spanning the specified length. The view will be a new File object, and will be a view of the same underlying file. The default values of start and length are 0 and -1 respectively, which will effectively create a view of the entire file. The view may have less features than the underlying file, but will always be seekable, and keep its seeking position independent of the underlying file. The view will hold a reference to the underlying file.
```lua
:subFile([start[, length]])
```

In addition to the above methods, the File API has these helpers, that'll read or write binary values off their corresponding stream position for the non-At variants, or at the indicated position for the At variants. All the values will be read or stored in Little Endian, regardless of the host's endianness.
```lua
:readU8(), :readU16(), :readU32(), :readU64(),
:readI8(), :readI16(), :readI32(), :readI64(),
:readU8At(pos), :readU16At(pos), :readU32At(pos), :readU64At(pos),
:readI8At(pos), :readI16At(pos), :readI32At(pos), :readI64At(pos),
:writeU8(val), :writeU16(val), :writeU32(val), :writeU64(val),
:writeI8(val), :writeI16(val), :writeI32(val), :writeI64(val),
:writeU8At(val, pos), :writeU16At(val, pos), :writeU32At(val, pos), :writeU64At(val, pos),
:writeI8At(val, pos), :writeI16At(val, pos), :writeI32At(val, pos), :writeI64At(val, pos),
```

## Creating File objects

The Lua VM can create File objects in different ways:
```lua
Support.File.open(filename[, type])
Support.File.buffer()
Support.File.buffer(ptr, size[, type])
Support.File.mem4g()
Support.File.uvFifo(address, port)
Support.File.zReader(file[, size[, raw]])
```

### Basic files

The `open` function will function on filesystem and network URLs, while the `buffer` function will generate a memory-only File object that's fully readable, writable, and seekable. The `type` argument of the `open` function will determine what happens exactly. It's a string that can have the following values:

- `READ`: Opens the file for reading only. Will fail if the file does not exist. This is the default type.
- `TRUNCATE`: Opens the file for reading and writing. If the file does not exist, it will be created. If it does exist, it will be truncated to 0 size.
- `CREATE`: Opens the file for reading and writing. If the file does not exist, it will be created. If it does exist, it will be left untouched.
- `READWRITE`: Opens the file for reading and writing. Will fail if the file does not exist.
- `DOWNLOAD_URL`: Opens the file for reading only. Will immediately start downloading the file from the network. The `filename` argument will be treated as a URL. The [curl](http://curl.se/libcurl) is the backend for this feature, and its [url schemes](https://everything.curl.dev/cmdline/urls) are supported. The progress of the download can be monitored with the `:cacheProgress()` method.
- `DOWNLOAD_URL_AND_WAIT`: As above, but suspends the current coroutine until the download is done. Cannot be used with the main thread.

### Buffers

When calling `.buffer()` with no argument, this will create an empty read-write buffer. When calling it with a cdata pointer and a size, this will have the following behavior, depending on type:

- `READWRITE` (or no type): The memory passed as an argument will be copied first.
- `READ`: The memory passed as an argument will be referenced, and the lifespan of said memory needs to outlast the File object. The File object will be read-only.
- `ACQUIRE`: It will acquire the pointer passed as an argument, and free it later using `free()`, meaning it needs to have been allocated using `malloc()` in the first place.

The `.mem4g()` constructor will return a sparse buffer that has a virtual 4GB span. It can be used to read and write data in the 4GB range, but will not actually allocate any memory until the data is actually written to. This is useful for doing operations that are similar to that of the PlayStation memory. The `.mem4g()` constructor will return a File object that's fully readable, writable, and seekable. Its size will always be 4GB. The returned object will have 3 additional methods:

- `:lowestAddress()`: Returns the lowest address that has been written to.
- `:highestAddress()`: Returns the highest address that has been written to.
- `:actualSize()`: Returns the size of the buffer, which is the highest address minus the lowest address.

This is a useful object to use with the `:subFile()` method, as it will allow you to create a view of a specific range of the 4GB memory. Specifically, `obj:subFile(obj:lowestAddress(), obj:actualSize())` will create a view of the entire memory that has been written to.

### Network streams

The `uvFifo` function will create a File object that will read from and write to the specified TCP address and port after connecting to it. The `:failed()` method will return true in case of a connection failure. The address is a string, and must be a strict IP address, no hostnames allowed. The port is a number between 1 and 65535 inclusive. As the name suggests, this object is a FIFO, meaning that incoming bytes will be consumed by any read operation. The `:size()` method will return the number of bytes in the FIFO. Writes will be immediately sent over. There are no reception guarantees, as the other side might have disconnected at any point. The `:eof()` method will return true when the opposite end of the stream has been disconnected and there's no more bytes in the FIFO. In addition to the normal `File` API, a `uvFifo` has a method called `:isConnecting()`, which returns a boolean indicating the fifo is still connecting, meaning it's possible to verify if the fifo has successfully connected using the boolean expression `not fifo:isConnecting() and not fifo:failed()`.

### Compressed streams

The `zReader` function will create a read-only File object which decompresses the data from the specified File object. The `file` argument is a File object, and the `size` argument is an optional number that will be used to determine the size of the decompressed data. If not specified, the resulting file won't be seekable, and its `:size()` method won't work, but the file will be readable until `:eof()` returns true. The `raw` argument is an optional string that needs to be equal to `'RAW'`, and will determine whether the data is compressed using the raw deflate format, or the zlib format. Any other string means the zlib format will be used.

## Iso files
There is a fairly complete API for working with ISO files. Two related topics have their own pages: creating new images from scratch is covered in [Building ISO images](iso-builder.md), and the patch generation mechanism behind the `:savePPF()` method below is covered in [PPF patches](ppf.md).

- `PCSX.getCurrentIso()` will return an `Iso` object representing the currently loaded ISO file by the emulator.
- `PCSX.openIso(pathOrFile)` will return an `Iso` object opened from the specified argument, which can either be a filesystem path, or a `File` object.

The following methods are available on the `Iso` object:

```lua
:failed()       -- Returns true if the Iso file failed in some ways. The Iso object is defunct if this is true.
:createReader() -- Returns an ISOReader object off the Iso file.
:open(lba[, size[, mode]]) -- Returns a File object off the specified span of sectors.
:clearPPF()     -- Clears out all of the currently applied patches.
:savePPF()      -- Saves the currently applied patches to a PPF file named after the ISO file.
:getTN()        -- Returns the number of tracks. An image without a track table reports 1.
:getTD(track)   -- Returns the start of the track as an MSF struct with m, s and f fields, as plain numbers. Track 0 returns the position past the last track, and a single track image or an out of range track returns 00:02:00.
```

The `:open` method has some magic built-in. The size argument is optional, and if missing, the code will attempt to guess the size of the underlying file within the Iso. It will represent the size of the virtual file in bytes. The size guessing mechanism can only work on MODE2 FORM1 or FORM2 sectors, and will result in a failed File object otherwise. The mode argument is optional, and can be one of the following:

- `'GUESS'`: will attempt to guess the mode of the file. This is the default.
- `'RAW'`: the returned File object will read 2352 bytes per sector.
- `'M1'`: the returned File object will read 2048 bytes per sector.
- `'M2_RAW'`: the returned File object will read 2336 bytes per sector. This can't be guessed. This is useful for extracting STR files that require the subheaders to be present.
- `'M2_FORM1'`: the returned File object will read 2048 bytes per sector.
- `'M2_FORM2'`: the returned File object will read 2324 bytes per sector.

The resulting File object will cache a single full sector in memory, meaning that small sequential reads won't read the same sector over and over from the disk.

The resulting File object will be writable, which will temporarily patch the CD-Rom image file in memory. It is possible to flush the patches to a PPF file by calling the `:savePPF()` method of the corresponding Iso object. When writing to one of these files, the filesystem metadata information will not be updated, meaning that the size of the file on the filesystem will not change, despite it being possible to write past the end of it and overflow on the next sectors. Note that while the virtual File object will enlarge to accommodate the writes, it will not be filled with zeroes as with typical filesystem operations, but instead will be filled with the existing data from the iso image. The written data is laid over the existing sector: only the part of the sector the mode exposes is replaced, and the rest, such as the sync header and location, is kept as it is on the image. ECC and EDC will be recalculated on the fly for files opened in `'M2_FORM1'` or `'M2_FORM2'` mode, including when that mode was guessed. In any other mode, including `'M1'`, they are not recalculated.

The ISOReader object has the following methods:

```lua
:open(filename) -- Returns a File object off the specified file within the ISO.
```

This method is basically a helper over the `:open()` method of the Iso object, and will automatically guess the mode and size of the file.