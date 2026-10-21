#include "ArchiveFileStream.h"
#include "ByteIO.h"
#include "../crypto/XorCipher.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>

NS_VFS_BEGIN

ArchiveFileStream::ArchiveFileStream(std::shared_ptr<const ArchiveIndex> index, const ArchiveEntry* entry,
									 std::unique_ptr<IFileStream> file)
	: m_index(std::move(index))
	, m_entry(entry)
	, m_file(std::move(file))
	, m_open(true)
	, m_dataStart(m_index->baseOffset + static_cast<int64_t>(entry->offset))
	, m_pos(0)
	, m_filePos(-1)
	, m_curBlock(UINT64_MAX)
{
}

ArchiveFileStream::~ArchiveFileStream()
{
}

bool ArchiveFileStream::open(std::string_view /*path*/, IFileStream::Mode /*mode*/)
{
	// Archive streams can only be constructed by ArchiveBackend
	errno = ENOTSUP;
	return false;
}

int ArchiveFileStream::close()
{
	m_file.reset();
	m_open = false;
	// Drop the index too, so a closed but not yet destroyed stream does not pin the archive source.
	m_entry = nullptr;
	m_index.reset();
	std::vector<uint64_t>().swap(m_blockTable);
	std::vector<uint8_t>().swap(m_block);
	std::vector<uint8_t>().swap(m_scratch);
	return 0;
}

int64_t ArchiveFileStream::seek(int64_t offset, int origin) const
{
	if (!m_open)
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
	if ((offset < 0 && (offset == INT64_MIN || base < -offset)) || (offset > 0 && base > INT64_MAX - offset))
	{
		errno = EINVAL;
		return -1;
	}
	// Seeking past EOF is allowed; a later read returns 0
	m_pos = base + offset;
	return m_pos;
}

int ArchiveFileStream::read(void* buf, unsigned int size) const
{
	if (!m_open)
	{
		errno = EBADF;
		return -1;
	}
	// At or past EOF: return 0, matching POSIX
	const int64_t fileSize = static_cast<int64_t>(m_entry->size);
	if (m_pos >= fileSize || size == 0)
		return 0;

	// The read length cannot exceed the remainder of the file or the per-call IO limit
	size_t want = static_cast<size_t>(std::min<int64_t>(std::min<unsigned int>(size, VFS_MAX_IO_CHUNK), fileSize - m_pos));
	uint8_t* out = static_cast<uint8_t*>(buf);
	if (m_entry->compression == CompressionType::LZ4)
	{
		const int64_t n = readCompressed(out, want);
		if (n < 0)
			return -1;
		m_pos += n;
		return static_cast<int>(n);
	}
	const int64_t got64 = readSource(out, want, m_dataStart + m_pos);
	if (got64 < 0)
		return -1;
	const size_t got = static_cast<size_t>(got64);

	// XOR encryption: key position = offset within the file % keyLen, so decryption can start at any offset
	if (m_entry->cipher == CipherType::XOR)
		xorCipher(out, got, m_entry->key.data(), m_entry->keyLength, static_cast<uint64_t>(m_pos));

	m_pos += static_cast<int64_t>(got);
	return static_cast<int>(got);
}

int ArchiveFileStream::write(const void* /*buf*/, unsigned int /*size*/) const
{
	// Runtime archives are read-only; after close this is EBADF, same as the other methods
	errno = m_open ? EROFS : EBADF;
	return -1;
}

bool ArchiveFileStream::resize(int64_t /*size*/) const
{
	errno = m_open ? ENOTSUP : EBADF;
	return false;
}

int64_t ArchiveFileStream::tell() const
{
	if (!m_open)
	{
		errno = EBADF;
		return -1;
	}
	return m_pos;
}

int64_t ArchiveFileStream::size() const
{
	if (!m_open)
	{
		errno = EBADF;
		return -1;
	}
	return static_cast<int64_t>(m_entry->size);
}

bool ArchiveFileStream::isOpen() const
{
	return m_open;
}

bool ArchiveFileStream::readStored(uint8_t* out, size_t len, uint64_t storedOff) const
{
	size_t got = 0;
	while (got < len)
	{
		const int64_t n = readSource(out + got, len - got, m_dataStart + static_cast<int64_t>(storedOff + got));
		if (n < 0)
			return false;
		got += static_cast<size_t>(n);
	}
	if (m_entry->cipher == CipherType::XOR)
		xorCipher(out, len, m_entry->key.data(), m_entry->keyLength, storedOff);
	return true;
}

bool ArchiveFileStream::loadBlock(uint64_t block) const
{
	if (block == m_curBlock)
		return true;
	try
	{
		const uint64_t blocks = m_entry->blockCount();
		const uint64_t table = m_entry->blockTableSize();
		if (m_blockTable.empty())
		{
			// size_t may be narrower than uint64_t (32-bit targets): refuse tables that do not fit in memory
			if (blocks > m_blockTable.max_size() || table > static_cast<uint64_t>(SIZE_MAX))
			{
				errno = EFBIG;
				return false;
			}
			// Load and decrypt the whole table once: later block lookups are O(1)
			std::vector<uint8_t> raw(static_cast<size_t>(table));
			if (!readStored(raw.data(), raw.size(), 0))
				return false;
			m_blockTable.resize(static_cast<size_t>(blocks));
			for (uint64_t b = 0; b < blocks; ++b)
				m_blockTable[b] = ByteIO::readU64LE(raw.data() + b * ArchiveFormat::BLOCK_ENTRY_SIZE);
		}
		const uint64_t prevEnd = block == 0 ? 0 : (m_blockTable[block - 1] & ArchiveFormat::BLOCK_END_MASK);
		uint64_t start = 0, len = 0;
		bool raw = false;
		// Each item is validated against its neighbor when used, so a corrupt table is caught without a full scan at open
		if (block > 0 && (m_blockTable[block - 1] & ArchiveFormat::BLOCK_END_MASK) == 0)
		{
			errno = EIO;
			return false;
		}
		if (!ArchiveReader::blockSpan(*m_entry, block, prevEnd, m_blockTable[block], start, len, raw))
		{
			errno = EIO;
			return false;
		}
		m_curBlock = UINT64_MAX;
		m_scratch.resize(static_cast<size_t>(len));
		if (!readStored(m_scratch.data(), m_scratch.size(), table + start))
			return false;
		m_block.resize(static_cast<size_t>(m_entry->blockSize()));
		if (!ArchiveReader::decodeBlock(*m_entry, block, m_scratch.data(), m_scratch.size(), raw, m_block.data()))
		{
			errno = EIO;
			return false;
		}
		m_curBlock = block;
		return true;
	}
	catch (const std::bad_alloc&)
	{
		errno = ENOMEM;
		return false;
	}
}

int64_t ArchiveFileStream::readCompressed(uint8_t* out, size_t want) const
{
	const uint8_t log2 = m_entry->blockLog2;
	const uint64_t mask = m_entry->blockSize() - 1;
	size_t done = 0;
	while (done < want)
	{
		const uint64_t pos = static_cast<uint64_t>(m_pos) + done;
		if (!loadBlock(pos >> log2))
			return -1;   // A corrupt block fails the whole read, even if earlier blocks were copied
		const uint64_t inBlock = pos & mask;
		const uint64_t blockLen = std::min<uint64_t>(m_entry->blockSize(), m_entry->size - ((pos >> log2) << log2));
		const size_t n = static_cast<size_t>(std::min<uint64_t>(want - done, blockLen - inBlock));
		std::memcpy(out + done, m_block.data() + inBlock, n);
		done += n;
	}
	return static_cast<int64_t>(done);
}

int64_t ArchiveFileStream::readSource(uint8_t* out, size_t want, int64_t target) const
{
	size_t got = 0;
	if (!m_file)
	{
		// Positional read: there is no cursor, so no seek is needed
		while (got < want)
		{
			int64_t n = m_index->source->readAt(out + got, want - got, static_cast<uint64_t>(target) + got);
			if (n < 0)
				return -1;
			if (n == 0)
				break;
			got += static_cast<size_t>(n);
		}
	}
	else
	{
		if (m_filePos != target)
		{
			// Seek only when the position is not contiguous: sequential reads skip one system call
			if (m_file->seek(target, SEEK_SET) != target)
			{
				m_filePos = -1;
				errno = EIO;
				return -1;
			}
			m_filePos = target;
		}
		while (got < want)
		{
			int n = m_file->read(out + got, static_cast<unsigned int>(want - got));
			if (n < 0)
			{
				m_filePos = -1;
				return -1;
			}
			if (n == 0)
				break;
			got += static_cast<size_t>(n);
		}
		m_filePos = target + static_cast<int64_t>(got);
	}
	// The source hit EOF before the entry ended: the archive was truncated (or replaced while mounted). Report an IO error instead of quietly returning a short read.
	// Return the bytes already read first; the next read reports the error
	if (got == 0)
	{
		errno = EIO;
		return -1;
	}
	return static_cast<int64_t>(got);
}

NS_VFS_END
