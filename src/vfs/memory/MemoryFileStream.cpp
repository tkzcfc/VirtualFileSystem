#include "MemoryFileStream.h"
#include <new>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

NS_VFS_BEGIN

MemoryFileStream::MemoryFileStream(std::shared_ptr<MemoryBuffer> buffer, IFileStream::Mode mode)
	: m_buffer(std::move(buffer))
	, m_mode(mode)
	, m_pos(0)
{
}

MemoryFileStream::~MemoryFileStream()
{
}

bool MemoryFileStream::open(std::string_view /*path*/, IFileStream::Mode /*mode*/)
{
	// A memory stream can only be constructed by the backend
	errno = ENOTSUP;
	return false;
}

int MemoryFileStream::close()
{
	m_buffer.reset();
	return 0;
}

int64_t MemoryFileStream::seek(int64_t offset, int origin) const
{
	if (!m_buffer)
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
		base = size();
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	// If the result would be negative, return EINVAL as POSIX does and leave the position unchanged (the check itself must not overflow: -INT64_MIN is undefined)
	if ((offset < 0 && (offset == INT64_MIN || base < -offset)) || (offset > 0 && base > INT64_MAX - offset))
	{
		errno = EINVAL;
		return -1;
	}
	m_pos = base + offset;
	return m_pos;
}

int MemoryFileStream::read(void* buf, unsigned int size) const
{
	if (!m_buffer)
	{
		errno = EBADF;
		return -1;
	}
	if (size > VFS_MAX_IO_CHUNK)
		size = VFS_MAX_IO_CHUNK;

	std::shared_lock<SharedMutex> lock(m_buffer->mutex);
	const int64_t total = static_cast<int64_t>(m_buffer->data.size());
	// Position at or past EOF: return 0
	if (m_pos >= total)
		return 0;
	size_t len = static_cast<size_t>(std::min<int64_t>(size, total - m_pos));
	std::memcpy(buf, m_buffer->data.data() + m_pos, len);
	m_pos += static_cast<int64_t>(len);
	return static_cast<int>(len);
}

int MemoryFileStream::write(const void* buf, unsigned int size) const
{
	if (!m_buffer)
	{
		errno = EBADF;
		return -1;
	}
	if (m_mode == Mode::READ)
	{
		// Matches POSIX behavior for a write to a read-only fd
		errno = EBADF;
		return -1;
	}
	if (size > VFS_MAX_IO_CHUNK)
		size = VFS_MAX_IO_CHUNK;

	std::unique_lock<SharedMutex> lock(m_buffer->mutex);
	auto& data = m_buffer->data;
	// APPEND: same as O_APPEND, move the position to the current end before every write
	if (m_mode == Mode::APPEND)
		m_pos = static_cast<int64_t>(data.size());

	const uint64_t end = static_cast<uint64_t>(m_pos) + size;
	if (end > data.max_size())
	{
		errno = EFBIG;
		return -1;
	}
	// Write after a seek past EOF: resize fills the hole with 0. On allocation failure, return an error in the POSIX style and do not let an exception cross the stream interface
	if (end > data.size())
	{
		try
		{
			// Grow capacity geometrically (2x) ourselves: MSVC's vector grows by 1.5x, so a file written in
			// chunks is copied many more times while it grows (a 64 MiB file written in 1 MiB chunks ran at
			// about 1/3 of the speed of writing to the OS file cache on Windows).
			if (end > data.capacity())
			{
				// Near max_size, doubling would ask for more than can exist: then reserve exactly what is needed
				const uint64_t cap = data.capacity();
				const uint64_t want = cap <= data.max_size() / 2 ? std::max<uint64_t>(end, cap * 2) : end;
				data.reserve(static_cast<size_t>(want));
			}
			data.resize(static_cast<size_t>(end));
		}
		catch (const std::bad_alloc&)
		{
			errno = ENOMEM;
			return -1;
		}
	}
	if (size > 0)
		std::memcpy(data.data() + m_pos, buf, size);
	m_pos += size;
	m_buffer->mtime.store(currentUnixTime(), std::memory_order_relaxed);
	return static_cast<int>(size);
}

int64_t MemoryFileStream::tell() const
{
	if (!m_buffer)
	{
		errno = EBADF;
		return -1;
	}
	return m_pos;
}

int64_t MemoryFileStream::size() const
{
	if (!m_buffer)
	{
		errno = EBADF;
		return -1;
	}
	std::shared_lock<SharedMutex> lock(m_buffer->mutex);
	return static_cast<int64_t>(m_buffer->data.size());
}

bool MemoryFileStream::resize(int64_t newSize) const
{
	if (!m_buffer || m_mode == Mode::READ)
	{
		errno = EBADF;
		return false;
	}
	if (newSize < 0)
	{
		errno = EINVAL;
		return false;
	}
	if (static_cast<uint64_t>(newSize) > m_buffer->data.max_size())
	{
		errno = EFBIG;
		return false;
	}
	std::unique_lock<SharedMutex> lock(m_buffer->mutex);
	try
	{
		m_buffer->data.resize(static_cast<size_t>(newSize));
	}
	catch (const std::bad_alloc&)
	{
		errno = ENOMEM;
		return false;
	}
	m_buffer->mtime.store(currentUnixTime(), std::memory_order_relaxed);
	// Same as ftruncate: do not move the current position
	return true;
}

bool MemoryFileStream::isOpen() const
{
	return m_buffer != nullptr;
}

NS_VFS_END
