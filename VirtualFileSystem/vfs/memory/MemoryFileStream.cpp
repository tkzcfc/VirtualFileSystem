#pragma once

#include "MemoryFileStream.h"

// Allowing the same file to be opened by multiple writable streams
#define ALLOW_MULTIPLE_WRITES 1

NS_VFS_BEGIN

MemoryFileStream::MemoryFileStream()
	: m_offset(0)
	, m_mode(FileStream::Mode::READ)
{}

MemoryFileStream::~MemoryFileStream()
{
	close();
}

bool MemoryFileStream::open(std::string path, FileStream::Mode mode)
{
	return false;
}

bool MemoryFileStream::open(std::shared_ptr<MemoryData> data, FileStream::Mode mode)
{
	if (mode != FileStream::Mode::READ)
	{
#if ALLOW_MULTIPLE_WRITES
#else
		if (data->wirteNum() > 0)
			return false;
#endif
		data->acquireWriteLock();
	}

	m_offset = 0;
	m_data = data;
	m_mode = mode;


	if (mode == FileStream::Mode::APPEND)
		seek(0, FileStream::SeekOrigin::END);

	return true;
}

void MemoryFileStream::close()
{
	if (m_data && m_mode != FileStream::Mode::READ)
	{
		m_data->releaseWriteLock();
	}
	m_offset = 0;
	m_data = nullptr;
}

uint64_t MemoryFileStream::seek(uint64_t offset, SeekOrigin origin)
{
	if (m_data == nullptr)
		return 0;

	if (origin == SeekOrigin::CUR)
	{
		m_offset += offset;
	}
	else if (origin == SeekOrigin::END)
	{
		auto dataLen = m_data->len();

		if (dataLen < offset)
			return 0;

		m_offset = dataLen - offset;
	}
	else if (origin == SeekOrigin::SET)
	{
		m_offset = offset;
	}

	return m_offset;
}

uint64_t MemoryFileStream::read(void* buf, uint64_t size)
{
	if (m_data == nullptr)
		return 0;

	uint64_t result = m_data->read((uint8_t*)buf, size, m_offset);
	m_offset += result;
	return result;
}

uint64_t MemoryFileStream::write(const void* buf, uint64_t size)
{
	if (m_data == nullptr || m_mode == FileStream::Mode::READ)
		return 0;

	uint64_t result = m_data->write((uint8_t*)buf, size, m_offset);
	m_offset += result;
	return result;
}

uint64_t MemoryFileStream::tell()
{
	return m_offset;
}

uint64_t MemoryFileStream::size()
{
	if (m_data == nullptr)
		return 0;
	return m_data->len();
}

bool MemoryFileStream::isOpen() const
{
	return m_data != nullptr;
}

NS_VFS_END
