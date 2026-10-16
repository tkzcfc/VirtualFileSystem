// 本文件：本机文件流，参照 axmol `FileStream` 的 lowio 实现。
// POSIX 上使用 open/lseek64/read/write；Windows 上使用 CreateFileW 系列（UTF-8 转 UTF-16）。
#pragma once

#include "../Common.h"
#include "../IFileStream.h"

NS_VFS_BEGIN

/**
 * 本机文件流。
 * - 与 axmol FileStream 相同的打开语义：READ=O_RDONLY；WRITE=O_CREAT|O_RDWR|O_TRUNC；
 *   APPEND=O_APPEND|O_CREAT|O_RDWR；OVERLAPPED=O_CREAT|O_RDWR。
 * - Windows 上额外带 FILE_SHARE_DELETE，打开期间允许删除/替换文件。
 * - 打开目录会失败（errno=EISDIR）。
 * 线程安全：单个对象不能跨线程同时使用；不同对象互不影响。只能移动，不能拷贝。
 */
class VFS_DLL NativeFileStream : public IFileStream
{
public:

	NativeFileStream();

	NativeFileStream(NativeFileStream&& other) noexcept;

	NativeFileStream& operator=(NativeFileStream&& other) noexcept;

	NativeFileStream(const NativeFileStream&) = delete;

	NativeFileStream& operator=(const NativeFileStream&) = delete;

	virtual ~NativeFileStream();

	/**
	 * 打开本机文件。
	 * @param path UTF-8 本机路径
	 * @param mode 打开模式
	 * @return 成功返回 true；失败返回 false 并设置 errno
	 */
	virtual bool open(std::string_view path, IFileStream::Mode mode) override;

	virtual int close() override;

	virtual int64_t seek(int64_t offset, int origin) const override;

	virtual int read(void* buf, unsigned int size) const override;

	virtual int write(const void* buf, unsigned int size) const override;

	virtual int64_t tell() const override;

	virtual int64_t size() const override;

	virtual bool resize(int64_t size) const override;

	virtual bool isOpen() const override;

	virtual osfhnd_t nativeHandle() const override;

private:
	osfhnd_t m_handle;   // 本机句柄；未打开时为 (osfhnd_t)-1
	bool m_append;       // APPEND 模式：Windows 上每次写前 seek 到末尾
};

NS_VFS_END
