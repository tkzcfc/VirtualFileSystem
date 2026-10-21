#include "VirtualFileSystem.h"
#include "Path.h"
#include "native/SingleFileBackend.h"
#include <algorithm>
#include <map>
#include <thread>

NS_VFS_BEGIN

namespace
{

constexpr size_t COPY_BUFFER_SIZE = 64 * 1024;

// Every API that returns Error goes through here: on failure it also sets errno = toErrno(e) (see api.md)
inline Error report(Error e)
{
	if (e != Error::OK)
		errno = toErrno(e);
	return e;
}

// A process-unique temporary file name: concurrent copy-up / writeFileAtomic on the same path do not share one temporary file
std::string uniqueTempName(std::string_view base, const char* tag)
{
	static std::atomic<uint64_t> s_counter{0};
	std::string name(base);
	name += '.';
	name += tag;
	name += '-';
	name += std::to_string(s_counter.fetch_add(1, std::memory_order_relaxed));
	name += ".tmp";
	return name;
}

/**
 * Stream wrapper the VFS returns to the caller:
 * - holds the backend's shared_ptr so the backend stays alive after a lazy unmount;
 * - maintains the mount's open-stream count for UnmountFlags::FailIfBusy.
 * Every call is forwarded straight to the inner stream; the cost is one virtual call.
 */
class TrackedFileStream : public IFileStream
{
public:

	TrackedFileStream(std::unique_ptr<IFileStream> inner, std::shared_ptr<IBackend> backend,
					  std::shared_ptr<MountState> state)
		: m_inner(std::move(inner))
		, m_backend(std::move(backend))
		, m_state(std::move(state))
	{
		m_state->openStreams.fetch_add(1, std::memory_order_acq_rel);
	}

	virtual ~TrackedFileStream()
	{
		close();
	}

	virtual bool open(std::string_view /*path*/, IFileStream::Mode /*mode*/) override
	{
		// A stream can only be obtained through VirtualFileSystem::openFileStream
		errno = ENOTSUP;
		return false;
	}

	virtual int close() override
	{
		int ret = 0;
		if (m_inner)
		{
			ret = m_inner->close();
			m_inner.reset();
			m_state->openStreams.fetch_sub(1, std::memory_order_acq_rel);
			m_backend.reset();
		}
		return ret;
	}

	virtual int64_t seek(int64_t offset, int origin) const override
	{
		if (!m_inner)
		{
			errno = EBADF;
			return -1;
		}
		return m_inner->seek(offset, origin);
	}

	virtual int read(void* buf, unsigned int size) const override
	{
		if (!m_inner)
		{
			errno = EBADF;
			return -1;
		}
		return m_inner->read(buf, size);
	}

	virtual int write(const void* buf, unsigned int size) const override
	{
		if (!m_inner)
		{
			errno = EBADF;
			return -1;
		}
		return m_inner->write(buf, size);
	}

	virtual int64_t tell() const override
	{
		if (!m_inner)
		{
			errno = EBADF;
			return -1;
		}
		return m_inner->tell();
	}

	virtual int64_t size() const override
	{
		if (!m_inner)
		{
			errno = EBADF;
			return -1;
		}
		return m_inner->size();
	}

	virtual bool resize(int64_t size) const override
	{
		if (!m_inner)
		{
			errno = EBADF;
			return false;
		}
		return m_inner->resize(size);
	}

	virtual bool isOpen() const override { return m_inner && m_inner->isOpen(); }

	virtual osfhnd_t nativeHandle() const override { return m_inner ? m_inner->nativeHandle() : (osfhnd_t)-1; }

private:
	std::unique_ptr<IFileStream> m_inner;
	std::shared_ptr<IBackend> m_backend;
	std::shared_ptr<MountState> m_state;
};

// On failure, set the error and return an empty stream
std::unique_ptr<IFileStream> failStream(Error* err, Error e)
{
	setError(err, e);
	return nullptr;
}

constexpr size_t SNAPSHOT_SHARDS = 16;

// Snapshot shard for the current thread: assigned round-robin the first time the thread uses one, then fixed
size_t threadShard()
{
	static std::atomic<size_t> s_next{0};
	thread_local const size_t t_shard = s_next.fetch_add(1, std::memory_order_relaxed) % SNAPSHOT_SHARDS;
	return t_shard;
}

// Build a snapshot reference for one shard: the outer shared_ptr has its own control block (pointing at the same table),
// so copying the reference for different shards updates different counters and threads do not fight over one cache line
std::shared_ptr<const MountTable> shardReference(const std::shared_ptr<const MountTable>& table)
{
	auto holder = std::make_shared<std::shared_ptr<const MountTable>>(table);
	const MountTable* raw = holder->get();
	return std::shared_ptr<const MountTable>(std::move(holder), raw);
}

} // namespace

struct VirtualFileSystem::SnapshotShard
{
	std::mutex mutex;                           // only protects copying and replacing the table; almost never contended
	std::shared_ptr<const MountTable> table;
	char padding[64];                           // separate adjacent shards to avoid false sharing
};

VirtualFileSystem::VirtualFileSystem()
	: m_table(std::make_shared<MountTable>())
	, m_shards(new SnapshotShard[SNAPSHOT_SHARDS])
	, m_nextId(1)
	, m_generation(0)
{
	for (size_t i = 0; i < SNAPSHOT_SHARDS; ++i)
		m_shards[i].table = shardReference(m_table);
}

VirtualFileSystem::~VirtualFileSystem()
{
}

// ================================================================ Mount management

std::shared_ptr<const MountTable> VirtualFileSystem::snapshot() const
{
	// The lock only protects copying this shard's pointer; later queries run on that immutable snapshot
	SnapshotShard& shard = m_shards[threadShard()];
	std::lock_guard<std::mutex> lock(shard.mutex);
	return shard.table;
}

template <typename Fn>
Error VirtualFileSystem::modifyTable(Fn&& fn)
{
	std::lock_guard<std::mutex> modifyLock(m_modifyMutex);
	auto table = std::make_shared<MountTable>(*m_table);
	Error e = fn(*table);
	if (e != Error::OK)
		return report(e);
	table->sort();
	std::shared_ptr<const MountTable> published = std::move(table);
	std::vector<std::shared_ptr<const MountTable>> old;
	old.reserve(SNAPSHOT_SHARDS + 1);
	old.push_back(std::move(m_table));
	m_table = published;
	for (size_t i = 0; i < SNAPSHOT_SHARDS; ++i)
	{
		auto ref = shardReference(published);
		std::lock_guard<std::mutex> lock(m_shards[i].mutex);
		old.push_back(std::move(m_shards[i].table));
		m_shards[i].table = std::move(ref);
	}
	// The old reference is released outside the shard lock: if it holds the last backend reference, destroying the backend can be slow
	old.clear();
	bump();
	return report(Error::OK);
}

MountId VirtualFileSystem::mount(std::string_view mountPoint, std::shared_ptr<IBackend> backend,
								 const MountOptions& options, Error* err)
{
	if (!backend)
	{
		setError(err, Error::UNSUPPORTED);
		return 0;
	}
	std::string point;
	Error e = Path::normalize(mountPoint, point);
	if (e != Error::OK)
	{
		setError(err, e);
		return 0;
	}
	MountId id = m_nextId.fetch_add(1);
	e = modifyTable([&](MountTable& table)
	{
		MountEntry entry;
		entry.id = id;
		entry.point = point;
		entry.backend = backend;
		entry.priority = options.priority;
		entry.flags = options.flags;
		entry.state = std::make_shared<MountState>();
		table.mounts.push_back(std::move(entry));
		return Error::OK;
	});
	setError(err, e);
	return e == Error::OK ? id : 0;
}

MountId VirtualFileSystem::mountNative(std::string_view mountPoint, std::string_view nativeDir,
									   const MountOptions& options, const NativeOptions& nativeOptions, Error* err)
{
	auto backend = NativeBackend::create(nativeDir, nativeOptions, err);
	if (!backend)
		return 0;
	return mount(mountPoint, std::move(backend), options, err);
}

MountId VirtualFileSystem::mountMemory(std::string_view mountPoint, const MountOptions& options,
									   std::shared_ptr<MemoryBackend>* backendOut, Error* err)
{
	auto backend = MemoryBackend::create();
	if (backendOut)
		*backendOut = backend;
	return mount(mountPoint, std::move(backend), options, err);
}

MountId VirtualFileSystem::mountArchive(std::string_view mountPoint, std::string_view nativePath,
										const ArchiveOptions& archiveOptions, const MountOptions& options, Error* err)
{
	auto backend = ArchiveBackend::create(nativePath, archiveOptions, err);
	if (!backend)
		return 0;
	return mount(mountPoint, std::move(backend), options, err);
}

MountId VirtualFileSystem::mountFile(std::string_view mountPoint, std::string_view nativeFile,
									 const MountOptions& options, Error* err)
{
	auto backend = SingleFileBackend::create(nativeFile, false, err);
	if (!backend)
		return 0;
	return mount(mountPoint, std::move(backend), options, err);
}

Error VirtualFileSystem::unmount(MountId id, uint32_t flags)
{
	return modifyTable([&](MountTable& table)
	{
		auto it = std::find_if(table.mounts.begin(), table.mounts.end(), [id](const MountEntry& m) { return m.id == id; });
		if (it == table.mounts.end())
			return report(Error::NOT_FOUND);
		// Strict mode: refuse to unmount while streams are still open (like umount's default EBUSY)
		// Mark closing, then read the count; the opener increments the count, then reads closing (both seq_cst).
		// At least one side sees the other: either this side sees count > 0 and returns BUSY, or the opener sees closing, drops this open, and retries
		if (flags & UnmountFlags::FailIfBusy)
		{
			it->state->closing.store(true);
			if (it->state->openStreams.load() > 0)
			{
				it->state->closing.store(false);
				return report(Error::BUSY);
			}
		}
		// Lazy unmount: remove it from the namespace only; open streams hold the backend until the last stream closes
		table.mounts.erase(it);
		return report(Error::OK);
	});
}

Error VirtualFileSystem::remount(MountId id, std::shared_ptr<IBackend> backend)
{
	if (!backend)
		return report(Error::UNSUPPORTED);
	return modifyTable([&](MountTable& table)
	{
		for (auto& m : table.mounts)
		{
			if (m.id == id)
			{
				// Old streams keep the old backend; the open-stream count stays on the same MountState,
				// so a FailIfBusy unmount after remount still sees streams opened before the remount that have not closed yet
				m.backend = backend;
				return report(Error::OK);
			}
		}
		return report(Error::NOT_FOUND);
	});
}

std::vector<MountInfo> VirtualFileSystem::mounts() const
{
	auto table = snapshot();
	std::vector<MountInfo> result;
	result.reserve(table->mounts.size());
	for (const auto& m : table->mounts)
		result.push_back(MountInfo{m.id, m.point, m.priority, m.flags, m.backend});
	return result;
}

Error VirtualFileSystem::addAlias(std::string_view prefix, std::string_view target)
{
	if (prefix.empty())
		return report(Error::INVALID_PATH);
	std::string normTarget;
	Error e = Path::normalize(target, normTarget);
	if (e != Error::OK)
		return report(e);
	if (normTarget == "/")
		normTarget.clear();   // avoid "//" when mapping onto the root
	return modifyTable([&](MountTable& table)
	{
		auto& aliases = table.aliases;
		aliases.erase(std::remove_if(aliases.begin(), aliases.end(), [&](const auto& a) { return a.first == prefix; }),
					  aliases.end());
		aliases.emplace_back(std::string(prefix), normTarget);
		// Longest prefix wins
		std::stable_sort(aliases.begin(), aliases.end(),
						 [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
		return report(Error::OK);
	});
}

Error VirtualFileSystem::removeAlias(std::string_view prefix)
{
	return modifyTable([&](MountTable& table)
	{
		auto& aliases = table.aliases;
		auto it = std::find_if(aliases.begin(), aliases.end(), [&](const auto& a) { return a.first == prefix; });
		if (it == aliases.end())
			return report(Error::NOT_FOUND);
		aliases.erase(it);
		return report(Error::OK);
	});
}

// ================================================================ Internal helpers

Error VirtualFileSystem::normalize(const MountTable& table, std::string_view raw, std::string& out) const
{
	if (table.aliases.empty())
		return report(Path::normalize(raw, out));
	return report(Path::normalize(table.applyAlias(raw), out));
}

Error VirtualFileSystem::resolve(const MountTable& table, std::string_view path, std::vector<Layer>& layers,
								 int& visible, Stat& st) const
{
	table.collectLayers(path, layers);
	visible = -1;
	bool invalid = false;   // some layer considers this name illegal (for example the native backend rejects "con")
	for (size_t i = 0; i < layers.size(); ++i)
	{
		const Layer& layer = layers[i];
		Stat s;
		Error e = layer.mount->backend->stat(layer.rel, s);
		if (e == Error::OK)
		{
			// An ancestor of a mount point must be a directory: a file at that location on a lower layer is hidden by the mount path, otherwise the mount would be unreachable
			if (s.type != FileType::DIR && table.isVirtualDir(path))
				continue;
			// A hit on the highest layer is the visible entry
			visible = static_cast<int>(i);
			st = s;
			st.mount = layer.mount->id;
			st.writable = layer.mount->writable();
			return report(Error::OK);
		}
		// This layer does not have the path, or the name is illegal on that backend (for example Native rejects "con"): look at the next layer
		if (e == Error::NOT_FOUND)
			continue;
		if (e == Error::INVALID_PATH)
		{
			invalid = true;
			continue;
		}
		if (e == Error::NOT_DIR)
		{
			// Overlay rule: "an upper file hides a lower directory" holds only when the parent path is not a directory in the merged view;
			// if a higher layer provides the parent path as a directory, this layer's file is already hidden, as if the entry were absent
			if (path.size() > 1)
			{
				std::string parent(Path::parent(path));
				std::vector<Layer> parentLayers;
				int parentVisible = -1;
				Stat ps;
				Error pe = resolve(table, parent, parentLayers, parentVisible, ps);
				if (pe == Error::OK && ps.type == FileType::DIR)
					continue;
			}
			return report(Error::NOT_DIR);
		}
		return report(e);
	}

	// Ancestors of a mount point are synthesized as read-only virtual directories
	if (table.isVirtualDir(path))
	{
		st = Stat();
		st.type = FileType::DIR;
		return report(Error::OK);
	}
	// No layer provides it: if some layer rejected the name, report INVALID_PATH (the same error the backend returns on write; see mount-rules.md section 1)
	return report(invalid ? Error::INVALID_PATH : Error::NOT_FOUND);
}

int VirtualFileSystem::chooseWriteTarget(const std::vector<Layer>& layers)
{
	// A writable layer explicitly marked WriteTarget wins, otherwise the writable layer with the highest priority
	for (size_t i = 0; i < layers.size(); ++i)
	{
		if ((layers[i].mount->flags & MountFlags::WriteTarget) && layers[i].mount->writable())
			return static_cast<int>(i);
	}
	for (size_t i = 0; i < layers.size(); ++i)
	{
		if (layers[i].mount->writable())
			return static_cast<int>(i);
	}
	return -1;
}

Error VirtualFileSystem::ensureParents(const Layer& layer, std::string_view relParent)
{
	// When the parent directory exists in the merged view, mkdir -p the relative path chain in the write layer (overlay copy-up directories)
	if (relParent.empty())
		return report(Error::OK);
	IBackend* backend = layer.mount->backend.get();
	size_t pos = 0;
	while (pos <= relParent.size())
	{
		size_t end = relParent.find('/', pos);
		if (end == std::string_view::npos)
			end = relParent.size();
		std::string_view prefix = relParent.substr(0, end);
		Stat st;
		Error e = backend->stat(prefix, st);
		if (e == Error::OK)
		{
			if (st.type != FileType::DIR)
				return report(Error::NOT_DIR);
		}
		else if (e == Error::NOT_FOUND)
		{
			e = backend->createDir(prefix);
			// Concurrent creation of the same directory may return EXISTS, which is not an error; a concurrent same-named file is also possible, so confirm the type
			if (e == Error::EXISTS)
			{
				e = backend->stat(prefix, st);
				if (e != Error::OK)
					return report(e);
				if (st.type != FileType::DIR)
					return report(Error::NOT_DIR);
			}
			else if (e != Error::OK)
				return report(e);
		}
		else
			return report(e);
		pos = end + 1;
	}
	return report(Error::OK);
}

Error VirtualFileSystem::copyUp(const Layer& src, const Layer& dst)
{
	Error e = Error::OK;
	auto in = src.mount->backend->open(src.rel, IFileStream::Mode::READ, &e);
	if (!in)
		return report(e);

	// Backends that support rename: write a temporary file, then rename, so a failure halfway does not leave a partial file
	const bool atomic = (dst.mount->backend->capabilities() & BackendCaps::Rename) != 0;
	std::string target(dst.rel);
	std::string tmp = atomic ? uniqueTempName(target, "vfs-copyup") : target;
	std::unique_ptr<IFileStream> out;
	try
	{
		std::vector<uint8_t> buf(COPY_BUFFER_SIZE);   // allocate before creating the temporary file: a failed allocation must not leave a temporary file
		out = dst.mount->backend->open(tmp, IFileStream::Mode::WRITE, &e);
		if (!out)
			return report(e);
		e = copyStream(*in, *out);
		if (e == Error::OK && out->close() != 0)
			e = Error::IO_ERROR;
	}
	catch (const std::bad_alloc&)
	{
		// Errors are reported by return value (Error.h); exceptions must not cross the VFS API
		e = Error::IO_ERROR;
	}
	const bool created = out != nullptr;
	out.reset();
	if (e != Error::OK)
	{
		if (created)
			dst.mount->backend->removeFile(tmp);
		return report(e);
	}
	if (atomic)
	{
		e = dst.mount->backend->rename(tmp, target);
		if (e != Error::OK)
			dst.mount->backend->removeFile(tmp);
	}
	return report(e);
}

namespace
{
// Set when track inside openOnce sees the mount being unmounted with FailIfBusy; openFileStream retries on that
thread_local bool t_openRetry = false;
}

std::unique_ptr<IFileStream> VirtualFileSystem::track(const MountEntry& mount, std::unique_ptr<IFileStream> stream) const
{
	auto tracked = std::make_unique<TrackedFileStream>(std::move(stream), mount.backend, mount.state);
	// The constructor already incremented the count; look at closing here (see unmount): drop this stream if a strict unmount is in progress
	if (mount.state->closing.load())
	{
		t_openRetry = true;
		return nullptr;
	}
	return tracked;
}

std::unique_ptr<IFileStream> VirtualFileSystem::openFileStream(std::string_view rawPath, IFileStream::Mode mode,
															   Error* err) const
{
	// Retry when racing a FailIfBusy unmount: the unmount either finishes (on retry the mount is gone from the snapshot) or clears the flag because the count is > 0
	for (int attempt = 0; attempt < 1000; ++attempt)
	{
		t_openRetry = false;
		auto stream = openOnce(rawPath, mode, err);
		if (!t_openRetry)
			return stream;
		std::this_thread::yield();
	}
	t_openRetry = false;
	return failStream(err, Error::BUSY);
}

// ================================================================ Queries

Error VirtualFileSystem::stat(std::string_view rawPath, Stat& out) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return report(e);
	std::vector<Layer> layers;
	int visible = -1;
	return report(resolve(*table, path, layers, visible, out));
}

bool VirtualFileSystem::exists(std::string_view path) const
{
	Stat st;
	return stat(path, st) == Error::OK;
}

bool VirtualFileSystem::isFile(std::string_view path) const
{
	Stat st;
	return stat(path, st) == Error::OK && st.type == FileType::FILE;
}

bool VirtualFileSystem::isDir(std::string_view path) const
{
	Stat st;
	return stat(path, st) == Error::OK && st.type == FileType::DIR;
}

int64_t VirtualFileSystem::fileSize(std::string_view path) const
{
	Stat st;
	if (stat(path, st) != Error::OK || st.type != FileType::FILE)
		return -1;
	return st.size;
}

Error VirtualFileSystem::listIn(const MountTable& table, const std::string& path,
								const std::function<bool(const DirEntry&)>& cb) const
{
	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	Error e = resolve(table, path, layers, visible, st);
	if (e != Error::OK)
		return report(e);
	if (st.type != FileType::DIR)
		return report(Error::NOT_DIR);

	// Deduplicate by name; higher layers are inserted first so they decide the type; std::map emits names in byte order
	std::map<std::string, FileType, std::less<>> merged;
	for (const auto& layer : layers)
	{
		// Skip when this layer does not have the directory (or it is a file); other errors (I/O, permissions) are returned as-is and must not be treated as an empty directory
		e = layer.mount->backend->list(layer.rel, [&merged](const DirEntry& entry)
		{
			merged.emplace(entry.name, entry.type);
			return true;
		});
		if (e != Error::OK && e != Error::NOT_FOUND && e != Error::NOT_DIR && e != Error::INVALID_PATH)
			return report(e);
	}

	// Ancestors of a mount point are synthesized as virtual directories: mounting at /a/b, listing / shows a
	for (const auto& m : table.mounts)
	{
		if (!Path::isStrictAncestor(path, m.point))
			continue;
		std::string_view rest = std::string_view(m.point).substr(path.size() == 1 ? 1 : path.size() + 1);
		std::string_view name = rest.substr(0, rest.find('/'));
		// A child's type comes from resolving that child (for example a single-file mount point is a file; an ancestor of a mount point is shown as a directory even if the lower layer has a file there)
		std::string child = Path::join(path, name);
		std::vector<Layer> childLayers;
		int childVisible = -1;
		Stat cs;
		Error ce = resolve(table, child, childLayers, childVisible, cs);
		if (ce == Error::OK)
			merged[std::string(name)] = cs.type;
		else if (ce != Error::NOT_FOUND && ce != Error::NOT_DIR && ce != Error::INVALID_PATH)
			return report(ce);   // same as a layer list: an I/O or permission error fails the whole list
	}

	DirEntry entry;
	for (const auto& kv : merged)
	{
		entry.name = kv.first;
		entry.type = kv.second;
		if (!cb(entry))
			break;
	}
	return report(Error::OK);
}

Error VirtualFileSystem::list(std::string_view rawPath, const std::function<bool(const DirEntry&)>& cb) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return report(e);
	return report(listIn(*table, path, cb));
}

std::vector<DirEntry> VirtualFileSystem::list(std::string_view path, Error* err) const
{
	std::vector<DirEntry> result;
	Error e = list(path, [&result](const DirEntry& entry)
	{
		result.push_back(entry);
		return true;
	});
	setError(err, e);
	return result;
}

Error VirtualFileSystem::walkIn(const MountTable& table, const std::string& path,
								const std::function<bool(const std::string&, const DirEntry&)>& cb, bool& stop) const
{
	std::vector<DirEntry> entries;
	Error e = listIn(table, path, [&entries](const DirEntry& entry)
	{
		entries.push_back(entry);
		return true;
	});
	if (e != Error::OK)
		return report(e);
	for (const auto& entry : entries)
	{
		std::string full = Path::join(path, entry.name);
		if (!cb(full, entry))
		{
			stop = true;
			return report(Error::OK);
		}
		if (entry.type == FileType::DIR)
		{
			e = walkIn(table, full, cb, stop);
			if (stop)
				return report(Error::OK);
			// A subdirectory deleted or replaced by a file after it was listed (NOT_FOUND / NOT_DIR) is not an error;
			// other errors (I/O, permissions) fail the whole walk, same as list, so the caller does not treat a partial walk as success
			if (e != Error::OK && e != Error::NOT_FOUND && e != Error::NOT_DIR)
				return report(e);
		}
	}
	return report(Error::OK);
}

Error VirtualFileSystem::walk(std::string_view rawPath,
							  const std::function<bool(const std::string& fullPath, const DirEntry& entry)>& cb) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return report(e);
	bool stop = false;
	return report(walkIn(*table, path, cb, stop));
}

std::string VirtualFileSystem::realPath(std::string_view rawPath) const
{
	auto table = snapshot();
	std::string path;
	if (normalize(*table, rawPath, path) != Error::OK)
		return std::string();
	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	if (resolve(*table, path, layers, visible, st) != Error::OK || visible < 0)
		return std::string();
	return layers[visible].mount->backend->realPath(layers[visible].rel);
}

// ================================================================ Streams

std::unique_ptr<IFileStream> VirtualFileSystem::openOnce(std::string_view rawPath, IFileStream::Mode mode,
													 Error* err) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return failStream(err, e);

	std::vector<Layer> layers;
	if (mode == IFileStream::Mode::READ)
	{
		// Fast path: when the path falls on only one mount (the common case), open directly and skip a stat (one system call on the native backend).
		// Success means it is a file on that layer, which is the visible entry; IS_DIR matches the slow path; other errors go to the slow path so the error code stays identical
		table->collectLayers(path, layers);
		if (layers.size() == 1 && !table->isVirtualDir(path))
		{
			const Layer& layer = layers[0];
			auto stream = layer.mount->backend->open(layer.rel, mode, &e);
			if (stream)
			{
				setError(err, Error::OK);
				return track(*layer.mount, std::move(stream));
			}
			if (e == Error::IS_DIR)
				return failStream(err, e);
		}
	}
	int visible = -1;
	Stat st;
	e = resolve(*table, path, layers, visible, st);

	if (mode == IFileStream::Mode::READ)
	{
		// Read: take the highest-layer hit
		if (e != Error::OK)
			return failStream(err, e);
		if (st.type == FileType::DIR)
			return failStream(err, Error::IS_DIR);
		const Layer& layer = layers[visible];
		auto stream = layer.mount->backend->open(layer.rel, mode, &e);
		if (!stream)
			return failStream(err, e);
		setError(err, Error::OK);
		return track(*layer.mount, std::move(stream));
	}

	if (e == Error::OK && st.type == FileType::DIR)
		return failStream(err, Error::IS_DIR);
	// Only some layers reject this name: the write layer may accept it (the write layer's backend decides)
	if (e == Error::INVALID_PATH)
		e = Error::NOT_FOUND;
	if (e != Error::OK && e != Error::NOT_FOUND)
		return failStream(err, e);
	const bool found = e == Error::OK;

	int target = -1;
	if (found && layers[visible].mount->writable())
	{
		// If the layer that holds the visible entry is writable, write in place
		target = visible;
		const Layer& layer = layers[target];
		auto stream = layer.mount->backend->open(layer.rel, mode, &e);
		if (!stream)
			return failStream(err, e);
		bump();
		setError(err, Error::OK);
		return track(*layer.mount, std::move(stream));
	}

	// Otherwise write to the highest-priority writable mount that covers the path
	target = chooseWriteTarget(layers);
	if (target < 0)
		return failStream(err, Error::READ_ONLY);
	// The write layer is below the read-only layer that holds the visible entry, so a write would not be readable; reject it
	if (found && target > visible)
		return failStream(err, Error::READ_ONLY);
	if (!found)
	{
		// The parent directory must exist in the merged view (same as open(O_CREAT); parents are not created out of nothing)
		std::string parent(Path::parent(path));
		std::vector<Layer> parentLayers;
		int parentVisible = -1;
		Stat ps;
		Error pe = resolve(*table, parent, parentLayers, parentVisible, ps);
		if (pe != Error::OK)
			return failStream(err, pe);
		if (ps.type != FileType::DIR)
			return failStream(err, Error::NOT_DIR);
	}
	const Layer& dst = layers[target];
	if (dst.rel.empty())
	{
		// The path is exactly the write layer's mount point: the root of a directory mount is a directory; the root of a single-file mount is that file, opened as usual
		Stat rs;
		if (dst.mount->backend->stat("", rs) != Error::OK || rs.type == FileType::DIR)
			return failStream(err, Error::IS_DIR);
	}

	// Copy-up, create, and open in the write layer finish inside the same (path-striped) lock, until the stream is open:
	// - when several threads append the same read-only-layer file, only the first copies; the others see the copy already in the write layer while holding the lock and open it;
	// - the copy-up rename does not overwrite a same-named file another thread just created (WRITE) in the write layer;
	// - after the copy finishes and before it is opened, the copy is not replaced by another copy-up
	std::lock_guard<std::recursive_mutex> lock(copyUpLock(*dst.mount, dst.rel));
	e = ensureParents(dst, Path::parentRelative(dst.rel));
	if (e != Error::OK)
	{
		bump();   // some parent directories may already have been created
		return failStream(err, e);
	}
	// When APPEND/OVERLAPPED modifies a file on a read-only layer, copy the lower contents into the write layer first
	if (found && (mode == IFileStream::Mode::APPEND || mode == IFileStream::Mode::OVERLAPPED))
	{
		Stat copied;
		if (dst.mount->backend->stat(dst.rel, copied) != Error::OK || copied.type != FileType::FILE)
		{
			e = copyUp(layers[visible], dst);
			if (e != Error::OK)
			{
				bump();
				return failStream(err, e);
			}
		}
	}
	auto stream = dst.mount->backend->open(dst.rel, mode, &e);
	// Even if open fails, parent directories or the copy-up copy may already be in the write layer: the namespace changed
	bump();
	if (!stream)
		return failStream(err, e);
	setError(err, Error::OK);
	return track(*dst.mount, std::move(stream));
}

std::recursive_mutex& VirtualFileSystem::copyUpLock(const MountEntry& mount, std::string_view rel) const
{
	const size_t h = std::hash<std::string_view>()(rel) ^ (static_cast<size_t>(mount.id) * 0x9E3779B97F4A7C15ull);
	return m_copyUpLocks[h % COPY_UP_STRIPES];
}

// ================================================================ Mutations

Error VirtualFileSystem::createDirIn(const MountTable& table, const std::string& path) const
{
	if (path == "/")
		return report(Error::EXISTS);
	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	Error e = resolve(table, path, layers, visible, st);
	// Already a directory or a file: return EXISTS
	if (e == Error::OK)
		return report(Error::EXISTS);
	// Only some layers reject this name: the write layer may accept it (the write layer's backend decides)
	if (e == Error::INVALID_PATH)
		e = Error::NOT_FOUND;
	if (e != Error::NOT_FOUND)
		return report(e);

	std::string parent(Path::parent(path));
	std::vector<Layer> parentLayers;
	int parentVisible = -1;
	Stat ps;
	e = resolve(table, parent, parentLayers, parentVisible, ps);
	if (e != Error::OK)
		return report(e);   // NOT_FOUND / NOT_DIR / other errors (IO_ERROR and so on) are returned as-is
	if (ps.type != FileType::DIR)
		return report(Error::NOT_DIR);

	int target = chooseWriteTarget(layers);
	if (target < 0)
		return report(Error::READ_ONLY);
	const Layer& dst = layers[target];
	if (dst.rel.empty())
		return report(Error::EXISTS);
	e = ensureParents(dst, Path::parentRelative(dst.rel));
	if (e != Error::OK)
		return report(e);
	e = dst.mount->backend->createDir(dst.rel);
	if (e == Error::OK)
		bump();
	return report(e);
}

Error VirtualFileSystem::createDir(std::string_view rawPath) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return report(e);
	return report(createDirIn(*table, path));
}

Error VirtualFileSystem::createDirs(std::string_view rawPath) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return report(e);
	if (path == "/")
		return report(Error::OK);

	// Handle each prefix in order: skip if it is already a directory, create it if it is missing.
	// Concurrency: another thread may delete a just-created (or just-checked) parent between the two steps, and createDirIn then returns NOT_FOUND;
	// start over. There is a limit; if it keeps being deleted, eventually return NOT_FOUND (mkdir -p fails in the same situation)
	const int MAX_RESTARTS = 16;
	for (int attempt = 0;; ++attempt)
	{
		bool restart = false;
		size_t pos = 1;
		while (pos <= path.size())
		{
			size_t end = path.find('/', pos);
			if (end == std::string::npos)
				end = path.size();
			std::string prefix = path.substr(0, end);
			const bool last = end == path.size();

			std::vector<Layer> layers;
			int visible = -1;
			Stat st;
			e = resolve(*table, prefix, layers, visible, st);
			if (e == Error::NOT_FOUND)
			{
				e = createDirIn(*table, prefix);
				if (e == Error::EXISTS)
				{
					// A same-named entry was created concurrently: confirm it is a directory (it might be a file, or it might already have been deleted again)
					e = resolve(*table, prefix, layers, visible, st);
				}
				else if (e == Error::NOT_FOUND && pos > 1)
				{
					restart = true;   // the parent directory disappeared after the check
					break;
				}
				else if (e != Error::OK)
					return report(e);
				else
				{
					pos = end + 1;
					continue;
				}
			}
			if (e == Error::OK)
			{
				if (st.type != FileType::DIR)
					return report(last ? Error::EXISTS : Error::NOT_DIR);
			}
			else if (e == Error::NOT_FOUND && pos > 1)
			{
				restart = true;
				break;
			}
			else
				return report(e);
			pos = end + 1;
		}
		if (!restart)
			return report(Error::OK);
		if (attempt >= MAX_RESTARTS)
			return report(Error::NOT_FOUND);
	}
}

namespace
{

// Recursively delete a directory tree inside a backend (called only on a writable layer that already passed the "check first" step)
Error removeTree(IBackend* backend, const std::string& rel)
{
	std::vector<DirEntry> children;
	Error e = backend->list(rel, [&children](const DirEntry& entry)
	{
		children.push_back(entry);
		return true;
	});
	if (e != Error::OK)
		return e;
	for (const auto& child : children)
	{
		std::string childRel = Path::joinRelative(rel, child.name);
		e = child.type == FileType::DIR ? removeTree(backend, childRel) : backend->removeFile(childRel);
		if (e != Error::OK)
			return e;
	}
	return backend->removeDir(rel);
}

} // namespace

Error VirtualFileSystem::removeDir(std::string_view rawPath, bool recursive) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return report(e);

	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	e = resolve(*table, path, layers, visible, st);
	if (e != Error::OK)
		return report(e);
	if (st.type != FileType::DIR)
		return report(Error::NOT_DIR);
	// The mount point itself and its ancestors (including the root and synthesized virtual directories) cannot be deleted
	if (table->isMountPointOrAncestor(path))
		return report(Error::BUSY);

	// Check first, then act. Count how many layers provide this directory in the merged view
	int contributor = -1;
	int count = 0;
	for (size_t i = 0; i < layers.size(); ++i)
	{
		Stat s;
		Error le = layers[i].mount->backend->stat(layers[i].rel, s);
		if (le == Error::OK && s.type == FileType::DIR)
		{
			contributor = static_cast<int>(i);
			++count;
		}
		else if (le != Error::OK && le != Error::NOT_FOUND && le != Error::NOT_DIR && le != Error::INVALID_PATH)
			return report(le);   // cannot tell whether this layer has the directory (I/O or permission error): delete nothing
	}
	if (count == 0)
		return report(Error::NOT_FOUND);
	// Other layers (read-only or not the write layer) still contribute this directory or its descendants: delete nothing and return EROFS
	if (count > 1 || !layers[contributor].mount->writable())
		return report(Error::READ_ONLY);

	const Layer& layer = layers[contributor];
	IBackend* backend = layer.mount->backend.get();
	if (!recursive)
	{
		bool empty = true;
		e = backend->list(layer.rel, [&empty](const DirEntry&)
		{
			empty = false;
			return false;
		});
		// A failed list must not be treated as an empty directory
		if (e != Error::OK)
			return report(e);
		if (!empty)
			return report(Error::NOT_EMPTY);
		e = backend->removeDir(layer.rel);
		if (e == Error::OK)
			bump();
	}
	else
	{
		e = removeTree(backend, std::string(layer.rel));
		// A failed recursive delete may already have removed part of the tree, so the namespace changed as well
		bump();
	}
	return report(e);
}

Error VirtualFileSystem::removeFile(std::string_view rawPath) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return report(e);

	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	e = resolve(*table, path, layers, visible, st);
	if (e != Error::OK)
		return report(e);
	if (st.type == FileType::DIR || visible < 0)
		return report(Error::IS_DIR);
	// A single-file mount point itself cannot be deleted
	if (table->isMountPoint(path))
		return report(Error::BUSY);
	// Delete only the visible entry; if it is on a read-only layer, return EROFS
	const Layer& layer = layers[visible];
	if (!layer.mount->writable())
		return report(Error::READ_ONLY);
	e = layer.mount->backend->removeFile(layer.rel);
	if (e == Error::OK)
		bump();
	return report(e);
}

Error VirtualFileSystem::checkSingleLayerDir(const std::vector<Layer>& layers, int keep)
{
	for (size_t i = 0; i < layers.size(); ++i)
	{
		if (static_cast<int>(i) == keep)
			continue;
		Stat s;
		Error e = layers[i].mount->backend->stat(layers[i].rel, s);
		if (e == Error::OK && s.type == FileType::DIR)
			return report(Error::READ_ONLY);
		// Cannot tell whether this layer has the directory: do not take the risk
		if (e != Error::OK && e != Error::NOT_FOUND && e != Error::NOT_DIR && e != Error::INVALID_PATH)
			return report(e);
	}
	return report(Error::OK);
}

Error VirtualFileSystem::rename(std::string_view rawFrom, std::string_view rawTo) const
{
	auto table = snapshot();
	std::string from, to;
	Error e = normalize(*table, rawFrom, from);
	if (e != Error::OK)
		return report(e);
	e = normalize(*table, rawTo, to);
	if (e != Error::OK)
		return report(e);

	std::vector<Layer> fromLayers;
	int fromVisible = -1;
	Stat fromStat;
	e = resolve(*table, from, fromLayers, fromVisible, fromStat);
	if (e != Error::OK)
		return report(e);
	if (from == to)
		return report(Error::OK);
	// A mount point and its ancestors cannot be renamed
	if (fromVisible < 0 || table->isMountPointOrAncestor(from) || table->isMountPointOrAncestor(to))
		return report(Error::BUSY);
	const Layer& src = fromLayers[fromVisible];
	if (!src.mount->writable())
		return report(Error::READ_ONLY);
	// When a directory is merged from several layers, only the part in the write layer could be moved and the rest would stay behind;
	// same rule as removeDir, so reject it outright
	if (fromStat.type == FileType::DIR)
	{
		e = checkSingleLayerDir(fromLayers, fromVisible);
		if (e != Error::OK)
			return report(e);
	}
	// A directory cannot be moved into its own subtree
	if (fromStat.type == FileType::DIR && Path::isStrictAncestor(from, to))
		return report(Error::INVALID_PATH);

	std::vector<Layer> toLayers;
	int toVisible = -1;
	Stat toStat;
	e = resolve(*table, to, toLayers, toVisible, toStat);
	// INVALID_PATH: only some layers reject this name; the write layer may accept it (the write layer's backend decides)
	if (e == Error::INVALID_PATH)
		e = Error::NOT_FOUND;
	if (e != Error::OK && e != Error::NOT_FOUND)
		return report(e);
	const bool toFound = e == Error::OK;

	int target = -1;
	if (toFound)
	{
		if (toStat.type == FileType::DIR && fromStat.type == FileType::FILE)
			return report(Error::IS_DIR);
		if (toStat.type == FileType::FILE && fromStat.type == FileType::DIR)
			return report(Error::NOT_DIR);
		if (toVisible >= 0 && toLayers[toVisible].mount->writable())
			target = toVisible;
	}
	if (target < 0)
	{
		target = chooseWriteTarget(toLayers);
		if (target < 0)
			return report(Error::READ_ONLY);
		if (toFound && target > toVisible)
			return report(Error::READ_ONLY);
		if (!toFound)
		{
			std::string parent(Path::parent(to));
			std::vector<Layer> parentLayers;
			int parentVisible = -1;
			Stat ps;
			Error pe = resolve(*table, parent, parentLayers, parentVisible, ps);
			if (pe != Error::OK)
				return report(pe);
			if (ps.type != FileType::DIR)
				return report(Error::NOT_DIR);
		}
	}

	if (toFound && toStat.type == FileType::DIR)
	{
		// Directory replacing an existing directory: whether the target in the write layer is empty is decided atomically by the backend (POSIX rename);
		// other layers are checked here: if a same-named directory on another layer has contents, those contents would show up inside the moved directory after a "successful" rename,
		// so the target in the merged view is not an empty directory and rename(2) returns NOT_EMPTY
		for (size_t i = 0; i < toLayers.size(); ++i)
		{
			if (static_cast<int>(i) == target)
				continue;
			bool nonEmpty = false;
			Error le = toLayers[i].mount->backend->list(toLayers[i].rel, [&nonEmpty](const DirEntry&)
			{
				nonEmpty = true;
				return false;
			});
			if (le != Error::OK && le != Error::NOT_FOUND && le != Error::NOT_DIR && le != Error::INVALID_PATH)
				return report(le);
			if (le == Error::OK && nonEmpty)
				return report(Error::NOT_EMPTY);
		}
	}

	const Layer& dst = toLayers[target];
	// An atomic rename is possible only when source and target fall on the same mount; otherwise EXDEV
	if (dst.mount->id != src.mount->id)
		return report(Error::CROSS_DEVICE);
	if (!(src.mount->backend->capabilities() & BackendCaps::Rename))
		return report(Error::UNSUPPORTED);
	e = ensureParents(dst, Path::parentRelative(dst.rel));
	if (e != Error::OK)
	{
		bump();   // some parent directories may already have been created
		return report(e);
	}
	e = src.mount->backend->rename(src.rel, dst.rel);
	bump();   // success, or failure after ensureParents already created parents in the write layer
	return report(e);
}

Error VirtualFileSystem::moveFile(std::string_view from, std::string_view to) const
{
	Error e = rename(from, to);
	if (e != Error::CROSS_DEVICE && e != Error::UNSUPPORTED)
		return report(e);
	// Across mounts: fall back to copy + delete, files only, not atomic
	Stat st;
	Error se = stat(from, st);
	if (se != Error::OK)
		return report(se);   // the source was deleted concurrently after the rename: report NOT_FOUND, not CROSS_DEVICE
	if (st.type != FileType::FILE)
		return report(e);
	e = copyFile(from, to);
	if (e != Error::OK)
		return report(e);
	return report(removeFile(from));
}

Error VirtualFileSystem::copyFile(std::string_view from, std::string_view to) const
{
	Error e = Error::OK;
	std::string nt;
	{
		// Copy onto itself: if the visible entry is writable, do nothing (opening the target WRITE would truncate the source first);
		// if the visible entry is on a read-only layer, copy as usual, which is an explicit copy-up
		auto table = snapshot();
		std::string nf;
		e = normalize(*table, from, nf);
		if (e != Error::OK)
			return report(e);
		e = normalize(*table, to, nt);
		if (e != Error::OK)
			return report(e);
		if (nf == nt)
		{
			Stat st;
			e = stat(nf, st);
			if (e != Error::OK)
				return report(e);
			if (st.type == FileType::DIR)
				return report(Error::IS_DIR);
			if (st.writable)
				return report(Error::OK);
		}
	}
	auto in = openFileStream(from, IFileStream::Mode::READ, &e);
	if (!in)
		return report(e);

	// Copy to a temporary file next to the target, then rename it into place: a failure halfway does not truncate or hide the original target
	// (same as copy-up and writeFileAtomic). If the temporary file cannot be written (for example a single-file mount) or is not on the same layer as the target, fall back to a direct write
	const std::string tmp = uniqueTempName(nt, "vfs-copy");
	auto out = openFileStream(tmp, IFileStream::Mode::WRITE, &e);
	const bool viaTemp = out != nullptr;
	if (!out)
	{
		out = openFileStream(nt, IFileStream::Mode::WRITE, &e);
		if (!out)
			return report(e);
	}
	e = copyStream(*in, *out);
	if (e == Error::OK && out->close() != 0)
		e = Error::IO_ERROR;
	out.reset();
	if (!viaTemp)
		return report(e);
	if (e != Error::OK)
	{
		removeFile(tmp);
		return report(e);
	}
	e = rename(tmp, nt);
	if (e == Error::CROSS_DEVICE || e == Error::UNSUPPORTED)
	{
		// The temporary file and the target land on different layers: fall back to a direct copy
		removeFile(tmp);
		in = openFileStream(from, IFileStream::Mode::READ, &e);
		if (!in)
			return report(e);
		out = openFileStream(nt, IFileStream::Mode::WRITE, &e);
		if (!out)
			return report(e);
		e = copyStream(*in, *out);
		if (e == Error::OK && out->close() != 0)
			e = Error::IO_ERROR;
		return report(e);
	}
	if (e != Error::OK)
		removeFile(tmp);
	return report(e);
}

Error VirtualFileSystem::copyStream(IFileStream& in, IFileStream& out)
{
	std::vector<uint8_t> buf;
	try
	{
		buf.resize(COPY_BUFFER_SIZE);
	}
	catch (const std::bad_alloc&)
	{
		// Report instead of throwing, so callers still remove their temporary files
		return Error::IO_ERROR;
	}
	while (true)
	{
		int n = in.read(buf.data(), static_cast<unsigned int>(buf.size()));
		if (n < 0)
			return Error::IO_ERROR;
		if (n == 0)
			return Error::OK;
		int done = 0;
		while (done < n)
		{
			int w = out.write(buf.data() + done, static_cast<unsigned int>(n - done));
			if (w <= 0)
				return Error::IO_ERROR;
			done += w;
		}
	}
}

// ================================================================ Convenience functions

Error VirtualFileSystem::readFile(std::string_view path, std::vector<uint8_t>& out) const
{
	Error e = Error::OK;
	auto stream = openFileStream(path, IFileStream::Mode::READ, &e);
	if (!stream)
		return report(e);
	int64_t size = stream->size();
	out.clear();
	if (size > 0)
		out.reserve(static_cast<size_t>(size));
	// A single read or write is limited to about 2 GB; loop in chunks until EOF
	std::vector<uint8_t> buf(COPY_BUFFER_SIZE);
	while (true)
	{
		int n = stream->read(buf.data(), static_cast<unsigned int>(buf.size()));
		if (n < 0)
			return report(Error::IO_ERROR);
		if (n == 0)
			break;
		out.insert(out.end(), buf.begin(), buf.begin() + n);
	}
	return report(Error::OK);
}

Error VirtualFileSystem::readFile(std::string_view path, std::string& out) const
{
	std::vector<uint8_t> data;
	Error e = readFile(path, data);
	if (e != Error::OK)
		return report(e);
	out.assign(reinterpret_cast<const char*>(data.data()), data.size());
	return report(Error::OK);
}

Error VirtualFileSystem::writeFile(std::string_view path, const void* data, size_t size) const
{
	Error e = Error::OK;
	auto stream = openFileStream(path, IFileStream::Mode::WRITE, &e);
	if (!stream)
		return report(e);
	const uint8_t* p = static_cast<const uint8_t*>(data);
	while (size > 0)
	{
		unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(size, VFS_MAX_IO_CHUNK));
		int n = stream->write(p, chunk);
		if (n <= 0)
			return report(Error::IO_ERROR);
		p += n;
		size -= static_cast<size_t>(n);
	}
	return report(stream->close() == 0 ? Error::OK : Error::IO_ERROR);
}

Error VirtualFileSystem::writeFile(std::string_view path, std::string_view data) const
{
	return report(writeFile(path, data.data(), data.size()));
}

Error VirtualFileSystem::writeFileAtomic(std::string_view path, const void* data, size_t size) const
{
	// Normalize first (for example drop a trailing '/'); the temporary file sits next to the target: path.vfs-N.tmp.
	// Each call uses a different temporary file: concurrent writes of the same path do not interfere
	std::string norm;
	{
		auto table = snapshot();
		Error ne = normalize(*table, path, norm);
		if (ne != Error::OK)
			return report(ne);
	}
	if (norm == "/")
		return report(Error::IS_DIR);
	const std::string tmp = uniqueTempName(norm, "vfs");
	Error e = Error::OK;
	try
	{
		e = writeFile(tmp, data, size);
	}
	catch (const std::bad_alloc&)
	{
		// Out of memory halfway: do not leave the temporary file behind
		e = Error::IO_ERROR;
	}
	if (e == Error::READ_ONLY)
	{
		// The temporary file's location is not writable (for example the target is a single-file mount and there is no writable layer beside it): write the target directly, no longer atomic
		removeFile(tmp);
		return report(writeFile(norm, data, size));
	}
	if (e != Error::OK)
	{
		// A failure halfway through the write must not leave the temporary file either
		removeFile(tmp);
		return report(e);
	}
	e = rename(tmp, norm);
	if (e == Error::CROSS_DEVICE || e == Error::UNSUPPORTED)
	{
		// The temporary file and the target land on different layers: fall back to a direct write (no longer atomic)
		removeFile(tmp);
		return report(writeFile(norm, data, size));
	}
	if (e != Error::OK)
		removeFile(tmp);
	return report(e);
}

NS_VFS_END
