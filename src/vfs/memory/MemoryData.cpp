#include "MemoryData.h"
#include <assert.h>

#undef LIKELY
#undef UNLIKELY

#if defined(__GNUC__) && __GNUC__ >= 4
#    define LIKELY(x) (__builtin_expect((x), 1))
#    define UNLIKELY(x) (__builtin_expect((x), 0))
#else
#    define LIKELY(x) (x)
#    define UNLIKELY(x) (x)
#endif

NS_VFS_BEGIN

MemoryData::MemoryData()
{
	m_data.reserve(8192);
	m_wirteNum.store(0, std::memory_order_relaxed);
}

MemoryData::~MemoryData()
{}

uint64_t MemoryData::write(uint8_t* data, uint64_t len, uint64_t offset)
{
	if (len == 0)
		return 0;

	std::lock_guard<std::mutex> lock(m_mutex);

	uint64_t totalLen = len + offset;
	if (totalLen > m_data.size())
	{
		m_data.resize(totalLen);
	}

	::memcpy(&m_data[offset], data, len);
	return len;
}

uint64_t MemoryData::read(uint8_t* data, uint64_t len, uint64_t offset)
{
	if (len == 0)
		return 0;

	if (UNLIKELY(m_wirteNum.load(std::memory_order_relaxed) > 0))
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return read_impl(data, len, offset);
	}
	else
	{
		return read_impl(data, len, offset);
	}
}

uint64_t MemoryData::read_impl(uint8_t* data, uint64_t len, uint64_t offset)
{
	if (offset >= m_data.size())
		return 0;

	uint64_t readLen = std::min(len, static_cast<uint64_t>(m_data.size()) - offset);
	if (readLen > 0)
	{
		::memcpy(data, &m_data[offset], readLen);
		return readLen;
	}
	return 0;
}

uint64_t MemoryData::len()
{
	if (UNLIKELY(m_wirteNum.load(std::memory_order_relaxed) > 0))
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return static_cast<uint64_t>(m_data.size());
	}
	else
	{
		return static_cast<uint64_t>(m_data.size());
	}
}

void MemoryData::acquireWriteLock()
{
	m_wirteNum.store(wirteNum() + 1, std::memory_order_relaxed);
}

void MemoryData::releaseWriteLock()
{
	m_wirteNum.store(wirteNum() - 1, std::memory_order_relaxed);
	assert(wirteNum() >= 0);
}

NS_VFS_END
