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
	// 内存流只能由后端构造
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
	// 结果为负时按 POSIX 返回 EINVAL，位置保持不变（判断本身不能溢出：-INT64_MIN 没有定义）
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
	// 位置在 EOF 或之后：返回 0
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
		// 与 POSIX 对只读 fd 写入的行为一致
		errno = EBADF;
		return -1;
	}
	if (size > VFS_MAX_IO_CHUNK)
		size = VFS_MAX_IO_CHUNK;

	std::unique_lock<SharedMutex> lock(m_buffer->mutex);
	auto& data = m_buffer->data;
	// APPEND：与 O_APPEND 一致，每次写之前把位置移到当前末尾
	if (m_mode == Mode::APPEND)
		m_pos = static_cast<int64_t>(data.size());

	const uint64_t end = static_cast<uint64_t>(m_pos) + size;
	if (end > data.max_size())
	{
		errno = EFBIG;
		return -1;
	}
	// seek 越过 EOF 后写入：resize 会用 0 填充空洞。分配失败按 POSIX 风格返回错误，不让异常穿过流接口
	if (end > data.size())
	{
		try
		{
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
	// 与 ftruncate 一致：不移动当前位置
	return true;
}

bool MemoryFileStream::isOpen() const
{
	return m_buffer != nullptr;
}

NS_VFS_END
