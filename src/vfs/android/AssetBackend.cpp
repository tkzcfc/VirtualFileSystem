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
 * Asset file stream. Each stream owns one IAssetFile (or one descriptor); streams share no state.
 */
class AssetFileStream : public IFileStream
{
public:
	explicit AssetFileStream(std::unique_ptr<IAssetFile> asset) : m_asset(std::move(asset))
	{
		m_size = m_asset->length();
#if VFS_PLATFORM_POSIX
		// Uncompressed asset: switch to descriptor reads by offset, so the AAsset cursor is no longer needed (and the mapping it holds is released)
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
		// Asset streams can only be constructed by AssetBackend
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
		// Record only the logical position and seek on the actual read; seeking past EOF is allowed, and a later read returns 0
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
		// Compressed asset: AAsset has its own cursor; seek first when it disagrees with the logical position
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
	// IFileStream read methods are const, so the cursor and the underlying object must be mutable
	mutable std::unique_ptr<IAssetFile> m_asset;
#if VFS_PLATFORM_POSIX
	int m_fd = -1;         // Descriptor of an uncompressed asset (-1 means use m_asset)
	int64_t m_fdStart = 0; // Start of the asset within the descriptor
#endif
	int64_t m_size = 0;
	mutable int64_t m_pos = 0;
	mutable int64_t m_assetPos = 0;   // AAsset cursor position; -1 means unknown
	bool m_open = true;
};

/**
 * Archive stored as an asset. When a descriptor is available, reads are positional (pread is thread-safe by itself); otherwise each openStream opens a new asset stream through the backend
 */
class AssetArchiveSource : public IArchiveSource
{
public:
	AssetArchiveSource(std::shared_ptr<AssetBackend> backend, std::string rel, std::string describe)
		: m_backend(std::move(backend)), m_rel(std::move(rel)), m_describe(std::move(describe))
	{
	}

	~AssetArchiveSource() override
	{
#if VFS_PLATFORM_POSIX
		if (m_fd >= 0)
			::close(m_fd);
#endif
	}

	// Called once after construction, before the object is published to other threads
	void attachDescriptor([[maybe_unused]] IAssetFile& file)
	{
#if VFS_PLATFORM_POSIX
		int64_t start = 0, len = 0;
		int fd = file.openFileDescriptor(start, len);
		if (fd >= 0)
		{
			m_fd = fd;
			m_start = start;
			m_length = len;
		}
#endif
	}

	std::unique_ptr<IFileStream> openStream(Error* err) const override { return m_backend->open(m_rel, IFileStream::Mode::READ, err); }

#if VFS_PLATFORM_POSIX
	bool supportsReadAt() const override { return m_fd >= 0; }

	int64_t readAt(void* buf, size_t size, uint64_t offset) const override
	{
		if (m_fd < 0)
			return IArchiveSource::readAt(buf, size, offset);
		if (offset >= static_cast<uint64_t>(m_length))
			return 0;
		size = static_cast<size_t>(std::min<uint64_t>(size, static_cast<uint64_t>(m_length) - offset));
		ssize_t n;
		do
			n = ::pread(m_fd, buf, size, static_cast<off_t>(m_start + static_cast<int64_t>(offset)));
		while (n < 0 && errno == EINTR);
		return n;
	}
#endif

	std::string describe() const override { return m_describe; }

private:
	std::shared_ptr<AssetBackend> m_backend;   // Keeps the backend (and its asset source) alive
	std::string m_rel;
	std::string m_describe;
#if VFS_PLATFORM_POSIX
	int m_fd = -1;
	int64_t m_start = 0;
	int64_t m_length = 0;
#endif
};

}   // namespace

std::shared_ptr<IArchiveSource> AssetBackend::archiveSource(std::string_view rel, Error* err)
{
	std::string key;
	if (Path::normalizeRelative(rel, key) != Error::OK)
	{
		if (err)
			*err = Error::INVALID_PATH;
		return nullptr;
	}
	Error e = Error::OK;
	if (!open(key, IFileStream::Mode::READ, &e))   // Shared checks: missing, a directory, or outside the manifest
	{
		if (err)
			*err = e;
		return nullptr;
	}
	auto file = m_provider->open(assetPath(key));
	if (!file)
	{
		if (err)
			*err = Error::IO_ERROR;
		return nullptr;
	}
	auto self = weak_from_this().lock();
	if (!self)
	{
		if (err)
			*err = Error::UNSUPPORTED;
		return nullptr;
	}
	auto source = std::make_shared<AssetArchiveSource>(std::move(self), key, "asset:" + assetPath(key));
	source->attachDescriptor(*file);
	if (err)
		*err = Error::OK;
	return source;
}

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
		return m_root.substr(0, m_root.size() - 1);   // The root directory itself has no trailing '/'
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
		return Error::OK;   // No manifest: probe mode
	std::string text;
	char buf[16384];
	int n;
	while ((n = file->read(buf, sizeof(buf))) > 0)
		text.append(buf, static_cast<size_t>(n));
	if (n < 0)
		return Error::IO_ERROR;

	// Collect every file first, then synthesize directories bottom-up (std::map keeps children of one directory ordered by name)
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
			continue;   // Blank lines and comments
		std::string rel;
		if (Path::normalizeRelative(line, rel) != Error::OK || rel.empty())
			return Error::CORRUPTED;
		if (dirs.count(rel))
			return Error::CORRUPTED;   // The same path is both a directory and a file
		m_files.insert(rel);
		// Register parent directories level by level
		std::string_view child = rel;
		FileType type = FileType::FILE;
		while (true)
		{
			std::string_view parent = Path::parentRelative(child);
			std::string_view name = child.substr(parent.empty() ? 0 : parent.size() + 1);
			std::string parentKey(parent);
			if (m_files.count(parentKey))
				return Error::CORRUPTED;   // A parent path is also a file
			auto& children = dirs[parentKey];
			auto it = children.find(std::string(name));
			if (it != children.end())
			{
				if (it->second != type)
					return Error::CORRUPTED;
				break;   // This parent chain is already registered
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
	// Walk top-down for the first ancestor that is a file; if none, the result is NOT_FOUND
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
		// In probe mode directories win: AAssetManager_open always fails on a directory, so the two cannot conflict
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
		return m_hasManifest ? Error::IO_ERROR : classifyMissing(rel);   // Listed in the manifest but cannot be opened: APK and manifest disagree
	out.type = FileType::FILE;
	out.size = file->length();
	out.mtime = 0;   // APK assets have no modification time
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
			return Error::OK;   // The root always exists (it may be empty, or contain only subdirectories)
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
