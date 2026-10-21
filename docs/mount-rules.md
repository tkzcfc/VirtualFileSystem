# Mount and layering rules

This document defines how `VirtualFileSystem` looks up paths, lists directories, writes, deletes, and renames under multi-layer mounts. Every rule is deterministic: the same mount table and the same call always produce the same result.

## 1. Paths

- Virtual paths are separated by `/`, encoded as UTF-8, and **case-sensitive** (compared by bytes).
- Every entry point normalizes the same way: `\` is treated as `/`; repeated `/` are collapsed; `.` is removed; `..` is resolved literally, and **crossing the root returns `INVALID_PATH`**; a trailing `/` is ignored. The canonical form is `/a/b`, and the root is `/`. A path without a leading `/` is treated as a path under the root (`a/b` is the same as `/a/b`).
- A path containing NUL or any other control character (`0x00-0x1F`, `0x7F`) returns `INVALID_PATH`.
- The native backend also rejects names that contain `:`, names that end with `.` or a space, and Windows reserved names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`, case-insensitive, and also rejected with an extension), returning `INVALID_PATH` consistently on every platform.
- **Case and the native file system**: the memory backend and the archive backend match names by bytes. The native backend hands lookup to the operating system by default, so the result follows the file system: Linux (ext4 and similar) is case-sensitive; Windows NTFS and macOS APFS/HFS+ are **case-insensitive** by default (the original spelling is preserved), so `/res/A.png` can open `a.png` on disk, and writing `case.txt` overwrites an existing `Case.txt`. The same difference applies when a native directory is a read-only lower layer (for example a memory layer on top and a native directory underneath). When every platform must behave the same way, turn on `NativeOptions::exactCase` for the native layer:
  - Every path component is compared byte-for-byte with the real name on disk. A name that differs only in case (or in Unicode normalization, such as NFC/NFD on macOS) is treated as absent: `stat`/`exists`/read/list/delete/rename-source return `NOT_FOUND`.
  - On a case-insensitive file system, a write, `createDir`, or rename target that differs from an existing entry only in case returns `EXISTS`, instead of silently writing onto that existing file (on a case-sensitive file system they are already different files and are created normally). A rename that changes only case (`a.txt` -> `A.txt`) is allowed.
  - The cost is one extra system call per path component on every access (`FindFirstFileExW` on Windows, `getattrlist` on macOS; one `lstat` on Linux). It is off by default; `strictCase` only prints a warning on a mismatch and does not change the result.
  - Recommendation: during development on Windows/macOS, turn on `exactCase` for asset directories (or at least `strictCase`), so case errors that would only show up after publishing to an archive are found early.
- Aliases are replaced by longest prefix before normalization: after `addAlias("res://", "/res")`, `res://img/a.png` is the same as `/res/img/a.png`.

## 2. Mounts and the layer stack

```cpp
MountId mount(mountPoint, backend, MountOptions{priority, flags});
```

- For a path P, **every mount whose mount point is a prefix of P (compared by path segments)** forms P's layer stack, regardless of how deep the mount point is.
- Order: **higher priority is on top; when priority is equal, the later mount is on top**.
- `MountFlags::Opaque`: below that mount's mount point, every lower layer is invisible (the same idea as a Linux mount hiding what is underneath). Higher layers remain visible.
- `MountFlags::ReadOnly`: this layer is treated as read-only even if the backend is writable.
- `MountFlags::WriteTarget`: among writable layers, prefer this one as the write target (see section 5).
- A mount point may be created on a path that does not exist in the merged view. Its ancestor paths appear as **read-only virtual directories**. The root `/` always exists.
- An ancestor of a mount point **is always a directory**: if a lower layer has a file of the same name at that location, the file is hidden (`stat`/`list` show a directory, and `removeFile`/`writeFile` return `IS_DIR`), so the mount point stays reachable.
- A mount point and its ancestors cannot be removed by `removeDir` or renamed, and the call returns `BUSY`.

Recommended use: mount read-only assets at `/res` (an archive stacked with a development directory), and mount writable data at `/user` (a single native directory). Stacking a read-only layer and a writable layer on the same mount point fits a "patch overlay" scenario.

## 3. Lookup

- P's **visible entry** is the entry in the topmost layer of the stack that contains P; `Stat::mount` is the mount id it lives on, and `Stat::writable` says whether that layer is writable.
- **Type conflict**: when the upper layer is a file and the lower layer is a directory of the same name, only the file is seen, and looking up a child path returns `NOT_DIR`. When the upper layer is a directory and the lower layer is a file of the same name, it is treated as a directory.
- If some intermediate component of the path is a file in the merged view, the result is `NOT_DIR`.

## 4. Directory listing

- Children from every layer of the stack under that directory are combined, **deduplicated by name, with the upper layer deciding the type** when names collide; type conflicts follow section 3.
- An ancestor directory of a mount point lists the next mount-path segment.
- Results are **sorted by name in byte order**.
- The callback form is `bool(const DirEntry&)`, and returning `false` stops the walk. There is also an overload that returns `std::vector<DirEntry>` and a depth-first `walk`.
- A file target returns `NOT_DIR`; a missing target returns `NOT_FOUND`.
- If one layer returns an error other than `NOT_FOUND`/`NOT_DIR`/`INVALID_PATH` while listing (for example `IO_ERROR` or `PERMISSION_DENIED`), the whole `list` returns that error and does not treat the failing layer as an empty directory.

## 5. Write target

Opens in a write mode (`WRITE`, `APPEND`, `OVERLAPPED`), `writeFile`, `createDir`, and other write operations choose a layer in this order:

1. Find P's visible entry.
2. If the layer that holds the visible entry is writable, write on that layer.
3. Otherwise (P does not exist, or the visible entry is on a read-only layer), write to a **writable layer that covers P**: a layer with the `WriteTarget` flag comes first, then the highest writable layer in the stack. If there is no writable layer, the result is `READ_ONLY`.
4. If the chosen writable layer is **below** the read-only layer that holds the visible entry, the write would be hidden by the read-only layer and could not be read. In that case return `READ_ONLY` and do not write.

### Parent directories (directory copy-up)

When writing `/a/b/new.txt`, as long as the parent directory `/a/b` exists in the merged view (on any layer), the missing parent chain is created automatically on the write layer. If the parent does not exist in the merged view, the result is `NOT_FOUND` (same as `open(O_CREAT)`: parents are not created from nothing). If the parent path is a file, the result is `NOT_DIR`.

### Modifying a file on a read-only layer (file copy-up)

- `WRITE` (truncate): create a new file on the write layer, hiding the lower layer, without copying the old contents.
- `APPEND`, `OVERLAPPED`: first copy the lower file in full onto the write layer (write a uniquely named temporary file `path.vfs-copyup-N.tmp`, then rename it; a failure does not leave a partial file), then open the copy. Copy-up runs serially inside the VFS, so concurrent appends of the same read-only file copy it only once, and every thread's appended data is kept. Copy-up of a large file has a matching copy cost.

## 6. Creating directories

- `createDir(P)`: if P already exists (directory or file), the result is `EXISTS`; the parent must exist in the merged view, otherwise `NOT_FOUND`; if the parent path is a file, the result is `NOT_DIR`. The directory is created on the write layer chosen by section 5. Intermediate directories that exist only on a read-only layer are copied onto the write layer automatically.
- `createDirs(P)`: `mkdir -p` semantics. An existing directory (including one created concurrently by another thread) is not an error; if some component of the path is a file, the result is `NOT_DIR`, and if the last component is a file, the result is `EXISTS`. If another thread deletes a parent midway, the operation retries from the start (at most 16 times) and returns `NOT_FOUND` only if the deletion keeps happening.
- When the same set of directories is created and deleted concurrently, the native backend reports only the error codes listed above: while a parent is being deleted, systems return different errno values (Linux `ENOENT`; macOS may return something else). After `mkdir` fails, the parent is checked again, and if it is gone the result is `NOT_FOUND`.
- Creating under a virtual directory when no writable layer covers it returns `READ_ONLY`.

## 7. Deleting files

- Only the visible entry is deleted. If it is on a read-only layer, the result is `READ_ONLY`; if it is a directory, `IS_DIR`; if it does not exist, `NOT_FOUND`.
- After an overlay file on a writable layer is deleted, **the same-named file on a lower layer becomes visible again**. This is intentional: deleting the overlay "restores the original". The runtime writes no whiteout marker, and a file on a read-only layer cannot be deleted.
- The mount point of a single-file mount itself cannot be deleted (`BUSY`).
- Deleting an open file: the memory backend uses unlink semantics and the stream stays usable; the native backend follows the operating system. Windows allows it too: the stream is opened with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`, and deletion uses POSIX semantics (`FileDispositionInfoEx`, NTFS on Windows 10 1709+), so the name disappears immediately and an already open stream stays usable. File systems that do not support POSIX semantics (FAT/exFAT, some network drives, Wine) fall back to classic deletion: the name disappears only after the last handle closes, and creating a same-named file in that spot fails until then.

## 8. Deleting directories

`removeDir(P, recursive = false)`:

1. If P does not exist, the result is `NOT_FOUND`; if it is a file, `NOT_DIR`.
2. If P is a mount point or an ancestor of a mount point (including the root), the result is `BUSY`.
3. **Check first, then act**: count how many layers in the stack provide P as a directory. If more than one does (other layers may contribute descendants), or if the only such layer is read-only, the result is `READ_ONLY` and nothing is deleted.
4. Non-recursive: if P is not empty, the result is `NOT_EMPTY`.
5. Recursive: after the checks pass, delete the whole subtree on that layer.

Example: an archive mounted at `/` provides `/a/b/c.txt`, and a writable native directory is also mounted at `/` and contains `/a/mine.txt`. `removeDir("/a")` returns `READ_ONLY`; `/a/mine.txt` can be deleted on its own, and `/a/b/c.txt` always remains.

## 9. Rename and move

- `rename(from, to)`: when **the source's visible entry and the target both land on the same writable mount**, the backend's native rename is used (atomic). If the target already exists and its layer is writable, the target lands on that layer; otherwise the write layer is chosen by section 5, and the target's parent directory must exist. When the target already exists, overwrite behavior is the backend's POSIX rules (a file replaces a file, a directory replaces an empty directory).
- Errors: missing source -> `NOT_FOUND`; source or target is a mount point or an ancestor of one -> `BUSY`; source is on a read-only layer -> `READ_ONLY`; the source directory is merged from several layers (other layers also have a directory of that name) -> `READ_ONLY` (same as `removeDir` in section 8, otherwise only the part on the write layer would be moved); moving a directory into its own subtree -> `INVALID_PATH`; renaming a file onto an existing directory -> `IS_DIR`; renaming a directory onto an existing file -> `NOT_DIR`; source and target land on different mounts -> `CROSS_DEVICE`; the backend does not support rename -> `UNSUPPORTED`. If `from` and `to` are the same, the call succeeds immediately.
- After a rename, a same-named file on a layer below the source path may become visible again (same as section 7).
- On Windows the native backend follows `rename(2)` semantics: replacing a file with a file first uses `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`. If that fails because another handle has the target open, it switches to POSIX-semantics `FileRenameInfoEx` (also requiring NTFS on Windows 10 1709+), so an open file can be replaced and the old handle keeps reading the old contents. When POSIX semantics are unavailable, replacing a target that is open returns `PERMISSION_DENIED` (`writeFileAtomic` does the same). Replacing an empty directory on Windows is two steps (delete the target, then rename) and is not atomic.
- Under a race, the reported error matches an atomic `rename(2)` (a conflict seen by the Windows pre-check is performed again to confirm, so a transient race state is not reported as `NOT_EMPTY`/`NOT_DIR`/`IS_DIR`): if another thread moves or deletes the source, the result is `NOT_FOUND` (rather than the `ACCESS_DENIED` Windows returns); if the target is a non-empty directory, the result is `NOT_EMPTY` (macOS/BSD `rename`/`rmdir` return `EEXIST` in that case, which is also mapped to `NOT_EMPTY`). On Windows, a short hold by antivirus or the indexer that produces `ACCESS_DENIED`/`SHARING_VIOLATION` is retried a limited number of times (`ACCESS_DENIED` for about 63ms; `SHARING_VIOLATION`/`LOCK_VIOLATION` are always caused by another handle, for at most about 255ms). A genuine lack of permission costs only about 63ms extra.
- `moveFile(from, to)`: try `rename` first; across mounts, a **file** falls back to copy plus delete (not guaranteed atomic); moving a directory across mounts returns `CROSS_DEVICE`.
- `copyFile(from, to)`: read the visible entry and write it to the target path by section 5. When `from` and `to` are the same: if the visible entry is writable, do nothing (the file is not truncated to empty); if it is on a read-only layer, copy it onto the write layer, which is an explicit copy-up; a directory returns `IS_DIR`.
- `writeFileAtomic(path, data, size)`: first write a uniquely named temporary file `path.vfs-N.tmp` in the same directory (N is an in-process incrementing counter, so concurrent writes of the same path do not interfere), then rename it to `path`; on write failure the temporary file is deleted; if the temporary file and the target land on different layers, the operation falls back to a direct write.

## 10. Unmount and replace

- `unmount(id)`: **lazy unmount**. The mount disappears from the namespace immediately, already open streams stay usable, and the backend is released after the last stream closes.
- `unmount(id, UnmountFlags::FailIfBusy)`: if any stream opened through the VFS is still open, return `BUSY` and do not unmount.
- `remount(id, backend)`: atomically replace a mount's backend. The mount point, priority, and flags stay the same; old streams keep reading the old backend.
- `generation()`: incremented after the mount table changes or after any write, so a higher layer can invalidate caches from it.

## 11. Error codes

| `vfs::Error` | errno | Typical situation |
|---|---|---|
| `OK` | 0 | Success |
| `NOT_FOUND` | `ENOENT` | Path does not exist; parent directory does not exist on write; native root does not exist |
| `EXISTS` | `EEXIST` | `createDir` target already exists; duplicate path while packing |
| `NOT_DIR` | `ENOTDIR` | An intermediate path component is a file; `list`/`removeDir` on a file; a child path of a single-file mount |
| `IS_DIR` | `EISDIR` | Reading a directory as a file, `removeFile`, or opening a directory as a file |
| `NOT_EMPTY` | `ENOTEMPTY` | Non-recursive delete of a non-empty directory |
| `READ_ONLY` | `EROFS` | Read-only layer; no writable layer; the write would be hidden; `removeDir` / `rename` of a directory that involves other layers |
| `CROSS_DEVICE` | `EXDEV` | `rename` across mounts; `moveFile` of a directory across mounts |
| `BUSY` | `EBUSY` | Deleting or renaming a mount point; `FailIfBusy` unmount while streams are still open |
| `INVALID_PATH` | `EINVAL` | Crossing the root, a control character, or a native name that is not portable |
| `PERMISSION_DENIED` | `EACCES` | Refused by the operating system; a symlink that must not be followed escapes the root |
| `IO_ERROR` | `EIO` | Read or write failed |
| `UNSUPPORTED` | `ENOTSUP` | The backend does not support the operation; an unsupported argument (for example an illegal alignment) |
| `CORRUPTED` | `EIO` | Archive is corrupt |
| `VERSION_MISMATCH` | `EIO` | Archive version is not supported |
| `KEY_REQUIRED` | `EACCES` | The archive index is encrypted and no key was provided |
| `KEY_INVALID` | `EACCES` | Key length is wrong, or the key is wrong |

`errorName(e)` returns the enumerator name, `errorMessage(e)` returns the English description, and `toErrno(e)` returns the errno from the table above.
