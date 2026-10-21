#include "ArchiveSource.h"
#include "../native/NativeFileStream.h"
#include "../native/NativeUtils.h"
#include "../memory/MemoryFileStream.h"
#include <algorithm>
#include <cstring>
#include <mutex>

NS_VFS_BEGIN

int64_t IArchiveSource::readAt(void* /*buf*/, size_t /*size*/, uint64_t /*offset*/) const
{
	errno = ENOTSUP;
	return -1;
}

FileArchiveSource::FileArchiveSource(std::string nativePath)
	: m_nativePath(std::move(nativePath))
	, m_mtime(0)
{
	Stat st;
	if (native::statPath(m_nativePath, st) == Error::OK)
		m_mtime = st.mtime;
	auto reader = std::make_unique<native::PositionalReader>();
	if (reader->open(m_nativePath))
		m_reader = std::move(reader);
}

FileArchiveSource::~FileArchiveSource()
{
}

bool FileArchiveSource::supportsReadAt() const
{
	return m_reader != nullptr;
}

int64_t FileArchiveSource::readAt(void* buf, size_t size, uint64_t offset) const
{
	if (!m_reader)
		return IArchiveSource::readAt(buf, size, offset);
	return m_reader->readAt(buf, size, offset);
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

int64_t MemoryArchiveSource::readAt(void* buf, size_t size, uint64_t offset) const
{
	// Take a shared lock, as MemoryFileStream does, so even an illegal mutation of the buffer cannot read out of bounds
	std::shared_lock<SharedMutex> lock(m_buffer->mutex);
	const auto& data = m_buffer->data;
	if (offset >= data.size())
		return 0;
	const size_t n = std::min<size_t>(size, data.size() - static_cast<size_t>(offset));
	if (n)
		std::memcpy(buf, data.data() + offset, n);
	return static_cast<int64_t>(n);
}

NS_VFS_END
