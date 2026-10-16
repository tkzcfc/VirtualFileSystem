// 本文件：内存文件流。既用于内存后端，也用于归档中压缩文件解压后的数据（只读模式）。
#pragma once

#include "../IFileStream.h"
#include "MemoryNode.h"

NS_VFS_BEGIN

/**
 * 内存文件流。
 * - 由后端工厂构造，公开的 open(path, mode) 返回 false 并设置 errno=ENOTSUP。
 * - READ 模式下 write/resize 返回失败（errno=EBADF）。
 * - APPEND 模式每次 write 都写到末尾；seek 越界后 write 会用 0 填充空洞。
 * 线程安全：单个流对象不能跨线程使用；多个流共享同一缓冲区时由缓冲区的锁保证内存安全。
 */
class VFS_DLL MemoryFileStream : public IFileStream
{
public:

	/**
	 * @param buffer 数据缓冲区（共享所有权）
	 * @param mode   打开模式；截断（WRITE）由调用方在构造前完成
	 */
	MemoryFileStream(std::shared_ptr<MemoryBuffer> buffer, IFileStream::Mode mode);

	virtual ~MemoryFileStream();

	virtual bool open(std::string_view path, IFileStream::Mode mode) override;

	virtual int close() override;

	virtual int64_t seek(int64_t offset, int origin) const override;

	virtual int read(void* buf, unsigned int size) const override;

	virtual int write(const void* buf, unsigned int size) const override;

	virtual int64_t tell() const override;

	virtual int64_t size() const override;

	virtual bool resize(int64_t size) const override;

	virtual bool isOpen() const override;

private:
	std::shared_ptr<MemoryBuffer> m_buffer;   // 关闭后置空
	IFileStream::Mode m_mode;
	mutable int64_t m_pos;                    // 当前位置（axmol 接口要求 const 方法）
};

NS_VFS_END
