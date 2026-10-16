#include "NativeBackend.h"
#include "NativeFileStream.h"
#include "NativeUtils.h"
#include "../Path.h"
#include "../Log.h"

namespace stdfs = std::filesystem;

NS_VFS_BEGIN

std::shared_ptr<NativeBackend> NativeBackend::create(std::string_view rootDir, const NativeOptions& options, Error* err)
{
	std::error_code ec;
	stdfs::path p = native::toFsPath(rootDir.empty() ? std::string_view(".") : rootDir);
	p = stdfs::absolute(p, ec);
	if (ec)
	{
		setError(err, native::errorFromCode(ec));
		return nullptr;
	}
	p = p.lexically_normal();

	std::u8string u8 = p.generic_u8string();
	std::string root(reinterpret_cast<const char*>(u8.data()), u8.size());
	// 去掉结尾的 '/'（但保留 "/" 和 "C:/" 这种根）
	while (root.size() > 1 && root.back() == '/' && !(root.size() == 3 && root[1] == ':'))
		root.pop_back();

	Stat st;
	Error e = native::statPath(root, st);
	if (e == Error::NOT_FOUND && options.createRoot && !options.readOnly)
	{
		stdfs::create_directories(native::toFsPath(root), ec);
		if (ec)
		{
			setError(err, native::errorFromCode(ec));
			return nullptr;
		}
		e = native::statPath(root, st);
	}
	if (e != Error::OK)
	{
		setError(err, e);
		return nullptr;
	}
	if (st.type != FileType::DIR)
	{
		setError(err, Error::NOT_DIR);
		return nullptr;
	}
	setError(err, Error::OK);
	return std::make_shared<NativeBackend>(std::move(root), options);
}

NativeBackend::NativeBackend(std::string root, const NativeOptions& options)
	: m_root(std::move(root))
	, m_options(options)
{
	if (!m_options.followSymlinks)
	{
		// 规范根目录本身允许是 symlink，比较的是解析后的真实位置
		std::error_code ec;
		stdfs::path canon = stdfs::weakly_canonical(native::toFsPath(m_root), ec);
		m_canonicalRoot = ec ? m_root : native::fromFsPath(canon.generic_u8string());
		while (m_canonicalRoot.size() > 1 && m_canonicalRoot.back() == '/')
			m_canonicalRoot.pop_back();
	}
}

NativeBackend::~NativeBackend()
{
}

uint32_t NativeBackend::capabilities() const
{
	uint32_t caps = BackendCaps::Read | BackendCaps::Resize;
	if (!m_options.readOnly)
		caps |= BackendCaps::Write | BackendCaps::Rename;
	return caps;
}

Error NativeBackend::toNative(std::string_view rel, std::string& out) const
{
	// 逐段检查：后端收到的路径已经规范化，这里只额外拒绝不可移植的名字
	size_t start = 0;
	while (start < rel.size())
	{
		size_t end = rel.find('/', start);
		if (end == std::string_view::npos)
			end = rel.size();
		if (!Path::isPortableComponent(rel.substr(start, end - start)))
			return Error::INVALID_PATH;
		start = end + 1;
	}

	out.clear();
	out.reserve(m_root.size() + rel.size() + 1);
	out.append(m_root);
	if (!rel.empty())
	{
		if (out.back() != '/')
			out.push_back('/');
		out.append(rel);
	}

	if (!m_options.followSymlinks && !rel.empty())
	{
		// 解析 symlink 后必须仍在根目录之内
		std::error_code ec;
		stdfs::path canon = stdfs::weakly_canonical(native::toFsPath(out), ec);
		if (ec)
			return Error::OK;
		std::string c = native::fromFsPath(canon.generic_u8string());
		std::string_view relOut;
		if (!Path::isUnder(c, m_canonicalRoot, relOut))
			return Error::PERMISSION_DENIED;
	}
	return Error::OK;
}

Error NativeBackend::matchCase(std::string_view rel, bool& lastMismatch) const
{
	lastMismatch = false;
	std::string dir = m_root;
	if (!dir.empty() && dir.back() == '/')
		dir.pop_back();   // 根是 "/" 或 "C:/" 时 realName 会再补一个 '/'
	std::string actual;
	size_t start = 0;
	while (start < rel.size())
	{
		size_t end = rel.find('/', start);
		if (end == std::string_view::npos)
			end = rel.size();
		std::string_view seg = rel.substr(start, end - start);
		// 不存在或出错时不在这里判断，交给后续的实际操作报告
		if (native::realName(dir, seg, actual) != Error::OK)
			return Error::OK;
		if (actual != seg)
		{
			if (end == rel.size())
			{
				lastMismatch = true;
				return Error::OK;
			}
			return Error::NOT_FOUND;
		}
		dir.push_back('/');
		dir.append(seg);
		start = end + 1;
	}
	return Error::OK;
}

void NativeBackend::checkCase(std::string_view rel) const
{
	// 在大小写不敏感的文件系统上（Windows/macOS），"A.png" 能打开 "a.png"，发布到归档后就找不到了
	bool last = false;
	if (matchCase(rel, last) != Error::OK || last)
		log(LogLevel::WARN, "vfs: 路径大小写与磁盘上的真实名字不一致: " + std::string(rel));
}

Error NativeBackend::stat(std::string_view rel, Stat& out) const
{
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	err = native::statPath(full, out);
	if (err == Error::OK && !rel.empty())
	{
		if (m_options.exactCase)
		{
			// 按字节匹配：任何一级只差大小写都视为不存在
			bool last = false;
			if (matchCase(rel, last) != Error::OK || last)
				return Error::NOT_FOUND;
		}
		else if (m_options.strictCase)
			checkCase(rel);
	}
	return err;
}

std::unique_ptr<IFileStream> NativeBackend::open(std::string_view rel, IFileStream::Mode mode, Error* err)
{
	if (m_options.readOnly && mode != IFileStream::Mode::READ)
	{
		setError(err, Error::READ_ONLY);
		return nullptr;
	}
	std::string full;
	Error e = toNative(rel, full);
	if (e == Error::OK && m_options.exactCase)
	{
		// 读：只差大小写视为不存在；写：会落到那个只差大小写的已有文件上，返回 EXISTS 而不是悄悄覆盖它
		bool last = false;
		e = matchCase(rel, last);
		if (e == Error::OK && last)
			e = mode == IFileStream::Mode::READ ? Error::NOT_FOUND : Error::EXISTS;
	}
	if (e != Error::OK)
	{
		setError(err, e);
		return nullptr;
	}
	auto stream = std::make_unique<NativeFileStream>();
	if (!stream->open(full, mode))
	{
		setError(err, native::errorFromErrno(errno));
		return nullptr;
	}
	setError(err, Error::OK);
	return stream;
}

Error NativeBackend::list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const
{
	std::string full;
	Error err = toNative(relDir, full);
	if (err != Error::OK)
		return err;
	if (m_options.exactCase && !relDir.empty())
	{
		bool last = false;
		if (matchCase(relDir, last) != Error::OK || last)
			return Error::NOT_FOUND;
	}

	DirEntry entry;
	return native::listDir(full, [&](const std::string& name, FileType type)
	{
		entry.name = name;
		entry.type = type;
		return cb(entry);
	});
}

Error NativeBackend::createDir(std::string_view rel)
{
	if (m_options.readOnly)
		return Error::READ_ONLY;
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	if (m_options.exactCase)
	{
		bool last = false;
		err = matchCase(rel, last);
		if (err != Error::OK)
			return err;
		if (last)
			return Error::EXISTS;
	}
	return native::makeDir(full);
}

Error NativeBackend::removeDir(std::string_view rel)
{
	if (m_options.readOnly)
		return Error::READ_ONLY;
	// 后端根目录就是挂载点，不允许删除
	if (rel.empty())
		return Error::BUSY;
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	if (m_options.exactCase)
	{
		bool last = false;
		if (matchCase(rel, last) != Error::OK || last)
			return Error::NOT_FOUND;
	}
	Stat st;
	err = native::statPath(full, st);
	if (err != Error::OK)
		return err;
	if (st.type != FileType::DIR)
		return Error::NOT_DIR;
	return native::removeDirPath(full);
}

Error NativeBackend::removeFile(std::string_view rel)
{
	if (m_options.readOnly)
		return Error::READ_ONLY;
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	if (m_options.exactCase)
	{
		bool last = false;
		if (matchCase(rel, last) != Error::OK || last)
			return Error::NOT_FOUND;
	}
	Stat st;
	err = native::statPath(full, st);
	if (err != Error::OK)
		return err;
	if (st.type == FileType::DIR)
		return Error::IS_DIR;
	return native::removeFilePath(full);
}

Error NativeBackend::rename(std::string_view from, std::string_view to)
{
	if (m_options.readOnly)
		return Error::READ_ONLY;
	if (from.empty() || to.empty())
		return Error::BUSY;
	std::string fullFrom, fullTo;
	Error err = toNative(from, fullFrom);
	if (err != Error::OK)
		return err;
	err = toNative(to, fullTo);
	if (err != Error::OK)
		return err;
	if (m_options.exactCase)
	{
		bool last = false;
		if (matchCase(from, last) != Error::OK || last)
			return Error::NOT_FOUND;
		err = matchCase(to, last);
		if (err != Error::OK)
			return err;
		// 目标只差大小写：如果就是源自身（同一目录、真实名字就是源的名字，即只改大小写）照常改名，
		// 否则会覆盖另一个名字的文件，返回 EXISTS
		if (last)
		{
			const size_t fs = from.rfind('/'), ts = to.rfind('/');
			std::string_view fromDir = fs == std::string_view::npos ? std::string_view() : from.substr(0, fs);
			std::string_view toDir = ts == std::string_view::npos ? std::string_view() : to.substr(0, ts);
			std::string_view fromName = from.substr(fs == std::string_view::npos ? 0 : fs + 1);
			std::string_view toName = to.substr(ts == std::string_view::npos ? 0 : ts + 1);
			std::string actual;
			std::string parent = m_root;
			if (!parent.empty() && parent.back() == '/')
				parent.pop_back();
			if (!toDir.empty())
				parent.append("/").append(toDir);
			if (fromDir != toDir || native::realName(parent, toName, actual) != Error::OK || actual != fromName)
				return Error::EXISTS;
		}
	}
	// POSIX 上是 rename(2)；Windows 上按 rename(2) 的语义实现（见 NativeUtils.h）
	return native::renamePath(fullFrom, fullTo);
}

std::string NativeBackend::realPath(std::string_view rel) const
{
	std::string full;
	if (toNative(rel, full) != Error::OK)
		return std::string();
	return full;
}

NS_VFS_END
