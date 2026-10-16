// 本文件：归档的数据来源抽象（决策 #25、#26）。归档可以来自本机文件，也可以来自内存（例如嵌入在可执行文件里）。
#pragma once

#include "../IFileStream.h"
#include "../Error.h"
#include "../memory/MemoryNode.h"

NS_VFS_BEGIN

/**
 * 归档数据来源。
 * 线程安全：openStream 可以被多个线程并发调用；每次调用返回一个独立的只读流（独立的 OS 句柄或独立的游标），
 * 所以多个线程读同一归档时互不加锁（决策 #25）。
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
	 * 来源的修改时间（Unix 秒），条目 mtime 为 0 时用它代替（决策 #27）。
	 */
	virtual int64_t mtime() const { return 0; }

	/**
	 * 描述（本机路径或 "<memory>"），用于日志。
	 */
	virtual std::string describe() const = 0;
};

/**
 * 本机文件来源：每次 openStream 都打开一个新的 OS 句柄。
 */
class VFS_DLL FileArchiveSource : public IArchiveSource
{
public:

	explicit FileArchiveSource(std::string nativePath);

	virtual std::unique_ptr<IFileStream> openStream(Error* err) const override;

	virtual int64_t mtime() const override;

	virtual std::string describe() const override { return m_nativePath; }

private:
	std::string m_nativePath;
	int64_t m_mtime;   // 构造时记录，保证同一次挂载内稳定
};

/**
 * 内存来源：数据由 shared_ptr 共享，每次 openStream 返回一个独立游标的只读内存流。
 */
class VFS_DLL MemoryArchiveSource : public IArchiveSource
{
public:

	explicit MemoryArchiveSource(std::vector<uint8_t> data);

	explicit MemoryArchiveSource(std::shared_ptr<MemoryBuffer> buffer);

	virtual std::unique_ptr<IFileStream> openStream(Error* err) const override;

	virtual std::string describe() const override { return "<memory>"; }

private:
	std::shared_ptr<MemoryBuffer> m_buffer;
};

NS_VFS_END
