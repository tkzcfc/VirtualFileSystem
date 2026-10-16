# `.vfs` 归档格式规范（v1）

本文描述 `src/vfs/archive` 实际读写的二进制格式。设计依据见 `design/vfs-format-draft.md` 与 `design/final-decisions.md`（后者优先）。

## 1. 总体约定

- **字节序**：所有整数一律小端（与平台无关，读写使用 `ByteIO`）。
- **偏移**：除特别说明外，所有偏移都相对**归档起点**（header 的第一个字节）。归档可以附加在其他文件（例如 exe）后面，见 §6。
- **路径**：UTF-8，`/` 分隔，已规范化（无前导 `/`、无 `.`/`..`、无空段、无 `\`），区分大小写，长度 1..4096 字节。
- **版本**：只有一个版本号（当前为 `1`），不一致直接拒绝（`VERSION_MISMATCH`）。没有 major/minor、没有特性位。
- **完整性**：只校验 header 与索引（XXH3-64），用于快速发现损坏；文件内容的哈希只在 `vfstool verify` / `ArchiveReader::verify` 离线检查时使用，运行时读取不校验。
- **安全性**：XOR 与“密钥存在文件里”的 ChaCha20 **只是混淆**，不能抵御有心的逆向。需要保密时用外部密钥（不存入文件）。

## 2. 文件布局

```
+----------------------+  0
| Header (128 B)       |
+----------------------+  dataOffset (= 128)
| Data                 |  每个文件的存储数据，按 alignment 对齐，填充字节为 0
|   ...                |
+----------------------+  indexOffset (= dataOffset + dataSize)
| Index (indexSize B)  |  Entry[entryCount] (每个 96 B) + StringPool；可选 ChaCha20 加密
+----------------------+  indexOffset + indexSize
| Footer (16 B)        |
+----------------------+  archiveSize
```

各区段必须依次排列且不越界：`128 <= dataOffset`、`dataOffset + dataSize <= indexOffset`、`indexOffset + indexSize + 16 <= 可用长度`。

## 3. Header（128 字节）

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u8[8] | magic | `89 56 46 53 0D 0A 1A 0A`（`\x89VFS\r\n\x1A\n`，与 PNG 同样能发现换行转换与截断） |
| 8 | u32 | version | `1` |
| 12 | u32 | headerSize | `128` |
| 16 | u32 | flags | `ArchiveFlags`：bit0 `IndexEncrypted`，bit1 `KeyStored`；其他位必须为 0 |
| 20 | u32 | alignment | 数据对齐，2 的幂，1..65536（1 = 不对齐） |
| 24 | u32 | entryCount | 条目数 |
| 28 | u32 | entrySize | `96` |
| 32 | u64 | dataOffset | 数据区起点（写入端固定为 128） |
| 40 | u64 | dataSize | 数据区长度（含填充） |
| 48 | u64 | indexOffset | 索引起点 |
| 56 | u64 | indexSize | 索引长度（`entryCount*96 + StringPool 长度`），上限 512 MB |
| 64 | u8[32] | indexKey | `KeyStored` 时为 ChaCha20 密钥，否则全 0 |
| 96 | u8[12] | indexNonce | ChaCha20 nonce（未加密时为 0） |
| 108 | u32 | keyCheck | `XXH3-64(明文索引)` 的低 32 位；解密后比对，用来区分“密钥错误”和“数据损坏” |
| 112 | u64 | indexHash | `XXH3-64(存储的索引字节)`（加密后的密文），不需要密钥就能检查损坏 |
| 120 | u64 | headerChecksum | `XXH3-64(header[0..120))` |

**解析顺序**：magic → version（不等于 1 → `VERSION_MISMATCH`）→ headerChecksum → headerSize/entrySize/alignment/未知 flags（任一不合法 → `CORRUPTED`）。先检查版本号，使未来版本的文件得到明确的“版本不匹配”错误，而不是“损坏”。

## 4. Index

明文索引 = `Entry[entryCount]` 紧接 `StringPool`。条目按 `path` 的**字节序严格升序**排列（不允许重复），运行时可直接二分/建哈希表。

### 4.1 Entry（96 字节）

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u8 | type | `1` FILE，`2` DIR（只用于显式保留的空目录） |
| 1 | u8 | cipher | `0` NONE，`1` XOR |
| 2 | u8 | compression | `0` NONE，`1` LZ4（block 格式） |
| 3 | u8 | keyLength | cipher=XOR 时 1..32，否则 0 |
| 4 | u32 | nameOffset | 路径在 StringPool 中的偏移 |
| 8 | u32 | nameLength | 路径字节数，1..4096 |
| 12 | u32 | reserved | 0 |
| 16 | u64 | offset | 存储数据起点（相对归档起点） |
| 24 | u64 | storedSize | 存储字节数（压缩后） |
| 32 | u64 | size | 原始字节数 |
| 40 | i64 | mtime | Unix 秒；0 = 未知（默认写 0 以保证可复现；运行时 stat 回退为归档文件自身的 mtime） |
| 48 | u64 | hash | `XXH3-128(原始内容)` 的低 64 位 |
| 56 | u64 | reserved | 0 |
| 64 | u8[32] | key | XOR 密钥，前 keyLength 字节有效，其余为 0 |

校验规则（读取端把每个字段都当作不可信输入）：type/cipher/compression 取值合法；`[nameOffset, nameOffset+nameLength)` 在 StringPool 内；路径已规范化；条目严格升序；文件数据范围 `[offset, offset+storedSize)` 落在数据区内；未压缩时 `storedSize == size`；压缩时 `size` 不超过 LZ4 上限；同一路径不能既是文件又是目录的祖先。不同条目的数据范围**允许重叠**（去重共享）。

### 4.2 StringPool

所有路径按条目顺序首尾相接，不含结尾 0。

### 4.3 索引加密（ChaCha20，RFC 8439）

- `IndexEncrypted` 时整个索引区（Entry + StringPool）用 ChaCha20 加密，初始计数器 `1`，nonce 取 header 的 `indexNonce`。
- 密钥来源（优先级从高到低）：运行时/CLI 传入的外部密钥 → header 中的 `indexKey`（`KeyStored`）→ 都没有则 `KEY_REQUIRED`。
- 解密后 `low32(XXH3-64(明文)) != keyCheck` → `KEY_INVALID`。
- 实现位于 `src/vfs/crypto/ChaCha20.cpp`，测试用 RFC 8439 §2.3.2、§2.4.2、附录 A.1/A.2 的全部向量验证。

## 5. 文件数据

### 5.1 写入顺序

条目按路径排序后依次写数据。每个非空数据块的起点按 `alignment` 向上对齐（填充 0），空文件不占空间（`offset` 为当前位置，`storedSize = 0`）。

### 5.2 压缩（LZ4）

- 每文件可选，默认关闭，由打包规则决定。使用 LZ4 HC（默认级别 9）压缩整个文件为一个 block。
- 压缩后不比原文小时自动存原文（`compression = 0`）。
- 运行时打开压缩文件时**整体解压到内存**（`MemoryFileStream`），之后随机读写都在内存中进行。
- 顺序：**先压缩，再加密**。

### 5.3 XOR 加密

- 每文件可选。密钥长度 1..32（默认 32，CLI 允许 16..32）。
- 存储字节 `s[i] = p[i] ^ key[i % keyLength]`，其中 `i` 是在**存储数据**中的偏移（压缩文件即压缩数据中的偏移），所以可以从任意位置开始解密，支持随机 seek。
- 密钥确定性派生（保证可复现）：`key[16k..16k+15] = LE128(XXH3-128(LE64(h.low) || LE64(h.high), seed = 0x5646532D4B455931 + 16k))`，`h = XXH3-128(原始内容)`。读取端不依赖派生方式，只用索引里存的 key。

### 5.4 去重

内容（XXH3-128 + 长度）相同、且存储方式（cipher、compression、keyLength）相同的文件只写一份，多个条目指向同一 `offset`。由于 XOR key 由内容派生，相同内容的加密结果也相同，所以加密文件同样可以去重。

## 6. Footer（16 字节）与起点定位

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u8[8] | magic | `89 56 46 45 0D 0A 1A 0A`（`\x89VFE\r\n\x1A\n`） |
| 8 | u64 | archiveSize | 从 header 起点到 footer 末尾的总长度 |

打开时如果未指定 `ArchiveOptions::baseOffset`：偏移 0 处是 header magic → 起点为 0；否则读取文件末尾的 footer，起点 = 文件长度 − archiveSize。这样归档可以直接附加在 exe 等文件后面。

## 7. 可复现构建

同一输入每次输出逐字节一致：

1. 条目按路径字节序排序，与添加顺序无关；
2. mtime 默认写 0（`vfstool pack --keep-mtime` 才写真实值）；
3. XOR key 由内容派生；
4. 未提供外部密钥时，索引密钥与 nonce 由明文索引派生：`key = XXH3-128(index, seed=0x5646532D49445831+0) || XXH3-128(index, seed=…+16)`，`nonce = 低 12 字节(XXH3-128(index, seed=0x5646532D4E4F4E31))`；有外部密钥时 nonce 同样派生；
5. LZ4 HC 输出是确定的；填充字节为 0。

## 8. 错误码

| 情况 | 错误 |
|---|---|
| magic 不对、校验和不对、区段越界、条目非法 | `CORRUPTED` |
| 版本号不是 1 | `VERSION_MISMATCH` |
| 索引加密但没有任何密钥 | `KEY_REQUIRED` |
| 密钥长度不是 32、或 keyCheck 不匹配 | `KEY_INVALID` |
| 内容哈希不匹配、LZ4 解压失败（仅 verify 时） | `CORRUPTED` |

## 9. 运行时读取

- 每个打开的流各自持有一个来源句柄（`IArchiveSource::openStream`），用 `offset` 定位读取，互不干扰，无需加锁；顺序读时缓存文件位置，省掉多余的 seek。
- 索引（`ArchiveIndex`）只读、用 `shared_ptr<const>` 在后端与所有流之间共享，卸载后由仍打开的流保活。
- 运行时归档只读：写模式打开返回 `READ_ONLY`，`write()` 返回 -1 / `EROFS`。
