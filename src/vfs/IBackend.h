// This file: the backend (mount type) interface.
// A backend only handles its own relative paths and knows nothing about layering; union, copy-up, recursive removal, and similar logic all live in VirtualFileSystem.
#pragma once

#include "Common.h"
#include "Error.h"
#include "IFileStream.h"
#include "Stat.h"
#include "DirEntry.h"

NS_VFS_BEGIN

/**
 * Backend capability bits. Always qualify them with the type name, for example BackendCaps::Write.
 */
enum BackendCaps : uint32_t
{
	Read = 1u << 0,    // Can read
	Write = 1u << 1,   // Can write files and create or delete files and directories
	Rename = 1u << 2,  // Supports atomic rename within the backend
	Resize = 1u << 3,  // Streams support resize
};

/**
 * Backend interface.
 *
 * Path convention: rel is relative to the backend root, already normalized, with no leading '/', and with no `.` or `..`;
 * the empty string is the backend root.
 *
 * Thread safety: every method may be called concurrently from multiple threads; the implementation must guarantee thread safety itself.
 * Ownership: the backend is held by std::shared_ptr; a stream returned by open should keep alive any data it needs
 * (for example an archive index or a memory buffer), so the stream remains usable after unmount.
 */
class VFS_DLL IBackend
{
public:
	virtual ~IBackend() = default;

	/**
	 * @return A combination of BackendCaps bits
	 */
	virtual uint32_t capabilities() const = 0;

	/**
	 * Query metadata.
	 * @param rel Relative path
	 * @param out Output metadata (the mount and writable fields are filled in by the VFS; the backend ignores them)
	 * @return OK; NOT_FOUND if missing; NOT_DIR if an intermediate component is a file
	 */
	virtual Error stat(std::string_view rel, Stat& out) const = 0;

	/**
	 * Open a file stream.
	 * @param rel  Relative path
	 * @param mode Open mode; see IFileStream.h for the semantics
	 * @param err  Optional error output
	 * @return The stream on success (ownership passes to the caller), or nullptr on failure.
	 *         If rel is a directory this must fail with IS_DIR (when there is only one layer the VFS calls open directly without a prior stat, and relies on this)
	 */
	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) = 0;

	/**
	 * List a directory. Order is not required; the VFS sorts.
	 * @param relDir Relative directory
	 * @param cb     Callback; return false to stop the walk
	 */
	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const = 0;

	/**
	 * Create a single directory level. The parent must exist.
	 */
	virtual Error createDir(std::string_view /*rel*/) { return Error::READ_ONLY; }

	/**
	 * Remove an empty directory.
	 */
	virtual Error removeDir(std::string_view /*rel*/) { return Error::READ_ONLY; }

	/**
	 * Remove a file.
	 */
	virtual Error removeFile(std::string_view /*rel*/) { return Error::READ_ONLY; }

	/**
	 * Atomic rename within the backend. Unsupported by default (consistent with BackendCaps omitting Rename; returns UNSUPPORTED).
	 */
	virtual Error rename(std::string_view /*from*/, std::string_view /*to*/) { return Error::UNSUPPORTED; }

	/**
	 * Return the native path (only native backends return a non-empty string), for modules that must use a real path.
	 */
	virtual std::string realPath(std::string_view /*rel*/) const { return std::string(); }

	/**
	 * Backend type name, for example "native", "memory", or "archive", used in logs and debugging.
	 */
	virtual const char* typeName() const = 0;
};

NS_VFS_END
