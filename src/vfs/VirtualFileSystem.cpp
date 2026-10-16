#include "VirtualFileSystem.h"
#include "Path.h"
#include "native/SingleFileBackend.h"
#include <algorithm>
#include <map>

NS_VFS_BEGIN

namespace
{

constexpr size_t COPY_BUFFER_SIZE = 64 * 1024;

/**
 * VFS 返回给调用方的流包装：
 * - 持有后端的 shared_ptr，保证惰性卸载后后端仍然存活（决策 #18）；
 * - 维护挂载的打开流计数，供 UnmountFlags::FailIfBusy 使用。
 * 所有调用直接转发给内部流，开销只是一次虚函数调用。
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
		// 流只能通过 VirtualFileSystem::openFileStream 获得（决策 (b)-3）
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

// 失败时设置错误并返回空流
std::unique_ptr<IFileStream> failStream(Error* err, Error e)
{
	setError(err, e);
	return nullptr;
}

} // namespace

VirtualFileSystem::VirtualFileSystem()
	: m_table(std::make_shared<MountTable>())
	, m_nextId(1)
	, m_generation(0)
{
}

VirtualFileSystem::~VirtualFileSystem()
{
}

// ================================================================ 挂载管理

std::shared_ptr<const MountTable> VirtualFileSystem::snapshot() const
{
	// 决策 #17：锁只保护指针复制，之后的查询都在这份不可变快照上进行
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_table;
}

template <typename Fn>
Error VirtualFileSystem::modifyTable(Fn&& fn)
{
	std::lock_guard<std::mutex> modifyLock(m_modifyMutex);
	auto table = std::make_shared<MountTable>(*snapshot());
	Error e = fn(*table);
	if (e != Error::OK)
		return e;
	table->sort();
	std::shared_ptr<const MountTable> old;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		old = std::move(m_table);
		m_table = std::move(table);
	}
	// old 在锁外释放：如果它持有最后一个后端引用，后端析构可能较慢
	old.reset();
	bump();
	return Error::OK;
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
	auto backend = ArchiveBackend::open(nativePath, archiveOptions, err);
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
			return Error::NOT_FOUND;
		// 严格模式：仍有打开的流时拒绝卸载（相当于 umount 默认的 EBUSY）
		if ((flags & UnmountFlags::FailIfBusy) && it->state->openStreams.load(std::memory_order_acquire) > 0)
			return Error::BUSY;
		// 惰性卸载：只从命名空间移除；已打开的流持有后端，直到最后一个流关闭才真正释放（决策 #18）
		table.mounts.erase(it);
		return Error::OK;
	});
}

Error VirtualFileSystem::remount(MountId id, std::shared_ptr<IBackend> backend)
{
	if (!backend)
		return Error::UNSUPPORTED;
	return modifyTable([&](MountTable& table)
	{
		for (auto& m : table.mounts)
		{
			if (m.id == id)
			{
				// 新后端使用新的状态计数；旧流继续持有旧后端（决策 #34）
				m.backend = backend;
				m.state = std::make_shared<MountState>();
				return Error::OK;
			}
		}
		return Error::NOT_FOUND;
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
		return Error::INVALID_PATH;
	std::string normTarget;
	Error e = Path::normalize(target, normTarget);
	if (e != Error::OK)
		return e;
	if (normTarget == "/")
		normTarget.clear();   // 映射到根目录时避免出现 "//"
	return modifyTable([&](MountTable& table)
	{
		auto& aliases = table.aliases;
		aliases.erase(std::remove_if(aliases.begin(), aliases.end(), [&](const auto& a) { return a.first == prefix; }),
					  aliases.end());
		aliases.emplace_back(std::string(prefix), normTarget);
		// 最长前缀优先
		std::stable_sort(aliases.begin(), aliases.end(),
						 [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
		return Error::OK;
	});
}

Error VirtualFileSystem::removeAlias(std::string_view prefix)
{
	return modifyTable([&](MountTable& table)
	{
		auto& aliases = table.aliases;
		auto it = std::find_if(aliases.begin(), aliases.end(), [&](const auto& a) { return a.first == prefix; });
		if (it == aliases.end())
			return Error::NOT_FOUND;
		aliases.erase(it);
		return Error::OK;
	});
}

// ================================================================ 内部工具

Error VirtualFileSystem::normalize(const MountTable& table, std::string_view raw, std::string& out) const
{
	if (table.aliases.empty())
		return Path::normalize(raw, out);
	return Path::normalize(table.applyAlias(raw), out);
}

Error VirtualFileSystem::resolve(const MountTable& table, std::string_view path, std::vector<Layer>& layers,
								 int& visible, Stat& st) const
{
	table.collectLayers(path, layers);
	visible = -1;
	for (size_t i = 0; i < layers.size(); ++i)
	{
		const Layer& layer = layers[i];
		Stat s;
		Error e = layer.mount->backend->stat(layer.rel, s);
		if (e == Error::OK)
		{
			// 最高层命中即为可见项
			visible = static_cast<int>(i);
			st = s;
			st.mount = layer.mount->id;
			st.writable = layer.mount->writable();
			return Error::OK;
		}
		// 这一层没有该路径，或者这个名字在该后端上不合法（例如 Native 拒绝 "con"）：看下一层
		if (e == Error::NOT_FOUND || e == Error::INVALID_PATH)
			continue;
		if (e == Error::NOT_DIR)
		{
			// overlay 规则（决策 #13）：只有当父路径在合并视图中不是目录时，“上层文件遮蔽下层目录”才成立；
			// 如果更高层把父路径提供为目录，那么这一层的文件已被遮蔽，相当于没有该项
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
			return Error::NOT_DIR;
		}
		return e;
	}

	// 挂载点的祖先合成为只读虚拟目录（决策 #13、#15）
	if (table.isVirtualDir(path))
	{
		st = Stat();
		st.type = FileType::DIR;
		return Error::OK;
	}
	return Error::NOT_FOUND;
}

int VirtualFileSystem::chooseWriteTarget(const std::vector<Layer>& layers)
{
	// 决策 #5：显式标记 WriteTarget 的可写层优先，其次是优先级最高的可写层
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
	// 决策 #6：父目录在合并视图中存在时，在写入层中 mkdir -p 出相对路径链（overlay 的 copy-up 目录）
	if (relParent.empty())
		return Error::OK;
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
				return Error::NOT_DIR;
		}
		else if (e == Error::NOT_FOUND)
		{
			e = backend->createDir(prefix);
			// 并发创建同一目录时可能返回 EXISTS，不算错误
			if (e != Error::OK && e != Error::EXISTS)
				return e;
		}
		else
			return e;
		pos = end + 1;
	}
	return Error::OK;
}

Error VirtualFileSystem::copyUp(const Layer& src, const Layer& dst)
{
	Error e = Error::OK;
	auto in = src.mount->backend->open(src.rel, IFileStream::Mode::READ, &e);
	if (!in)
		return e;

	// 支持 rename 的后端：先写临时文件再 rename，保证中途失败不会留下半个文件（决策 #7）
	const bool atomic = (dst.mount->backend->capabilities() & BackendCaps::Rename) != 0;
	std::string target(dst.rel);
	std::string tmp = atomic ? target + ".vfs-copyup.tmp" : target;
	auto out = dst.mount->backend->open(tmp, IFileStream::Mode::WRITE, &e);
	if (!out)
		return e;

	std::vector<uint8_t> buf(COPY_BUFFER_SIZE);
	while (true)
	{
		int n = in->read(buf.data(), static_cast<unsigned int>(buf.size()));
		if (n < 0)
		{
			e = Error::IO_ERROR;
			break;
		}
		if (n == 0)
			break;
		int done = 0;
		while (done < n)
		{
			int w = out->write(buf.data() + done, static_cast<unsigned int>(n - done));
			if (w <= 0)
			{
				e = Error::IO_ERROR;
				break;
			}
			done += w;
		}
		if (e != Error::OK)
			break;
	}
	out.reset();
	if (e != Error::OK)
	{
		dst.mount->backend->removeFile(tmp);
		return e;
	}
	if (atomic)
	{
		e = dst.mount->backend->rename(tmp, target);
		if (e != Error::OK)
			dst.mount->backend->removeFile(tmp);
	}
	return e;
}

std::unique_ptr<IFileStream> VirtualFileSystem::track(const MountEntry& mount, std::unique_ptr<IFileStream> stream) const
{
	return std::make_unique<TrackedFileStream>(std::move(stream), mount.backend, mount.state);
}

// ================================================================ 查询

Error VirtualFileSystem::stat(std::string_view rawPath, Stat& out) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return e;
	std::vector<Layer> layers;
	int visible = -1;
	return resolve(*table, path, layers, visible, out);
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
		return e;
	if (st.type != FileType::DIR)
		return Error::NOT_DIR;

	// 决策 #13：按名字去重，高层先插入所以高层决定类型；std::map 保证按名字字节序输出
	std::map<std::string, FileType, std::less<>> merged;
	for (const auto& layer : layers)
	{
		// 这一层没有该目录（或者是文件）时 list 返回错误，直接跳过
		layer.mount->backend->list(layer.rel, [&merged](const DirEntry& entry)
		{
			merged.emplace(entry.name, entry.type);
			return true;
		});
	}

	// 挂载点的祖先合成为虚拟目录：例如挂载在 /a/b，列 / 时能看到 a
	for (const auto& m : table.mounts)
	{
		if (!Path::isStrictAncestor(path, m.point))
			continue;
		std::string_view rest = std::string_view(m.point).substr(path.size() == 1 ? 1 : path.size() + 1);
		std::string_view name = rest.substr(0, rest.find('/'));
		if (merged.find(name) != merged.end())
			continue;
		// 子项类型由它自身的解析结果决定（例如单文件挂载点是文件）
		std::string child = Path::join(path, name);
		std::vector<Layer> childLayers;
		int childVisible = -1;
		Stat cs;
		if (resolve(table, child, childLayers, childVisible, cs) == Error::OK)
			merged.emplace(std::string(name), cs.type);
	}

	DirEntry entry;
	for (const auto& kv : merged)
	{
		entry.name = kv.first;
		entry.type = kv.second;
		if (!cb(entry))
			break;
	}
	return Error::OK;
}

Error VirtualFileSystem::list(std::string_view rawPath, const std::function<bool(const DirEntry&)>& cb) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return e;
	return listIn(*table, path, cb);
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
		return e;
	for (const auto& entry : entries)
	{
		std::string full = Path::join(path, entry.name);
		if (!cb(full, entry))
		{
			stop = true;
			return Error::OK;
		}
		if (entry.type == FileType::DIR)
		{
			walkIn(table, full, cb, stop);
			if (stop)
				return Error::OK;
		}
	}
	return Error::OK;
}

Error VirtualFileSystem::walk(std::string_view rawPath,
							  const std::function<bool(const std::string& fullPath, const DirEntry& entry)>& cb) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return e;
	bool stop = false;
	return walkIn(*table, path, cb, stop);
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

// ================================================================ 流

std::unique_ptr<IFileStream> VirtualFileSystem::openFileStream(std::string_view rawPath, IFileStream::Mode mode,
															   Error* err) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return failStream(err, e);

	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	e = resolve(*table, path, layers, visible, st);

	if (mode == IFileStream::Mode::READ)
	{
		// 读：取最高层命中
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
	if (e != Error::OK && e != Error::NOT_FOUND)
		return failStream(err, e);
	const bool found = e == Error::OK;

	int target = -1;
	if (found && layers[visible].mount->writable())
	{
		// 决策 #5-2：可见项所在的层可写，就写在原处
		target = visible;
	}
	else
	{
		// 决策 #5-3：否则写入覆盖该路径的最高优先级可写挂载
		target = chooseWriteTarget(layers);
		if (target < 0)
			return failStream(err, Error::READ_ONLY);
		// 决策 #5-4：写入层比持有可见项的只读层低，写了也读不到，拒绝
		if (found && target > visible)
			return failStream(err, Error::READ_ONLY);
		if (!found)
		{
			// 决策 #6：父目录必须在合并视图中存在（与 open(O_CREAT) 一致，不会凭空创建父目录）
			std::string parent(Path::parent(path));
			std::vector<Layer> parentLayers;
			int parentVisible = -1;
			Stat ps;
			Error pe = resolve(*table, parent, parentLayers, parentVisible, ps);
			if (pe != Error::OK)
				return failStream(err, pe == Error::NOT_DIR ? Error::NOT_DIR : Error::NOT_FOUND);
			if (ps.type != FileType::DIR)
				return failStream(err, Error::NOT_DIR);
		}
		const Layer& dst = layers[target];
		// 路径正好是写入层的挂载点：后端根目录是目录
		if (dst.rel.empty())
			return failStream(err, Error::IS_DIR);
		e = ensureParents(dst, Path::parentRelative(dst.rel));
		if (e != Error::OK)
			return failStream(err, e);
		// 决策 #7：APPEND/OVERLAPPED 修改只读层中的文件时，先把下层内容完整复制到写入层
		if (found && (mode == IFileStream::Mode::APPEND || mode == IFileStream::Mode::OVERLAPPED))
		{
			e = copyUp(layers[visible], dst);
			if (e != Error::OK)
				return failStream(err, e);
		}
	}

	const Layer& layer = layers[target];
	auto stream = layer.mount->backend->open(layer.rel, mode, &e);
	if (!stream)
		return failStream(err, e);
	bump();
	setError(err, Error::OK);
	return track(*layer.mount, std::move(stream));
}

// ================================================================ 修改

Error VirtualFileSystem::createDirIn(const MountTable& table, const std::string& path) const
{
	if (path == "/")
		return Error::EXISTS;
	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	Error e = resolve(table, path, layers, visible, st);
	// 决策 #10：已经是目录或文件都返回 EXISTS
	if (e == Error::OK)
		return Error::EXISTS;
	if (e != Error::NOT_FOUND)
		return e;

	std::string parent(Path::parent(path));
	std::vector<Layer> parentLayers;
	int parentVisible = -1;
	Stat ps;
	e = resolve(table, parent, parentLayers, parentVisible, ps);
	if (e != Error::OK)
		return e == Error::NOT_DIR ? Error::NOT_DIR : Error::NOT_FOUND;
	if (ps.type != FileType::DIR)
		return Error::NOT_DIR;

	int target = chooseWriteTarget(layers);
	if (target < 0)
		return Error::READ_ONLY;
	const Layer& dst = layers[target];
	if (dst.rel.empty())
		return Error::EXISTS;
	e = ensureParents(dst, Path::parentRelative(dst.rel));
	if (e != Error::OK)
		return e;
	e = dst.mount->backend->createDir(dst.rel);
	if (e == Error::OK)
		bump();
	return e;
}

Error VirtualFileSystem::createDir(std::string_view rawPath) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return e;
	return createDirIn(*table, path);
}

Error VirtualFileSystem::createDirs(std::string_view rawPath) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return e;
	if (path == "/")
		return Error::OK;

	// 逐级处理每个前缀：已是目录则跳过，不存在则创建
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
		if (e == Error::OK)
		{
			if (st.type != FileType::DIR)
				return last ? Error::EXISTS : Error::NOT_DIR;
		}
		else if (e == Error::NOT_FOUND)
		{
			e = createDirIn(*table, prefix);
			if (e != Error::OK && e != Error::EXISTS)
				return e;
		}
		else
			return e;
		pos = end + 1;
	}
	return Error::OK;
}

namespace
{

// 递归删除后端内的目录树（只在已经通过“先检查”的可写层上调用）
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
		return e;

	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	e = resolve(*table, path, layers, visible, st);
	if (e != Error::OK)
		return e;
	if (st.type != FileType::DIR)
		return Error::NOT_DIR;
	// 决策 #15：挂载点本身和它的祖先（包括根目录、合成的虚拟目录）不能删除
	if (table->isMountPointOrAncestor(path))
		return Error::BUSY;

	// 决策 #1：先检查，再执行。统计有多少层在合并视图中提供了这个目录
	int contributor = -1;
	int count = 0;
	for (size_t i = 0; i < layers.size(); ++i)
	{
		Stat s;
		if (layers[i].mount->backend->stat(layers[i].rel, s) == Error::OK && s.type == FileType::DIR)
		{
			contributor = static_cast<int>(i);
			++count;
		}
	}
	if (count == 0)
		return Error::NOT_FOUND;
	// 还有其他层（只读层或非写入层）贡献了该目录或其子孙：什么都不删，返回 EROFS
	if (count > 1 || !layers[contributor].mount->writable())
		return Error::READ_ONLY;

	const Layer& layer = layers[contributor];
	IBackend* backend = layer.mount->backend.get();
	if (!recursive)
	{
		bool empty = true;
		backend->list(layer.rel, [&empty](const DirEntry&)
		{
			empty = false;
			return false;
		});
		if (!empty)
			return Error::NOT_EMPTY;
		e = backend->removeDir(layer.rel);
	}
	else
		e = removeTree(backend, std::string(layer.rel));
	if (e == Error::OK)
		bump();
	return e;
}

Error VirtualFileSystem::removeFile(std::string_view rawPath) const
{
	auto table = snapshot();
	std::string path;
	Error e = normalize(*table, rawPath, path);
	if (e != Error::OK)
		return e;

	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	e = resolve(*table, path, layers, visible, st);
	if (e != Error::OK)
		return e;
	if (st.type == FileType::DIR || visible < 0)
		return Error::IS_DIR;
	// 单文件挂载点本身不能删除（决策 #15）
	if (table->isMountPoint(path))
		return Error::BUSY;
	// 决策 #8：只删可见项；可见项在只读层时返回 EROFS
	const Layer& layer = layers[visible];
	if (!layer.mount->writable())
		return Error::READ_ONLY;
	e = layer.mount->backend->removeFile(layer.rel);
	if (e == Error::OK)
		bump();
	return e;
}

Error VirtualFileSystem::rename(std::string_view rawFrom, std::string_view rawTo) const
{
	auto table = snapshot();
	std::string from, to;
	Error e = normalize(*table, rawFrom, from);
	if (e != Error::OK)
		return e;
	e = normalize(*table, rawTo, to);
	if (e != Error::OK)
		return e;

	std::vector<Layer> fromLayers;
	int fromVisible = -1;
	Stat fromStat;
	e = resolve(*table, from, fromLayers, fromVisible, fromStat);
	if (e != Error::OK)
		return e;
	if (from == to)
		return Error::OK;
	// 挂载点及其祖先不能改名（决策 #15）
	if (fromVisible < 0 || table->isMountPointOrAncestor(from) || table->isMountPointOrAncestor(to))
		return Error::BUSY;
	const Layer& src = fromLayers[fromVisible];
	if (!src.mount->writable())
		return Error::READ_ONLY;
	// 不能把目录移到它自己的子树里
	if (fromStat.type == FileType::DIR && Path::isStrictAncestor(from, to))
		return Error::INVALID_PATH;

	std::vector<Layer> toLayers;
	int toVisible = -1;
	Stat toStat;
	e = resolve(*table, to, toLayers, toVisible, toStat);
	if (e != Error::OK && e != Error::NOT_FOUND)
		return e;
	const bool toFound = e == Error::OK;

	int target = -1;
	if (toFound)
	{
		if (toStat.type == FileType::DIR && fromStat.type == FileType::FILE)
			return Error::IS_DIR;
		if (toStat.type == FileType::FILE && fromStat.type == FileType::DIR)
			return Error::NOT_DIR;
		if (toVisible >= 0 && toLayers[toVisible].mount->writable())
			target = toVisible;
	}
	if (target < 0)
	{
		target = chooseWriteTarget(toLayers);
		if (target < 0)
			return Error::READ_ONLY;
		if (toFound && target > toVisible)
			return Error::READ_ONLY;
		if (!toFound)
		{
			std::string parent(Path::parent(to));
			std::vector<Layer> parentLayers;
			int parentVisible = -1;
			Stat ps;
			Error pe = resolve(*table, parent, parentLayers, parentVisible, ps);
			if (pe != Error::OK)
				return pe == Error::NOT_DIR ? Error::NOT_DIR : Error::NOT_FOUND;
			if (ps.type != FileType::DIR)
				return Error::NOT_DIR;
		}
	}

	const Layer& dst = toLayers[target];
	// 决策 #9：只有源和目标落在同一个挂载上才能原子 rename，否则 EXDEV
	if (dst.mount->id != src.mount->id)
		return Error::CROSS_DEVICE;
	if (!(src.mount->backend->capabilities() & BackendCaps::Rename))
		return Error::UNSUPPORTED;
	e = ensureParents(dst, Path::parentRelative(dst.rel));
	if (e != Error::OK)
		return e;
	e = src.mount->backend->rename(src.rel, dst.rel);
	if (e == Error::OK)
		bump();
	return e;
}

Error VirtualFileSystem::moveFile(std::string_view from, std::string_view to) const
{
	Error e = rename(from, to);
	if (e != Error::CROSS_DEVICE && e != Error::UNSUPPORTED)
		return e;
	// 跨挂载：退化为复制 + 删除，只支持文件，不保证原子性
	if (!isFile(from))
		return e;
	e = copyFile(from, to);
	if (e != Error::OK)
		return e;
	return removeFile(from);
}

Error VirtualFileSystem::copyFile(std::string_view from, std::string_view to) const
{
	Error e = Error::OK;
	auto in = openFileStream(from, IFileStream::Mode::READ, &e);
	if (!in)
		return e;
	auto out = openFileStream(to, IFileStream::Mode::WRITE, &e);
	if (!out)
		return e;
	std::vector<uint8_t> buf(COPY_BUFFER_SIZE);
	while (true)
	{
		int n = in->read(buf.data(), static_cast<unsigned int>(buf.size()));
		if (n < 0)
			return Error::IO_ERROR;
		if (n == 0)
			break;
		int done = 0;
		while (done < n)
		{
			int w = out->write(buf.data() + done, static_cast<unsigned int>(n - done));
			if (w <= 0)
				return Error::IO_ERROR;
			done += w;
		}
	}
	return Error::OK;
}

// ================================================================ 便利函数

Error VirtualFileSystem::readFile(std::string_view path, std::vector<uint8_t>& out) const
{
	Error e = Error::OK;
	auto stream = openFileStream(path, IFileStream::Mode::READ, &e);
	if (!stream)
		return e;
	int64_t size = stream->size();
	out.clear();
	if (size > 0)
		out.reserve(static_cast<size_t>(size));
	// 单次读写上限约 2 GB（决策 #4），这里分块循环读到 EOF
	std::vector<uint8_t> buf(COPY_BUFFER_SIZE);
	while (true)
	{
		int n = stream->read(buf.data(), static_cast<unsigned int>(buf.size()));
		if (n < 0)
			return Error::IO_ERROR;
		if (n == 0)
			break;
		out.insert(out.end(), buf.begin(), buf.begin() + n);
	}
	return Error::OK;
}

Error VirtualFileSystem::readFile(std::string_view path, std::string& out) const
{
	std::vector<uint8_t> data;
	Error e = readFile(path, data);
	if (e != Error::OK)
		return e;
	out.assign(reinterpret_cast<const char*>(data.data()), data.size());
	return Error::OK;
}

Error VirtualFileSystem::writeFile(std::string_view path, const void* data, size_t size) const
{
	Error e = Error::OK;
	auto stream = openFileStream(path, IFileStream::Mode::WRITE, &e);
	if (!stream)
		return e;
	const uint8_t* p = static_cast<const uint8_t*>(data);
	while (size > 0)
	{
		unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(size, VFS_MAX_IO_CHUNK));
		int n = stream->write(p, chunk);
		if (n <= 0)
			return Error::IO_ERROR;
		p += n;
		size -= static_cast<size_t>(n);
	}
	return stream->close() == 0 ? Error::OK : Error::IO_ERROR;
}

Error VirtualFileSystem::writeFile(std::string_view path, std::string_view data) const
{
	return writeFile(path, data.data(), data.size());
}

Error VirtualFileSystem::writeFileAtomic(std::string_view path, const void* data, size_t size) const
{
	std::string tmp = std::string(path) + ".tmp";
	Error e = writeFile(tmp, data, size);
	if (e != Error::OK)
		return e;
	e = rename(tmp, path);
	if (e == Error::CROSS_DEVICE || e == Error::UNSUPPORTED)
	{
		// 临时文件和目标落在不同的层：退化为直接写入
		removeFile(tmp);
		return writeFile(path, data, size);
	}
	if (e != Error::OK)
		removeFile(tmp);
	return e;
}

NS_VFS_END
