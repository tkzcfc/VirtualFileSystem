// 本文件：归档中单个未压缩文件的只读流。明文与 XOR 加密共用一个类。
#pragma once

#include "../Common.h"
#include "../IFileStream.h"
#include "ArchiveReader.h"

NS_VFS_BEGIN

/**
 * 归档中单个文件的只读流。
 * 来源支持位置读（本机文件、内存）时直接调用 IArchiveSource::readAt，流本身不持有 OS 句柄；
 * 否则持有一个独立的来源流，按偏移读取。两种方式都不加锁。
 * - 来源比索引记录的短（归档被截断或在挂载期间被替换）：read 返回 -1（errno=EIO），不会把截断的数据当成完整文件。
 * - seek 只修改 m_pos，越界规则与 POSIX 一致。
 * - read：先 clamp 到 [0, size)，再从底层句柄的 base+offset+pos 处读取，XOR 文件按位置即时解密。
 * - write 返回 -1（errno=EROFS），resize 返回 false（errno=ENOTSUP），nativeHandle() 返回 -1。
 * 同一个对象不能跨线程使用。
 */
class VFS_DLL ArchiveFileStream : public IFileStream
{
public:

	/**
	 * @param index 归档索引（共享所有权，保活索引和密钥，惰性卸载时用）
	 * @param entry 条目，指向 index 内部，生命周期由 index 保证
	 * @param file  独立的来源流；为空时使用 index->source->readAt（来源必须 supportsReadAt()）
	 */
	ArchiveFileStream(std::shared_ptr<const ArchiveIndex> index, const ArchiveEntry* entry,
					  std::unique_ptr<IFileStream> file);

	virtual ~ArchiveFileStream();

	virtual bool open(std::string_view path, IFileStream::Mode mode) override;

	virtual int close() override;

	virtual int64_t seek(int64_t offset, int origin) const override;

	/**
	 * 读取数据。加密文件按位置即时解密。
	 * @param buf  输出缓冲区
	 * @param size 期望读取的字节数
	 * @return 实际读取的字节数，EOF 返回 0，出错返回 -1（errno 已设置）
	 */
	virtual int read(void* buf, unsigned int size) const override;

	virtual int write(const void* buf, unsigned int size) const override;

	virtual bool resize(int64_t size) const override;

	virtual int64_t tell() const override;

	virtual int64_t size() const override;

	virtual bool isOpen() const override;

private:
	// 从来源的绝对偏移 target 读取，返回字节数 / -1
	int64_t readSource(uint8_t* out, size_t want, int64_t target) const;

	std::shared_ptr<const ArchiveIndex> m_index;   // 保活索引和密钥
	const ArchiveEntry* m_entry;                   // 当前条目
	std::unique_ptr<IFileStream> m_file;           // 独立句柄（来源不支持位置读时）
	bool m_open;                                   // close() 之后为 false
	int64_t m_dataStart;                           // 存储数据在来源中的绝对偏移
	mutable int64_t m_pos;                         // 当前位置（axmol 接口要求 const）
	mutable int64_t m_filePos;                     // 底层句柄的当前位置，用于省掉连续读时的 seek；-1 表示未知
};

NS_VFS_END
