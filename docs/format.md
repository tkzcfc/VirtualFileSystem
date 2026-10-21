# `.vfs` archive format specification (v1)

This document is the binary format specification for `.vfs` archives. The read/write implementation lives in `src/vfs/archive` (constants and field offsets are defined in `ArchiveFormat.h`).

## 1. Overall conventions

- **Byte order**: every integer is little-endian (independent of the platform; reads and writes use `ByteIO`).
- **Offsets**: unless noted otherwise, every offset is relative to the **archive start** (the first byte of the header). An archive may be appended after another file (for example an exe); see section 6.
- **Paths**: UTF-8, separated by `/`, already normalized (no leading `/`, no `.`/`..`, no empty segments, no `\`), case-sensitive, length 1..4096 bytes.
- **Version**: there is a single version number (currently `1`). A mismatch is rejected immediately (`VERSION_MISMATCH`). There is no major/minor split and no feature bits.
- **Integrity**: only the header and the index are checked (XXH3-64), so corruption is found quickly. Content hashes are used only by offline checks (`vfstool verify` / `ArchiveReader::verify`); runtime reads do not verify them.
- **Security**: XOR and ChaCha20 with "the key stored in the file" are **obfuscation only**. They do not stop a determined reverse engineer. For confidentiality, use an external key (not stored in the file).

## 2. File layout

```
+----------------------+  0
| Header (128 B)       |
+----------------------+  dataOffset (= 128)
| Data                 |  stored bytes of each file, aligned to alignment, padding bytes are 0
|   ...                |
+----------------------+  indexOffset (= dataOffset + dataSize)
| Index (indexSize B)  |  Entry[entryCount] (96 B each) + StringPool; optional ChaCha20 encryption
+----------------------+  indexOffset + indexSize
| Footer (16 B)        |
+----------------------+  archiveSize
```

Regions must appear in this order and must not run past the end: `128 <= dataOffset`, `dataOffset + dataSize <= indexOffset`, `indexOffset + indexSize + 16 <= available length`.

## 3. Header (128 bytes)

| Offset | Type | Field | Description |
|---:|---|---|---|
| 0 | u8[8] | magic | `89 56 46 53 0D 0A 1A 0A` (`\x89VFS\r\n\x1A\n`; like PNG, this catches newline conversion and truncation) |
| 8 | u32 | version | `1` |
| 12 | u32 | headerSize | `128` |
| 16 | u32 | flags | `ArchiveFlags`: bit0 `IndexEncrypted`, bit1 `KeyStored`; every other bit must be 0 |
| 20 | u32 | alignment | data alignment, a power of two, 1..65536 (1 = no alignment) |
| 24 | u32 | entryCount | number of entries |
| 28 | u32 | entrySize | `96` |
| 32 | u64 | dataOffset | start of the data region (writers always use 128) |
| 40 | u64 | dataSize | length of the data region (including padding) |
| 48 | u64 | indexOffset | start of the index |
| 56 | u64 | indexSize | index length (`entryCount*96 + StringPool length`), at most 512 MB |
| 64 | u8[32] | indexKey | ChaCha20 key when `KeyStored`, otherwise all zeros |
| 96 | u8[12] | indexNonce | ChaCha20 nonce (0 when the index is not encrypted) |
| 108 | u32 | keyCheck | low 32 bits of `XXH3-64(plaintext index)`; compared after decrypt to tell "wrong key" from "corrupt data" |
| 112 | u64 | indexHash | `XXH3-64(stored index bytes)` (the ciphertext when encrypted); corruption can be detected without the key |
| 120 | u64 | headerChecksum | `XXH3-64(header[0..120))` |

**Parse order**: magic, then version (not equal to 1 means `VERSION_MISMATCH`), then headerChecksum, then headerSize/entrySize/alignment/unknown flags (any illegal value means `CORRUPTED`). The version is checked first so a future-version file gets a clear "version mismatch" error rather than "corrupt".

## 4. Index

The plaintext index is `Entry[entryCount]` immediately followed by `StringPool`. Entries are sorted by `path` in **strict ascending byte order** (duplicates are not allowed), so the runtime can binary-search or build a hash table directly.

### 4.1 Entry (96 bytes)

| Offset | Type | Field | Description |
|---:|---|---|---|
| 0 | u8 | type | `1` FILE, `2` DIR (only for empty directories kept explicitly) |
| 1 | u8 | cipher | `0` NONE, `1` XOR |
| 2 | u8 | compression | `0` NONE, `1` LZ4 (independent blocks, see 5.2) |
| 3 | u8 | keyLength | 1..32 when cipher=XOR, otherwise 0 |
| 4 | u32 | nameOffset | offset of the path inside the StringPool |
| 8 | u32 | nameLength | path length in bytes, 1..4096 |
| 12 | u8 | blockLog2 | LZ4: log2 of the block size, 12..22 (4 KiB..4 MiB); otherwise 0 |
| 13 | u8[3] | reserved | 0 |
| 16 | u64 | offset | start of stored data (relative to the archive start) |
| 24 | u64 | storedSize | stored byte count (after compression, including the LZ4 block table) |
| 32 | u64 | size | original byte count |
| 40 | i64 | mtime | Unix seconds; 0 = unknown (writers default to 0 so output is reproducible; runtime stat falls back to the archive file's own mtime) |
| 48 | u64 | hash | low 64 bits of `XXH3-128(original content)` |
| 56 | u64 | reserved | 0 |
| 64 | u8[32] | key | XOR key; the first keyLength bytes are significant, the rest are 0 |

Validation rules (the reader treats every field as untrusted input): type/cipher/compression are legal values; `[nameOffset, nameOffset+nameLength)` lies inside the StringPool; the path is normalized; entries are strictly sorted; the file data range `[offset, offset+storedSize)` lies inside the data region; when uncompressed, `storedSize == size`; when uncompressed, `blockLog2 == 0`; when compressed, `size > 0`, `blockLog2` is in 12..22, and `storedSize` holds at least the block table plus one byte per block, and `size` is at most about 255 times the block payload (the LZ4 expansion limit; `size` never exceeds INT64_MAX) (block spans are validated when the table is read, see 5.2); the same path cannot be both a file and an ancestor of a directory. Data ranges of different entries **may overlap** (shared by dedup).

### 4.2 StringPool

All paths are concatenated in entry order, with no trailing 0.

### 4.3 Index encryption (ChaCha20, RFC 8439)

- When `IndexEncrypted` is set, the whole index region (Entry + StringPool) is encrypted with ChaCha20, initial counter `1`, nonce taken from the header's `indexNonce`.
- Key source (highest priority first): an external key passed at runtime or on the CLI, then `indexKey` in the header (`KeyStored`), then `KEY_REQUIRED` if neither exists.
- After decryption, `low32(XXH3-64(plaintext)) != keyCheck` means `KEY_INVALID`.
- The implementation is in `src/vfs/crypto/ChaCha20.cpp`. Tests check every vector from RFC 8439 section 2.3.2, section 2.4.2, and appendix A.1/A.2.

## 5. File data

### 5.1 Write order

After entries are sorted by path, data is written in that order. The start of each non-empty block is rounded up to `alignment` (pad with 0). An empty file takes no space (`offset` is the current position, `storedSize = 0`).

### 5.2 Compression (LZ4 blocks)

- Optional per file, off by default, decided by pack rules. The file is split into blocks of `2^blockLog2` bytes (default 64 KiB; the last block may be shorter), and each block is compressed independently with LZ4 HC (default level 9).
- Stored data of a compressed entry = **block table** + blocks. The table has one u64 (little-endian) per block, `blockCount = ceil(size / blockSize)`:
  - bits 0..62: end offset of the block's stored bytes, relative to the end of the table (block `i` starts where block `i-1` ends; block 0 starts at 0);
  - bit 63: the block is stored raw (it did not shrink); its stored length then equals its original length.
- Reader checks per block: end offsets strictly increase, the last block ends exactly at `storedSize - tableSize`, a raw block's length equals its original length, and a compressed block is no longer than `LZ4_compressBound(blockLength)`. A violation or an LZ4 decode failure is reported as corruption (`EIO` from a stream read, `CORRUPTED` from verify/readEntry).
- If the whole compressed form (table + blocks) is not smaller than the original, the original is stored (`compression = 0`, `blockLog2 = 0`).
- At runtime a stream loads the table on its first read and then decompresses only the blocks a read touches, caching just the current block. Opening a file reads nothing; seeking is O(1).
- Order: **compress first, then encrypt**.

### 5.3 XOR encryption

- Optional per file. Key length is 1..32 (default 32; the CLI allows 16..32).
- Stored byte `s[i] = p[i] ^ key[i % keyLength]`, where `i` is the offset inside the **stored data** (for a compressed file, the offset inside the stored bytes, block table included). Decryption can therefore start at any position and random seek is supported.
- The key is derived deterministically (so output is reproducible): `key[16k..16k+15] = LE128(XXH3-128(LE64(h.low) || LE64(h.high), seed = 0x5646532D4B455931 + 16k))`, `h = XXH3-128(original content)`. Readers do not depend on the derivation; they use only the key stored in the index.

### 5.4 Deduplication

Files with the same content (XXH3-128 plus length) and the same storage mode (cipher, compression, keyLength, blockLog2) are written once; several entries point at the same `offset`. Because the XOR key is derived from the content, identical content encrypts to identical bytes, so encrypted files can be deduplicated too.

## 6. Footer (16 bytes) and locating the start

| Offset | Type | Field | Description |
|---:|---|---|---|
| 0 | u8[8] | magic | `89 56 46 45 0D 0A 1A 0A` (`\x89VFE\r\n\x1A\n`) |
| 8 | u64 | archiveSize | total length from the start of the header through the end of the footer |

On open, if `ArchiveOptions::baseOffset` is not set: header magic at offset 0 means the start is 0; otherwise the footer at the end of the file is read and the start is file length minus archiveSize. An archive can therefore be appended directly after an exe or similar file.

## 7. Reproducible builds

The same input produces byte-identical output every time:

1. Entries are sorted by path bytes, independent of insertion order;
2. mtime is written as 0 by default (only `vfstool pack --keep-mtime` writes the real value);
3. The XOR key is derived from the content;
4. When no external key is supplied, the index key and nonce are derived from the plaintext index: `key = XXH3-128(index, seed=0x5646532D49445831+0) || XXH3-128(index, seed=...+16)`, `nonce = low 12 bytes(XXH3-128(index, seed=0x5646532D4E4F4E31))`; with an external key the nonce is derived the same way;
5. LZ4 HC output is deterministic; padding bytes are 0.

## 8. Error codes

| Condition | Error |
|---|---|
| wrong magic, wrong checksum, region out of range, illegal entry | `CORRUPTED` |
| version is not 1 | `VERSION_MISMATCH` |
| index is encrypted but no key is available | `KEY_REQUIRED` |
| key length is not 32, or keyCheck does not match | `KEY_INVALID` |
| content hash mismatch, or LZ4 decompress failed (verify only) | `CORRUPTED` |

## 9. Runtime reads

- Each open stream holds its own source handle (`IArchiveSource::openStream`) and reads at `offset`. Streams do not interfere and need no lock; sequential reads cache the file position so extra seeks are skipped.
- The index (`ArchiveIndex`) is read-only and shared between the backend and every stream via `shared_ptr<const>`. After unmount, streams that are still open keep it alive.
- At runtime the archive is read-only: opening for write returns `READ_ONLY`, and `write()` returns -1 / `EROFS`.
