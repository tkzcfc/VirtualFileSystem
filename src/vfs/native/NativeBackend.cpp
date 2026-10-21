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
	// Strip a trailing '/' (but keep roots such as "/" and "C:/")
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
		// The canonical root itself may be a symlink; what is compared is the resolved real location
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
	// Check component by component: the path the backend receives is already normalized; this only additionally rejects non-portable names
	size_t start = 0;
	while (start < rel.size())
	{
		size_t end = rel.find('/', start);
		if (end == std::string_view::npos)
			end = rel.size();
		if (!Path::isPortableComponent(rel.substr(start, end - start)))
			return Error::INVALID_PATH;
#if VFS_PLATFORM_WASM
		// Emscripten's filesystem layer (including NODERAWFS) silently truncates a name longer than 255 bytes and still creates it,
		// so the write lands on a different file; reject it here at the usual filesystem limit
		if (end - start > 255)
			return Error::INVALID_PATH;
#endif
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
		// After resolving symlinks the path must still be inside the root
		std::error_code ec;
		stdfs::path canon = stdfs::weakly_canonical(native::toFsPath(out), ec);
		if (ec)
		{
			// If resolution fails (permissions, a symlink loop, and so on) it is impossible to confirm the path is still inside the root: reject it instead of allowing it.
			// A missing path does not reach here (weakly_canonical concatenates the missing tail literally)
			Error ce = native::errorFromCode(ec);
			return ce == Error::NOT_FOUND || ce == Error::NOT_DIR ? ce : Error::PERMISSION_DENIED;
		}
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
		dir.pop_back();   // when the root is "/" or "C:/", realName adds another '/'
	std::string actual;
	size_t start = 0;
	while (start < rel.size())
	{
		size_t end = rel.find('/', start);
		if (end == std::string_view::npos)
			end = rel.size();
		std::string_view seg = rel.substr(start, end - start);
		// A missing entry is not decided here; the later real operation reports it. Other errors (I/O, permissions) are returned as-is:
		// if case cannot be confirmed, do not treat it as "case matches" and let it through, or exactCase would fail open on errors
		Error re = native::realName(dir, seg, actual);
		if (re == Error::NOT_FOUND || re == Error::NOT_DIR)
			return Error::OK;
		if (re != Error::OK)
			return re;
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
	// On a case-insensitive filesystem (Windows/macOS), "A.png" can open "a.png", and after publishing into an archive that file is then missing
	bool last = false;
	if (matchCase(rel, last) != Error::OK || last)
		log(LogLevel::WARN, "vfs: path case differs from the actual name on disk: " + std::string(rel));
}

Error NativeBackend::stat(std::string_view rel, Stat& out) const
{
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	err = native::statPath(full, out, m_root.size());
	if (err == Error::OK && !rel.empty())
	{
		if (m_options.exactCase)
		{
			// Byte-for-byte match: any component that differs only in case is treated as not found
			bool last = false;
			Error ce = matchCase(rel, last);
			if (ce != Error::OK)
				return ce;
			if (last)
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
		// Read: a case-only difference is not found. Write: it would land on the existing file that differs only in case, so return EXISTS instead of quietly overwriting it
		bool last = false;
		e = matchCase(rel, last);
		if (e == Error::OK && last)
			e = mode == IFileStream::Mode::READ ? Error::NOT_FOUND : Error::EXISTS;
	}
	if (e == Error::OK && mode == IFileStream::Mode::READ)
		e = native::checkParentDir(full, m_root.size());   // Windows: avoid the serialized query for "intermediate directory does not exist"
	if (e != Error::OK)
	{
		setError(err, e);
		return nullptr;
	}
	auto stream = std::make_unique<NativeFileStream>();
	if (!stream->open(full, mode))
	{
		int code = errno;
		native::forgetParentDir();
		setError(err, native::errorFromErrno(code));
		return nullptr;
	}
	// With a single layer the VFS opens directly and does not stat first, so the case check has to happen here too
	if (m_options.strictCase && !m_options.exactCase && !rel.empty())
		checkCase(rel);
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
		Error ce = matchCase(relDir, last);
		if (ce != Error::OK)
			return ce;
		if (last)
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
	// The backend root is the mount point and must not be deleted
	if (rel.empty())
		return Error::BUSY;
	std::string full;
	Error err = toNative(rel, full);
	if (err != Error::OK)
		return err;
	if (m_options.exactCase)
	{
		bool last = false;
		Error ce = matchCase(rel, last);
		if (ce != Error::OK)
			return ce;   // if case cannot be confirmed (I/O or permission error), return it as-is instead of treating it as "not found"
		if (last)
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
		Error ce = matchCase(rel, last);
		if (ce != Error::OK)
			return ce;   // if case cannot be confirmed (I/O or permission error), return it as-is instead of treating it as "not found"
		if (last)
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
	// The backend root cannot be the source or target of a rename (same as MemoryBackend; see mount-rules.md section 12)
	if (from.empty() || to.empty())
		return Error::INVALID_PATH;
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
		err = matchCase(from, last);
		if (err != Error::OK)
			return err;
		if (last)
			return Error::NOT_FOUND;
		err = matchCase(to, last);
		if (err != Error::OK)
			return err;
		// Target differs only in case: if it is the source itself (same directory, and the real name is the source name, i.e. only the case changes) rename as usual;
		// otherwise it would overwrite a file with a different name, so return EXISTS
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
	// On POSIX this is rename(2); on Windows it is implemented with rename(2) semantics (see NativeUtils.h)
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
