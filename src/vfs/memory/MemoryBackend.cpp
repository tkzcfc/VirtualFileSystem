#include "MemoryBackend.h"
#include "MemoryFileStream.h"
#include "../Path.h"
#include <chrono>
#include <mutex>

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

Error MemoryBackend::findNode(std::string_view rel, std::shared_ptr<MemoryNode>& out) const
{
	std::shared_ptr<MemoryNode> node = m_root;
	size_t start = 0;
	while (start < rel.size())
	{
		size_t end = rel.find('/', start);
		if (end == std::string_view::npos)
			end = rel.size();
		// 当前节点是文件但还要继续往下走：中间一级不是目录
		if (node->type != FileType::DIR)
			return Error::NOT_DIR;
		auto it = node->children.find(rel.substr(start, end - start));
		if (it == node->children.end())
			return Error::NOT_FOUND;
		node = it->second;
		start = end + 1;
	}
	out = std::move(node);
	return Error::OK;
}

Error MemoryBackend::findParent(std::string_view rel, std::shared_ptr<MemoryNode>& parent, std::string_view& name) const
{
	if (rel.empty())
		return Error::BUSY;
	Error err = findNode(Path::parentRelative(rel), parent);
	if (err != Error::OK)
		return err;
	if (parent->type != FileType::DIR)
		return Error::NOT_DIR;
	name = Path::fileName(rel);
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

	std::unique_lock<std::shared_mutex> lock(m_mutex);
	// mkdir -p 父目录
	std::shared_ptr<MemoryNode> node = m_root;
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
		node = it->second;
		start = end + 1;
	}

	std::string_view name = path.substr(start);
	auto it = node->children.find(name);
	if (it != node->children.end() && it->second->type == FileType::DIR)
		return Error::IS_DIR;

	// 覆盖时换一个新的缓冲区，已经打开旧文件的流继续读旧数据（与 unlink+create 一致）
	auto file = std::make_shared<MemoryNode>();
	file->type = FileType::FILE;
	file->buffer = std::make_shared<MemoryBuffer>();
	file->buffer->data = std::move(data);
	file->buffer->mtime.store(currentUnixTime());
	node->children[std::string(name)] = std::move(file);
	return Error::OK;
}

Error MemoryBackend::addFile(std::string_view rel, std::string_view data)
{
	return addFile(rel, std::vector<uint8_t>(data.begin(), data.end()));
}

Error MemoryBackend::stat(std::string_view rel, Stat& out) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	std::shared_ptr<MemoryNode> node;
	Error err = findNode(rel, node);
	if (err != Error::OK)
		return err;
	out.type = node->type;
	if (node->type == FileType::FILE)
	{
		std::shared_lock<std::shared_mutex> bufLock(node->buffer->mutex);
		out.size = static_cast<int64_t>(node->buffer->data.size());
		out.mtime = node->buffer->mtime.load(std::memory_order_relaxed);
	}
	else
	{
		out.size = 0;
		out.mtime = node->mtime;
	}
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

	std::shared_ptr<MemoryBuffer> buffer;
	{
		// 只读打开只需要共享锁；可能创建文件时需要独占锁
		std::unique_lock<std::shared_mutex> wlock(m_mutex, std::defer_lock);
		std::shared_lock<std::shared_mutex> rlock(m_mutex, std::defer_lock);
		if (writeMode)
			wlock.lock();
		else
			rlock.lock();

		std::shared_ptr<MemoryNode> node;
		Error e = findNode(rel, node);
		if (e == Error::OK)
		{
			if (node->type == FileType::DIR)
			{
				setError(err, Error::IS_DIR);
				return nullptr;
			}
			buffer = node->buffer;
		}
		else if (e == Error::NOT_FOUND && writeMode)
		{
			// WRITE/APPEND/OVERLAPPED：不存在则创建，父目录必须存在（与 O_CREAT 一致）
			std::shared_ptr<MemoryNode> parent;
			std::string_view name;
			e = findParent(rel, parent, name);
			if (e != Error::OK)
			{
				setError(err, e);
				return nullptr;
			}
			auto file = std::make_shared<MemoryNode>();
			file->type = FileType::FILE;
			file->buffer = std::make_shared<MemoryBuffer>();
			file->buffer->mtime.store(currentUnixTime());
			buffer = file->buffer;
			parent->children.emplace(std::string(name), std::move(file));
			parent->mtime = currentUnixTime();
		}
		else
		{
			setError(err, e);
			return nullptr;
		}
	}

	// WRITE 模式截断已有内容（修正旧实现不截断的 bug，决策 #14）
	if (mode == IFileStream::Mode::WRITE)
	{
		std::unique_lock<std::shared_mutex> bufLock(buffer->mutex);
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
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		std::shared_ptr<MemoryNode> node;
		Error err = findNode(relDir, node);
		if (err != Error::OK)
			return err;
		if (node->type != FileType::DIR)
			return Error::NOT_DIR;
		entries.reserve(node->children.size());
		for (const auto& kv : node->children)
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
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	std::shared_ptr<MemoryNode> parent;
	std::string_view name;
	Error err = findParent(rel, parent, name);
	if (err != Error::OK)
		return err;
	if (parent->children.find(name) != parent->children.end())
		return Error::EXISTS;
	auto dir = std::make_shared<MemoryNode>();
	dir->type = FileType::DIR;
	dir->mtime = currentUnixTime();
	parent->children.emplace(std::string(name), std::move(dir));
	parent->mtime = currentUnixTime();
	return Error::OK;
}

Error MemoryBackend::removeDir(std::string_view rel)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	if (rel.empty())
		return Error::BUSY;
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	std::shared_ptr<MemoryNode> parent;
	std::string_view name;
	Error err = findParent(rel, parent, name);
	if (err != Error::OK)
		return err;
	auto it = parent->children.find(name);
	if (it == parent->children.end())
		return Error::NOT_FOUND;
	if (it->second->type != FileType::DIR)
		return Error::NOT_DIR;
	if (!it->second->children.empty())
		return Error::NOT_EMPTY;
	parent->children.erase(it);
	parent->mtime = currentUnixTime();
	return Error::OK;
}

Error MemoryBackend::removeFile(std::string_view rel)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	std::shared_ptr<MemoryNode> parent;
	std::string_view name;
	Error err = findParent(rel, parent, name);
	if (err != Error::OK)
		return err == Error::BUSY ? Error::IS_DIR : err;
	auto it = parent->children.find(name);
	if (it == parent->children.end())
		return Error::NOT_FOUND;
	if (it->second->type == FileType::DIR)
		return Error::IS_DIR;
	// unlink 语义：只移除名字，缓冲区由仍然打开的流继续持有（决策 #18）
	parent->children.erase(it);
	parent->mtime = currentUnixTime();
	return Error::OK;
}

Error MemoryBackend::rename(std::string_view from, std::string_view to)
{
	if (m_readOnly.load())
		return Error::READ_ONLY;
	if (from.empty() || to.empty())
		return Error::BUSY;
	if (from == to)
		return Error::OK;
	// 不能把目录移到它自己的子树里
	if (to.size() > from.size() && to.compare(0, from.size(), from) == 0 && to[from.size()] == '/')
		return Error::INVALID_PATH;

	std::unique_lock<std::shared_mutex> lock(m_mutex);
	std::shared_ptr<MemoryNode> fromParent, toParent;
	std::string_view fromName, toName;
	Error err = findParent(from, fromParent, fromName);
	if (err != Error::OK)
		return err;
	auto fromIt = fromParent->children.find(fromName);
	if (fromIt == fromParent->children.end())
		return Error::NOT_FOUND;
	err = findParent(to, toParent, toName);
	if (err != Error::OK)
		return err;

	std::shared_ptr<MemoryNode> node = fromIt->second;
	auto toIt = toParent->children.find(toName);
	if (toIt != toParent->children.end())
	{
		// 与 POSIX rename 一致的覆盖规则
		const auto& target = toIt->second;
		if (node->type == FileType::FILE && target->type == FileType::DIR)
			return Error::IS_DIR;
		if (node->type == FileType::DIR && target->type == FileType::FILE)
			return Error::NOT_DIR;
		if (target->type == FileType::DIR && !target->children.empty())
			return Error::NOT_EMPTY;
		toParent->children.erase(toIt);
	}
	fromParent->children.erase(fromParent->children.find(fromName));
	toParent->children.emplace(std::string(toName), std::move(node));
	fromParent->mtime = currentUnixTime();
	toParent->mtime = fromParent->mtime;
	return Error::OK;
}

NS_VFS_END
