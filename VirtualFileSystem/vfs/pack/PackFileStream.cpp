#include "PackFileStream.h"
#include <algorithm>
#include "PackUtils.h"
#include "PackFileSystem.h"

NS_VFS_BEGIN

PackFileStream::PackFileStream()
	: m_offset(0)
	, m_realData(0)
	, m_realDataLen(0)
	, m_rawOffset(0)
{
}

PackFileStream::~PackFileStream()
{
	close();
}

bool PackFileStream::open(std::string path, FileStream::Mode mode)
{
	return false;
}

bool PackFileStream::open(std::string path, const PackFileInfo& fileInfo, uint32_t dataSecret)
{
	if (fileInfo.length <= 0)
	{
		m_realData = (uint8_t*)malloc(1);
		m_realDataLen = 0;
		return true;
	}

	if (!m_fs.open(path, FileStream::Mode::READ))
		return false;

	if (fileInfo.compressionType == PackFileCompressionType::None)
	{
		m_realDataLen = fileInfo.length;
		m_rawOffset = fileInfo.offset;
		if (dataSecret == 0)
		{
			return true;
		}

		m_realData = (uint8_t*)malloc(m_realDataLen);
		m_fs.seek(fileInfo.offset, FileStream::SeekOrigin::SET);
		if (m_fs.read(m_realData, fileInfo.length) == fileInfo.length)
		{
			m_fs.close();
			pack::xorContent(dataSecret, (char*)m_realData, m_realDataLen);
			return true;
		}
		else
		{
			free(m_realData);
			return false;
		}
	}
	// unzip
	else if (fileInfo.compressionType == PackFileCompressionType::Gzip)
	{
		std::unique_ptr<char[]> data(new char[fileInfo.length]);
		if (!data)
			return false;

		m_fs.seek(fileInfo.offset, FileStream::SeekOrigin::SET);
		if (m_fs.read(&data[0], fileInfo.length) != fileInfo.length)
			return false;

		m_fs.close();
		pack::xorContent(dataSecret, &data[0], fileInfo.length);

		int errCode = 0;
		m_realData = (uint8_t*)pack::decompressData(&data[0], fileInfo.length, m_realDataLen, &errCode);
		if (m_realData)
			return true;
		
		printf("unzip error code: %d\n", errCode);
	}
	return false;
}

void PackFileStream::close()
{
	if (m_realData)
	{
		free(m_realData);
		m_realData = nullptr;
	}
	m_realDataLen = 0;
	m_fs.close();
}

uint64_t PackFileStream::seek(uint64_t offset, SeekOrigin origin)
{
	if (origin == SeekOrigin::CUR)
	{
		m_offset += offset;
	}
	else if (origin == SeekOrigin::END)
	{
		if (m_realDataLen < offset)
			return 0;

		m_offset = m_realDataLen - offset;
	}
	else if (origin == SeekOrigin::SET)
	{
		m_offset = offset;
	}

	return m_offset;
}

uint64_t PackFileStream::read(void* buf, uint64_t size)
{
	if (size == 0 || m_offset >= static_cast<int64_t>(m_realDataLen))
		return 0;

	uint64_t readLen = std::min(size, m_realDataLen - m_offset);
	if (readLen > 0)
	{
		if (m_realData)
		{
			::memcpy(buf, m_realData + m_offset, readLen);
			m_offset += readLen;
			return readLen;
		}
		else
		{
			m_fs.seek(m_offset + m_rawOffset, FileStream::SeekOrigin::SET);
			uint64_t result = m_fs.read(buf, readLen);
			m_offset += result;
			return result;
		}
	}
	return 0;
}

uint64_t PackFileStream::write(const void* buf, uint64_t size)
{
	return 0;
}

uint64_t PackFileStream::tell()
{
	return m_offset;
}

uint64_t PackFileStream::size()
{
	return m_realDataLen;
}

bool PackFileStream::isOpen() const
{
	return m_realData != nullptr || m_fs.isOpen();
}

NS_VFS_END
