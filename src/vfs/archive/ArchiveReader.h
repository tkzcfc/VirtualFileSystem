// 本文件：.vfs 归档读取：解析 header/footer/index、快速损坏校验、索引解密、构建目录树。
#pragma once

#include "ArchiveFormat.h"
#include "ArchiveSource.h"
#include "../DirEntry.h"
#include <array>
#include <unordered_map>

NS_VFS_BEGIN

/**
 * Header 的内存表示。字段含义与偏移见 ArchiveFormat.h。
 */
struct ArchiveHeader
{
	uint32_t version = ArchiveFormat::VERSION;
	uint32_t headerSize = ArchiveFormat::HEADER_SIZE;
	uint32_t flags = 0;
	uint32_t alignment = ArchiveFormat::DEFAULT_ALIGNMENT;
	uint32_t entryCount = 0;
	uint32_t entrySize = ArchiveFormat::ENTRY_SIZE;
	uint64_t dataOffset = 0;
	uint64_t dataSize = 0;
	uint64_t indexOffset = 0;
	uint64_t indexSize = 0;
	std::array<uint8_t, ArchiveFormat::INDEX_KEY_SIZE> indexKey{};
	std::array<uint8_t, ArchiveFormat::INDEX_NONCE_SIZE> indexNonce{};
	uint32_t keyCheck = 0;
	uint64_t indexHash = 0;
	uint64_t headerChecksum = 0;

	/**
	 * 序列化到 128 字节缓冲区，并计算 headerChecksum（写回本结构体）。
	 */
	void serialize(uint8_t* out);

	/**
	 * 从 128 字节缓冲区解析。依次检查 magic、版本、校验和、headerSize/entrySize。
	 * @return OK / CORRUPTED / VERSION_MISMATCH
	 */
	Error parse(const uint8_t* in);
};

/**
 * 归档条目（索引中的一项）。
 */
struct ArchiveEntry
{
	std::string path;                                     // 规范相对路径，无前导 '/'
	EntryType type = EntryType::FILE;
	CipherType cipher = CipherType::NONE;
	CompressionType compression = CompressionType::NONE;
	uint8_t keyLength = 0;
	uint64_t offset = 0;                                  // 存储数据起点（相对归档起点）
	uint64_t storedSize = 0;                              // 存储字节数
	uint64_t size = 0;                                    // 原始字节数
	int64_t mtime = 0;
	uint64_t hash = 0;                                    // XXH3-128(原始内容) 低 64 位
	std::array<uint8_t, ArchiveFormat::MAX_FILE_KEY_SIZE> key{};
};

/**
 * 读取选项。
 */
struct ArchiveOptions
{
	std::vector<uint8_t> indexKey;   // 非空（必须 32 字节）时覆盖归档中存储的索引密钥
	int64_t baseOffset = -1;         // 归档起点；-1 表示自动（偏移 0 处的 magic，否则看文件末尾的 footer）
};

// 透明哈希：允许用 std::string_view 直接查找 std::string 键，查找时不必构造临时字符串
struct TransparentStringHash
{
	using is_transparent = void;

	size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

/**
 * 解析后的归档索引，open 之后不可变，通过 shared_ptr<const> 在后端和所有流之间共享。
 * 最后一个流关闭后才释放，实现惰性卸载。
 * 线程安全：只读，可以任意并发访问。
 */
class VFS_DLL ArchiveIndex
{
public:
	ArchiveHeader header;                    // 解析后的 header
	int64_t baseOffset = 0;                  // 归档起点在来源中的偏移
	int64_t archiveSize = 0;                 // 归档总长度（含 footer）
	int64_t sourceMtime = 0;                 // 来源的 mtime
	std::shared_ptr<IArchiveSource> source;  // 数据来源（打开流时使用）
	std::vector<ArchiveEntry> entries;       // 按 path 字节序升序

	/**
	 * 查找文件条目（不含目录）。
	 * @return 找不到返回 nullptr
	 */
	const ArchiveEntry* findFile(std::string_view rel) const;

	/**
	 * 是否是目录（显式空目录或由文件路径推导出的目录，根目录为空串）。
	 */
	bool isDir(std::string_view rel) const;

	/**
	 * 查询元数据；中间一级是文件时返回 NOT_DIR。
	 */
	Error stat(std::string_view rel, Stat& out) const;

	/**
	 * 目录的直接子项（已按名字排序）；不是目录返回 nullptr。
	 */
	const std::vector<DirEntry>* children(std::string_view relDir) const;

	/**
	 * 根据 entries 构建查找表和目录树。
	 * @return 文件与目录冲突时返回 CORRUPTED
	 */
	Error build();

private:
	std::unordered_map<std::string_view, uint32_t> m_files;            // path -> entries 下标（视图指向 entries 中的字符串）
	std::unordered_map<std::string, std::vector<DirEntry>, TransparentStringHash, std::equal_to<>> m_dirs; // 目录 -> 子项
};

/**
 * 归档读取工具。全部是静态函数，线程安全。
 */
class VFS_DLL ArchiveReader
{
public:

	/**
	 * 打开并解析归档。
	 * 校验：magic、版本（不一致返回 VERSION_MISMATCH）、headerChecksum、所有 offset/size 越界与溢出、
	 * indexHash（XXH3-64，快速损坏检查）、索引密钥（keyCheck）、每个条目的合法性。
	 * @return 成功返回不可变索引；失败返回 nullptr，err 为 CORRUPTED / VERSION_MISMATCH / KEY_REQUIRED / KEY_INVALID / IO 错误
	 */
	static std::shared_ptr<const ArchiveIndex> open(std::shared_ptr<IArchiveSource> source,
													const ArchiveOptions& options = {}, Error* err = nullptr);

	static std::shared_ptr<const ArchiveIndex> open(std::string_view nativePath, const ArchiveOptions& options = {},
													Error* err = nullptr);

	/**
	 * 读出一个文件条目的完整原始内容（读取、XOR 解密、LZ4 解压）。
	 * @param stream 可选：复用一个已打开的来源流；为空时内部新开一个
	 */
	static Error readEntry(const ArchiveIndex& index, const ArchiveEntry& entry, std::vector<uint8_t>& out,
						   IFileStream* stream = nullptr);

private:
	static Error readEntryImpl(const ArchiveIndex& index, const ArchiveEntry& entry, std::vector<uint8_t>& out,
							   IFileStream* stream);

public:

	/**
	 * 离线校验每个文件的内容哈希（运行时默认不校验）。
	 * @param cb 每个文件条目校验完成后回调（可为空）
	 * @return 全部通过返回 OK；有文件哈希不符或损坏返回 CORRUPTED；读取失败时返回第一个读取错误（例如 IO_ERROR）
	 */
	static Error verify(const ArchiveIndex& index,
						const std::function<void(const ArchiveEntry& entry, Error result)>& cb = nullptr);
};

/**
 * 从来源流的绝对偏移处读满 size 字节（循环读，处理短读）。
 */
Error readFully(IFileStream& stream, int64_t offset, uint8_t* buf, size_t size);

NS_VFS_END
