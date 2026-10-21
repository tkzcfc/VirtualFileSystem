# Architecture

## Layering

```
            Application / engine (for example axmol's FileUtils)
                          │
                 VirtualFileSystem        Path normalization, aliases, layer-stack resolution, merged view, write rules
                          │
                  MountTable snapshot      Mount point -> backend, priority, flags (immutable, copy-on-write)
                          │
   ┌──────────────┬───────┴──────┬───────────────┬──────────────┐
NativeBackend  SingleFileBackend  MemoryBackend   ArchiveBackend   Custom IBackend
   │                 │                │               │
NativeFileStream  NativeFileStream  MemoryFileStream  ArchiveFileStream / MemoryFileStream
```

- **VirtualFileSystem** is the only entry point. It normalizes the caller's path into the `/a/b` form, finds every mount that covers that path (the layer stack), completes the query on the merged view, and decides which layer a write lands on according to the [mount rules](mount-rules.md).
- A **backend (IBackend)** is responsible only for its own directory tree. The path it receives is always an **already-normalized relative path** (no leading `/`, `.`, or `..`), and the root is the empty string. A backend does not know about mount points or other layers.
- A **stream (IFileStream)** is created by the backend and handed to the caller after the VFS wraps it. The interface matches axmol exactly.

## Source layout

| Directory / file | Contents |
|---|---|
| `src/vfs/Common.h`, `Config.h` | Namespace macros `NS_VFS_BEGIN/END`, export macro `VFS_DLL`, compile-time switches |
| `src/vfs/Error.h` | Error code `vfs::Error` and the errno mapping |
| `src/vfs/Path.h` | Path normalization, splitting, and portability checks |
| `src/vfs/IFileStream.h` | Stream interface |
| `src/vfs/IBackend.h`, `Stat.h`, `DirEntry.h` | Backend interface and metadata |
| `src/vfs/MountTable.h` | Mount table and layer-stack computation |
| `src/vfs/VirtualFileSystem.h` | Entry-point class |
| `src/vfs/Log.h` | Injectable log output |
| `src/vfs/native/` | `NativeBackend`, `SingleFileBackend`, `NativeFileStream`, platform path utilities |
| `src/vfs/memory/` | `MemoryBackend`, `MemoryFileStream` |
| `src/vfs/archive/` | Format constants, `ArchiveReader`, `ArchiveWriter`, `ArchiveBackend`, `ArchiveFileStream`, data sources |
| `src/vfs/crypto/` | ChaCha20 (RFC 8439), XXH3 wrappers, XOR |

## Mount table

Every `mount`/`unmount`/`remount`/`addAlias`/`removeAlias` copies the current `MountTable`, modifies the copy, then replaces the whole table (copy-on-write).

- Each item in `MountTable::mounts` (`MountEntry`) holds an id, a canonical mount point, a backend, a priority, flags, and a `MountState` that records how many streams are open.
- Mounts are sorted by **priority descending, and by id descending when the priority is equal** (a later mount comes first).
- When querying path P, `collectLayers` picks, in order, the mounts whose mount point is a prefix of P, producing P's layer stack (`Layer` = mount entry + P's path relative to that backend); after an `Opaque` mount, lower layers are not added.
- Ancestors of a mount point (and the root directory `/`) are synthesized as read-only virtual directories.
- `aliases` stores prefix aliases (for example `res://` -> `/res`), matched by descending prefix length and replaced before normalization.

`VirtualFileSystem` holds a `shared_ptr<const MountTable>` pointing at the current snapshot. One mutex protects only reading and replacing that pointer; another mutex serializes modifications. A query copies the pointer under the lock, then the rest of the operation runs on that immutable snapshot, so one call always sees a consistent mount table and queries do not block each other.

## Backends

| Backend | Capabilities | Notes |
|---|---|---|
| `NativeBackend` | read, write, rename, resize | A native directory. Path segments additionally reject `:`, a trailing `.`/space, and Windows reserved names, so behavior matches across platforms. Optionally read-only, refuse to follow a symlink that escapes the root, check the real filename's case (`strictCase` warns / `exactCase` forces a byte-wise match), and create the root automatically. No cache, so external changes are visible immediately. Mutations (mkdir/rmdir/unlink/rename/list) are implemented with system calls directly in `NativeUtils` (POSIX system calls / Win32), not through `std::filesystem`, so error codes map precisely and Windows long paths and POSIX semantics are supported |
| `SingleFileBackend` | read, write, resize | Mounts one native file onto a virtual path (like a file bind mount); any child path under the mount point returns `NOT_DIR` |
| `MemoryBackend` | read, write, rename, resize | An in-memory directory tree. Deleting an open file uses unlink semantics: the name disappears immediately and streams that are already open keep the data. `addFile` can prefill data in read-only mode |
| `ArchiveBackend` | read | A `.vfs` archive. The index `ArchiveIndex` is immutable after parsing and is shared by the backend and every stream |

A backend declares its capabilities through the `BackendCaps` bits returned by `IBackend::capabilities()` (`Read`, `Write`, `Rename`, `Resize`). A layer is writable when the backend has the `Write` capability and the mount was not given the `ReadOnly` flag.

## Streams

| Implementation | Source | Notes |
|---|---|---|
| `NativeFileStream` | Native file | POSIX uses `open/lseek/read/write`; Windows uses `CreateFileW` (UTF-8 to UTF-16, a `\\?\` prefix added automatically for long paths, share mode open for read, write, and delete so the file can be deleted or replaced while open; `APPEND` uses an OVERLAPPED offset of `0xFFFFFFFF` for an atomic append; brief sharing conflicts are retried a limited number of times). Refuses to open a directory |
| `MemoryFileStream` | Memory buffer | Files in the memory backend, and LZ4-compressed files in an archive (decompressed entirely when opened) |
| `ArchiveFileStream` | Uncompressed file in an archive | Each stream holds its own source handle and reads by offset; an XOR file is decrypted on the fly from the offset within the file; sequential reads skip extra seeks |

The stream returned by `VirtualFileSystem::openFileStream` is wrapped in a tracking wrapper: it holds a `shared_ptr` to the backend and maintains the open-stream count of its mount. Therefore:

- **Lazy unmount**: `unmount` removes the mount from the namespace immediately, but streams that are already open still hold the backend (the archive index and memory data stay alive) until the last stream closes;
- `unmount(id, UnmountFlags::FailIfBusy)` returns `BUSY` while any stream is still open;
- `remount` replaces the backend atomically; old streams keep reading the old backend.

The public `open()` on every stream is meaningful only for `NativeFileStream`; other streams can only be created by a backend, and calling `open()` directly returns `false` (`errno = ENOTSUP`).

## Threading model

- Every `VirtualFileSystem` method may be called from multiple threads at once, including mount and unmount concurrent with reads and writes.
- A backend must be thread-safe itself. Among the built-in backends, the native backend relies on the operating system; the memory backend protects the directory tree and each file buffer with reader-writer locks; the archive backend shares a read-only index, each stream has its own handle, and the read path takes no lock.
- **A single stream object is not thread-safe** and may be used by only one thread at a time; different stream objects are fully independent.
- The VFS does not provide file locks: when several streams read and write the same file at once, the result matches the operating system's behavior (the memory backend guarantees memory safety; the contents depend on timing).
- There is no asynchronous API; when asynchrony is needed, the caller's thread pool wraps the synchronous interface.

## Error reporting

Every interface that returns `vfs::Error` also sets `errno` on failure (`toErrno()` maps to the closest POSIX value), so it mixes with POSIX-style code. Interfaces that return a pointer or a stream also take an optional `Error* err` output parameter. Stream methods follow the POSIX convention of returning `-1` and setting `errno`.

Diagnostic messages inside the library go through the callback installed by `vfs::setLogger()` (levels `VERBOSE`, `INFO`, `WARN`, `FAIL`). Nothing is printed by default.
