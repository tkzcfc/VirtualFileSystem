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

// 所有返回 Error 的接口都经过这里返回：失败时同时设置 errno = toErrno(e)（见 api.md）
inline Error report(Error e)
{
	if (e != Error::OK)
		errno = toErrno(e);
	return e;
}

// 进程内唯一的临时文件名：并发的 copy-up / writeFileAtomic 针对同一路径时不会共用同一个临时文件
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
 * VFS 返回给调用方的流包装：
 * - 持有后端的 shared_ptr，保证惰性卸载后后端仍然存活；
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
		// 流只能通过 VirtualFileSystem::openFileStream 获得
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

constexpr size_t SNAPSHOT_SHARDS = 16;

// 当前线程使用的快照分片：线程第一次使用时轮流分配，之后固定
size_t threadShard()
{
	static std::atomic<size_t> s_next{0};
	thread_local const size_t t_shard = s_next.fetch_add(1, std::memory_order_relaxed) % SNAPSHOT_SHARDS;
	return t_shard;
}

// 为一个分片生成快照引用：外层 shared_ptr 有自己的控制块（指向同一张表），
// 不同分片复制引用时修改的是不同的计数，不会在线程之间争抢同一个缓存行
std::shared_ptr<const MountTable> shardReference(const std::shared_ptr<const MountTable>& table)
{
	auto holder = std::make_shared<std::shared_ptr<const MountTable>>(table);
	const MountTable* raw = holder->get();
	return std::shared_ptr<const MountTable>(std::move(holder), raw);
}

} // namespace

struct VirtualFileSystem::SnapshotShard
{
	std::mutex mutex;                           // 只保护 table 的复制与替换，几乎不会有竞争
	std::shared_ptr<const MountTable> table;
	char padding[64];                           // 隔开相邻分片，避免伪共享
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

// ================================================================ 挂载管理

std::shared_ptr<const MountTable> VirtualFileSystem::snapshot() const
{
	// 锁只保护本分片的指针复制，之后的查询都在这份不可变快照上进行
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
	// 旧引用在分片锁外释放：如果它持有最后一个后端引用，后端析构可能较慢
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
		// 严格模式：仍有打开的流时拒绝卸载（相当于 umount 默认的 EBUSY）
		// 先标记 closing 再读计数；打开流的一方先增加计数再读 closing（都是 seq_cst）。
		// 两者至少有一方能看到对方：要么这里看到计数 > 0 返回 BUSY，要么打开方看到 closing 放弃这次打开并重试
		if (flags & UnmountFlags::FailIfBusy)
		{
			it->state->closing.store(true);
			if (it->state->openStreams.load() > 0)
			{
				it->state->closing.store(false);
				return report(Error::BUSY);
			}
		}
		// 惰性卸载：只从命名空间移除；已打开的流持有后端，直到最后一个流关闭才真正释放
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
				// 旧流继续持有旧后端；打开流计数沿用同一个 MountState，
				// 所以 remount 之后 FailIfBusy 卸载仍然能看到 remount 之前打开、尚未关闭的流
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

// ================================================================ 内部工具

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
	bool invalid = false;   // 有层认为这个名字不合法（例如本机后端拒绝 "con"）
	for (size_t i = 0; i < layers.size(); ++i)
	{
		const Layer& layer = layers[i];
		Stat s;
		Error e = layer.mount->backend->stat(layer.rel, s);
		if (e == Error::OK)
		{
			// 挂载点的祖先必须是目录：底层在这个位置上的文件被挂载路径遮蔽，否则挂载点会变得不可达
			if (s.type != FileType::DIR && table.isVirtualDir(path))
				continue;
			// 最高层命中即为可见项
			visible = static_cast<int>(i);
			st = s;
			st.mount = layer.mount->id;
			st.writable = layer.mount->writable();
			return report(Error::OK);
		}
		// 这一层没有该路径，或者这个名字在该后端上不合法（例如 Native 拒绝 "con"）：看下一层
		if (e == Error::NOT_FOUND)
			continue;
		if (e == Error::INVALID_PATH)
		{
			invalid = true;
			continue;
		}
		if (e == Error::NOT_DIR)
		{
			// overlay 规则：只有当父路径在合并视图中不是目录时，“上层文件遮蔽下层目录”才成立；
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
			return report(Error::NOT_DIR);
		}
		return report(e);
	}

	// 挂载点的祖先合成为只读虚拟目录
	if (table.isVirtualDir(path))
	{
		st = Stat();
		st.type = FileType::DIR;
		return report(Error::OK);
	}
	// 没有任何层提供它：如果有层拒绝了这个名字，报告 INVALID_PATH（与写入时后端返回的错误一致，见 mount-rules.md §1）
	return report(invalid ? Error::INVALID_PATH : Error::NOT_FOUND);
}

int VirtualFileSystem::chooseWriteTarget(const std::vector<Layer>& layers)
{
	// 显式标记 WriteTarget 的可写层优先，其次是优先级最高的可写层
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
	// 父目录在合并视图中存在时，在写入层中 mkdir -p 出相对路径链（overlay 的 copy-up 目录）
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
			// 并发创建同一目录时可能返回 EXISTS，不算错误；但并发出现的也可能是同名文件，要确认类型
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

	// 支持 rename 的后端：先写临时文件再 rename，保证中途失败不会留下半个文件
	const bool atomic = (dst.mount->backend->capabilities() & BackendCaps::Rename) != 0;
	std::string target(dst.rel);
	std::string tmp = atomic ? uniqueTempName(target, "vfs-copyup") : target;
	std::unique_ptr<IFileStream> out;
	try
	{
		std::vector<uint8_t> buf(COPY_BUFFER_SIZE);   // 在创建临时文件之前分配：分配失败不会留下临时文件
		out = dst.mount->backend->open(tmp, IFileStream::Mode::WRITE, &e);
		if (!out)
			return report(e);
		e = copyStream(*in, *out);
		if (e == Error::OK && out->close() != 0)
			e = Error::IO_ERROR;
	}
	catch (const std::bad_alloc&)
	{
		// 错误通过返回值报告（Error.h），不让异常穿过 VFS 接口
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
// openOnce 中 track 发现挂载正在被 FailIfBusy 卸载时置位，openFileStream 据此重试
thread_local bool t_openRetry = false;
}

std::unique_ptr<IFileStream> VirtualFileSystem::track(const MountEntry& mount, std::unique_ptr<IFileStream> stream) const
{
	auto tracked = std::make_unique<TrackedFileStream>(std::move(stream), mount.backend, mount.state);
	// 构造函数已经增加计数，这里再看 closing（见 unmount）：正在严格卸载时放弃这个流
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
	// 与 FailIfBusy 卸载竞争时重试：卸载要么完成（重试时挂载已经不在快照里），要么因为计数 > 0 撤销标记
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

// ================================================================ 查询

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

	// 按名字去重，高层先插入所以高层决定类型；std::map 保证按名字字节序输出
	std::map<std::string, FileType, std::less<>> merged;
	for (const auto& layer : layers)
	{
		// 这一层没有该目录（或者是文件）时跳过；其他错误（I/O 错误、权限）原样返回，不能当作空目录
		e = layer.mount->backend->list(layer.rel, [&merged](const DirEntry& entry)
		{
			merged.emplace(entry.name, entry.type);
			return true;
		});
		if (e != Error::OK && e != Error::NOT_FOUND && e != Error::NOT_DIR && e != Error::INVALID_PATH)
			return report(e);
	}

	// 挂载点的祖先合成为虚拟目录：例如挂载在 /a/b，列 / 时能看到 a
	for (const auto& m : table.mounts)
	{
		if (!Path::isStrictAncestor(path, m.point))
			continue;
		std::string_view rest = std::string_view(m.point).substr(path.size() == 1 ? 1 : path.size() + 1);
		std::string_view name = rest.substr(0, rest.find('/'));
		// 子项类型由它自身的解析结果决定（例如单文件挂载点是文件；挂载点的祖先即使在底层是文件也显示为目录）
		std::string child = Path::join(path, name);
		std::vector<Layer> childLayers;
		int childVisible = -1;
		Stat cs;
		Error ce = resolve(table, child, childLayers, childVisible, cs);
		if (ce == Error::OK)
			merged[std::string(name)] = cs.type;
		else if (ce != Error::NOT_FOUND && ce != Error::NOT_DIR && ce != Error::INVALID_PATH)
			return report(ce);   // 与层 list 一样：I/O 错误、权限错误让整次 list 失败
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
			// 子目录在列出之后被并发删除或替换成文件（NOT_FOUND / NOT_DIR）不算错误；
			// 其他错误（I/O、权限）让整次 walk 失败，与 list 一致，避免调用方把不完整的遍历当成成功
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

// ================================================================ 流

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
		// 快速路径：路径只落在一个挂载上（最常见的情况）时直接 open，省掉一次 stat（本机后端上是一次系统调用）。
		// 成功说明它是该层的文件，就是可见项；IS_DIR 与慢速路径的结果相同；其他错误交给慢速路径，保证错误码完全一致
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
	// 只是某些层不接受这个名字：写入层可能接受（由写入层的后端决定）
	if (e == Error::INVALID_PATH)
		e = Error::NOT_FOUND;
	if (e != Error::OK && e != Error::NOT_FOUND)
		return failStream(err, e);
	const bool found = e == Error::OK;

	int target = -1;
	if (found && layers[visible].mount->writable())
	{
		// 可见项所在的层可写，就写在原处
		target = visible;
		const Layer& layer = layers[target];
		auto stream = layer.mount->backend->open(layer.rel, mode, &e);
		if (!stream)
			return failStream(err, e);
		bump();
		setError(err, Error::OK);
		return track(*layer.mount, std::move(stream));
	}

	// 否则写入覆盖该路径的最高优先级可写挂载
	target = chooseWriteTarget(layers);
	if (target < 0)
		return failStream(err, Error::READ_ONLY);
	// 写入层比持有可见项的只读层低，写了也读不到，拒绝
	if (found && target > visible)
		return failStream(err, Error::READ_ONLY);
	if (!found)
	{
		// 父目录必须在合并视图中存在（与 open(O_CREAT) 一致，不会凭空创建父目录）
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
		// 路径正好是写入层的挂载点：目录挂载的根是目录；单文件挂载的根就是那个文件，照常打开
		Stat rs;
		if (dst.mount->backend->stat("", rs) != Error::OK || rs.type == FileType::DIR)
			return failStream(err, Error::IS_DIR);
	}

	// 写入层里的 copy-up、创建与打开在同一把（按路径分段的）锁内完成，直到流打开为止：
	// - 多个线程同时追加同一个只读层文件时，只有第一个复制，其余线程在锁内看到写入层已有副本后直接打开它；
	// - copy-up 的 rename 不会覆盖另一个线程刚在写入层创建（WRITE）的同名文件；
	// - 复制完成之后、打开之前，副本不会被另一次 copy-up 替换
	std::lock_guard<std::recursive_mutex> lock(copyUpLock(*dst.mount, dst.rel));
	e = ensureParents(dst, Path::parentRelative(dst.rel));
	if (e != Error::OK)
	{
		bump();   // 可能已经创建了一部分父目录
		return failStream(err, e);
	}
	// APPEND/OVERLAPPED 修改只读层中的文件时，先把下层内容完整复制到写入层
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
	// 即使打开失败，父目录或 copy-up 的副本也可能已经出现在写入层里：命名空间变了
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

// ================================================================ 修改

Error VirtualFileSystem::createDirIn(const MountTable& table, const std::string& path) const
{
	if (path == "/")
		return report(Error::EXISTS);
	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	Error e = resolve(table, path, layers, visible, st);
	// 已经是目录或文件都返回 EXISTS
	if (e == Error::OK)
		return report(Error::EXISTS);
	// 只是某些层不接受这个名字：写入层可能接受（由写入层的后端决定）
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
		return report(e);   // NOT_FOUND / NOT_DIR / 其他错误（IO_ERROR 等）原样返回
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

	// 逐级处理每个前缀：已是目录则跳过，不存在则创建。
	// 并发：其他线程可能在两步之间删掉刚创建（或刚检查过）的上级目录，此时 createDirIn 返回 NOT_FOUND，
	// 从头重来；有上限，持续被删除时最终返回 NOT_FOUND（mkdir -p 在同样情况下也会失败）
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
					// 并发创建了同名项：确认它是目录（也可能是文件，或已经又被删掉）
					e = resolve(*table, prefix, layers, visible, st);
				}
				else if (e == Error::NOT_FOUND && pos > 1)
				{
					restart = true;   // 上级目录在检查之后消失了
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
		return report(e);

	std::vector<Layer> layers;
	int visible = -1;
	Stat st;
	e = resolve(*table, path, layers, visible, st);
	if (e != Error::OK)
		return report(e);
	if (st.type != FileType::DIR)
		return report(Error::NOT_DIR);
	// 挂载点本身和它的祖先（包括根目录、合成的虚拟目录）不能删除
	if (table->isMountPointOrAncestor(path))
		return report(Error::BUSY);

	// 先检查，再执行。统计有多少层在合并视图中提供了这个目录
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
			return report(le);   // 无法确定这一层有没有该目录（I/O、权限错误）：什么都不删
	}
	if (count == 0)
		return report(Error::NOT_FOUND);
	// 还有其他层（只读层或非写入层）贡献了该目录或其子孙：什么都不删，返回 EROFS
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
		// list 失败时不能当作空目录
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
		// 递归删除失败时可能已经删掉了一部分，命名空间同样变了
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
	// 单文件挂载点本身不能删除
	if (table->isMountPoint(path))
		return report(Error::BUSY);
	// 只删可见项；可见项在只读层时返回 EROFS
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
		// 无法确定这一层有没有该目录：不冒险
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
	// 挂载点及其祖先不能改名
	if (fromVisible < 0 || table->isMountPointOrAncestor(from) || table->isMountPointOrAncestor(to))
		return report(Error::BUSY);
	const Layer& src = fromLayers[fromVisible];
	if (!src.mount->writable())
		return report(Error::READ_ONLY);
	// 目录由多个层合并而成时，只能移动写入层中的那一部分，其余层的内容会留在原处；
	// 与 removeDir 的规则一致，直接拒绝
	if (fromStat.type == FileType::DIR)
	{
		e = checkSingleLayerDir(fromLayers, fromVisible);
		if (e != Error::OK)
			return report(e);
	}
	// 不能把目录移到它自己的子树里
	if (fromStat.type == FileType::DIR && Path::isStrictAncestor(from, to))
		return report(Error::INVALID_PATH);

	std::vector<Layer> toLayers;
	int toVisible = -1;
	Stat toStat;
	e = resolve(*table, to, toLayers, toVisible, toStat);
	// INVALID_PATH：只是某些层不接受这个名字，写入层可能接受（由写入层的后端决定）
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
		// 目录覆盖已有目录：写入层里的目标是否为空由后端原子地判断（POSIX rename）；
		// 这里检查其他层：其他层里同名目录下有内容时，改名“成功”后那些内容会出现在移过来的目录里，
		// 合并视图中的目标并不是空目录，按 rename(2) 返回 NOT_EMPTY
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
	// 只有源和目标落在同一个挂载上才能原子 rename，否则 EXDEV
	if (dst.mount->id != src.mount->id)
		return report(Error::CROSS_DEVICE);
	if (!(src.mount->backend->capabilities() & BackendCaps::Rename))
		return report(Error::UNSUPPORTED);
	e = ensureParents(dst, Path::parentRelative(dst.rel));
	if (e != Error::OK)
	{
		bump();   // 可能已经创建了一部分父目录
		return report(e);
	}
	e = src.mount->backend->rename(src.rel, dst.rel);
	bump();   // 成功，或者失败但 ensureParents 已经在写入层创建了父目录
	return report(e);
}

Error VirtualFileSystem::moveFile(std::string_view from, std::string_view to) const
{
	Error e = rename(from, to);
	if (e != Error::CROSS_DEVICE && e != Error::UNSUPPORTED)
		return report(e);
	// 跨挂载：退化为复制 + 删除，只支持文件，不保证原子性
	Stat st;
	Error se = stat(from, st);
	if (se != Error::OK)
		return report(se);   // 源在 rename 之后被并发删除：报告 NOT_FOUND，而不是 CROSS_DEVICE
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
		// 复制到自身：可见项可写时什么都不做（否则以 WRITE 打开目标会先截断源文件）；
		// 可见项在只读层时照常复制，相当于显式 copy-up
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

	// 先复制到目标旁边的临时文件，再 rename 成目标：中途失败不会截断或遮住原来的目标
	// （与 copy-up、writeFileAtomic 一致）。临时文件写不了（例如单文件挂载）或与目标不在同一层时退化为直接写
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
		// 临时文件和目标落在不同的层：退化为直接复制
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
	std::vector<uint8_t> buf(COPY_BUFFER_SIZE);
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

// ================================================================ 便利函数

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
	// 单次读写上限约 2 GB，这里分块循环读到 EOF
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
	// 先规范化（例如去掉结尾的 '/'），临时文件就在目标旁边：path.vfs-N.tmp。
	// 每次调用使用不同的临时文件：并发写同一路径时互不干扰
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
	Error e = writeFile(tmp, data, size);
	if (e == Error::READ_ONLY)
	{
		// 临时文件所在位置不可写（例如目标是单文件挂载，旁边没有可写层）：直接写目标，不再原子
		removeFile(tmp);
		return report(writeFile(norm, data, size));
	}
	if (e != Error::OK)
	{
		// 写到一半失败也不留下临时文件
		removeFile(tmp);
		return report(e);
	}
	e = rename(tmp, norm);
	if (e == Error::CROSS_DEVICE || e == Error::UNSUPPORTED)
	{
		// 临时文件和目标落在不同的层：退化为直接写入（不再原子）
		removeFile(tmp);
		return report(writeFile(norm, data, size));
	}
	if (e != Error::OK)
		removeFile(tmp);
	return report(e);
}

NS_VFS_END
