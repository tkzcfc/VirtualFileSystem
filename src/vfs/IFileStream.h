// 本文件：流接口 IFileStream。
// 与 axmol 引擎的 IFileStream 逐字一致（签名、枚举顺序、返回值约定都相同），只是命名空间和导出宏不同；
// 定义 VFS_WITH_AXMOL 时直接使用引擎的定义，库内实现无需任何改动。
//
// 语义约定（所有内置实现一致）：
//   READ       只读，文件必须存在
//   WRITE      创建或截断，可读可写
//   APPEND     不存在则创建，每次 write 都写到末尾，可读
//   OVERLAPPED 不存在则创建，不截断，随机读写
//   seek 结果为负返回 -1（errno=EINVAL）；允许 seek 到 EOF 之后，之后 read 返回 0，write 会用 0 填充空洞。
//   单个流对象不是线程安全的；不同流对象可以完全并发。
#pragma once

#include "Common.h"

#if VFS_WITH_AXMOL

// 接入 axmol 时直接复用引擎的接口。
// 较新的 axmol（dev 分支）头文件位于 axmol/platform/，2.x 位于 core/platform/（include 路径为 platform/）
#    if __has_include("axmol/platform/IFileStream.h")
#        include "axmol/platform/IFileStream.h"
#    else
#        include "platform/IFileStream.h"
#    endif

NS_VFS_BEGIN
using IFileStream = ax::IFileStream;
using osfhnd_t = ax::osfhnd_t;
NS_VFS_END

#else

#    include <errno.h>
#    include <stdint.h>
#    include <string>

NS_VFS_BEGIN

#    if VFS_PLATFORM_WINDOWS
using osfhnd_t = void*;
#    else
using osfhnd_t = int;
#    endif

class VFS_DLL IFileStream
{
public:
	virtual ~IFileStream() = default;

	enum class Mode
	{
		READ,
		WRITE,
		APPEND,
		OVERLAPPED,
	};

	/**
	 *  Open a file
	 *  @param path file to open
	 *  @param mode File open mode, being READ, WRITE, APPEND
	 *  @return true if successful, false if not
	 */
	virtual bool open(std::string_view path, IFileStream::Mode mode) = 0;

	/**
	 *  Close a file stream
	 *  @return 0 if successful, -1 if not
	 */
	virtual int close() = 0;

	/**
	 *  Seek to position in a file stream
	 *  @param offset how many bytes to move within the stream
	 *  @param origin SEEK_SET, SEEK_CUR, SEEK_END
	 *  @return offset from file begining
	 */
	virtual int64_t seek(int64_t offset, int origin) const = 0;

	/**
	 *  Read data from file stream
	 *  @param buf pointer to data
	 *  @param size the amount of data to read in bytes
	 *  @return amount of data read successfully, -1 if error
	 */
	virtual int read(void* buf, unsigned int size) const = 0;

	/**
	 *  Write data to file stream
	 *  @param buf pointer to data
	 *  @param size the amount of data to write in bytes
	 *  @return amount of data written successfully, -1 if error
	 */
	virtual int write(const void* buf, unsigned int size) const = 0;

	/**
	 *  Get the current position in the file stream
	 *  @return current position, -1 if error
	 */
	virtual int64_t tell() const = 0;

	/**
	 *  Get the size of the file stream
	 *  @return stream size, -1 if error (Mode::WRITE and Mode::APPEND may return -1)
	 */
	virtual int64_t size() const = 0;

	/*
	 * Resize file
	 */
	virtual bool resize(int64_t /*size*/) const
	{
		errno = ENOTSUP;
		return false;
	}

	/**
	 *  Get status of file stream
	 *  @return true if open, false if closed
	 */
	virtual bool isOpen() const = 0;

	/*
	 * Get native handle if support
	 * @returns -1 if not a local disk file or file not open
	 *   Windows: HANDLE
	 *   Other: file descriptor
	 */
	virtual osfhnd_t nativeHandle() const { return (osfhnd_t)-1; }

	virtual operator bool() const { return isOpen(); }

protected:
	IFileStream(){};
};

NS_VFS_END

#endif
