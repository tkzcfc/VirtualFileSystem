#include "MemoryData.h"

NS_VFS_BEGIN

MemoryData::MemoryData()
{
	m_data.reserve(8192);
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

	std::lock_guard<std::mutex> lock(m_mutex);

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
	std::lock_guard<std::mutex> lock(m_mutex);
	return static_cast<uint64_t>(m_data.size());
}

NS_VFS_END
