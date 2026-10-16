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
		// 没有更贴切的 errno，按“没有权限”处理
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
		return "成功";
	case Error::NOT_FOUND:
		return "文件或目录不存在";
	case Error::EXISTS:
		return "已经存在";
	case Error::NOT_DIR:
		return "不是目录";
	case Error::IS_DIR:
		return "是目录";
	case Error::NOT_EMPTY:
		return "目录非空";
	case Error::READ_ONLY:
		return "只读";
	case Error::CROSS_DEVICE:
		return "不能跨后端移动";
	case Error::BUSY:
		return "资源忙（挂载点或仍有打开的流）";
	case Error::INVALID_PATH:
		return "路径不合法";
	case Error::PERMISSION_DENIED:
		return "没有权限";
	case Error::IO_ERROR:
		return "IO 错误";
	case Error::UNSUPPORTED:
		return "不支持的操作";
	case Error::CORRUPTED:
		return "归档已损坏";
	case Error::VERSION_MISMATCH:
		return "归档版本不匹配";
	case Error::KEY_REQUIRED:
		return "需要密钥";
	case Error::KEY_INVALID:
		return "密钥错误";
	}
	return "未知错误";
}

NS_VFS_END
