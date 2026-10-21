#include "Error.h"
#include <cerrno>

NS_VFS_BEGIN

int toErrno(Error err)
{
	switch (err)
	{
	case Error::OK:
		return 0;
	case Error::NOT_FOUND:
		return ENOENT;
	case Error::EXISTS:
		return EEXIST;
	case Error::NOT_DIR:
		return ENOTDIR;
	case Error::IS_DIR:
		return EISDIR;
	case Error::NOT_EMPTY:
		return ENOTEMPTY;
	case Error::READ_ONLY:
		return EROFS;
	case Error::CROSS_DEVICE:
		return EXDEV;
	case Error::BUSY:
		return EBUSY;
	case Error::INVALID_PATH:
		return EINVAL;
	case Error::PERMISSION_DENIED:
		return EACCES;
	case Error::UNSUPPORTED:
		return ENOTSUP;
	case Error::KEY_REQUIRED:
	case Error::KEY_INVALID:
		// No closer errno; treat it as "permission denied".
		return EACCES;
	case Error::IO_ERROR:
	case Error::CORRUPTED:
	case Error::VERSION_MISMATCH:
	default:
		return EIO;
	}
}

const char* errorName(Error err)
{
	switch (err)
	{
	case Error::OK:
		return "OK";
	case Error::NOT_FOUND:
		return "NOT_FOUND";
	case Error::EXISTS:
		return "EXISTS";
	case Error::NOT_DIR:
		return "NOT_DIR";
	case Error::IS_DIR:
		return "IS_DIR";
	case Error::NOT_EMPTY:
		return "NOT_EMPTY";
	case Error::READ_ONLY:
		return "READ_ONLY";
	case Error::CROSS_DEVICE:
		return "CROSS_DEVICE";
	case Error::BUSY:
		return "BUSY";
	case Error::INVALID_PATH:
		return "INVALID_PATH";
	case Error::PERMISSION_DENIED:
		return "PERMISSION_DENIED";
	case Error::IO_ERROR:
		return "IO_ERROR";
	case Error::UNSUPPORTED:
		return "UNSUPPORTED";
	case Error::CORRUPTED:
		return "CORRUPTED";
	case Error::VERSION_MISMATCH:
		return "VERSION_MISMATCH";
	case Error::KEY_REQUIRED:
		return "KEY_REQUIRED";
	case Error::KEY_INVALID:
		return "KEY_INVALID";
	}
	return "UNKNOWN";
}

const char* errorMessage(Error err)
{
	switch (err)
	{
	case Error::OK:
		return "success";
	case Error::NOT_FOUND:
		return "no such file or directory";
	case Error::EXISTS:
		return "already exists";
	case Error::NOT_DIR:
		return "not a directory";
	case Error::IS_DIR:
		return "is a directory";
	case Error::NOT_EMPTY:
		return "directory not empty";
	case Error::READ_ONLY:
		return "read-only";
	case Error::CROSS_DEVICE:
		return "cannot move across backends";
	case Error::BUSY:
		return "resource busy (mount point or open streams)";
	case Error::INVALID_PATH:
		return "invalid path";
	case Error::PERMISSION_DENIED:
		return "permission denied";
	case Error::IO_ERROR:
		return "I/O error";
	case Error::UNSUPPORTED:
		return "operation not supported";
	case Error::CORRUPTED:
		return "archive corrupted";
	case Error::VERSION_MISMATCH:
		return "archive version mismatch";
	case Error::KEY_REQUIRED:
		return "key required";
	case Error::KEY_INVALID:
		return "invalid key";
	}
	return "unknown error";
}

NS_VFS_END
