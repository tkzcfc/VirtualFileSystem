// This file: the single-file backend. Mounts one native file onto a virtual path, equivalent to a Linux file bind mount.
#pragma once

#include "../IBackend.h"

NS_VFS_BEGIN

/**
 * Single-file backend: it contains one entry (the relative path is the empty string).
 * Any path under the mount point returns NOT_DIR. Use it to overlay a single config or debug file.
 * Thread safety: no shared mutable state.
 */
class VFS_DLL SingleFileBackend : public IBackend
{
public:

	/**
	 * Create a single-file backend.
	 * @param nativeFile Native file path (UTF-8); must be an existing regular file
	 * @param readOnly   When true, writes are forbidden
	 * @param err        Optional error output
	 */
	static std::shared_ptr<SingleFileBackend> create(std::string_view nativeFile, bool readOnly = false,
													 Error* err = nullptr);

	SingleFileBackend(std::string nativeFile, bool readOnly);

	virtual ~SingleFileBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual Error createDir(std::string_view rel) override;

	virtual Error removeFile(std::string_view rel) override;

	virtual std::string realPath(std::string_view rel) const override;

	virtual const char* typeName() const override { return "file"; }

	const std::string& nativeFile() const { return m_nativeFile; }

private:
	std::string m_nativeFile;
	bool m_readOnly;
};

NS_VFS_END
