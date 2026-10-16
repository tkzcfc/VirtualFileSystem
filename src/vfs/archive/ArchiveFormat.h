// 本文件：.vfs 归档格式的常量、字段偏移和枚举（单一事实来源）。
// 完整规范见 docs/vfs-format.md。所有整数固定小端，通过 ByteIO 逐字节读写，禁止 memcpy 结构体。
//
// 布局：
//   [0]            Header 128B（明文）
//   [dataOffset]   Data：每个文件的存储数据，起点按 alignment 对齐，填充 0
//   [indexOffset]  Index：Entry[entryCount] + StringPool；可选 ChaCha20 加密
//   [末尾 16B]     Footer：magic + archiveSize，用于定位附加在其他文件后面的归档
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

namespace ArchiveFormat
{

// "\x89VFS\r\n\x1A\n"：PNG 式 magic，能发现被当作文本传输造成的损坏
constexpr uint8_t MAGIC[8] = {0x89, 'V', 'F', 'S', '\r', '\n', 0x1A, '\n'};
// "\x89VFE\r\n\x1A\n"：footer magic
constexpr uint8_t FOOTER_MAGIC[8] = {0x89, 'V', 'F', 'E', '\r', '\n', 0x1A, '\n'};

// 格式版本：与读取方不一致直接拒绝（final-decisions #1）
constexpr uint32_t VERSION = 1;

constexpr uint32_t HEADER_SIZE = 128;
constexpr uint32_t ENTRY_SIZE = 96;
constexpr uint32_t FOOTER_SIZE = 16;

constexpr uint32_t INDEX_KEY_SIZE = 32;
constexpr uint32_t INDEX_NONCE_SIZE = 12;
// 索引用 ChaCha20 加密时的初始块计数器（与 RFC 8439 加密一致，从 1 开始）
constexpr uint32_t INDEX_CHACHA_COUNTER = 1;

constexpr uint32_t MAX_FILE_KEY_SIZE = 32;
constexpr uint32_t DEFAULT_FILE_KEY_SIZE = 32;
constexpr uint32_t DEFAULT_ALIGNMENT = 16;
constexpr uint32_t MAX_ALIGNMENT = 65536;
constexpr uint32_t MAX_PATH_LENGTH = 4096;

// ---- Header 字段偏移 ----
constexpr size_t H_MAGIC = 0;            // [0]   8  magic
constexpr size_t H_VERSION = 8;          // [8]   u32 version
constexpr size_t H_HEADER_SIZE = 12;     // [12]  u32 headerSize（=128）
constexpr size_t H_FLAGS = 16;           // [16]  u32 flags（ArchiveFlags）
constexpr size_t H_ALIGNMENT = 20;       // [20]  u32 alignment（2 的幂，1..65536）
constexpr size_t H_ENTRY_COUNT = 24;     // [24]  u32 entryCount
constexpr size_t H_ENTRY_SIZE = 28;      // [28]  u32 entrySize（=96）
constexpr size_t H_DATA_OFFSET = 32;     // [32]  u64 dataOffset
constexpr size_t H_DATA_SIZE = 40;       // [40]  u64 dataSize
constexpr size_t H_INDEX_OFFSET = 48;    // [48]  u64 indexOffset
constexpr size_t H_INDEX_SIZE = 56;      // [56]  u64 indexSize
constexpr size_t H_INDEX_KEY = 64;       // [64]  32  indexKey（KeyStored 时有效，否则全 0）
constexpr size_t H_INDEX_NONCE = 96;     // [96]  12  indexNonce
constexpr size_t H_KEY_CHECK = 108;      // [108] u32 keyCheck = 低 32 位 XXH3-64(明文索引)，用于发现密钥错误
constexpr size_t H_INDEX_HASH = 112;     // [112] u64 indexHash = XXH3-64(存储的索引字节)
constexpr size_t H_CHECKSUM = 120;       // [120] u64 headerChecksum = XXH3-64(header[0,120))

// ---- Entry 字段偏移（相对条目起点）----
constexpr size_t E_TYPE = 0;             // [0]  u8  type（EntryType）
constexpr size_t E_CIPHER = 1;           // [1]  u8  cipher（CipherType）
constexpr size_t E_COMPRESSION = 2;      // [2]  u8  compression（CompressionType）
constexpr size_t E_KEY_LENGTH = 3;       // [3]  u8  keyLength（cipher=XOR 时 1..32，否则 0）
constexpr size_t E_NAME_OFFSET = 4;      // [4]  u32 nameOffset（相对 StringPool 起点）
constexpr size_t E_NAME_LENGTH = 8;      // [8]  u32 nameLength（1..4096）
constexpr size_t E_RESERVED0 = 12;       // [12] u32 reserved（0）
constexpr size_t E_OFFSET = 16;          // [16] u64 offset（存储数据起点，相对归档起点）
constexpr size_t E_STORED_SIZE = 24;     // [24] u64 storedSize（存储字节数：压缩后）
constexpr size_t E_SIZE = 32;            // [32] u64 size（原始字节数）
constexpr size_t E_MTIME = 40;           // [40] i64 mtime（Unix 秒，0=未知）
constexpr size_t E_HASH = 48;            // [48] u64 hash = XXH3-128(原始内容) 的低 64 位
constexpr size_t E_RESERVED1 = 56;       // [56] u64 reserved（0）
constexpr size_t E_KEY = 64;             // [64] 32  XOR key（前 keyLength 字节有效，其余为 0）

// ---- Footer 字段偏移（相对 footer 起点）----
constexpr size_t F_MAGIC = 0;            // [0] 8   magic
constexpr size_t F_ARCHIVE_SIZE = 8;     // [8] u64 archiveSize（从 header 起点到 footer 末尾）

} // namespace ArchiveFormat

/**
 * Header 标志位。使用时必须带类型名限定，例如 ArchiveFlags::IndexEncrypted。
 */
enum ArchiveFlags : uint32_t
{
	IndexEncrypted = 1u << 0,   // 索引区用 ChaCha20 加密
	KeyStored = 1u << 1,        // 索引密钥存放在 header 中（不追求高安全性，final-decisions #5）
};

enum class EntryType : uint8_t
{
	FILE = 1,
	DIR = 2,    // 只用于空目录（final-decisions #7：默认不保留）
};

enum class CipherType : uint8_t
{
	NONE = 0,
	XOR = 1,    // 多字节 XOR，按 (文件内偏移 % keyLen) 异或
};

enum class CompressionType : uint8_t
{
	NONE = 0,
	LZ4 = 1,    // LZ4 block；打开时整体解压到内存
};

NS_VFS_END
