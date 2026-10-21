#include "AssetBackend.h"
#include "../Path.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <map>

#if VFS_PLATFORM_POSIX
#    include <errno.h>
#    include <unistd.h>
#endif

NS_VFS_BEGIN

namespace
{

/**
 * 资源文件流。每个流独占一个 IAssetFile（或一个描述符），不同流之间没有共享状态。
 */
class AssetFileStream : public IFileStream
{
public:
	explicit AssetFileStream(std::unique_ptr<IAssetFile> asset) : m_asset(std::move(asset))
	{
		m_size = m_asset->length();
#if VFS_PLATFORM_POSIX
		// 未压缩资源：改用描述符按偏移读取，不再需要 AAsset 的游标（也释放它占用的映射）
		int64_t start = 0, len = 0;
		int fd = m_asset->openFileDescriptor(start, len);
		if (fd >= 0)
		{
			m_fd = fd;
			m_fdStart = start;
			m_size = len;
			m_asset.reset();
		}
#endif
	}

	~AssetFileStream() override { close(); }

	bool open(std::string_view /*path*/, IFileStream::Mode /*mode*/) override
	{
		// 资源流只能由 AssetBackend 构造
		errno = ENOTSUP;
		return false;
	}

	int close() override
	{
#if VFS_PLATFORM_POSIX
		if (m_fd >= 0)
		{
			::close(m_fd);
			m_fd = -1;
		}
#endif
		m_asset.reset();
		m_open = false;
		return 0;
	}

	int64_t seek(int64_t offset, int origin) const override
	{
		if (!m_open)
		{
			errno = EBADF;
			return -1;
		}
		int64_t base = 0;
		switch (origin)
		{
		case SEEK_SET:
			base = 0;
			break;
		case SEEK_CUR:
			base = m_pos;
			break;
		case SEEK_END:
			base = m_size;
			break;
		default:
			errno = EINVAL;
			return -1;
		}
		if ((offset < 0 && (offset == INT64_MIN || base < -offset)) || (offset > 0 && base > INT64_MAX - offset))
		{
			errno = EINVAL;
			return -1;
		}
		// 只记录逻辑位置，真正读取时再定位；允许 seek 到 EOF 之后，之后 read 返回 0
		m_pos = base + offset;
		return m_pos;
	}

	int read(void* buf, unsigned int size) const override
	{
		if (!m_open)
		{
			errno = EBADF;
			return -1;
		}
		if (m_pos >= m_size || size == 0)
			return 0;
		size_t want = static_cast<size_t>(std::min<int64_t>(std::min<unsigned int>(size, VFS_MAX_IO_CHUNK), m_size - m_pos));
#if VFS_PLATFORM_POSIX
		if (m_fd >= 0)
		{
			ssize_t n;
			do
				n = ::pread(m_fd, buf, want, static_cast<off_t>(m_fdStart + m_pos));
			while (n < 0 && errno == EINTR);
			if (n < 0)
				return -1;
			m_pos += n;
			return static_cast<int>(n);
		}
#endif
		// 压缩资源：AAsset 有自己的游标，与逻辑位置不一致时先 seek
		if (m_assetPos != m_pos)
		{
			if (m_asset->seek(m_pos, SEEK_SET) != m_pos)
			{
				m_assetPos = -1;
				errno = EIO;
				return -1;
			}
			m_assetPos = m_pos;
		}
		int n = m_asset->read(buf, want);
		if (n < 0)
		{
			m_assetPos = -1;
			errno = EIO;
			return -1;
		}
		m_pos += n;
		m_assetPos = m_pos;
		return n;
	}

	int write(const void* /*buf*/, unsigned int /*size*/) const override
	{
		errno = m_open ? EROFS : EBADF;
		return -1;
	}

	bool resize(int64_t /*size*/) const override
	{
		errno = m_open ? ENOTSUP : EBADF;
		return false;
	}

	int64_t tell() const override
	{
		if (!m_open)
		{
			errno = EBADF;
			return -1;
		}
		return m_pos;
	}

	int64_t size() const override
	{
		if (!m_open)
		{
			errno = EBADF;
			return -1;
		}
		return m_size;
	}

	bool isOpen() const override { return m_open; }

private:
	// IFileStream 的读接口是 const，游标与底层对象需要 mutable
	mutable std::unique_ptr<IAssetFile> m_asset;
#if VFS_PLATFORM_POSIX
	int m_fd = -1;         // 未压缩资源的描述符（-1 表示使用 m_asset）
	int64_t m_fdStart = 0; // 资源在描述符中的起点
#endif
	int64_t m_size = 0;
	mutable int64_t m_pos = 0;
	mutable int64_t m_assetPos = 0;   // AAsset 游标位置；-1 表示未知
	bool m_open = true;
};

}   // namespace

std::shared_ptr<AssetBackend> AssetBackend::create(std::shared_ptr<IAssetProvider> provider, const AssetOptions& options,
												   Error* err)
{
	auto fail = [&](Error e) -> std::shared_ptr<AssetBackend>
	{
		if (err)
			*err = e;
		return nullptr;
	};
	if (!provider)
		return fail(Error::UNSUPPORTED);
	std::string root;
	if (Path::normalizeRelative(options.root, root) != Error::OK)
		return fail(Error::INVALID_PATH);
	auto backend = std::make_shared<AssetBackend>(std::move(provider), root);
	if (!options.manifest.empty())
	{
		Error e = backend->loadManifest(options.manifest);
		if (e != Error::OK)
			return fail(e);
	}
	if (err)
		*err = Error::OK;
	return backend;
}

AssetBackend::AssetBackend(std::shared_ptr<IAssetProvider> provider, std::string root)
	: m_provider(std::move(provider))
	, m_root(std::move(root))
{
	if (!m_root.empty())
		m_root.push_back('/');
}

AssetBackend::~AssetBackend()
{
}

uint32_t AssetBackend::capabilities() const
{
	return BackendCaps::Read;
}

std::string AssetBackend::assetPath(std::string_view rel) const
{
	if (rel.empty() && !m_root.empty())
		return m_root.substr(0, m_root.size() - 1);   // 根目录本身不带结尾的 '/'
	std::string p = m_root;
	p.append(rel);
	return p;
}

Error AssetBackend::loadManifest(std::string_view manifestPath)
{
	std::string mp;
	if (Path::normalizeRelative(manifestPath, mp) != Error::OK)
		return Error::INVALID_PATH;
	auto file = m_provider->open(assetPath(mp));
	if (!file)
		return Error::OK;   // 清单不存在：探测模式
	std::string text;
	char buf[16384];
	int n;
	while ((n = file->read(buf, sizeof(buf))) > 0)
		text.append(buf, static_cast<size_t>(n));
	if (n < 0)
		return Error::IO_ERROR;

	// 先收集所有文件，再自底向上生成目录（std::map 保证同一目录的子项按名字有序）
	std::map<std::string, std::map<std::string, FileType>> dirs;
	dirs[std::string()];
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		std::string_view line(text.data() + pos, end - pos);
		pos = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.remove_suffix(1);
		if (line.empty() || line.front() == '#')
			continue;   // 空行与注释
		std::string rel;
		if (Path::normalizeRelative(line, rel) != Error::OK || rel.empty())
			return Error::CORRUPTED;
		if (dirs.count(rel))
			return Error::CORRUPTED;   // 同一路径既是目录又是文件
		m_files.insert(rel);
		// 逐级登记父目录
		std::string_view child = rel;
		FileType type = FileType::FILE;
		while (true)
		{
			std::string_view parent = Path::parentRelative(child);
			std::string_view name = child.substr(parent.empty() ? 0 : parent.size() + 1);
			std::string parentKey(parent);
			if (m_files.count(parentKey))
				return Error::CORRUPTED;   // 父路径同时是文件
			auto& children = dirs[parentKey];
			auto it = children.find(std::string(name));
			if (it != children.end())
			{
				if (it->second != type)
					return Error::CORRUPTED;
				break;   // 父目录链已经登记过
			}
			children.emplace(std::string(name), type);
			if (parent.empty())
				break;
			child = parent;
			type = FileType::DIR;
		}
	}
	for (auto& [dir, children] : dirs)
	{
		std::vector<DirEntry> entries;
		entries.reserve(children.size());
		for (auto& [name, type] : children)
			entries.push_back(DirEntry{name, type});
		m_dirs.emplace(dir, std::move(entries));
	}
	m_hasManifest = true;
	return Error::OK;
}

bool AssetBackend::probeDir(std::string_view rel) const
{
	if (rel.empty())
		return true;
	std::vector<std::string> names;
	return m_provider->listFiles(assetPath(rel), names);
}

Error AssetBackend::classifyMissing(std::string_view rel) const
{
	// 从上往下找第一个是文件的祖先；找不到就是 NOT_FOUND
	size_t pos = 0;
	while (true)
	{
		size_t slash = rel.find('/', pos);
		if (slash == std::string_view::npos)
			return Error::NOT_FOUND;
		std::string_view prefix = rel.substr(0, slash);
		if (m_hasManifest ? m_files.count(std::string(prefix)) != 0 : m_provider->open(assetPath(prefix)) != nullptr)
			return Error::NOT_DIR;
		pos = slash + 1;
	}
}

Error AssetBackend::stat(std::string_view rel, Stat& out) const
{
	if (m_hasManifest)
	{
		std::string key(rel);
		if (m_dirs.count(key))
		{
			out.type = FileType::DIR;
			out.size = 0;
			out.mtime = 0;
			return Error::OK;
		}
		if (!m_files.count(key))
			return classifyMissing(rel);
	}
	else if (probeDir(rel))
	{
		// 探测模式下目录优先：AAssetManager_open 打开目录总是失败，二者不会冲突
		if (rel.empty() || !m_provider->open(assetPath(rel)))
		{
			out.type = FileType::DIR;
			out.size = 0;
			out.mtime = 0;
			return Error::OK;
		}
	}
	auto file = m_provider->open(assetPath(rel));
	if (!file)
		return m_hasManifest ? Error::IO_ERROR : classifyMissing(rel);   // 清单里有但打不开：APK 与清单不一致
	out.type = FileType::FILE;
	out.size = file->length();
	out.mtime = 0;   // APK 资源没有修改时间
	return Error::OK;
}

std::unique_ptr<IFileStream> AssetBackend::open(std::string_view rel, IFileStream::Mode mode, Error* err)
{
	auto fail = [&](Error e) -> std::unique_ptr<IFileStream>
	{
		if (err)
			*err = e;
		return nullptr;
	};
	if (mode != IFileStream::Mode::READ)
		return fail(Error::READ_ONLY);
	if (rel.empty())
		return fail(Error::IS_DIR);
	if (m_hasManifest && !m_files.count(std::string(rel)))
		return fail(m_dirs.count(std::string(rel)) ? Error::IS_DIR : classifyMissing(rel));
	auto file = m_provider->open(assetPath(rel));
	if (!file)
	{
		if (m_hasManifest)
			return fail(Error::IO_ERROR);
		return fail(probeDir(rel) ? Error::IS_DIR : classifyMissing(rel));
	}
	if (err)
		*err = Error::OK;
	return std::make_unique<AssetFileStream>(std::move(file));
}

Error AssetBackend::list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const
{
	if (m_hasManifest)
	{
		auto it = m_dirs.find(std::string(relDir));
		if (it == m_dirs.end())
			return m_files.count(std::string(relDir)) ? Error::NOT_DIR : classifyMissing(relDir);
		for (const auto& e : it->second)
			if (!cb(e))
				break;
		return Error::OK;
	}
	std::vector<std::string> names;
	if (!m_provider->listFiles(assetPath(relDir), names))
	{
		if (relDir.empty())
			return Error::OK;   // 根目录总是存在（可能为空，或只含子目录）
		if (m_provider->open(assetPath(relDir)))
			return Error::NOT_DIR;
		return classifyMissing(relDir);
	}
	for (const auto& name : names)
		if (!cb(DirEntry{name, FileType::FILE}))
			break;
	return Error::OK;
}

NS_VFS_END
