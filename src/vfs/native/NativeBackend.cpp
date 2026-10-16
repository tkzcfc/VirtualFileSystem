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

void NativeBackend::checkCase(std::string_view rel) const
{
	// 在大小写不敏感的文件系统上（Windows/macOS），"A.png" 能打开 "a.png"，发布到归档后就找不到了。
	// 这里逐级列目录比对真实名字，开销较大，所以只在 strictCase 时执行。
	std::string dir = m_root;
	size_t start = 0;
	while (start < rel.size())
	{
		size_t end = rel.find('/', start);
		if (end == std::string_view::npos)
			end = rel.size();
		std::string_view seg = rel.substr(start, end - start);

		bool exact = false;
		std::error_code ec;
		for (const auto& entry : stdfs::directory_iterator(native::toFsPath(dir), ec))
		{
			if (native::fromFsPath(entry.path().filename()) == seg)
			{
				exact = true;
				break;
			}
		}
		if (!exact)
		{
			log(LogLevel::WARN, "vfs: 路径大小写与磁盘上的真实名字不一致: " + std::string(rel));
			return;
		}
		dir.push_back('/');
		dir.append(seg);
		start = end + 1;
	}
}

Error NativeBackend::stat(std::string_view rel, Stat& out) const
{
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	err = native::statPath(full, out);
	if (err == Error::OK && m_options.strictCase && !rel.empty())
		checkCase(rel);
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

	Stat st;
	err = native::statPath(full, st);
	if (err != Error::OK)
		return err;
	if (st.type != FileType::DIR)
		return Error::NOT_DIR;

	std::error_code ec;
	stdfs::directory_iterator it(native::toFsPath(full), ec);
	if (ec)
		return native::errorFromCode(ec);

	DirEntry entry;
	for (; it != stdfs::directory_iterator(); it.increment(ec))
	{
		if (ec)
			return native::errorFromCode(ec);
		std::error_code ec2;
		// is_directory/is_regular_file 会跟随 symlink；特殊文件（设备、管道）直接跳过
		if (it->is_directory(ec2))
			entry.type = FileType::DIR;
		else if (it->is_regular_file(ec2))
			entry.type = FileType::FILE;
		else
			continue;
		entry.name = native::fromFsPath(it->path().filename());
		if (!cb(entry))
			break;
	}
	return Error::OK;
}

Error NativeBackend::createDir(std::string_view rel)
{
	if (m_options.readOnly)
		return Error::READ_ONLY;
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	std::error_code ec;
	if (stdfs::create_directory(native::toFsPath(full), ec))
		return Error::OK;
	if (ec)
		return native::errorFromCode(ec);
	// create_directory 返回 false 且无错误：目标已经存在（可能是目录，也可能是文件）
	return Error::EXISTS;
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
	Stat st;
	err = native::statPath(full, st);
	if (err != Error::OK)
		return err;
	if (st.type != FileType::DIR)
		return Error::NOT_DIR;
	std::error_code ec;
	stdfs::remove(native::toFsPath(full), ec);
	return native::errorFromCode(ec);
}

Error NativeBackend::removeFile(std::string_view rel)
{
	if (m_options.readOnly)
		return Error::READ_ONLY;
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	Stat st;
	err = native::statPath(full, st);
	if (err != Error::OK)
		return err;
	if (st.type == FileType::DIR)
		return Error::IS_DIR;
	std::error_code ec;
	stdfs::remove(native::toFsPath(full), ec);
	return native::errorFromCode(ec);
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
	std::error_code ec;
#ifdef _WIN32
	// MoveFileExW 不能用目录替换已有目录，也不能用文件替换目录。这里按 POSIX rename(2) 的语义预先处理：
	// 目录 -> 空目录：先删除目标（非原子，但结果与 POSIX 一致）；目录 -> 非空目录：NOT_EMPTY；
	// 目录 -> 文件：NOT_DIR；文件 -> 目录：IS_DIR
	{
		stdfs::path pf = native::toFsPath(fullFrom), pt = native::toFsPath(fullTo);
		stdfs::file_status sf = stdfs::status(pf, ec), stt = stdfs::status(pt, ec);
		ec.clear();
		if (stdfs::exists(sf) && stdfs::exists(stt) && !stdfs::equivalent(pf, pt, ec))
		{
			ec.clear();
			bool fromDir = stdfs::is_directory(sf), toDir = stdfs::is_directory(stt);
			if (fromDir && !toDir)
				return Error::NOT_DIR;
			if (!fromDir && toDir)
				return Error::IS_DIR;
			if (fromDir && toDir)
			{
				if (!stdfs::is_empty(pt, ec))
					return ec ? native::errorFromCode(ec) : Error::NOT_EMPTY;
				stdfs::remove(pt, ec);
				if (ec)
					return native::errorFromCode(ec);
			}
		}
		ec.clear();
	}
#endif
	// std::filesystem::rename 在 POSIX 上是 rename(2)，Windows 上是 MoveFileExW(REPLACE_EXISTING)，都是原子的
	stdfs::rename(native::toFsPath(fullFrom), native::toFsPath(fullTo), ec);
	return native::errorFromCode(ec);
}

std::string NativeBackend::realPath(std::string_view rel) const
{
	std::string full;
	if (toNative(rel, full) != Error::OK)
		return std::string();
	return full;
}

NS_VFS_END
