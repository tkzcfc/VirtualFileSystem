// This file: the native directory backend. Mounts a native directory onto a virtual path.
#pragma once

#include "../IBackend.h"

NS_VFS_BEGIN

/**
 * Options for the native backend.
 */
struct NativeOptions
{
	bool readOnly = false;        // Read-only: every write returns READ_ONLY
	bool followSymlinks = true;   // When false, any path that escapes the root after resolution is rejected
	bool strictCase = false;      // When true, check the real filename's case and warn on a mismatch (the result is unchanged)
	bool exactCase = false;       // When true, match names byte for byte: on a case-insensitive filesystem (Windows, and macOS by default),
	                              // a name that differs only in case is treated as missing, matching Linux. Each lookup costs one extra syscall per component
	bool createRoot = false;      // Create the root directory if it does not exist (mkdir -p)
};

/**
 * Native directory backend.
 * - No caching; every call hits the filesystem, so external changes are visible immediately.
 * - Path segments additionally reject ':' and Windows reserved names, so behavior matches across platforms.
 * Thread safety: no shared mutable state; calls may run concurrently without limit.
 */
class VFS_DLL NativeBackend : public IBackend
{
public:

	/**
	 * Create a native backend.
	 * @param rootDir Native directory (UTF-8; a relative path is converted to absolute)
	 * @param options Options
	 * @param err     Optional error output: NOT_FOUND if the directory is missing, NOT_DIR if it is not a directory
	 * @return The backend on success, or nullptr on failure
	 */
	static std::shared_ptr<NativeBackend> create(std::string_view rootDir, const NativeOptions& options = {},
												 Error* err = nullptr);

	NativeBackend(std::string root, const NativeOptions& options);

	virtual ~NativeBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual Error createDir(std::string_view rel) override;

	virtual Error removeDir(std::string_view rel) override;

	virtual Error removeFile(std::string_view rel) override;

	virtual Error rename(std::string_view from, std::string_view to) override;

	virtual std::string realPath(std::string_view rel) const override;

	virtual const char* typeName() const override { return "native"; }

	/**
	 * Native root directory (absolute path, '/' separated, UTF-8).
	 */
	const std::string& root() const { return m_root; }

	const NativeOptions& options() const { return m_options; }

private:

	// Convert a relative path to a native absolute path, and check path segments and symlink escape.
	Error toNative(std::string_view rel, std::string& out) const;

	// Compare rel, component by component, with the real names on disk (used by strictCase / exactCase).
	// NOT_FOUND: some parent directory differs only in case (under byte-wise matching that parent does not exist);
	// OK with lastMismatch=true: the final component exists under a name that differs only in case; otherwise OK
	Error matchCase(std::string_view rel, bool& lastMismatch) const;

	// In strictCase mode, check case and only warn; the result is unchanged.
	void checkCase(std::string_view rel) const;

private:
	std::string m_root;            // Native root directory
	std::string m_canonicalRoot;   // Canonical root used for escape checks when followSymlinks=false
	NativeOptions m_options;
};

NS_VFS_END
