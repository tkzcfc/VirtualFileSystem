// 本文件：.vfs 归档后端（只读，vfs-format F22/F24）。
#pragma once

#include "../IBackend.h"
#include "ArchiveReader.h"

NS_VFS_BEGIN

/**
 * 归档后端。
 * - 挂载时一次性读入并解析索引，之后 stat/list 都在内存中完成，O(1) 或 O(子项数)。
 * - 未压缩文件：每次 open 新建一个独立句柄，按偏移读取（决策 #25）；XOR 文件按位置即时解密。
 * - LZ4 压缩文件：open 时整体读出、解密、解压到内存，返回内存流（final-decisions #6）。
 * - 只读：写模式打开返回 READ_ONLY。需要写入时在上层叠加可写的 Native/Memory 层。
 * 线程安全：索引不可变，可以无限并发。
 */
class VFS_DLL ArchiveBackend : public IBackend
{
public:

	/**
	 * 打开本机归档文件。
	 * @param nativePath 归档路径（UTF-8）
	 * @param options    读取选项（索引密钥、起始偏移）
	 * @param err        可选的错误输出，见 ArchiveReader::open
	 */
	static std::shared_ptr<ArchiveBackend> open(std::string_view nativePath, const ArchiveOptions& options = {},
												Error* err = nullptr);

	/**
	 * 从任意来源打开归档（例如内存，决策 #26）。
	 */
	static std::shared_ptr<ArchiveBackend> open(std::shared_ptr<IArchiveSource> source,
												const ArchiveOptions& options = {}, Error* err = nullptr);

	explicit ArchiveBackend(std::shared_ptr<const ArchiveIndex> index);

	virtual ~ArchiveBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual const char* typeName() const override { return "archive"; }

	/**
	 * 解析后的索引（不可变）。
	 */
	const std::shared_ptr<const ArchiveIndex>& index() const { return m_index; }

	/**
	 * 校验所有文件的内容哈希（离线检查用，开销等于把整个归档读一遍）。
	 */
	Error verifyAll() const;

private:
	std::shared_ptr<const ArchiveIndex> m_index;
};

NS_VFS_END
