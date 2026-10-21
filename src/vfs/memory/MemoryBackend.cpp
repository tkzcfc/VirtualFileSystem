#include "MemoryBackend.h"
#include "MemoryFileStream.h"
#include "../Path.h"
#include <chrono>
#include <mutex>
#include <utility>

NS_VFS_BEGIN

int64_t currentUnixTime()
{
	using namespace std::chrono;
	return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::shared_ptr<MemoryBackend> MemoryBackend::create(const MemoryOptions& options)
{
	return std::make_shared<MemoryBackend>(options);
}

MemoryBackend::MemoryBackend(const MemoryOptions& options)
	: m_root(std::make_shared<MemoryNode>())
	, m_readOnly(options.readOnly)
{
	m_root->type = FileType::DIR;
	m_root->mtime = currentUnixTime();
}

MemoryBackend::~MemoryBackend()
{
}

uint32_t MemoryBackend::capabilities() const
{
	uint32_t caps = BackendCaps::Read | BackendCaps::Resize;
	if (!m_readOnly.load())
		caps |= BackendCaps::Write | BackendCaps::Rename;
	return caps;
}

/**
 * A lock (shared or exclusive) on one directory node, released on destruction. Used only inside MemoryBackend.
 */
class MemoryBackend::DirLock
{
public:
	DirLock() = default;
	DirLock(const DirLock&) = delete;
	DirLock& operator=(const DirLock&) = delete;
	~DirLock() { unlock(); }

	void lock(MemoryNode* node, bool exclusive)
	{
		unlock();
		if (exclusive)
			node->mutex.lock();
		else
			node->mutex.lock_shared();
		m_node = node;
		m_exclusive = exclusive;
	}

	void unlock()
	{
		if (!m_node)
			return;
		if (m_exclusive)
			m_node->mutex.unlock();
		else
			m_node->mutex.unlock_shared();
		m_node = nullptr;
	}

	// Transfer ownership of the lock to other (any lock other already holds is released first).
	void moveTo(DirLock& other)
	{
		other.unlock();
		other.m_node = m_node;
		other.m_exclusive = m_exclusive;
		m_node = nullptr;
	}

	MemoryNode* node() const { return m_node; }

private:
	MemoryNode* m_node = nullptr;
	bool m_exclusive = false;
};

namespace
{

// Look up a child in an already-locked directory; return nullptr if it is absent.
MemoryNode* findChild(const MemoryNode* dir, std::string_view name)
{
	auto it = dir->children.find(name);
	return it == dir->children.end() ? nullptr : it->second.get();
}

// Whether ancestor is an ancestor of path ("" is an ancestor of every non-empty path).
bool isAncestorPath(std::string_view ancestor, std::string_view path)
{
	if (ancestor.empty())
		return !path.empty();
	return path.size() > ancestor.size() && path.compare(0, ancestor.size(), ancestor) == 0 &&
		   path[ancestor.size()] == '/';
}

// Nearest common ancestor of two directory paths that do not contain each other (compared by path segment; "" is the root).
std::string_view commonAncestor(std::string_view a, std::string_view b)
{
	size_t lastSep = std::string_view::npos;   // End of the last complete path segment in the common prefix
	size_t i = 0;
	while (i < a.size() && i < b.size() && a[i] == b[i])
	{
		if (a[i] == '/')
			lastSep = i;
		++i;
	}
	// The case where one side ends exactly on a segment boundary is a "contains" relationship, which the caller has already excluded; here we cut only on complete path segments.
	return lastSep == std::string_view::npos ? std::string_view() : a.substr(0, lastSep);
}

} // namespace

Error MemoryBackend::lockDir(std::string_view dirRel, bool exclusive, DirLock& out, MemoryNode* from) const
{
	DirLock cur;
	MemoryNode* node = from;
	if (!node)
	{
		node = m_root.get();
		cur.lock(node, dirRel.empty() ? exclusive : false);
	}
	size_t start = 0;
	while (start < dirRel.size())
	{
		size_t end = dirRel.find('/', start);
		if (end == std::string_view::npos)
			end = dirRel.size();
		MemoryNode* child = findChild(node, dirRel.substr(start, end - start));
		if (!child)
			return Error::NOT_FOUND;
		if (child->type != FileType::DIR)
			return Error::NOT_DIR;
		// Lock the child directory before releasing the parent: the child cannot be deleted before we hold its lock (a deleter needs the parent's exclusive lock).
		DirLock next;
		next.lock(child, end == dirRel.size() ? exclusive : false);
		next.moveTo(cur);
		node = child;
		start = end + 1;
	}
	cur.moveTo(out);
	return Error::OK;
}

Error MemoryBackend::addFile(std::string_view rel, std::vector<uint8_t> data)
{
	std::string norm;
	Error err = Path::normalizeRelative(rel, norm);
	if (err != Error::OK)
		return err;
	if (norm.empty())
		return Error::IS_DIR;

	// Nodes are allocated outside the lock; an overwritten old node is released outside the lock.
	auto file = std::make_shared<MemoryNode>();
	file->type = FileType::FILE;
	file->buffer = std::make_shared<MemoryBuffer>();
	file->buffer->data = std::move(data);
	file->buffer->mtime.store(currentUnixTime());
	std::shared_ptr<MemoryNode> old;

	// mkdir -p the parents: walk downward holding an exclusive lock at each level (a data-prefill API; concurrency is not the goal).
	DirLock cur;
	MemoryNode* node = m_root.get();
	cur.lock(node, true);
	std::string_view path = norm;
	size_t start = 0;
	while (true)
	{
		size_t end = path.find('/', start);
		if (end == std::string_view::npos)
			break;
		std::string_view seg = path.substr(start, end - start);
		auto it = node->children.find(seg);
		if (it == node->children.end())
		{
			auto dir = std::make_shared<MemoryNode>();
			dir->type = FileType::DIR;
			dir->mtime = currentUnixTime();
			it = node->children.emplace(std::string(seg), std::move(dir)).first;
		}
		else if (it->second->type != FileType::DIR)
			return Error::NOT_DIR;
		MemoryNode* child = it->second.get();
		DirLock next;
		next.lock(child, true);
		next.moveTo(cur);
		node = child;
		start = end + 1;
	}

	std::string_view name = path.substr(start);
	auto it = node->children.find(name);
	if (it != node->children.end())
	{
		if (it->second->type == FileType::DIR)
			return Error::IS_DIR;
		// On overwrite, switch to a new buffer; streams that already opened the old file keep reading the old data (same as unlink+create).
		old = std::move(it->second);
		it->second = std::move(file);
	}
	else
		node->children.emplace(std::string(name), std::move(file));
	cur.unlock();
	return Error::OK;
}

Error MemoryBackend::addFile(std::string_view rel, std::string_view data)
{
	return addFile(rel, std::vector<uint8_t>(data.begin(), data.end()));
}

Error MemoryBackend::stat(std::string_view rel, Stat& out) const
{
	if (rel.empty())
	{
		out.type = FileType::DIR;
		out.size = 0;
		out.mtime = m_root->mtime.load(std::memory_order_relaxed);
		return Error::OK;
	}
	std::shared_ptr<MemoryBuffer> buffer;
	{
		DirLock parent;
		Error err = lockDir(Path::parentRelative(rel), false, parent);
		if (err != Error::OK)
			return err;
		const MemoryNode* node = findChild(parent.node(), Path::fileName(rel));
		if (!node)
			return Error::NOT_FOUND;
		out.type = node->type;
		if (node->type != FileType::FILE)
		{
			out.size = 0;
			out.mtime = node->mtime.load(std::memory_order_relaxed);
			return Error::OK;
		}
		buffer = node->buffer;
	}
	// Lock the buffer only after releasing the directory lock: a large write holding the buffer lock does not block create or delete in the same directory.
	std::shared_lock<SharedMutex> bufLock(buffer->mutex);
	out.size = static_cast<int64_t>(buffer->data.size());
	out.mtime = buffer->mtime.load(std::memory_order_relaxed);
	return Error::OK;
}

std::unique_ptr<IFileStream> MemoryBackend::open(std::string_view rel, IFileStream::Mode mode, Error* err)
{
	const bool writeMode = mode != IFileStream::Mode::READ;
	if (writeMode && m_readOnly.load())
	{
		setError(err, Error::READ_ONLY);
		return nullptr;
	}
	if (rel.empty())
	{
		setError(err, Error::IS_DIR);
		return nullptr;
	}

	const std::string_view parentRel = Path::parentRelative(rel);
	const std::string_view name = Path::fileName(rel);
	std::shared_ptr<MemoryBuffer> buffer;
	{
		// Look up under a shared lock first; take an exclusive lock on the parent (and look up again) only when a file must be created.
		DirLock parent;
		Error e = lockDir(parentRel, false, parent);
		if (e != Error::OK)
		{
			setError(err, e);
			return nullptr;
		}
		MemoryNode* node = findChild(parent.node(), name);
		if (!node && writeMode)
		{
			parent.unlock();
			// The new node is allocated outside the lock.
			auto file = std::make_shared<MemoryNode>();
			file->type = FileType::FILE;
			file->buffer = std::make_shared<MemoryBuffer>();
			file->buffer->mtime.store(currentUnixTime());
			e = lockDir(parentRel, true, parent);
			if (e != Error::OK)
			{
				setError(err, e);
				return nullptr;
			}
			node = findChild(parent.node(), name);
			if (!node)
			{
				// WRITE/APPEND/OVERLAPPED: create if missing; the parent directory must exist (same as O_CREAT).
				buffer = file->buffer;
				parent.node()->children.emplace(std::string(name), std::move(file));
				parent.node()->mtime = currentUnixTime();
			}
		}
		if (!buffer)
		{
			if (!node)
			{
				setError(err, Error::NOT_FOUND);
				return nullptr;
			}
			if (node->type == FileType::DIR)
			{
				setError(err, Error::IS_DIR);
				return nullptr;
			}
			buffer = node->buffer;
		}
	}

	// WRITE mode truncates existing content.
	if (mode == IFileStream::Mode::WRITE)
	{
		std::unique_lock<SharedMutex> bufLock(buffer->mutex);
		buffer->data.clear();
		buffer->mtime.store(currentUnixTime());
	}
	setError(err, Error::OK);
	return std::make_unique<MemoryFileStream>(std::move(buffer), mode);
}

Error MemoryBackend::list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const
{
	// Copy the directory entries while the lock is held, then invoke the callback outside the lock, so a callback that touches the VFS cannot deadlock.
	std::vector<DirEntry> entries;
	{
		DirLock dir;
		Error err = lockDir(relDir, false, dir);
		if (err != Error::OK)
			return err;
		entries.reserve(dir.node()->children.size());
		for (const auto& kv : dir.node()->children)
			entries.push_back(DirEntry{kv.first, kv.second->type});
	}
	for (const auto& entry : entries)
	{
		if (!cb(entry))
			break;
	}
	return Error::OK;
}

Error MemoryBackend::createDir(std::string_view rel)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	if (rel.empty())
		return Error::EXISTS;
	auto dir = std::make_shared<MemoryNode>();
	dir->type = FileType::DIR;
	dir->mtime = currentUnixTime();

	DirLock parent;
	Error err = lockDir(Path::parentRelative(rel), true, parent);
	if (err != Error::OK)
		return err;
	const std::string_view name = Path::fileName(rel);
	if (findChild(parent.node(), name))
		return Error::EXISTS;
	parent.node()->children.emplace(std::string(name), std::move(dir));
	parent.node()->mtime = currentUnixTime();
	return Error::OK;
}

Error MemoryBackend::removeDir(std::string_view rel)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	// The backend root is the mount point (same as the VFS refusing to delete a mount point).
	if (rel.empty())
		return Error::BUSY;
	std::shared_ptr<MemoryNode> victim;   // Destroyed only after every lock is released
	DirLock parent;
	Error err = lockDir(Path::parentRelative(rel), true, parent);
	if (err != Error::OK)
		return err;
	auto it = parent.node()->children.find(Path::fileName(rel));
	if (it == parent.node()->children.end())
		return Error::NOT_FOUND;
	if (it->second->type != FileType::DIR)
		return Error::NOT_DIR;
	{
		// Lock the target directory: wait for lookups and creates inside it to finish; after that it is unreachable.
		DirLock target;
		target.lock(it->second.get(), true);
		if (!it->second->children.empty())
			return Error::NOT_EMPTY;
		victim = std::move(it->second);
	}
	parent.node()->children.erase(it);
	parent.node()->mtime = currentUnixTime();
	return Error::OK;
}

Error MemoryBackend::removeFile(std::string_view rel)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	// The backend root is a directory.
	if (rel.empty())
		return Error::IS_DIR;
	std::shared_ptr<MemoryNode> victim;   // Destroyed only after the lock is released (the buffer may be large)
	DirLock parent;
	Error err = lockDir(Path::parentRelative(rel), true, parent);
	if (err != Error::OK)
		return err;
	auto it = parent.node()->children.find(Path::fileName(rel));
	if (it == parent.node()->children.end())
		return Error::NOT_FOUND;
	if (it->second->type == FileType::DIR)
		return Error::IS_DIR;
	// unlink semantics: only the name is removed; the buffer stays held by streams that are still open.
	victim = std::move(it->second);
	parent.node()->children.erase(it);
	parent.node()->mtime = currentUnixTime();
	return Error::OK;
}

Error MemoryBackend::rename(std::string_view from, std::string_view to)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	// The backend root cannot be the source or target of a rename: that is an invalid argument, not "busy" (the VFS layer returns BUSY for a mount point before calling the backend;
	// see mount-rules.md sections 9 and 12).
	if (from.empty() || to.empty())
		return Error::INVALID_PATH;
	if (from == to)
		return Error::OK;
	// A directory cannot be moved into its own subtree (if the source is missing or is a file, report NOT_FOUND / NOT_DIR after the source is looked up below).
	const bool intoSelf = isAncestorPath(from, to);

	// Only one rename runs at a time: ancestor relationships (the lock order below depends on them) cannot be changed by another rename while this one runs.
	std::lock_guard<std::mutex> renameLock(m_renameMutex);
	std::shared_ptr<MemoryNode> victim;   // The overwritten target, destroyed only after every lock is released

	if (intoSelf)
	{
		// Only the source needs to be confirmed: if it exists and is a directory -> INVALID_PATH; if it is a file -> the target's parent path crosses a file, NOT_DIR.
		Stat st;
		Error se = stat(from, st);
		if (se != Error::OK)
			return se;
		return st.type == FileType::DIR ? Error::INVALID_PATH : Error::NOT_DIR;
	}

	const std::string_view fromDir = Path::parentRelative(from);
	const std::string_view toDir = Path::parentRelative(to);
	DirLock first, second;
	MemoryNode* fromParent = nullptr;
	MemoryNode* toParent = nullptr;
	Error err = Error::OK;
	if (fromDir == toDir)
	{
		err = lockDir(fromDir, true, first);
		if (err != Error::OK)
			return err;
		fromParent = toParent = first.node();
	}
	else if (isAncestorPath(fromDir, toDir) || isAncestorPath(toDir, fromDir))
	{
		// One is an ancestor of the other: lock the ancestor first, then lock descendants downward from it (the same lock order as lookup, so this cannot deadlock).
		const bool fromIsAncestor = isAncestorPath(fromDir, toDir);
		const std::string_view up = fromIsAncestor ? fromDir : toDir;
		const std::string_view down = fromIsAncestor ? toDir : fromDir;
		err = lockDir(up, true, first);
		if (err != Error::OK)
			return err;
		err = lockDir(up.empty() ? down : down.substr(up.size() + 1), true, second, first.node());
		if (err != Error::OK)
			return err;
		fromParent = fromIsAncestor ? first.node() : second.node();
		toParent = fromIsAncestor ? second.node() : first.node();
	}
	else
	{
		// Neither contains the other: take a shared lock on the nearest common ancestor and hold it, then lock both sides downward from there.
		// Holding one side then never waits on its ancestor (otherwise this would wait against removeDir, which holds the parent's exclusive lock and waits for the child).
		const std::string_view lca = commonAncestor(fromDir, toDir);
		DirLock lcaLock;
		err = lockDir(lca, false, lcaLock);
		if (err != Error::OK)
			return err;
		auto below = [&](std::string_view dir) { return lca.empty() ? dir : dir.substr(lca.size() + 1); };
		// Lock the two sides in lexicographic path order: renames are already serialized by m_renameMutex; the fixed order only lets lock-order checkers (TSAN) follow it.
		const bool fromFirst = fromDir < toDir;
		err = lockDir(below(fromFirst ? fromDir : toDir), true, first, lcaLock.node());
		if (err != Error::OK)
			return err;
		err = lockDir(below(fromFirst ? toDir : fromDir), true, second, lcaLock.node());
		if (err != Error::OK)
			return err;
		fromParent = fromFirst ? first.node() : second.node();
		toParent = fromFirst ? second.node() : first.node();
		// lcaLock is released here: both exclusive locks are held, and the rename only modifies these two directories.
	}

	auto fromIt = fromParent->children.find(Path::fileName(from));
	if (fromIt == fromParent->children.end())
		return Error::NOT_FOUND;
	MemoryNode* node = fromIt->second.get();
	const std::string_view toName = Path::fileName(to);
	auto toIt = toParent->children.find(toName);
	if (toIt != toParent->children.end())
	{
		// Overwrite rules matching POSIX rename.
		MemoryNode* target = toIt->second.get();
		if (node->type == FileType::FILE && target->type == FileType::DIR)
			return Error::IS_DIR;
		if (node->type == FileType::DIR && target->type == FileType::FILE)
			return Error::NOT_DIR;
		if (target->type == FileType::DIR)
		{
			// The target is an ancestor of the source (for example "a/b" -> "a"): it contains at least the source, so it is not empty; we already hold its lock and must not lock it again.
			if (isAncestorPath(to, from))
				return Error::NOT_EMPTY;
			DirLock targetLock;
			targetLock.lock(target, true);
			if (!target->children.empty())
				return Error::NOT_EMPTY;
			victim = std::move(toIt->second);
		}
		else
			victim = std::move(toIt->second);
		toParent->children.erase(toIt);
	}
	// Insert the target before deleting the source: if allocation throws while inserting, the source is still in place and is not lost.
	toParent->children.emplace(std::string(toName), fromIt->second);
	fromParent->children.erase(fromIt);
	const int64_t now = currentUnixTime();
	fromParent->mtime = now;
	toParent->mtime = now;
	return Error::OK;
}

NS_VFS_END
