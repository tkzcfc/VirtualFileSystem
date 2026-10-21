# API reference

Day-to-day use needs a single header:

```cpp
#include "vfs/VirtualFileSystem.h"   // Link target vfs (alias vfs::vfs)
USING_NS_VFS;                         // Or write vfs::
```

It already includes mounts, the native / memory / archive backends, and the stream interface. Include other pieces as needed: `"vfs/native/SingleFileBackend.h"`, `"vfs/archive/ArchiveWriter.h"`, `"vfs/Log.h"`.

Full parameter notes are in the header comments; this page groups them by use. Path arguments are always UTF-8 virtual paths. Normalization and layering rules are in [Mount rules](mount-rules.md).

## 1. Error handling

```cpp
enum class Error { OK, NOT_FOUND, EXISTS, NOT_DIR, IS_DIR, NOT_EMPTY, READ_ONLY, CROSS_DEVICE, BUSY,
                   INVALID_PATH, PERMISSION_DENIED, IO_ERROR, UNSUPPORTED, CORRUPTED, VERSION_MISMATCH,
                   KEY_REQUIRED, KEY_INVALID };

int toErrno(Error e);              // Map to a POSIX errno; see mount-rules.md section 11
const char* errorName(Error e);    // "NOT_FOUND"
const char* errorMessage(Error e); // English description (for example "no such file or directory")
```

- A function that returns `Error` also sets `errno = toErrno(e)` on failure.
- A function that returns a pointer, a stream, or a `MountId` returns an empty value or `0` on failure, and reports the error through the optional `Error* err`.

## 2. Mounts

```cpp
struct MountOptions
{
	int priority = 0;     // Larger is higher; when equal, the later mount is higher
	uint32_t flags = 0;   // MountFlags::ReadOnly | Opaque | WriteTarget
};

MountId mount(mountPoint, std::shared_ptr<IBackend> backend, const MountOptions& = {}, Error* err = nullptr);
MountId mountNative(mountPoint, nativeDir, const MountOptions& = {}, const NativeOptions& = {}, Error* err = nullptr);
MountId mountMemory(mountPoint, const MountOptions& = {}, std::shared_ptr<MemoryBackend>* backendOut = nullptr, Error* err = nullptr);
MountId mountArchive(mountPoint, nativePath, const ArchiveOptions& = {}, const MountOptions& = {}, Error* err = nullptr);
MountId mountFile(mountPoint, nativeFile, const MountOptions& = {}, Error* err = nullptr);

Error unmount(MountId id, uint32_t flags = 0);         // flags: UnmountFlags::FailIfBusy
Error remount(MountId id, std::shared_ptr<IBackend>);  // Atomically replace the backend
std::vector<MountInfo> mounts() const;                 // {id, mountPoint, priority, flags, backend}, in layer order
Error addAlias(prefix, target);                        // For example addAlias("res://", "/res")
Error removeAlias(prefix);
uint64_t generation() const;                           // Increments after a mount-table change or a write
```

- Success returns a non-zero `MountId`. The mount point must be a legal path, otherwise `INVALID_PATH`; a null `backend` returns `UNSUPPORTED`.
- `mountNative` returns `NOT_FOUND` if the directory does not exist (it is created when `NativeOptions::createRoot` is set), and `NOT_DIR` if it is not a directory.
- The file passed to `mountFile` must be an existing regular file.
- If `mountArchive` fails to open, it returns an archive error (`CORRUPTED`, `VERSION_MISMATCH`, `KEY_REQUIRED`, `KEY_INVALID`, or an IO error).

### Backend options

```cpp
struct NativeOptions
{
	bool readOnly = false;        // Every write returns READ_ONLY
	bool followSymlinks = true;   // false: reject any path that escapes the root after resolution (PERMISSION_DENIED)
	bool strictCase = false;      // true: check the real filename's case and warn on a mismatch (the result is unchanged)
	bool exactCase = false;       // true: match names byte for byte; on a case-insensitive filesystem a name that differs only in case is missing (see mount-rules.md section 1)
	bool createRoot = false;      // Create the root directory if it does not exist (mkdir -p)
};

struct MemoryOptions
{
	bool readOnly = false;        // Read-only: data can only be prefilled through addFile
};

struct ArchiveOptions
{
	std::vector<uint8_t> indexKey;   // When non-empty it must be 32 bytes, and it overrides the index key stored in the archive
	int64_t baseOffset = -1;         // Archive start; -1 is automatic (magic at offset 0, otherwise the footer at the end of the file)
};
```

Platform notes for the native backend:

- **Windows long paths**: when converting a UTF-8 path to UTF-16, a length of `MAX_PATH - 12` (the `CreateDirectoryW` limit) is converted to an absolute path and given a `\\?\` prefix (a UNC path becomes `\\?\UNC\server\share\...`). Every native operation (open, stat, list, mkdir, delete, rename) uses the same conversion and does not depend on the system's `LongPathsEnabled` setting. Executables in this repository (MSVC) also embed a `longPathAware` manifest (`cmake/longpath.manifest`, CMake function `vfs_add_long_path_manifest`).
- **Windows sharing and replacement**: a native stream is opened with all three share modes; delete and rename-overwrite prefer POSIX semantics, so a file that is open can be deleted or replaced (see mount-rules.md sections 7 and 9). Append writes (`APPEND`) use an atomic "write at end of file", so concurrent appends from several streams do not overwrite each other.
- **Case**: the default follows the filesystem; `exactCase` forces a byte-wise match (mount-rules.md section 1).

A backend can also be created on its own and passed to `mount()`:

```cpp
#include "vfs/native/SingleFileBackend.h"

auto native  = NativeBackend::create("assets", NativeOptions{}, &err);
auto single  = SingleFileBackend::create("settings.ini", /*readOnly*/ false, &err);
auto memory  = MemoryBackend::create(MemoryOptions{});
memory->addFile("config/default.json", "{}");          // Creates parent directories; also available in read-only mode
memory->setReadOnly(true);

auto archive = ArchiveBackend::create("game.vfs", ArchiveOptions{}, &err);
auto inMem   = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(std::move(bytes)), ArchiveOptions{}, &err);
fs.mount("/res", archive, MountOptions{0, MountFlags::ReadOnly});
```

`ArchiveBackend::verifyAll()` checks the content hash of every file in the archive (for offline checks; equivalent to reading the archive once).

## 3. Queries

```cpp
Error stat(path, Stat& out) const;   // Stat{type, size, mtime, mount, writable}
bool exists(path) const;
bool isFile(path) const;
bool isDir(path) const;
int64_t fileSize(path) const;        // Returns -1 when it is not a file or does not exist

Error list(path, const std::function<bool(const DirEntry&)>& cb) const;  // The callback returns false to stop
std::vector<DirEntry> list(path, Error* err = nullptr) const;            // DirEntry{name, type}
Error walk(path, const std::function<bool(const std::string& fullPath, const DirEntry&)>& cb) const;
std::string realPath(path) const;    // Native path of the visible entry; empty when it is not on a native backend
```

- `Stat::mtime` is Unix seconds; `0` means unknown. An archive entry whose mtime is 0 returns the modification time of the archive file itself.
- `Stat::mount` is the mount id of the visible entry; a synthesized virtual directory is `0`.
- `list` results are ordered by name bytes; `walk` is depth-first and calls back a directory before its children. `fullPath` is a virtual absolute path.

## 4. Reads and writes

```cpp
std::unique_ptr<IFileStream> openFileStream(path, IFileStream::Mode mode, Error* err = nullptr) const;

Error readFile(path, std::vector<uint8_t>& out) const;
Error readFile(path, std::string& out) const;
Error writeFile(path, const void* data, size_t size) const;   // WRITE mode: create or truncate
Error writeFile(path, std::string_view data) const;
Error writeFileAtomic(path, const void* data, size_t size) const;   // Write path.vfs-N.tmp, then rename; delete the temporary file on failure
```

- Read mode uses the visible entry; write mode picks a layer by the [write-target rules](mount-rules.md#5-write-target) and copy-up when needed.
- A directory cannot be opened as a file (`IS_DIR`).
- An empty file still returns a valid stream (`size() == 0`).

## 5. Modifications

```cpp
Error createDir(path) const;                      // EXISTS if it already exists
Error createDirs(path) const;                     // mkdir -p
Error removeDir(path, bool recursive = false) const;
Error removeFile(path) const;
Error rename(from, to) const;                     // Atomic rename within one mount
Error moveFile(from, to) const;                   // Across mounts, a file degrades to copy + delete
Error copyFile(from, to) const;                   // Copy onto itself: a no-op when writable; equal to copy-up on a read-only layer
```

Where each operation lands, and its error codes, are in [Mount rules](mount-rules.md) sections 6-9.

## 6. IFileStream

`vfs::IFileStream` has the same signature as the axmol engine's `ax::IFileStream`; when `VFS_WITH_AXMOL` is defined it is an alias of `ax::IFileStream`.

```cpp
class IFileStream
{
public:
	enum class Mode { READ, WRITE, APPEND, OVERLAPPED };

	virtual bool open(std::string_view path, Mode mode) = 0;
	virtual int close() = 0;
	virtual int64_t seek(int64_t offset, int origin) const = 0;   // SEEK_SET / SEEK_CUR / SEEK_END
	virtual int read(void* buf, unsigned int size) const = 0;
	virtual int write(const void* buf, unsigned int size) const = 0;
	virtual int64_t tell() const = 0;
	virtual int64_t size() const = 0;
	virtual bool resize(int64_t size) const;          // Default: errno = ENOTSUP and return false
	virtual bool isOpen() const = 0;
	virtual osfhnd_t nativeHandle() const;            // Default -1; HANDLE on Windows, an fd on other platforms
	virtual operator bool() const { return isOpen(); }
};
```

### Open modes

| Mode | File missing | Truncate | Readable | Write position |
|---|---|---|---|---|
| `READ` | Fail (`NOT_FOUND`) | No | Yes | Not writable |
| `WRITE` | Create | Yes | Yes | Current position |
| `APPEND` | Create | No | Yes | Every `write` goes to the end |
| `OVERLAPPED` | Create | No | Yes | Current position (random read/write) |

### Return values and errno

Every built-in stream (`NativeFileStream`, `MemoryFileStream`, `ArchiveFileStream`) and the wrapper stream returned by the VFS follows the same contract, covered by the consistency tests in `tests/stream`:

- `read`/`write` return the number of bytes actually transferred, or `-1` and set `errno` on failure; `read` returns `0` at EOF.
- `seek` returns the new position; if the resulting position would be negative it returns `-1` (`errno = EINVAL`) and the position is unchanged.
- Seeking past EOF is allowed: a following `read` returns `0`; `write` fills the hole in between with `0`.
- `close` returns `0` on success. After close, every method returns `-1` (or `false`) with `errno = EBADF`; `isOpen()` returns `false`.
- `write` on a `READ` stream returns `-1`.
- A single `read` returns at most `VFS_MAX_IO_CHUNK` (`0x7FFFF000`) bytes, and a native stream's `write` is truncated the same way; large transfers should loop until finished or until `0`/`-1` is returned.

### Differences between implementations

| Behavior | Native | Memory | Archive (uncompressed) | Archive (LZ4) |
|---|---|---|---|---|
| `write` | Supported | Supported (`EBADF` in `READ` mode) | `-1`, `EROFS` | `-1`, `EBADF` (in-memory read-only stream) |
| `resize` | Supported | Supported | `false`, `ENOTSUP` | `false`, `EBADF` |
| `nativeHandle` | OS handle | `-1` | `-1` | `-1` |
| Calling `open()` directly | Supported | `false`, `ENOTSUP` | `false`, `ENOTSUP` | `false`, `ENOTSUP` |

An LZ4-compressed archive file is decompressed entirely into memory when opened; later reads do not touch the disk.

### Ownership and threads

- A stream is owned exclusively by `std::unique_ptr` and is closed automatically on destruction.
- A stream holds its backend, so it remains usable after the mount is unmounted (lazy unmount).
- A single stream object is not thread-safe; different stream objects may be used concurrently from different threads.

## 7. Custom backends

Implement `IBackend` to mount any data source (a network cache, a database, an encrypted container, and so on):

```cpp
class IBackend
{
public:
	virtual uint32_t capabilities() const = 0;                       // BackendCaps::Read | Write | Rename | Resize
	virtual Error stat(std::string_view rel, Stat& out) const = 0;
	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) = 0;
	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const = 0;
	virtual Error createDir(std::string_view rel);                   // Default READ_ONLY
	virtual Error removeDir(std::string_view rel);                   // Default READ_ONLY
	virtual Error removeFile(std::string_view rel);                  // Default READ_ONLY
	virtual Error rename(std::string_view from, std::string_view to);// Default READ_ONLY
	virtual std::string realPath(std::string_view rel) const;        // Default empty string
	virtual const char* typeName() const = 0;
};
```

Conventions:

- `rel` is an already-normalized relative path (no leading `/`, no `.`/`..`); the backend root is the empty string `""`.
- `stat("")` must return a directory.
- `list` lists only direct children and does not have to sort; the VFS merges and sorts.
- `removeDir` only has to remove an empty directory; recursive removal, copy-up, and cross-layer checks are done by the VFS.
- Write modes of `open` must follow the open-mode table above; the VFS guarantees that the parent directory exists.
- Every method may be called from multiple threads at once, so the backend must be thread-safe itself.

## 8. Archive read and write

At runtime, `mountArchive` is usually enough. To build or inspect an archive from a program:

```cpp
#include "vfs/archive/ArchiveWriter.h"

ArchiveWriteOptions options;          // alignment=16, encryptIndex=true, dedup=true, compressionLevel=9 ...
ArchiveWriter writer(options);
writer.addFile("scripts/main.lua", "assets/scripts/main.lua", FileWriteOptions{false, true});  // Compress
writer.addData("config.json", std::vector<uint8_t>(...), FileWriteOptions{true, false});       // XOR encrypt
writer.addDir("empty");                                    // Only a directory that ends up empty is written
ArchiveWriteStats stats;
Error e = writer.write("game.vfs", &stats);                // Write game.vfs.tmp, then rename
if (e != Error::OK)
	std::puts(writer.lastError().c_str());
```

- `ArchiveWriteOptions`: `alignment` (a power of two, 1..65536), `encryptIndex`, `indexKey` (empty means derive it deterministically and store it in the file; otherwise it must be 32 bytes), `storeKey`, `fileKeyLength` (1..32), `dedup`, `compressionLevel` (1..12).
- `FileWriteOptions`: `encrypt`, `compress` (if compression does not shrink the data, the original bytes are stored), `mtime` (default 0, so the archive is reproducible).
- A duplicate path, or one that differs only in case, returns `EXISTS`; illegal options return `UNSUPPORTED`. `lastError()` gives the specific explanation.
- `ArchiveReader::open` returns an immutable `ArchiveIndex`; `ArchiveReader::readEntry` reads the original content of one entry; `ArchiveReader::verify` checks content hashes one by one.

The binary layout is in [format.md](format.md).

## 9. Logging and configuration

```cpp
#include "vfs/Log.h"

enum class LogLevel { VERBOSE, INFO, WARN, FAIL };
using LogFunc = std::function<void(LogLevel, std::string_view)>;
void setLogger(LogFunc func);        // Pass an empty function to disable output; nothing is printed by default
```

Compile-time configuration (`Config.h`; override it from the compiler flags):

| Macro | Default | Meaning |
|---|---|---|
| `VFS_WITH_AXMOL` | `0` | Use axmol's `IFileStream` |
| `VFS_MAX_IO_CHUNK` | `0x7FFFF000` | Maximum bytes of one read/write |
| `VFS_MAX_INDEX_SIZE` | 512 MB | Upper bound accepted for an archive index |
| `VFS_DLL` | empty | Export macro (`Common.h`); define it per platform when building or using a shared library |
