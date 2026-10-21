// This file: virtual file system core. Mount table, layered (union) lookup, write-target selection, copy-up, and mkdir/rmdir/rename rules.
// Layering rules are in docs/mount-rules.md.
#pragma once

#include "Common.h"
#include "Error.h"
#include "IBackend.h"
#include "IFileStream.h"
#include "MountTable.h"
#include "native/NativeBackend.h"
#include "memory/MemoryBackend.h"
#include "archive/ArchiveBackend.h"
#include <atomic>
#include <mutex>

NS_VFS_BEGIN

/**
 * Virtual file system.
 *
 * Core model:
 * - One virtual namespace: absolute paths, '/' separated, UTF-8, case-sensitive.
 * - A mount = (mount point, backend, priority, flags). For a path P, every mount whose mount point is a prefix of P forms P's layer stack, sorted by priority from high to low.
 * - Reads are a union (overlay style); a write lands on exactly one determined layer; deletion acts only on writable layers; there is no runtime whiteout.
 *
 * Thread safety: every method may be called concurrently from multiple threads. The mount table is a copy-on-write snapshot, and queries take no lock.
 * Returned stream objects are owned by the caller and must not be used from more than one thread at a time.
 */
class VFS_DLL VirtualFileSystem
{
public:

	VirtualFileSystem();

	~VirtualFileSystem();

	VirtualFileSystem(const VirtualFileSystem&) = delete;

	VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

	// ------------------------------------------------------------ mount management

	/**
	 * Mount a backend.
	 * @param mountPoint Virtual mount point (normalized; it may be a path that does not exist in the merged view, in which case ancestors show up as virtual directories)
	 * @param backend    Backend (shared ownership)
	 * @param options    Priority and flags
	 * @param err        Optional error output
	 * @return Mount ID; 0 on failure
	 */
	MountId mount(std::string_view mountPoint, std::shared_ptr<IBackend> backend, const MountOptions& options = {},
				  Error* err = nullptr);

	/**
	 * Mount a native directory.
	 */
	MountId mountNative(std::string_view mountPoint, std::string_view nativeDir, const MountOptions& options = {},
						const NativeOptions& nativeOptions = {}, Error* err = nullptr);

	/**
	 * Mount a new empty memory backend.
	 * @param backendOut Optional: receives the created backend, so addFile can preload data
	 */
	MountId mountMemory(std::string_view mountPoint, const MountOptions& options = {},
						std::shared_ptr<MemoryBackend>* backendOut = nullptr, Error* err = nullptr);

	/**
	 * Mount a .vfs archive (read-only).
	 */
	MountId mountArchive(std::string_view mountPoint, std::string_view nativePath,
						 const ArchiveOptions& archiveOptions = {}, const MountOptions& options = {},
						 Error* err = nullptr);

	/**
	 * Mount a single native file.
	 */
	MountId mountFile(std::string_view mountPoint, std::string_view nativeFile, const MountOptions& options = {},
					  Error* err = nullptr);

	/**
	 * Unmount. Lazy by default: removed from the namespace immediately, and streams already open stay usable.
	 * @param flags With UnmountFlags::FailIfBusy, returns BUSY if any stream is still open
	 * @return OK; NOT_FOUND if the ID does not exist
	 */
	Error unmount(MountId id, uint32_t flags = 0);

	/**
	 * Atomically replace one mount's backend inside a single snapshot swap (hot update). Old streams keep reading the old backend.
	 */
	Error remount(MountId id, std::shared_ptr<IBackend> backend);

	/**
	 * Every current mount (priority high to low).
	 */
	std::vector<MountInfo> mounts() const;

	/**
	 * Add a prefix alias, for example addAlias("res://", "/res").
	 */
	Error addAlias(std::string_view prefix, std::string_view target);

	Error removeAlias(std::string_view prefix);

	/**
	 * Generation counter: incremented on mount, unmount, remount, or any write, so upper layers can invalidate caches.
	 */
	uint64_t generation() const { return m_generation.load(std::memory_order_acquire); }

	// ------------------------------------------------------------ queries

	/**
	 * Look up the visible entry in the merged view.
	 * @return OK / NOT_FOUND / NOT_DIR (some component is hidden by a file on a higher layer) / INVALID_PATH
	 */
	Error stat(std::string_view path, Stat& out) const;

	bool exists(std::string_view path) const;

	bool isFile(std::string_view path) const;

	bool isDir(std::string_view path) const;

	/**
	 * File size; -1 if it does not exist or is not a file.
	 */
	int64_t fileSize(std::string_view path) const;

	/**
	 * List a directory: merge children from every layer, deduplicate by name (the higher layer decides the type), sort by name byte order.
	 * @param cb Callback; returning false stops the walk
	 */
	Error list(std::string_view path, const std::function<bool(const DirEntry&)>& cb) const;

	std::vector<DirEntry> list(std::string_view path, Error* err = nullptr) const;

	/**
	 * Recursive walk (depth-first, ordered within each directory). The callback receives the full virtual path and the entry; returning false stops.
	 */
	Error walk(std::string_view path, const std::function<bool(const std::string& fullPath, const DirEntry& entry)>& cb) const;

	/**
	 * Native path of the visible entry; only a native backend returns non-empty.
	 */
	std::string realPath(std::string_view path) const;

	// ------------------------------------------------------------ streams

	/**
	 * Open a file stream. The landing layer is chosen by the write-target rules; a parent that exists only on other layers is created on the write layer; APPEND/OVERLAPPED that modifies a read-only-layer file copy-up first.
	 * @param path Virtual path
	 * @param mode Open mode (semantics in IFileStream.h)
	 * @param err  Optional error output (errno is set as well)
	 * @return The stream on success (ownership passes to the caller); nullptr on failure
	 */
	std::unique_ptr<IFileStream> openFileStream(std::string_view path, IFileStream::Mode mode,
												Error* err = nullptr) const;

	// ------------------------------------------------------------ mutation

	/**
	 * Create a single directory level: EXISTS if it already exists; the parent must exist in the merged view.
	 */
	Error createDir(std::string_view path) const;

	/**
	 * mkdir -p: an existing directory is not an error.
	 */
	Error createDirs(std::string_view path) const;

	/**
	 * Remove a directory. If any other layer (read-only or not the write layer) contributes that directory or its descendants, returns READ_ONLY and deletes nothing;
	 * when not recursive, a non-empty merged view returns NOT_EMPTY; a mount point or its ancestor returns BUSY.
	 */
	Error removeDir(std::string_view path, bool recursive = false) const;

	/**
	 * Remove a file: only the visible entry is removed; a visible entry on a read-only layer returns READ_ONLY; after removal a same-named file on a lower layer shows through again.
	 */
	Error removeFile(std::string_view path) const;

	/**
	 * Rename: succeeds only when source and destination land on the same mount; cross-mount returns CROSS_DEVICE; a source on a read-only layer returns READ_ONLY;
	 * a source directory merged from several layers also returns READ_ONLY (same as removeDir).
	 */
	Error rename(std::string_view from, std::string_view to) const;

	/**
	 * Move a file: try rename first, and fall back to copy + remove across mounts (not atomic).
	 */
	Error moveFile(std::string_view from, std::string_view to) const;

	/**
	 * Copy a file (the destination is opened in WRITE mode and truncated). Copying onto itself: if the visible entry is writable, do nothing;
	 * if the visible entry is on a read-only layer, copy it to the write layer (an explicit copy-up).
	 */
	Error copyFile(std::string_view from, std::string_view to) const;

	// ------------------------------------------------------------ convenience functions

	Error readFile(std::string_view path, std::vector<uint8_t>& out) const;

	Error readFile(std::string_view path, std::string& out) const;

	Error writeFile(std::string_view path, const void* data, size_t size) const;

	Error writeFile(std::string_view path, std::string_view data) const;

	/**
	 * Atomic save: write a uniquely named temporary file in the same directory (path.vfs-N.tmp), then rename it to path.
	 * On failure the temporary file is removed; concurrent writes of the same path leave the contents of one complete write.
	 */
	Error writeFileAtomic(std::string_view path, const void* data, size_t size) const;

private:

	// Current mount-table snapshot
	std::shared_ptr<const MountTable> snapshot() const;

	// Alias + normalization
	Error normalize(const MountTable& table, std::string_view raw, std::string& out) const;

	// Resolve the visible entry of a path on a given snapshot. visible is a layer index; -1 means a synthesized virtual directory
	Error resolve(const MountTable& table, std::string_view path, std::vector<Layer>& layers, int& visible,
				  Stat& st) const;

	// Choose the write layer: the WriteTarget flag first, otherwise the highest-priority writable layer; -1 if there is none
	static int chooseWriteTarget(const std::vector<Layer>& layers);

	// No layer other than keep provides this path as a directory: OK; if one does, READ_ONLY; if that cannot be determined, return that layer's error
	static Error checkSingleLayerDir(const std::vector<Layer>& layers, int keep);

	// Write everything remaining in in to out
	static Error copyStream(IFileStream& in, IFileStream& out);

	// Create the relative parent-directory chain on the write layer
	static Error ensureParents(const Layer& layer, std::string_view relParent);

	// copy-up: copy the file at src on the visible layer in full to dst on the write layer, writing a temporary file then renaming when possible
	static Error copyUp(const Layer& src, const Layer& dst);

	// The striped copy-up lock for relative path rel on write-layer mount
	std::recursive_mutex& copyUpLock(const MountEntry& mount, std::string_view rel) const;

	// One attempt of openFileStream; openFileStream retries when it races a FailIfBusy unmount
	std::unique_ptr<IFileStream> openOnce(std::string_view path, IFileStream::Mode mode, Error* err) const;

	// Wrap the stream returned by the backend: count opens and keep the backend alive. Returns null and asks for a retry while the mount is being unmounted with FailIfBusy
	std::unique_ptr<IFileStream> track(const MountEntry& mount, std::unique_ptr<IFileStream> stream) const;

	Error createDirIn(const MountTable& table, const std::string& path) const;

	Error listIn(const MountTable& table, const std::string& path, const std::function<bool(const DirEntry&)>& cb) const;

	Error walkIn(const MountTable& table, const std::string& path,
				 const std::function<bool(const std::string&, const DirEntry&)>& cb, bool& stop) const;

	// Modify a mount table: copy, edit, publish
	template <typename Fn>
	Error modifyTable(Fn&& fn);

	void bump() const { m_generation.fetch_add(1, std::memory_order_acq_rel); }

private:
	// Snapshot shards: each shard holds an independent reference to the current snapshot (its own control block) and a lock. A thread reads a fixed shard,
	// so under multithreaded queries neither the reference count nor the lock shares a cache line across threads. Defined in the .cpp
	struct SnapshotShard;

	std::mutex m_modifyMutex;                      // Serializes mount-table edits; m_table is read and written only under it
	// copy-up and opens whose write does not land on the visible layer are striped by path (the same path always takes the same lock):
	// concurrent appends of one read-only file copy it only once, and the copy does not clobber a same-named file another thread creates on the write layer;
	// copy-up of different paths does not wait on each other. The lock is recursive: a backend or log callback that re-enters the VFS and opens the same path while holding the lock does not deadlock itself
	static constexpr size_t COPY_UP_STRIPES = 64;
	mutable std::recursive_mutex m_copyUpLocks[COPY_UP_STRIPES];
	std::shared_ptr<const MountTable> m_table;     // Current snapshot (the primary reference used for publishing)
	std::unique_ptr<SnapshotShard[]> m_shards;     // Shard references used for reads
	std::atomic<MountId> m_nextId;                 // Next mount ID
	mutable std::atomic<uint64_t> m_generation;    // Generation counter
};

NS_VFS_END
