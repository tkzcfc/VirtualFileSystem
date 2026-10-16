# VirtualFileSystem 重写：代码风格 / 目录结构 / .vfs 归档格式设计

> 状态：设计稿，尚未实现。日期：2026-10-16
> 前置：`controversies.md` 中的 35 条决策已定稿。本文件**取代**其中第 24 条（pack 格式），并在第 29 条命名上做一处修正（见 §1.3）。旧 paktool 格式及 `PackFileSystem` 全部删除。新格式称为 **VFS Archive**，扩展名 `.vfs`，运行时后端为 `ArchiveBackend`。
> 依赖版本为 2026-10-16 查到的最新 release，实现时固定（pin）：Monocypher 4.0.3、doctest v2.5.3、xxHash v0.8.4、CLI11 v2.7.2。

---

## 1. 代码风格指南（依据现有仓库归纳）

### 1.1 现状观察
| 项 | 现有仓库实际情况 | 统一后的规则 |
|---|---|---|
| 头文件保护 | 全部使用 `#pragma once` | 保持。**注意**：`MemoryFileStream.cpp`、`NativeFileStream.cpp` 里误写了 `#pragma once`，新代码中 .cpp 禁止出现 |
| 文件命名 | 一类一文件，`PascalCase.h/.cpp`，内联实现放 `Utils.inl` | 保持；子目录用小写（`native/ memory/ archive/`） |
| 命名空间 | 宏 `NS_VFS_BEGIN / NS_VFS_END / USING_NS_VFS / NS_VFS`（定义在 Common.h） | **保持宏**（见 1.3） |
| 类、结构体、枚举类型名 | `PascalCase`：`VirtualFileSystem`、`FileInfo`、`FileFlags` | 保持 |
| 方法名 | `camelCase`：`openFileStream`、`isFile`、`createDir` | 保持。布尔查询用 `isXxx/hasXxx`；简单取值器不加 `get`（如 `mntpoint()`、`basePath()`）；设置器用 `setXxx` |
| 成员变量 | `m_` 加 camelCase：`m_fileSystems`、`m_mntpoint`、`m_readonly` | 保持；新增静态成员用 `s_` 前缀 |
| 结构体字段 | 纯数据结构体的字段不加前缀：`FileInfo::filePath`、`PackFileInfo::offset` | 保持 |
| 局部变量、参数 | camelCase：`filePath`、`fullFilePath`、`dirPath` | 保持（旧代码里个别 `header_length` 这种 snake_case 统一改掉） |
| 枚举值 | `enum class` 用全大写：`Mode::READ`、`SeekOrigin::CUR`；位标志用无作用域 enum 加 PascalCase 值：`FileFlags::File`，并加底层类型 `: uint8_t` | 保持：`enum class Error { OK, NOT_FOUND, ... }`；位标志 `enum ArchiveFlags : uint32_t { IndexEncrypted = 1u << 0 }`，使用时必须带类型名限定 |
| 宏 | 全大写：`CHECK_SIZE`、`VFS_HAS_ZLIB`；局部宏用完立刻 `#undef` | 保持；公开宏统一加 `VFS_` 前缀 |
| 大括号 | **Allman 风格**，函数、类、if、for 的左括号都单独占一行 | 保持 |
| 缩进 | .cpp 大多用 Tab，.h 大多用 4 空格（混用） | **统一用 Tab（宽度 4）**，由 `.clang-format` 和 `.editorconfig` 强制 |
| 构造函数初始化列表 | 前置逗号，每项一行（见下例） | 保持 |
| 单行 if | 常见无括号写法：`if (x)` 换行后 `return false;` | 只允许 return/break/continue 这类单语句省略括号 |
| 指针、引用 | 靠类型一侧：`const std::string& path`、`FileSystem* fs` | 保持；新接口用 `std::string_view`（与 axmol 一致） |
| virtual/override | 两者同时写：`virtual bool open(...) override;` | 保持 |
| 访问区段 | `public:` 在前，`protected:/private:` 在后；方法声明之间空一行 | 保持 |
| include | 自己的头文件放第一行，库内用相对路径 `"../FileStream.h"`，系统头用 `<>` | 保持 |
| 注释 | `//` 单行注释，中英文混合；`PackFileSystem.cpp` 是 UTF-8 BOM，`Utils.cpp` 是 GBK（乱码） | **统一用中文注释，文件编码 UTF-8 无 BOM**，MSVC 编译加 `/utf-8`；公开 API 用 `/** */` 加 `@param/@return`（与 axmol 一致） |
| CMake | 小写命令、`lib_name` 变量、`option(VFS_...)`、中文 `#` 注释，第三方库目录叫 `3rd` | 保持；第三方库目录统一为根目录 `3rd/` |
| C++ 标准 | C++20 | 保持 C++20（用到 `std::atomic<std::shared_ptr>`、`starts_with`、`std::span`） |

### 1.2 示例（必须照这个样子写）
```cpp
#pragma once

#include "../Common.h"
#include "../IFileStream.h"
#include <memory>

NS_VFS_BEGIN

/**
 * 归档中单个文件的只读流。
 * 每次 open 都持有一个独立的本机句柄；同一个对象不能跨线程使用。
 */
class ArchiveFileStream : public IFileStream
{
public:

	ArchiveFileStream(std::shared_ptr<const ArchiveIndex> index, const ArchiveEntry& entry);

	virtual ~ArchiveFileStream();

	/**
	 * 读取数据。加密文件按位置即时解密。
	 * @param buf  输出缓冲区
	 * @param size 期望读取的字节数
	 * @return 实际读取的字节数，出错返回 -1（errno 已设置）
	 */
	virtual int read(void* buf, unsigned int size) const override;

private:

	// 解密 [m_pos, m_pos + len) 范围内的数据，buf 中原来是密文
	void decryptInPlace(uint8_t* buf, size_t len) const;

private:
	std::shared_ptr<const ArchiveIndex> m_index;   // 保活索引和密钥（惰性卸载时用）
	NativeFileStream m_file;                       // 独立本机句柄
	mutable int64_t m_pos;                         // 当前位置（axmol 接口要求 const）
};

NS_VFS_END
```
```cpp
#include "ArchiveFileStream.h"
#include <algorithm>

NS_VFS_BEGIN

ArchiveFileStream::ArchiveFileStream(std::shared_ptr<const ArchiveIndex> index, const ArchiveEntry& entry)
	: m_index(std::move(index))
	, m_entry(entry)
	, m_pos(0)
{
}

int ArchiveFileStream::read(void* buf, unsigned int size) const
{
	// 已到达或超过 EOF：按 POSIX 语义返回 0
	if (m_pos >= static_cast<int64_t>(m_entry.size))
		return 0;

	// 读取长度不能超过文件剩余部分
	auto readLen = std::min<uint64_t>(size, m_entry.size - m_pos);
	...
}

NS_VFS_END
```

### 1.3 对第 29 条的修正
第 29 条原定“去掉 `NS_VFS_BEGIN` 宏”。考虑到“风格与用户现有代码一致”这一新要求，**改为保留 `NS_VFS_BEGIN / NS_VFS_END` 宏**；类名部分（`IFileStream`、`NativeFileStream`、`XxxBackend`）不变。

### 1.4 注释要求（“大量注释”的落地方式）
1. 每个头文件开头用一段 `//` 说明本文件职责。
2. 每个公开类、公开方法都有 `/** */`，写清参数、返回值、错误码、**线程安全**和**所有权**。
3. .cpp 中每个非平凡分支都要用 `//` 说明“为什么这样做”，不只是复述代码。
4. 二进制格式相关代码，每个字段读写处注明偏移，例如 `// [48] dataOffset u64`，与本文档 §3 的表格对应。
5. 引用了某条决策的地方，标注 `// 决策 #18：惰性卸载`。

### 1.5 工具化
- `.clang-format`：`BasedOnStyle: Microsoft`、`BreakBeforeBraces: Allman`、`UseTab: ForIndentation`、`IndentWidth: 4`、`TabWidth: 4`、`ColumnLimit: 120`、`PointerAlignment: Left`、`BreakConstructorInitializers: BeforeComma`、`AccessModifierOffset: -4`、`SortIncludes: false`。
- `.editorconfig`：charset 为 utf-8，结尾换行，缩进用 tab。

---

## 2. 仓库目录结构与 CMake 目标

```
VirtualFileSystem/
├─ CMakeLists.txt              # 顶层：选项、子目录
├─ cmake/
│   ├─ VfsOptions.cmake        # VFS_BUILD_TOOLS / TESTS / EXAMPLES / FUZZ / VFS_ENABLE_CRYPTO / VFS_WITH_AXMOL
│   └─ VfsCompiler.cmake       # 警告等级、/utf-8、-Wall -Wextra
├─ .clang-format  .editorconfig  .gitignore  ReadMe.md  LICENSE
├─ docs/
│   ├─ controversies.md        # 35 条决策
│   ├─ vfs-format.md           # 本文件（格式定稿后改为规范文档）
│   ├─ style.md                # §1 抽出来
│   └─ vfstool.md              # CLI 手册
├─ src/vfs/                    # ── 目标 vfs（静态库，运行时）
│   ├─ Common.h  Config.h      # 命名空间宏、编译期开关
│   ├─ Error.h/.cpp            # vfs::Error 与 errno 映射
│   ├─ IFileStream.h           # 与 axmol 逐字一致（VFS_WITH_AXMOL 时改为 using）
│   ├─ IBackend.h  Stat.h  DirEntry.h
│   ├─ Path.h/.cpp             # 规范化（决策 #11）
│   ├─ MountTable.h/.cpp       # COW 快照（决策 #17）
│   ├─ VirtualFileSystem.h/.cpp# union、写入目标、copy-up 等核心逻辑
│   ├─ native/                 # NativeBackend NativeFileStream SingleFileBackend
│   ├─ memory/                 # MemoryBackend MemoryFileStream MemoryNode
│   ├─ archive/                # .vfs 读写
│   │   ├─ ArchiveFormat.h     # 常量、字段偏移、枚举（单一事实来源）
│   │   ├─ ByteIO.h            # 小端序列化 ByteReader / ByteWriter
│   │   ├─ ArchiveReader.h/.cpp# 解析 header/footer/index、校验、解密
│   │   ├─ ArchiveWriter.h/.cpp# 流式写入（CLI 与应用共用）
│   │   ├─ ArchiveBackend.h/.cpp
│   │   ├─ ArchiveFileStream.h/.cpp
│   │   └─ KeyProvider.h  ObfuscatedKey.h
│   └─ crypto/                 # 对第三方库的薄封装，库外不暴露第三方头文件
│       ├─ Cipher.h/.cpp       # XChaCha20 可定位 seek、AEAD
│       ├─ Hash.h/.cpp         # XXH3-128 / BLAKE2b-256 增量哈希
│       ├─ Kdf.h/.cpp          # HKDF-SHA512
│       ├─ Signature.h/.cpp    # Ed25519
│       └─ Random.h/.cpp       # OS CSPRNG（getrandom/BCryptGenRandom/arc4random）
├─ tools/vfstool/
│   ├─ core/                   # ── 目标 vfstool_core（静态库）：各命令实现，供测试直接调用
│   │   ├─ PackCommand.cpp  UnpackCommand.cpp  InfoCommand.cpp  ListCommand.cpp
│   │   ├─ CatCommand.cpp  ExtractCommand.cpp  VerifyCommand.cpp  KeygenCommand.cpp
│   │   ├─ Glob.h/.cpp         # ** * ? [] 规则匹配
│   │   └─ KeyOptions.h/.cpp   # --key-file / --key-hex / VFS_KEY
│   └─ main.cpp                # ── 目标 vfstool（可执行文件）：CLI11 参数解析后调用 core
├─ tests/                      # ── 目标 vfs_tests（doctest，注册到 ctest）
│   ├─ main.cpp                # DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
│   ├─ path/ union/ native/ memory/ stream/ archive/ crypto/ tool/ concurrency/
│   └─ data/                   # golden .vfs、已知答案向量
├─ fuzz/                       # ── 目标 vfs_fuzz_archive（libFuzzer，VFS_BUILD_FUZZ）
├─ examples/
│   ├─ basic/                  # 挂载、读写、加密归档示例
│   └─ test_gui/               # 保留现有 imgui 示例（依赖移入 3rd/）
└─ 3rd/
    ├─ doctest/ doctest.h
    ├─ monocypher/ monocypher.c/.h  optional/monocypher-ed25519.c/.h
    ├─ xxhash/ xxhash.h        # XXH_INLINE_ALL，header-only
    ├─ CLI11/ CLI11.hpp        # 只给 vfstool 用
    └─ glfw/ glad/ imgui/ glm/ md5/   # 只给 examples 用
```
| CMake 目标 | 类型 | 依赖 | 默认 |
|---|---|---|---|
| `vfs` | 静态库 | `vfs_crypto`（PRIVATE） | 总是构建 |
| `vfs_crypto` | 静态库（内部） | monocypher、xxhash | `VFS_ENABLE_CRYPTO=ON`；为 OFF 时，读到加密归档返回 `Error::UNSUPPORTED`，xxhash 仍然编译 |
| `vfstool_core` | 静态库 | vfs | `VFS_BUILD_TOOLS=ON` |
| `vfstool` | 可执行文件 | vfstool_core、CLI11 | `VFS_BUILD_TOOLS=ON` |
| `vfs_tests` | 可执行文件加 ctest | vfs、vfstool_core、doctest | `VFS_BUILD_TESTS=ON`（作为子项目时 OFF） |
| `vfs_fuzz_archive` | 可执行文件 | vfs | `VFS_BUILD_FUZZ=OFF` |
| `vfs_example_*` | 可执行文件 | vfs（以及 glfw 等） | `VFS_BUILD_EXAMPLES=OFF` |

### 测试范围（doctest，力求覆盖全面）
- **path**：规范化、`..` 越根、反斜杠、非法字符、Windows 保留名。
- **union**：35 条决策**每条至少一个 TEST_CASE**，包括争议 1 的场景复现。
- **stream**：IFileStream **一致性套件**，用 `TEST_CASE_TEMPLATE` 对 Native、Memory、Archive（明文和加密）四种流跑同一套用例：4 种 Mode、seek 越界、EOF、resize、size。
- **archive**：写入后读回；每种字段损坏（逐字节翻转 header、截断、footer 错误）；版本策略（major+1 拒绝、minor+1 接受、未知 incompat 位拒绝）；对齐；空目录；tombstone 解析；附在 exe 后的归档；同一输入**两次 build 结果逐字节相同**；golden 文件（防止格式意外变化）。
- **crypto**：XChaCha20、HKDF-SHA512（RFC 5869 风格向量）、Ed25519（RFC 8032）已知答案测试；随机 offset/len 解密结果与明文一致（属性测试，跑 10k 次）；错误 key 返回 `AUTH_FAILED`。
- **concurrency**：16 线程同时读同一归档；读的同时 mount/unmount；惰性卸载。
- **tool**：直接调用 vfstool_core 的各命令（pack/unpack/info/list/cat/extract/verify/keygen）、glob 规则、key 来源优先级。
- **字节序**：ByteIO 对已知字节序列的读写测试；CI 可选用 qemu s390x（大端）跑一遍。

---

## 3. `.vfs` 二进制布局草案（v1.0）

**总则**：所有整数**固定小端**，通过 `ByteReader/ByteWriter` 逐字节移位读写，**不允许 memcpy 结构体**，因此任何字节序的机器上读写结果都一致。所有 offset 都是**相对归档起点**（header 第 0 字节）的距离，这样归档可以整体搬移或附加到 exe 后面。

```
┌──────────────┐ 0
│ Header 256B  │ 明文；[0,160) 作为 AEAD 的附加认证数据（AAD）
├──────────────┤ dataOffset（按 alignment 对齐）
│ Data         │ 每个文件起点按 alignment 对齐，填充字节为 0；加密文件密文与明文等长
├──────────────┤ indexOffset
│ Index        │ 有 key 时用 XChaCha20-Poly1305 加密，否则明文
├──────────────┤
│ Signature    │ 可选（compat 标志 Signed），80B
├──────────────┤
│ Footer 32B   │ 位于文件末尾，用于定位 header
└──────────────┘
```

### 3.1 Header（256 字节）
| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 8 | magic | `89 56 46 53 0D 0A 1A 0A`（即 `\x89VFS\r\n\x1A\n`，PNG 式设计，可以发现被当作文本传输造成的损坏） |
| 8 | 2 | versionMajor | =1；与读取方不同则拒绝 |
| 10 | 2 | versionMinor | =0；比读取方新也可以读（见争议 F1） |
| 12 | 4 | headerSize | =256；以后扩展时读取方跳过未知部分 |
| 16 | 4 | incompatFlags | bit0 IndexEncrypted，bit1 HasEncryptedFiles；**存在未知位就拒绝** |
| 20 | 4 | compatFlags | bit0 Signed，bit1 HasTombstones，bit2 Reproducible；未知位忽略 |
| 24 | 4 | alignment | 2 的幂，范围 1..65536，默认 16 |
| 28 | 1 | hashAlgo | 1=XXH3-128，2=BLAKE2b-256 |
| 29 | 1 | indexCipher | 0=无，1=XChaCha20-Poly1305 |
| 30 | 1 | kdf | 0=无，1=HKDF-SHA512 |
| 31 | 1 | reserved | 0 |
| 32 | 4 | keyId | 交给 KeyProvider 选择主密钥 |
| 36 | 4 | entryCount | 条目数 |
| 40 | 4 | entrySize | =96；条目可在以后的 minor 版本中扩展 |
| 44 | 4 | reserved | 0 |
| 48 | 8 | dataOffset | 数据区起点 |
| 56 | 8 | dataSize | 数据区长度（含填充） |
| 64 | 8 | indexOffset | 索引起点 |
| 72 | 8 | indexSize | 索引存储字节数（流密码下密文与明文等长） |
| 80 | 8 | indexPlainSize | 明文长度（为以后的格式预留），读取方上限 512 MB |
| 88 | 8 | createdTime | Unix 秒；可复现构建时为 0 |
| 96 | 32 | salt | 每个归档的 KDF 盐 |
| 128 | 24 | indexNonce | XChaCha20 nonce |
| 152 | 8 | reserved | 0（属于 AAD 范围） |
| 160 | 16 | indexTag | Poly1305 MAC；未加密时为 0 |
| 176 | 32 | indexHash | 对**存储的索引字节**按 hashAlgo 计算的哈希（XXH3-128 只占前 16 字节）；不需要 key 就能校验 |
| 208 | 40 | reserved | 写入方填 0，读取方忽略 |
| 248 | 8 | headerChecksum | `XXH3-64(header[0,248))` |

### 3.2 Index（明文布局）
```
Entry[entryCount]（每项 entrySize 字节，按 path 字节序升序排列，path 唯一）
StringPool（UTF-8 path 依次紧排，无 NUL；规范化，无前导 '/'）
```
**Entry（96 字节）**
| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | type | 1=File，2=Dir（只用于空目录），3=Tombstone（补丁包删除标记） |
| 1 | 1 | cipher | 0=无，1=XChaCha20，2=保留给 AES-256-CTR，0x80–0xFF 用户自定义 |
| 2 | 2 | flags | bit0 HashPresent；其余保留 |
| 4 | 4 | nameOffset | 在 StringPool 中的偏移 |
| 8 | 2 | nameLength | ≤ 4096 |
| 10 | 6 | reserved | 0 |
| 16 | 8 | offset | 数据起点（相对归档起点），Dir/Tombstone 为 0 |
| 24 | 8 | size | 字节数（u64，不设 4 GB 上限） |
| 32 | 8 | mtime | Unix 秒，0 表示未知（运行时回退到归档文件本身的 mtime，见决策 #27） |
| 40 | 24 | nonce | 文件 nonce；cipher=0 时为 0 |
| 64 | 32 | hash | 对**存储字节**（密文）计算的哈希；XXH3-128 只占前 16 字节 |

### 3.3 Signature（可选，80 字节，紧跟 Index）
| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | magic `VSIG` |
| 4 | 2 | algo（1=Ed25519，RFC 8032 标准版，使用 monocypher-ed25519） |
| 6 | 2 | reserved |
| 8 | 8 | keyFingerprint（公钥 BLAKE2b 的前 8 字节，用于选择公钥） |
| 16 | 64 | signature，签名内容为 `header[0,248) ‖ 存储的 index 字节` |

### 3.4 Footer（32 字节，位于文件末尾）
| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 8 | magic `89 56 46 45 0D 0A 1A 0A`（`\x89VFE...`） |
| 8 | 8 | archiveSize（从 header 起点到 footer 末尾的长度）；header 位置 = 文件长度 − archiveSize |
| 16 | 4 | footerVersion=1 |
| 20 | 4 | reserved |
| 24 | 8 | checksum `XXH3-64(footer[0,24))` |

### 3.5 密钥派生与加密
```
masterKey(32B) = KeyProvider(keyId)
indexKey = HKDF-SHA512(ikm=masterKey, salt=header.salt, info="vfs/1/index", 32)
dataKey  = HKDF-SHA512(ikm=masterKey, salt=header.salt, info="vfs/1/data",  32)
index:  crypto_aead_lock(XChaCha20-Poly1305, key=indexKey, nonce=indexNonce, ad=header[0,160))
file :  crypto_chacha20_x(key=dataKey, nonce=entry.nonce, ctr=pos/64)  // 可随机定位
nonce:  默认由 CSPRNG 生成；可复现模式下 = BLAKE2b-192(key=dataKey, path ‖ 0x00 ‖ XXH3-128(明文))
```

### 3.6 打开流程（ArchiveReader::open）
1. 读偏移 0 处的 8 字节。magic 匹配则 base=0；否则读文件末尾 32 字节 footer，由 archiveSize 计算出 base。也可以用 `options.baseOffset` 显式指定。
2. 校验 headerChecksum、major、未知的 incompat 位、headerSize ≥ 256、entrySize ≥ 96，以及所有 offset/size 是否越界、是否溢出。
3. 读索引并计算 indexHash 进行比较（用于发现损坏）。
4. 如果 IndexEncrypted：用 keyId 向 KeyProvider 取 key，派生出 indexKey 后执行 AEAD unlock。失败返回 `AUTH_FAILED`，此时**一个字节的明文都不会被解析**。
5. 如果 Signed，或者 options 要求签名：校验 Ed25519 签名。
6. 解析条目：检查 path 规范化、排序且唯一、条目间不重叠、数据在数据区内；然后构建 `unordered_map<string, idx>` 和目录树，得到不可变的 `shared_ptr<const ArchiveIndex>`。

---

## 4. 格式相关争议与决策

### F1. Magic 与版本兼容策略
**问题**：格式升级后，旧程序和新归档、新程序和旧归档之间怎么办？
**选项**：只用一个版本号；major/minor；major/minor 加 ext4 式的 compat/incompat 特性位。
**决策**：采用 **major/minor 加特性位**。
- major 不同则拒绝（布局不兼容）。
- minor 只做追加：可以在条目或 header 末尾追加字段（靠 entrySize/headerSize 跳过），可以加 compat 位。
- 读取方遇到更高的 minor 也接受（向前兼容），但只要有**未知的 incompat 位就拒绝**。
- 写入方总是写自己的当前版本。新读取方必须能读所有 1.x 版本（向后兼容），用 golden 文件测试保证。

### F2. Header 认证 vs 普通校验（“版本认证”的含义）
**问题**：只做校验能发现损坏，但防不了篡改，比如攻击者把 IndexEncrypted 位清掉做降级，或者改版本号。
**选项**：CRC32；XXH3（快，但不是密码学算法）；SHA-256（无密钥，攻击者可以重新计算）；HMAC/AEAD（需要密钥）；Ed25519 签名（客户端只有公钥）。
**决策**：分三层，按场景启用。
1. **始终有**：`XXH3-64` headerChecksum，只防损坏。CRC32 被淘汰，因为 XXH3 更快、更强，而且已经在依赖里。
2. **有 key 时**：header `[0,160)` 作为 AEAD 的 AAD，版本、标志、offset、salt 一旦被改，索引解锁就会失败。这就是“版本认证”：是带密钥的认证，不是签名。
3. **需要真正防篡改时**（热更新包从网络下发）：用 **Ed25519 签名**。私钥只放在构建服务器，客户端只嵌公钥，这是唯一不怕客户端被逆向的手段。
- 读取方策略：`requireEncryptedIndex` 和 `requireSignature` 选项用来防降级。只要配置了 KeyProvider，**默认 requireEncryptedIndex=true**。

### F3. 单文件完整性：哈希算法与校验时机
**问题**：随机 offset 读取时，没法每读一块就校验整个文件。
**选项**：只在 open 时整读校验；读到 EOF 时校验；不校验；分块哈希（Merkle 或每 64KB 一个哈希）。
**决策**：
- 每个条目存一个哈希。默认 **XXH3-128**（约 30 GB/s，用于发现损坏）；签名归档**强制使用 BLAKE2b-256**，因为签名只覆盖索引，XXH3 可以被构造碰撞，起不到传递认证的作用。
- 哈希对象是**存储字节（密文）**，这样 CLI 的 `verify` 不需要 key，也可以先校验后解密。
- 运行时 `VerifyMode`：
  - `NONE`：不校验。
  - **`SEQUENTIAL`（默认）**：从 0 开始连续读取时增量计算哈希，到 EOF 时比较，不一致则这次 read 返回 -1、errno 为 EIO。中途发生非顺序 seek 就自动停止校验。
  - `ON_OPEN`：open 时整读一遍做校验，开销大，只用于小文件或调试。
- 另外提供 `ArchiveBackend::verifyAll()` 和 CLI `verify`，用于下载完成后的全量校验。
- 分块哈希暂不做（v2 再考虑），以免让格式和随机读取变复杂。

### F4. 索引加密算法与依赖选择
**选项**：AES-256-GCM（软件实现慢，而且有时序侧信道；硬件实现要引入 OpenSSL 或平台 API）；AES-CTR+HMAC（要自己组合，容易出错）；XChaCha20-Poly1305。
**决策**：使用 **XChaCha20-Poly1305**，由 **Monocypher 4.0.3** 提供：单个 .c/.h、BSD-2/CC0 许可、有审计、可移植、没有时序侧信道。24 字节 nonce 可以安全随机生成。HKDF-SHA512 和 Ed25519 使用其 optional 部分 `monocypher-ed25519`。哈希使用 header-only 的 xxHash。**不依赖 OpenSSL、mbedTLS 或 libsodium**。所有第三方 API 只出现在 `src/vfs/crypto/` 里。

### F5. 单文件加密必须支持随机 seek
**问题**：块链模式（CBC）和整文件 AEAD 都没法从任意位置开始解密。
**选项**：AES-CTR；ChaCha20/XChaCha20（计数器模式）；分块 AEAD（每 64KB 一个 tag）。
**决策**：
- 使用 **XChaCha20**（cipher=1），每个文件一个 24 字节 nonce。Monocypher 的 `crypto_chacha20_x(..., ctr)` 可以直接从 `ctr = pos/64` 开始。
- 读取实现：先把密文读进用户缓冲区，再**原地解密**（Monocypher 允许输入输出是同一块缓冲区）。如果起点不在 64 字节块边界上，先生成一块 64 字节的密钥流，取后半部分做 XOR，再按块批量处理，没有额外拷贝。
- 加密文件的完整性靠 F3 的密文哈希加上已认证的索引来保证。纯 CTR 模式本身可以被篡改，这一点在文档中说明。
- 预留 cipher=2 给 AES-256-CTR，等有硬件 AES 需求时再实现。0x80–0xFF 通过 `registerCipher(id, factory)` 留给用户自定义。

### F6. 密钥管理
**决策**：
- 运行时只接受回调：`using KeyProvider = std::function<bool(uint32_t keyId, uint8_t outKey[32])>;`。key 可以来自编译期嵌入、服务器下发或平台安全存储，这些由应用决定。
- 提供 `ObfuscatedKey<32>` 辅助（constexpr XOR 拆分），用于编译期嵌入，**文档中明确写明这只是混淆**。
- 每个归档有随机 salt，经 HKDF 派生出 indexKey 和 dataKey，所以不同归档、不同用途的 key 都不同，同一个主密钥可以长期使用。
- header 中的 **keyId** 支持多个主密钥和密钥轮换。一个归档只使用一个 keyId（v1 不做单文件级别的 key）。
- 密钥材料放在不可变结构里，析构时调用 `crypto_wipe` 清零。

### F7. 安全性说明（混淆 vs 真安全）
**决策**：在文档和 ReadMe 中**明确说明**：
- 客户端的加密只能**提高提取资源的门槛**。key 必然存在于客户端内存，挡不住有决心的逆向者。
- **签名**能防止第三方分发被篡改的归档；但如果攻击者能修改客户端二进制本身，任何校验都可以被绕过。
- 不要在归档里放真正的秘密（服务器凭据等）。

### F8. 文件名存储
**选项**：明文 path 放在加密索引里；只存 path 哈希（隐藏文件名，但无法列目录）。
**决策**：**完整的 UTF-8 path 放在加密索引中**，条目按字节序排序。运行时把索引全部解密后建哈希表，查找时用完整字符串比较，**不会有哈希碰撞问题**。写入方拒绝重复 path 和大小写冲突（决策 #12）。只存哈希的方案不做，因为它会破坏列目录。（第 24 条中“按 pathHash 排序”的设计同时作废。）

### F9. 数据块对齐
**决策**：默认 **16 字节**，额外开销很小。CLI 提供 `--align 4096`，供 mmap、direct I/O 或 Android APK 内直接映射使用。对齐与加密无关，因为 ChaCha 能处理块内偏移。填充字节为 0。上限 64KB。

### F10. 字节序
**决策**：**固定小端**。`ByteIO.h` 提供 `readU16LE/readU32LE/readU64LE/writeXxxLE`，全部用移位实现，**禁止对结构体 memcpy、禁止 `#pragma pack`**。有单元测试，CI 可选跑大端。

### F11. 大小限制
**决策**：offset 和 size 用 u64，entryCount 用 u32，path ≤ 4096 字节。读取方的安全上限：indexPlainSize ≤ 512 MB、entryCount × entrySize ≤ indexSize。所有加法都做溢出检查。

### F12. 空目录
**决策**：只为**空目录**写 type=Dir 条目，非空目录由文件路径推导，这样索引更小。CLI 默认保留空目录，`--no-empty-dirs` 可以关掉。

### F13. 删除标记（tombstone，用于补丁归档）
**决策**：**v1 格式就定义 type=3 Tombstone 和 compat 位 HasTombstones**，读取方能解析，`info/list` 也会显示。运行时屏蔽下层同名文件的功能放到 v1.1（与决策 #4 一致）。CLI 的 `diff` 命令（由旧归档和新目录生成补丁包）也放到 v1.1。

### F14. 多卷 / 附加在 exe 后面 / 起始偏移
**决策**：**不做多卷**，需要拆分时用多个归档分层挂载。**支持附加在其他文件后面**：所有 offset 都相对归档起点，靠 footer 自动定位，也可以用 `ArchiveOptions::baseOffset` 显式指定（与决策 #24 一致）。

### F15. 可复现构建
**决策**：
- 条目和数据都**按 path 字节序**排列（`--order list.txt` 可以按加载顺序重排数据区，提高局部性，索引仍然有序）。
- `--reproducible`（或检测到 `SOURCE_DATE_EPOCH` 环境变量）时：createdTime 为 0 或该环境变量的值；mtime 为 0；salt = BLAKE2b(所有 path ‖ 哈希)；nonce 用 §3.5 的派生方式。同样的输入加同样的 key 得到**逐字节相同**的输出。
- 内容相同时 nonce 才会相同，所以只会泄露“两个文件内容相同”这一点，可以接受。
- 非 reproducible 模式下 salt 和 nonce 全部随机。

### F16. mtime 存储
**决策**：每个条目有 i64 mtime，**默认写 0**（便于复现）；`--keep-mtime` 写源文件的真实 mtime。运行时 mtime 为 0 时回退到归档文件本身的 mtime（决策 #27）。

### F17. 增量更新 vs 全量重建
**决策**：**只做全量重建**。不做原地追加：要重写索引、签名会失效、写到一半崩溃不安全。增量需求通过“补丁归档加分层挂载”满足（v1.1 的 `diff` 加 tombstone）。

### F18. 流式写入的内存占用
**决策**：
- `ArchiveWriter` 的流程：先写占位 header，再逐个文件以 1 MB 缓冲边读边加密边计算哈希，然后写入，最后写 index、签名、footer，再 seek 回开头写真正的 header。
- 内存占用为 O(条目数 × (96 + path 长度)) 加 1 MB。
- 可复现模式下 nonce 依赖明文哈希，**每个文件要读两遍**（先算哈希，再加密），这一点在文档中说明。
- 先写到 `out.vfs.tmp`，完成后再 rename，保证原子性。

### F19. CLI 命令与选项
**决策**：工具名 `vfstool`，子命令如下：
| 命令 | 用法 |
|---|---|
| `pack` | `vfstool pack <dir> -o out.vfs [--prefix p] [--include G]... [--exclude G]... [--encrypt G]... [--no-encrypt G]... [--encrypt-all] [key 选项] [--key-id N] [--align N] [--hash xxh3\|blake2b] [--sign sk.key] [--keep-mtime] [--reproducible] [--salt HEX] [--order file] [--no-empty-dirs] [--no-default-excludes] [--dry-run] [-v]` |
| `unpack` | `vfstool unpack a.vfs -o dir [--include/--exclude] [key] [--force] [--no-verify]` |
| `info` | `vfstool info a.vfs [key] [--json]`：显示 header 字段、条目统计、加密统计、签名状态 |
| `list` / `ls` | `vfstool list a.vfs [glob] [-l] [--json]`（加密索引需要 key） |
| `cat` | `vfstool cat a.vfs <path> [key]`：二进制输出到 stdout |
| `extract` | `vfstool extract a.vfs <path>... -o dir [key]` |
| `verify` | `vfstool verify a.vfs [key] [--pubkey pk]`：检查 header、footer、索引、每个文件的哈希和签名 |
| `keygen` | `vfstool keygen -o master.key`，或 `--ed25519 sk.key pk.key` |
- 退出码：0 成功，1 用法错误，2 IO 错误，3 格式或版本错误，4 完整性或认证失败，5 缺少 key 或 key 错误。
- 每个命令先实现为 `vfstool_core` 中的函数，CLI 只负责解析参数，测试直接调用函数。

### F20. CLI 中传递 key
**决策**：优先级 `--key-file` > `--key-hex` > 环境变量 `VFS_KEY`（hex）。key 文件可以是 32 字节原始数据，也可以是 64 个 hex 字符（允许末尾换行）。使用 `--key-hex` 时**打印警告**（参数会被 shell history 和 ps 看到），CI 中推荐用 key 文件或环境变量。签名私钥只能用 `--sign <file>` 传入。

### F21. include/exclude 与加密规则（glob）
**决策**：
- glob 匹配的是相对于源目录的 `/` 路径。`*` 不跨越 `/`，`**` 可以跨越，另支持 `?` 和 `[abc]`。
- **同类规则按顺序，最后匹配的生效**（类似 gitignore）。include 默认为 `**`。
- 默认排除：`.git/ .svn/ .DS_Store Thumbs.db desktop.ini`，用 `--no-default-excludes` 关闭。
- 加密规则：给了 key 时**索引一定加密**。文件默认**不加密**（性能优先），`--encrypt "**/*.lua" --no-encrypt "**/*.png"` 按最后匹配生效，或者用 `--encrypt-all`。
- 所有参数都可以写进 `@args.txt` 文件。

### F22. 运行时挂载 API
**决策**：
```cpp
struct ArchiveOptions
{
	KeyProvider keyProvider;                 // 为空时只能打开未加密的归档
	VerifyMode verifyMode = VerifyMode::SEQUENTIAL;
	int64_t baseOffset = -1;                 // -1 表示自动（偏移 0 处的 magic 或 footer）
	bool requireEncryptedIndex = false;      // 设置了 keyProvider 时自动视为 true
	bool requireSignature = false;
	std::vector<std::array<uint8_t, 32>> trustedPublicKeys;
};
std::shared_ptr<ArchiveBackend> ArchiveBackend::open(std::string_view nativePath, const ArchiveOptions& options, Error* err = nullptr);
MountId VirtualFileSystem::mountArchive(std::string_view mountPoint, std::string_view nativePath, const ArchiveOptions& options, const MountOptions& mountOptions = {}, Error* err = nullptr);
```
另外提供 `ArchiveBackend::open(std::unique_ptr<IArchiveSource>)`，数据来源可以是内存或 Android AAsset 的 fd 加偏移（决策 #25、#26）。

### F23. 加密流如何实现 IFileStream
**决策**：`ArchiveFileStream` 只有一个类，明文和密文共用：
- `seek` 只修改 `m_pos`，越界规则与 POSIX 一致（决策 #33）。
- `read`：先 clamp，然后从底层句柄 seek 到 `entry.offset + m_pos` 读取；如果 cipher≠0，用 `ctr=m_pos/64` 加块内偏移原地解密；再推进 `m_pos`，最后更新 SEQUENTIAL 模式的哈希。
- `write`、`resize` 返回 -1，errno 为 EROFS/ENOTSUP；`nativeHandle()` 返回 -1；`size()` 返回 entry.size。

### F24. 运行时是否支持写入 .vfs
**决策**：**只读**（与决策 #14、#24 一致）。需要写入时，在归档上层挂一个可写的 Native/Memory 层。应用确实需要在运行时生成归档时（例如缓存打包），直接调用 `ArchiveWriter`（库中可用），生成后再 mount。

### F25. 解密的线程安全
**决策**：ArchiveIndex（条目和派生出的 key）在 open 后**不可变**，通过 `shared_ptr<const>` 共享。每个流有自己的句柄、位置和临时块缓冲区。XChaCha20 只要给定 ctr 就没有共享状态。**解密路径上没有任何锁**，可以无限并发。key 的生命周期跟随 ArchiveIndex，最后一个流关闭时清零（与决策 #18 的惰性卸载一致）。

### F26. 解析安全与模糊测试（补充）
**决策**：读取方把每个字段都当作不可信输入：做边界和溢出检查、拒绝不规范的 path（含 `..`、前导 `/`、重复）、条目之间不能重叠。提供 libFuzzer 目标 `vfs_fuzz_archive`，CI 中定期运行。

### F27. 没有 key 时的行为（补充）
**决策**：
- 未加密的索引：任何人都能 list 或 extract 明文文件。加密文件在没有 key 时 open 返回 `AUTH_REQUIRED`。
- 但规则规定“有 key 就加密索引”，所以“索引明文、文件加密”的组合**写入方不会生成**。读取方遇到 HasEncryptedFiles 却没有 IndexEncrypted 时，按格式错误拒绝。

---

## 决策速查
| # | 主题 | 决策 |
|---|---|---|
| F1 | 版本 | major/minor 加 incompat/compat 位；未知 incompat 位拒绝，minor 向前兼容 |
| F2 | header 认证 | 始终有 XXH3 校验；有 key 时 header 作为 AEAD 的 AAD；需要防篡改时用 Ed25519 签名 |
| F3 | 单文件完整性 | 默认 XXH3-128，签名时用 BLAKE2b；哈希密文；默认 SEQUENTIAL 校验 |
| F4 | 索引加密 | XChaCha20-Poly1305，使用 Monocypher；不依赖 OpenSSL |
| F5 | 文件加密 | XChaCha20 计数器模式，每文件一个 nonce，原地解密；预留 AES-CTR 和自定义 cipher |
| F6 | 密钥 | KeyProvider 回调加 keyId；HKDF(salt) 派生；ObfuscatedKey 辅助；用后清零 |
| F7 | 安全性说明 | 客户端加密只是混淆；签名才防篡改 |
| F8 | 文件名 | 明文 path 放在加密索引中，有序，用完整字符串查找 |
| F9 | 对齐 | 默认 16，可选 4096 |
| F10 | 字节序 | 固定小端，用移位序列化 |
| F11 | 大小限制 | 64 位；path ≤ 4096；读取方设安全上限 |
| F12 | 空目录 | 只为空目录写 Dir 条目 |
| F13 | tombstone | v1 格式定义，运行时 v1.1 实现 |
| F14 | 多卷/附加 | 不做多卷；用 footer 定位附加在 exe 后的归档 |
| F15 | 可复现 | 有序加 --reproducible 派生 salt 和 nonce |
| F16 | mtime | 默认 0，--keep-mtime 保留 |
| F17 | 增量 | 只做全量重建，增量靠补丁归档 |
| F18 | 写入内存 | 流式，O(条目)+1MB，tmp 加 rename |
| F19 | CLI | pack/unpack/info/list/cat/extract/verify/keygen，固定退出码 |
| F20 | CLI key | key-file > key-hex（会警告）> VFS_KEY |
| F21 | glob 规则 | 最后匹配生效；默认只加密索引，文件按规则选择是否加密 |
| F22 | 挂载 API | ArchiveOptions 加 mountArchive |
| F23 | 加密流 | 单一 ArchiveFileStream，按位置解密 |
| F24 | 运行时写入 | 只读，上层叠可写层 |
| F25 | 线程安全 | 不可变索引，无锁 |
| F26 | 解析安全 | 全字段校验加 fuzz |
| F27 | 无 key | 明文索引可读；加密文件返回 AUTH_REQUIRED；拒绝不合法的组合 |
