#include "ArchiveFileStream.h"
#include "../crypto/XorCipher.h"
#include <algorithm>

NS_VFS_BEGIN

ArchiveFileStream::ArchiveFileStream(std::shared_ptr<const ArchiveIndex> index, const ArchiveEntry* entry,
									 std::unique_ptr<IFileStream> file)
	: m_index(std::move(index))
	, m_entry(entry)
	, m_file(std::move(file))
	, m_dataStart(m_index->baseOffset + static_cast<int64_t>(entry->offset))
	, m_pos(0)
	, m_filePos(-1)
{
}

ArchiveFileStream::~ArchiveFileStream()
{
}

bool ArchiveFileStream::open(std::string_view /*path*/, IFileStream::Mode /*mode*/)
{
	// 归档流只能由 ArchiveBackend 构造
	errno = ENOTSUP;
	return false;
}

int ArchiveFileStream::close()
{
	m_file.reset();
	return 0;
}

int64_t ArchiveFileStream::seek(int64_t offset, int origin) const
{
	if (!m_file)
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
		base = static_cast<int64_t>(m_entry->size);
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if ((offset < 0 && base + offset < 0) || (offset > 0 && base > INT64_MAX - offset))
	{
		errno = EINVAL;
		return -1;
	}
	// 允许 seek 到 EOF 之后，之后的 read 返回 0
	m_pos = base + offset;
	return m_pos;
}

int ArchiveFileStream::read(void* buf, unsigned int size) const
{
	if (!m_file)
	{
		errno = EBADF;
		return -1;
	}
	// 已到达或超过 EOF：按 POSIX 语义返回 0
	const int64_t fileSize = static_cast<int64_t>(m_entry->size);
	if (m_pos >= fileSize || size == 0)
		return 0;

	// 读取长度不能超过文件剩余部分，也不能超过单次 IO 上限
	size_t want = static_cast<size_t>(std::min<int64_t>(std::min<unsigned int>(size, VFS_MAX_IO_CHUNK), fileSize - m_pos));
	const int64_t target = m_dataStart + m_pos;
	if (m_filePos != target)
	{
		// 只有位置不连续时才 seek：顺序读时省掉一次系统调用
		if (m_file->seek(target, SEEK_SET) != target)
		{
			m_filePos = -1;
			errno = EIO;
			return -1;
		}
		m_filePos = target;
	}

	uint8_t* out = static_cast<uint8_t*>(buf);
	size_t got = 0;
	while (got < want)
	{
		int n = m_file->read(out + got, static_cast<unsigned int>(want - got));
		if (n < 0)
		{
			m_filePos = -1;
			return -1;
		}
		if (n == 0)
			break;   // 来源被截断：返回已读到的部分
		got += static_cast<size_t>(n);
	}
	m_filePos = target + static_cast<int64_t>(got);

	// XOR 加密：密钥位置 = 文件内偏移 % keyLen，可以从任意位置开始解密
	if (m_entry->cipher == CipherType::XOR)
		xorCipher(out, got, m_entry->key.data(), m_entry->keyLength, static_cast<uint64_t>(m_pos));

	m_pos += static_cast<int64_t>(got);
	return static_cast<int>(got);
}

int ArchiveFileStream::write(const void* /*buf*/, unsigned int /*size*/) const
{
	// 运行时归档只读
	errno = EROFS;
	return -1;
}

int64_t ArchiveFileStream::tell() const
{
	if (!m_file)
	{
		errno = EBADF;
		return -1;
	}
	return m_pos;
}

int64_t ArchiveFileStream::size() const
{
	if (!m_file)
	{
		errno = EBADF;
		return -1;
	}
	return static_cast<int64_t>(m_entry->size);
}

bool ArchiveFileStream::isOpen() const
{
	return m_file != nullptr;
}

NS_VFS_END
