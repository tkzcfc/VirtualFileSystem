// This file: error codes for the VFS layer.
// The stream layer keeps axmol's "-1 + errno" style; VFS-layer APIs all return vfs::Error.
#pragma once

#include "Common.h"
#include <cerrno>

NS_VFS_BEGIN

/**
 * VFS error codes. Exceptions are not used.
 * Each code maps through toErrno() to the closest POSIX errno.
 */
enum class Error
{
	OK,                 // success
	NOT_FOUND,          // ENOENT: file or directory does not exist
	EXISTS,             // EEXIST: already exists
	NOT_DIR,            // ENOTDIR: an intermediate path component is not a directory, or a directory operation was applied to a file
	IS_DIR,             // EISDIR: a file operation was applied to a directory
	NOT_EMPTY,          // ENOTEMPTY: directory is not empty
	READ_ONLY,          // EROFS: read-only layer, no writable layer, or the write would be hidden
	CROSS_DEVICE,       // EXDEV: rename across backends
	BUSY,               // EBUSY: a mount point cannot be removed or renamed, or a strict unmount still has open streams
	INVALID_PATH,       // EINVAL: illegal path (`..` escapes the root, control characters, Windows reserved names, and so on)
	PERMISSION_DENIED,  // EACCES: the operating system refused, or a symlink escapes the root
	IO_ERROR,           // EIO: underlying IO failed
	UNSUPPORTED,        // ENOTSUP: the backend does not support this operation
	CORRUPTED,          // archive is corrupt (magic, checksum, or illegal index contents)
	VERSION_MISMATCH,   // archive version does not match this program
	KEY_REQUIRED,       // the archive index is encrypted, but neither a stored key nor a supplied key is available
	KEY_INVALID,        // the supplied key is wrong, or its format is wrong
};

/**
 * Convert an error code to a POSIX errno value (for example Error::NOT_FOUND -> ENOENT).
 */
VFS_DLL int toErrno(Error err);

/**
 * Return the English name of an error code (for example "NOT_FOUND"), for logs and CLI output.
 */
VFS_DLL const char* errorName(Error err);

/**
 * Return the English description of an error code, for CLI output and logs.
 */
VFS_DLL const char* errorMessage(Error err);

// Helper: write the error into an optional out parameter; on failure also set errno, for compatibility with the axmol style.
inline void setError(Error* out, Error err)
{
	if (out)
		*out = err;
	if (err != Error::OK)
		errno = toErrno(err);
}

NS_VFS_END
