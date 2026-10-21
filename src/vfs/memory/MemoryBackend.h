// This file: the memory backend. A read-write directory tree held in memory.
#pragma once

#include "../IBackend.h"
#include "MemoryNode.h"
#include <atomic>
#include <mutex>

NS_VFS_BEGIN

struct MemoryOptions
{
	bool readOnly = false;   // Read-only: data can only be prefilled through addFile
};

/**
 * Memory backend.
 * - A node is a directory or a file holding shared_ptr<MemoryBuffer>; mkdir/rmdir/rename/resize are supported.
 * - Deleting an open file uses unlink semantics: the name is removed immediately, and the stream keeps the data.
 * Thread safety: each directory has its own reader-writer lock (it protects only that directory's children). Lookup walks down from the root, locking the child directory before releasing the parent,
 * so create and delete under different directories do not block each other. rename also holds a global rename lock (only one rename changes the shape of the tree at a time).
 * File data is protected by each buffer's own lock.
 */
class VFS_DLL MemoryBackend : public IBackend
{
public:

	static std::shared_ptr<MemoryBackend> create(const MemoryOptions& options = {});

	explicit MemoryBackend(const MemoryOptions& options = {});

	virtual ~MemoryBackend();

	/**
	 * Prefill a file: parent directories are created automatically (mkdir -p), and an existing file is overwritten. This may also be called in read-only mode.
	 * @param rel  Relative path (it is normalized)
	 * @param data File contents
	 * @return OK; INVALID_PATH if the path is illegal; IS_DIR/NOT_DIR if the path conflicts with an existing directory
	 */
	Error addFile(std::string_view rel, std::vector<uint8_t> data);

	Error addFile(std::string_view rel, std::string_view data);

	void setReadOnly(bool value) { m_readOnly.store(value); }

	bool isReadOnly() const { return m_readOnly.load(); }

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual Error createDir(std::string_view rel) override;

	virtual Error removeDir(std::string_view rel) override;

	virtual Error removeFile(std::string_view rel) override;

	virtual Error rename(std::string_view from, std::string_view to) override;

	virtual const char* typeName() const override { return "memory"; }

private:

	class DirLock;

	/**
	 * Walk down to directory dirRel, locking each level. On return, out holds that directory's lock (an exclusive lock when exclusive is true, otherwise a shared lock).
	 * Intermediate levels hold a shared lock only briefly (the child is locked before the parent is released).
	 * @param from When non-null, start from this directory already locked by the caller (dirRel is relative to it and must not be empty); from's lock is not released
	 * @return OK; NOT_FOUND if some level does not exist; NOT_DIR if some level (including the target itself) is a file
	 */
	Error lockDir(std::string_view dirRel, bool exclusive, DirLock& out, MemoryNode* from = nullptr) const;

private:
	std::mutex m_renameMutex;              // Serializes rename: while a rename is in progress, other renames cannot change the shape of the tree
	std::shared_ptr<MemoryNode> m_root;    // Root directory node
	std::atomic<bool> m_readOnly;
};

NS_VFS_END
