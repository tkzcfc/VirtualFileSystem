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
 * 一个目录节点上的锁（共享或独占），析构时释放。只在 MemoryBackend 内部使用。
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

	// 把锁的所有权转给 other（other 原来持有的锁先释放）
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

// 在已加锁的目录中查找子项；没有返回 nullptr
MemoryNode* findChild(const MemoryNode* dir, std::string_view name)
{
	auto it = dir->children.find(name);
	return it == dir->children.end() ? nullptr : it->second.get();
}

// ancestor 是否是 path 的祖先（""是所有非空路径的祖先）
bool isAncestorPath(std::string_view ancestor, std::string_view path)
{
	if (ancestor.empty())
		return !path.empty();
	return path.size() > ancestor.size() && path.compare(0, ancestor.size(), ancestor) == 0 &&
		   path[ancestor.size()] == '/';
}

// 两个互不包含的目录路径的最近公共祖先（按路径段比较，"" 表示根）
std::string_view commonAncestor(std::string_view a, std::string_view b)
{
	size_t lastSep = std::string_view::npos;   // 共同前缀中最后一个完整路径段的结束位置
	size_t i = 0;
	while (i < a.size() && i < b.size() && a[i] == b[i])
	{
		if (a[i] == '/')
			lastSep = i;
		++i;
	}
	// 一边恰好在段边界处结束的情况属于“包含”关系，调用方已经排除；这里只按完整的路径段截取
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
		// 先锁子目录再放父目录：子目录在我们拿到它的锁之前不会被删除（删除者需要父目录的独占锁）
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

	// 节点在锁外分配；被覆盖的旧节点在锁外释放
	auto file = std::make_shared<MemoryNode>();
	file->type = FileType::FILE;
	file->buffer = std::make_shared<MemoryBuffer>();
	file->buffer->data = std::move(data);
	file->buffer->mtime.store(currentUnixTime());
	std::shared_ptr<MemoryNode> old;

	// mkdir -p 父目录：逐级持有独占锁下行（预填数据的接口，不追求并发）
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
		// 覆盖时换一个新的缓冲区，已经打开旧文件的流继续读旧数据（与 unlink+create 一致）
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
	// 目录锁释放之后再锁缓冲区：大文件写入持有缓冲区锁时，不会因此挡住同目录里的创建、删除
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
		// 先用共享锁查找；只有需要创建文件时才对父目录加独占锁（并重新查找）
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
			// 新节点在锁外分配
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
				// WRITE/APPEND/OVERLAPPED：不存在则创建，父目录必须存在（与 O_CREAT 一致）
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

	// WRITE 模式截断已有内容
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
	// 先在锁内复制目录项，再在锁外回调，避免回调里访问 VFS 造成死锁
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
	// 后端根目录就是挂载点（与 VFS 删除挂载点一致）
	if (rel.empty())
		return Error::BUSY;
	std::shared_ptr<MemoryNode> victim;   // 在所有锁释放之后才析构
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
		// 锁住目标目录：等正在里面查找、创建的操作结束，之后它就不可达了
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
	// 后端根目录是目录
	if (rel.empty())
		return Error::IS_DIR;
	std::shared_ptr<MemoryNode> victim;   // 在锁释放之后才析构（缓冲区可能很大）
	DirLock parent;
	Error err = lockDir(Path::parentRelative(rel), true, parent);
	if (err != Error::OK)
		return err;
	auto it = parent.node()->children.find(Path::fileName(rel));
	if (it == parent.node()->children.end())
		return Error::NOT_FOUND;
	if (it->second->type == FileType::DIR)
		return Error::IS_DIR;
	// unlink 语义：只移除名字，缓冲区由仍然打开的流继续持有
	victim = std::move(it->second);
	parent.node()->children.erase(it);
	parent.node()->mtime = currentUnixTime();
	return Error::OK;
}

Error MemoryBackend::rename(std::string_view from, std::string_view to)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	// 后端根目录不能作为改名的源或目标：这是非法参数而不是“忙”（VFS 层在调用后端之前就对挂载点返回 BUSY，
	// 见 mount-rules.md §9、§12）
	if (from.empty() || to.empty())
		return Error::INVALID_PATH;
	if (from == to)
		return Error::OK;
	// 不能把目录移到它自己的子树里（源不存在或是文件时，在下面查到源之后再报告 NOT_FOUND / NOT_DIR）
	const bool intoSelf = isAncestorPath(from, to);

	// 同一时刻只有一个 rename：祖先关系（下面的加锁顺序依赖它）在改名期间不会被其他 rename 改变
	std::lock_guard<std::mutex> renameLock(m_renameMutex);
	std::shared_ptr<MemoryNode> victim;   // 被覆盖的目标，在所有锁释放之后才析构

	if (intoSelf)
	{
		// 只需确认源：存在且是目录 → INVALID_PATH；是文件 → 目标的父路径穿过了文件，NOT_DIR
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
		// 一个是另一个的祖先：先锁祖先，再从祖先往下锁后代（与查找的加锁顺序一致，不会死锁）
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
		// 互不包含：先对最近公共祖先加共享锁并一直持有，再从它分别往下锁两边。
		// 这样持有一边的锁时不会再去等待它的祖先（否则会和“持有父目录独占锁、等待子目录”的 removeDir 互相等待）
		const std::string_view lca = commonAncestor(fromDir, toDir);
		DirLock lcaLock;
		err = lockDir(lca, false, lcaLock);
		if (err != Error::OK)
			return err;
		auto below = [&](std::string_view dir) { return lca.empty() ? dir : dir.substr(lca.size() + 1); };
		// 两边按路径的字典序加锁：rename 之间本来就由 m_renameMutex 串行，固定顺序只是让锁序检查工具（TSAN）也能看懂
		const bool fromFirst = fromDir < toDir;
		err = lockDir(below(fromFirst ? fromDir : toDir), true, first, lcaLock.node());
		if (err != Error::OK)
			return err;
		err = lockDir(below(fromFirst ? toDir : fromDir), true, second, lcaLock.node());
		if (err != Error::OK)
			return err;
		fromParent = fromFirst ? first.node() : second.node();
		toParent = fromFirst ? second.node() : first.node();
		// lcaLock 在这里释放：两边的独占锁已经拿到，改名只修改这两个目录
	}

	auto fromIt = fromParent->children.find(Path::fileName(from));
	if (fromIt == fromParent->children.end())
		return Error::NOT_FOUND;
	MemoryNode* node = fromIt->second.get();
	const std::string_view toName = Path::fileName(to);
	auto toIt = toParent->children.find(toName);
	if (toIt != toParent->children.end())
	{
		// 与 POSIX rename 一致的覆盖规则
		MemoryNode* target = toIt->second.get();
		if (node->type == FileType::FILE && target->type == FileType::DIR)
			return Error::IS_DIR;
		if (node->type == FileType::DIR && target->type == FileType::FILE)
			return Error::NOT_DIR;
		if (target->type == FileType::DIR)
		{
			// 目标是源的祖先（例如 "a/b" -> "a"）：它至少包含源，一定非空；它已经被我们锁住，不能再锁一次
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
	// 先插入目标再删除源：插入时分配内存失败抛异常的话，源仍然在原处，不会丢失
	toParent->children.emplace(std::string(toName), fromIt->second);
	fromParent->children.erase(fromIt);
	const int64_t now = currentUnixTime();
	fromParent->mtime = now;
	toParent->mtime = now;
	return Error::OK;
}

NS_VFS_END
