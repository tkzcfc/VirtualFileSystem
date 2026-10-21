// 本文件：归档的数据来源抽象。归档可以来自本机文件，也可以来自内存（例如嵌入在可执行文件里）。
#pragma once

#include "../IFileStream.h"
#include "../Error.h"
#include "../memory/MemoryNode.h"

NS_VFS_BEGIN

/**
 * 归档数据来源。
 * 线程安全：openStream / readAt 可以被多个线程并发调用。
 * - 支持位置读（supportsReadAt() 为 true）的来源：归档内的文件流直接用 readAt 读取，打开文件时没有系统调用，
 *   多个线程读同一归档时互不加锁；
 * - 其他来源：每个流调用 openStream 拿一个独立的只读流（独立的 OS 句柄或独立的游标）。
 */
class VFS_DLL IArchiveSource
{
public:
	virtual ~IArchiveSource() = default;

	/**
	 * 打开一个新的只读流，位置为 0。
	 */
	virtual std::unique_ptr<IFileStream> openStream(Error* err) const = 0;

	/**
	 * 是否支持 readAt。返回 true 时，readAt 必须可以被多个线程同时调用。
	 */
	virtual bool supportsReadAt() const { return false; }

	/**
	 * 位置读：从 offset 处读取最多 size 字节，不改变任何共享状态。
	 * @return 实际读取的字节数（到达末尾时可能少于 size），出错返回 -1（errno 已设置）。默认实现不支持，返回 -1（ENOTSUP）
	 */
	virtual int64_t readAt(void* buf, size_t size, uint64_t offset) const;

	/**
	 * 来源的修改时间（Unix 秒），条目 mtime 为 0 时用它代替。
	 */
	virtual int64_t mtime() const { return 0; }

	/**
	 * 描述（本机路径或 "<memory>"），用于日志。
	 */
	virtual std::string describe() const = 0;
};

namespace native
{
class PositionalReader;
}

/**
 * 本机文件来源：构造时打开一个共享的只读句柄，所有流通过它做位置读（POSIX pread / Windows 重叠 ReadFile），
 * 打开归档内的文件不需要再打开 OS 句柄。句柄允许其他进程删除、改名、替换归档文件。
 * 共享句柄打开失败时退回“每次 openStream 打开一个新句柄”。
 */
class VFS_DLL FileArchiveSource : public IArchiveSource
{
public:

	explicit FileArchiveSource(std::string nativePath);

	virtual ~FileArchiveSource();

	virtual std::unique_ptr<IFileStream> openStream(Error* err) const override;

	virtual bool supportsReadAt() const override;

	virtual int64_t readAt(void* buf, size_t size, uint64_t offset) const override;

	virtual int64_t mtime() const override;

	virtual std::string describe() const override { return m_nativePath; }

private:
	std::string m_nativePath;
	int64_t m_mtime;   // 构造时记录，保证同一次挂载内稳定
	std::unique_ptr<native::PositionalReader> m_reader;   // 共享的位置读句柄（打开失败时为空）
};

/**
 * 内存来源：数据由 shared_ptr 共享，每次 openStream 返回一个独立游标的只读内存流；readAt 直接复制。
 * 数据在挂载期间不能被修改。
 */
class VFS_DLL MemoryArchiveSource : public IArchiveSource
{
public:

	explicit MemoryArchiveSource(std::vector<uint8_t> data);

	explicit MemoryArchiveSource(std::shared_ptr<MemoryBuffer> buffer);

	virtual std::unique_ptr<IFileStream> openStream(Error* err) const override;

	virtual bool supportsReadAt() const override { return true; }

	virtual int64_t readAt(void* buf, size_t size, uint64_t offset) const override;

	virtual std::string describe() const override { return "<memory>"; }

private:
	std::shared_ptr<MemoryBuffer> m_buffer;
};

NS_VFS_END
