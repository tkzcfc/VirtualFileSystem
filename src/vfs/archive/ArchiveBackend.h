// This file: the .vfs archive backend (read-only).
#pragma once

#include "../IBackend.h"
#include "ArchiveReader.h"

NS_VFS_BEGIN

/**
 * Archive backend.
 * - On mount the index is read and parsed once; later stat/list run entirely in memory, O(1) or O(child count).
 * - Uncompressed files: each open creates an independent handle and reads by offset; XOR files are decrypted in place by position.
 * - LZ4 compressed files: block-compressed; the stream decompresses only the blocks a read touches (see ArchiveFileStream).
 * - Read-only: opening for write returns READ_ONLY. To write, stack a writable Native/Memory layer above.
 * Thread safety: the index is immutable and may be used with unlimited concurrency.
 */
class VFS_DLL ArchiveBackend : public IBackend
{
public:

	/**
	 * Open a native archive file (named create rather than open, to avoid clashing with the IBackend::open overload).
	 * @param nativePath archive path (UTF-8)
	 * @param options    read options (index key, start offset)
	 * @param err        optional error output; see ArchiveReader::open
	 */
	static std::shared_ptr<ArchiveBackend> create(std::string_view nativePath, const ArchiveOptions& options = {},
												  Error* err = nullptr);

	/**
	 * Open an archive from any source (for example memory).
	 */
	static std::shared_ptr<ArchiveBackend> create(std::shared_ptr<IArchiveSource> source,
												  const ArchiveOptions& options = {}, Error* err = nullptr);

	explicit ArchiveBackend(std::shared_ptr<const ArchiveIndex> index);

	virtual ~ArchiveBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual const char* typeName() const override { return "archive"; }

	/**
	 * The parsed index (immutable).
	 */
	const std::shared_ptr<const ArchiveIndex>& index() const { return m_index; }

	/**
	 * Check the content hash of every file (offline check; the cost is one full read of the archive).
	 */
	Error verifyAll() const;

private:
	std::shared_ptr<const ArchiveIndex> m_index;
};

NS_VFS_END
