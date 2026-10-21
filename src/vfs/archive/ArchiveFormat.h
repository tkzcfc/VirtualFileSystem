// This file: constants, field offsets, and enums for the .vfs archive format (single source of truth).
// The full specification is docs/format.md. Every integer is little-endian and is read and written byte by byte through ByteIO; memcpy of structs is forbidden.
//
// Layout:
//   [0]            Header 128B (plaintext)
//   [dataOffset]   Data: stored bytes of each file, start aligned to alignment, pad with 0
//   [indexOffset]  Index: Entry[entryCount] + StringPool; optional ChaCha20 encryption
//   [last 16B]     Footer: magic + archiveSize, used to locate an archive appended after another file
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

namespace ArchiveFormat
{

// "\x89VFS\r\n\x1A\n": PNG-style magic that detects corruption from text-mode transfer
constexpr uint8_t MAGIC[8] = {0x89, 'V', 'F', 'S', '\r', '\n', 0x1A, '\n'};
// "\x89VFE\r\n\x1A\n": footer magic
constexpr uint8_t FOOTER_MAGIC[8] = {0x89, 'V', 'F', 'E', '\r', '\n', 0x1A, '\n'};

// Format version: a mismatch with the reader is rejected immediately
constexpr uint32_t VERSION = 1;

constexpr uint32_t HEADER_SIZE = 128;
constexpr uint32_t ENTRY_SIZE = 96;
constexpr uint32_t FOOTER_SIZE = 16;

constexpr uint32_t INDEX_KEY_SIZE = 32;
constexpr uint32_t INDEX_NONCE_SIZE = 12;
// Initial block counter when the index is encrypted with ChaCha20 (matches RFC 8439 encryption, starting at 1)
constexpr uint32_t INDEX_CHACHA_COUNTER = 1;

constexpr uint32_t MAX_FILE_KEY_SIZE = 32;
constexpr uint32_t DEFAULT_FILE_KEY_SIZE = 32;
constexpr uint32_t DEFAULT_ALIGNMENT = 16;
constexpr uint32_t MAX_ALIGNMENT = 65536;
constexpr uint32_t MAX_PATH_LENGTH = 4096;

// LZ4 entries are split into fixed-size blocks (2^blockLog2 bytes, the last one may be shorter), each compressed independently.
constexpr uint8_t MIN_BLOCK_LOG2 = 12;       // 4 KiB
constexpr uint8_t MAX_BLOCK_LOG2 = 22;       // 4 MiB
constexpr uint8_t DEFAULT_BLOCK_LOG2 = 16;   // 64 KiB
// Block table item (u64 LE): bits 0..62 = end offset of the block's stored bytes, relative to the end of the table; bit 63 = stored raw
constexpr uint32_t BLOCK_ENTRY_SIZE = 8;
constexpr uint64_t BLOCK_RAW_FLAG = 1ull << 63;
constexpr uint64_t BLOCK_END_MASK = BLOCK_RAW_FLAG - 1;

// ---- Header field offsets ----
constexpr size_t H_MAGIC = 0;            // [0]   8  magic
constexpr size_t H_VERSION = 8;          // [8]   u32 version
constexpr size_t H_HEADER_SIZE = 12;     // [12]  u32 headerSize (=128)
constexpr size_t H_FLAGS = 16;           // [16]  u32 flags (ArchiveFlags)
constexpr size_t H_ALIGNMENT = 20;       // [20]  u32 alignment (power of two, 1..65536)
constexpr size_t H_ENTRY_COUNT = 24;     // [24]  u32 entryCount
constexpr size_t H_ENTRY_SIZE = 28;      // [28]  u32 entrySize (=96)
constexpr size_t H_DATA_OFFSET = 32;     // [32]  u64 dataOffset
constexpr size_t H_DATA_SIZE = 40;       // [40]  u64 dataSize
constexpr size_t H_INDEX_OFFSET = 48;    // [48]  u64 indexOffset
constexpr size_t H_INDEX_SIZE = 56;      // [56]  u64 indexSize
constexpr size_t H_INDEX_KEY = 64;       // [64]  32  indexKey (valid when KeyStored, otherwise all zeros)
constexpr size_t H_INDEX_NONCE = 96;     // [96]  12  indexNonce
constexpr size_t H_KEY_CHECK = 108;      // [108] u32 keyCheck = low 32 bits of XXH3-64(plaintext index), used to detect a wrong key
constexpr size_t H_INDEX_HASH = 112;     // [112] u64 indexHash = XXH3-64(stored index bytes)
constexpr size_t H_CHECKSUM = 120;       // [120] u64 headerChecksum = XXH3-64(header[0,120))

// ---- Entry field offsets (relative to the start of the entry) ----
constexpr size_t E_TYPE = 0;             // [0]  u8  type (EntryType)
constexpr size_t E_CIPHER = 1;           // [1]  u8  cipher (CipherType)
constexpr size_t E_COMPRESSION = 2;      // [2]  u8  compression (CompressionType)
constexpr size_t E_KEY_LENGTH = 3;       // [3]  u8  keyLength (1..32 when cipher=XOR, otherwise 0)
constexpr size_t E_NAME_OFFSET = 4;      // [4]  u32 nameOffset (relative to the start of the StringPool)
constexpr size_t E_NAME_LENGTH = 8;      // [8]  u32 nameLength (1..4096)
constexpr size_t E_BLOCK_LOG2 = 12;      // [12] u8  blockLog2 (LZ4: MIN_BLOCK_LOG2..MAX_BLOCK_LOG2, otherwise 0)
constexpr size_t E_RESERVED0 = 13;       // [13] 3   reserved (0)
constexpr size_t E_OFFSET = 16;          // [16] u64 offset (start of stored data, relative to the archive start)
constexpr size_t E_STORED_SIZE = 24;     // [24] u64 storedSize (stored byte count: after compression, including the LZ4 block table)
constexpr size_t E_SIZE = 32;            // [32] u64 size (original byte count)
constexpr size_t E_MTIME = 40;           // [40] i64 mtime (Unix seconds, 0=unknown)
constexpr size_t E_HASH = 48;            // [48] u64 hash = low 64 bits of XXH3-128(original content)
constexpr size_t E_RESERVED1 = 56;       // [56] u64 reserved (0)
constexpr size_t E_KEY = 64;             // [64] 32  XOR key (the first keyLength bytes are significant, the rest are 0)

// ---- Footer field offsets (relative to the start of the footer) ----
constexpr size_t F_MAGIC = 0;            // [0] 8   magic
constexpr size_t F_ARCHIVE_SIZE = 8;     // [8] u64 archiveSize (from the start of the header through the end of the footer)

} // namespace ArchiveFormat

/**
 * Header flag bits. Always qualify with the type name, for example ArchiveFlags::IndexEncrypted.
 */
enum ArchiveFlags : uint32_t
{
	IndexEncrypted = 1u << 0,   // index region is encrypted with ChaCha20
	KeyStored = 1u << 1,        // the index key is stored in the header (not meant to be highly secure)
};

enum class EntryType : uint8_t
{
	FILE = 1,
	DIR = 2,    // only for empty directories
};

enum class CipherType : uint8_t
{
	NONE = 0,
	XOR = 1,    // multi-byte XOR, XOR with (offset within the file % keyLen)
};

enum class CompressionType : uint8_t
{
	NONE = 0,
	LZ4 = 1,    // independent LZ4 blocks: stored data = block table (u64 per block) + blocks; blocks are decompressed on demand
};

NS_VFS_END
