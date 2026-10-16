// 本文件：.vfs 归档写入（CLI 与应用共用）。
// 流程：占位 header -> 按 path 顺序写数据（对齐、去重、可选压缩/加密）-> 写索引 -> 写 footer -> 回填 header。
#pragma once

#include "ArchiveFormat.h"
#include "../Error.h"
#include "../IFileStream.h"

NS_VFS_BEGIN

/**
 * 归档级写入选项。
 */
struct ArchiveWriteOptions
{
	uint32_t alignment = ArchiveFormat::DEFAULT_ALIGNMENT;   // 数据对齐，2 的幂，1 表示不对齐
	bool encryptIndex = true;                                // 用 ChaCha20 加密索引区
	std::vector<uint8_t> indexKey;                           // 空：由索引内容确定性派生并存入文件；否则必须 32 字节
	bool storeKey = false;                                   // indexKey 非空时是否把它也存进文件
	uint32_t fileKeyLength = ArchiveFormat::DEFAULT_FILE_KEY_SIZE;  // 单文件 XOR key 长度，1..32
	bool dedup = true;                                       // 内容相同的文件只存一份
	int compressionLevel = 9;                                // LZ4 HC 级别 1..12
};

/**
 * 单文件写入选项（由打包规则决定）。
 */
struct FileWriteOptions
{
	bool encrypt = false;    // XOR 加密
	bool compress = false;   // LZ4 压缩（压缩后不变小时自动存原文）
	int64_t mtime = 0;       // 写入索引的 mtime；默认 0 保证可复现
};

/**
 * 写入统计。
 */
struct ArchiveWriteStats
{
	uint32_t fileCount = 0;        // 文件条目数
	uint32_t dirCount = 0;         // 空目录条目数
	uint32_t dedupCount = 0;       // 与前面文件共享数据的条目数
	uint32_t encryptedCount = 0;   // XOR 加密的文件数
	uint32_t compressedCount = 0;  // 实际压缩存储的文件数
	uint64_t totalSize = 0;        // 原始内容总字节数
	uint64_t storedSize = 0;       // 数据区实际写入的字节数（不含填充）
	uint64_t archiveSize = 0;      // 归档总字节数
};

/**
 * 归档写入器。
 * 输出可复现：条目和数据都按 path 字节序排列，mtime 默认 0，每文件 XOR key 与索引密钥/nonce
 * 都由内容确定性派生，同一输入每次输出逐字节一致。
 * 线程安全：单个对象不能跨线程使用。
 */
class VFS_DLL ArchiveWriter
{
public:

	explicit ArchiveWriter(const ArchiveWriteOptions& options = {});

	~ArchiveWriter();

	/**
	 * 添加本机文件。文件内容在 write() 时才读取。
	 * @param archivePath 归档内路径（会被规范化为无前导 '/' 的相对路径）
	 * @param nativePath  本机文件路径（UTF-8）
	 */
	Error addFile(std::string_view archivePath, std::string_view nativePath, const FileWriteOptions& options = {});

	/**
	 * 添加内存数据。
	 */
	Error addData(std::string_view archivePath, std::vector<uint8_t> data, const FileWriteOptions& options = {});

	/**
	 * 添加目录。只有最终为空的目录才会写入 Dir 条目，非空目录由文件路径推导。
	 */
	Error addDir(std::string_view archivePath);

	size_t pendingCount() const { return m_pending.size(); }

	/**
	 * 写到本机文件：先写 out.tmp，成功后 rename，保证原子性。
	 */
	Error write(std::string_view outPath, ArchiveWriteStats* stats = nullptr);

	/**
	 * 写到任意可读写、可 seek 的流（从位置 0 开始）。
	 */
	Error write(IFileStream& out, ArchiveWriteStats* stats = nullptr);

	/**
	 * 最近一次失败的说明（例如冲突的路径），用于 CLI 提示。
	 */
	const std::string& lastError() const { return m_lastError; }

private:
	struct Pending
	{
		std::string path;                              // 规范相对路径
		EntryType type = EntryType::FILE;
		std::string nativePath;                        // 本机文件（与 data 二选一）
		std::shared_ptr<std::vector<uint8_t>> data;    // 内存数据
		FileWriteOptions options;
	};

	// 排序并检查重复、大小写冲突、文件/目录冲突；去掉非空目录的 Dir 条目
	Error prepare(std::vector<Pending>& sorted);

	Error fail(Error err, std::string message);

private:
	ArchiveWriteOptions m_options;
	std::vector<Pending> m_pending;
	std::string m_lastError;
};

NS_VFS_END
