#include "SingleFileBackend.h"
#include "NativeFileStream.h"
#include "NativeUtils.h"

namespace stdfs = std::filesystem;

NS_VFS_BEGIN

std::shared_ptr<SingleFileBackend> SingleFileBackend::create(std::string_view nativeFile, bool readOnly, Error* err)
{
	std::error_code ec;
	stdfs::path p = stdfs::absolute(native::toFsPath(nativeFile), ec);
	if (ec)
	{
		setError(err, native::errorFromCode(ec));
		return nullptr;
	}
	std::u8string u8 = p.lexically_normal().generic_u8string();
	std::string file(reinterpret_cast<const char*>(u8.data()), u8.size());

	Stat st;
	Error e = native::statPath(file, st);
	if (e != Error::OK)
	{
		setError(err, e);
		return nullptr;
	}
	if (st.type != FileType::FILE)
	{
		setError(err, Error::IS_DIR);
		return nullptr;
	}
	setError(err, Error::OK);
	return std::make_shared<SingleFileBackend>(std::move(file), readOnly);
}

SingleFileBackend::SingleFileBackend(std::string nativeFile, bool readOnly)
	: m_nativeFile(std::move(nativeFile))
	, m_readOnly(readOnly)
{
}

SingleFileBackend::~SingleFileBackend()
{
}

uint32_t SingleFileBackend::capabilities() const
{
	uint32_t caps = BackendCaps::Read | BackendCaps::Resize;
	if (!m_readOnly)
		caps |= BackendCaps::Write;
	return caps;
}

Error SingleFileBackend::stat(std::string_view rel, Stat& out) const
{
	// The mount point itself is a file, so any path under it means "an intermediate component is not a directory".
	if (!rel.empty())
		return Error::NOT_DIR;
	Error err = native::statPath(m_nativeFile, out);
	if (err == Error::OK && out.type != FileType::FILE)
		return Error::NOT_FOUND;
	return err;
}

std::unique_ptr<IFileStream> SingleFileBackend::open(std::string_view rel, IFileStream::Mode mode, Error* err)
{
	if (!rel.empty())
	{
		setError(err, Error::NOT_DIR);
		return nullptr;
	}
	if (m_readOnly && mode != IFileStream::Mode::READ)
	{
		setError(err, Error::READ_ONLY);
		return nullptr;
	}
	auto stream = std::make_unique<NativeFileStream>();
	if (!stream->open(m_nativeFile, mode))
	{
		setError(err, native::errorFromErrno(errno));
		return nullptr;
	}
	setError(err, Error::OK);
	return stream;
}

Error SingleFileBackend::list(std::string_view /*relDir*/, const std::function<bool(const DirEntry&)>& /*cb*/) const
{
	return Error::NOT_DIR;
}

Error SingleFileBackend::createDir(std::string_view rel)
{
	return rel.empty() ? Error::EXISTS : Error::NOT_DIR;
}

Error SingleFileBackend::removeFile(std::string_view rel)
{
	// The mount point cannot be deleted.
	return rel.empty() ? Error::BUSY : Error::NOT_DIR;
}

std::string SingleFileBackend::realPath(std::string_view rel) const
{
	return rel.empty() ? m_nativeFile : std::string();
}

NS_VFS_END
