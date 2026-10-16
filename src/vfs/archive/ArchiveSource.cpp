#include "ArchiveSource.h"
#include "../native/NativeFileStream.h"
#include "../native/NativeUtils.h"
#include "../memory/MemoryFileStream.h"

NS_VFS_BEGIN

FileArchiveSource::FileArchiveSource(std::string nativePath)
	: m_nativePath(std::move(nativePath))
	, m_mtime(0)
{
	Stat st;
	if (native::statPath(m_nativePath, st) == Error::OK)
		m_mtime = st.mtime;
}

std::unique_ptr<IFileStream> FileArchiveSource::openStream(Error* err) const
{
	auto stream = std::make_unique<NativeFileStream>();
	if (!stream->open(m_nativePath, IFileStream::Mode::READ))
	{
		setError(err, native::errorFromErrno(errno));
		return nullptr;
	}
	setError(err, Error::OK);
	return stream;
}

int64_t FileArchiveSource::mtime() const
{
	return m_mtime;
}

MemoryArchiveSource::MemoryArchiveSource(std::vector<uint8_t> data)
	: m_buffer(std::make_shared<MemoryBuffer>())
{
	m_buffer->data = std::move(data);
}

MemoryArchiveSource::MemoryArchiveSource(std::shared_ptr<MemoryBuffer> buffer)
	: m_buffer(std::move(buffer))
{
}

std::unique_ptr<IFileStream> MemoryArchiveSource::openStream(Error* err) const
{
	setError(err, Error::OK);
	return std::make_unique<MemoryFileStream>(m_buffer, IFileStream::Mode::READ);
}

NS_VFS_END
