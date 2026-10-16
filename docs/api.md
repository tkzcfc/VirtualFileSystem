# API 参考

日常使用只需包含一个头文件：

```cpp
#include "vfs/VirtualFileSystem.h"   // 链接目标 vfs（别名 vfs::vfs）
USING_NS_VFS;                         // 或者写 vfs::
```

它已包含挂载、本机 / 内存 / 归档后端和流接口。其他组件按需包含：`"vfs/native/SingleFileBackend.h"`、`"vfs/archive/ArchiveWriter.h"`、`"vfs/Log.h"`。

完整的参数说明见各头文件的注释，本文按用途汇总。路径参数一律是 UTF-8 的虚拟路径，规范化与分层规则见 [挂载规则](mount-rules.md)。

## 1. 错误处理

```cpp
enum class Error { OK, NOT_FOUND, EXISTS, NOT_DIR, IS_DIR, NOT_EMPTY, READ_ONLY, CROSS_DEVICE, BUSY,
                   INVALID_PATH, PERMISSION_DENIED, IO_ERROR, UNSUPPORTED, CORRUPTED, VERSION_MISMATCH,
                   KEY_REQUIRED, KEY_INVALID };

int toErrno(Error e);              // 映射到 POSIX errno，见 mount-rules.md §11
const char* errorName(Error e);    // "NOT_FOUND"
const char* errorMessage(Error e); // 英文说明（例如 "no such file or directory"）
```

- 返回 `Error` 的函数失败时同时设置 `errno = toErrno(e)`。
- 返回指针、流或 `MountId` 的函数失败时返回空值或 `0`，错误通过可选的 `Error* err` 输出。

## 2. 挂载

```cpp
struct MountOptions
{
	int priority = 0;     // 大的在上；相同时后挂载的在上
	uint32_t flags = 0;   // MountFlags::ReadOnly | Opaque | WriteTarget
};

MountId mount(mountPoint, std::shared_ptr<IBackend> backend, const MountOptions& = {}, Error* err = nullptr);
MountId mountNative(mountPoint, nativeDir, const MountOptions& = {}, const NativeOptions& = {}, Error* err = nullptr);
MountId mountMemory(mountPoint, const MountOptions& = {}, std::shared_ptr<MemoryBackend>* backendOut = nullptr, Error* err = nullptr);
MountId mountArchive(mountPoint, nativePath, const ArchiveOptions& = {}, const MountOptions& = {}, Error* err = nullptr);
MountId mountFile(mountPoint, nativeFile, const MountOptions& = {}, Error* err = nullptr);

Error unmount(MountId id, uint32_t flags = 0);         // flags: UnmountFlags::FailIfBusy
Error remount(MountId id, std::shared_ptr<IBackend>);  // 原子替换后端
std::vector<MountInfo> mounts() const;                 // {id, mountPoint, priority, flags, backend}，按层序排列
Error addAlias(prefix, target);                        // 例如 addAlias("res://", "/res")
Error removeAlias(prefix);
uint64_t generation() const;                           // 挂载表变化或写操作后递增
```

- 成功返回非 0 的 `MountId`。挂载点必须是合法路径，否则 `INVALID_PATH`；`backend` 为空返回 `UNSUPPORTED`。
- `mountNative` 的目录不存在返回 `NOT_FOUND`（设置 `NativeOptions::createRoot` 时自动创建），不是目录返回 `NOT_DIR`。
- `mountFile` 的文件必须是已存在的普通文件。
- `mountArchive` 打开失败时返回归档错误（`CORRUPTED`、`VERSION_MISMATCH`、`KEY_REQUIRED`、`KEY_INVALID` 或 IO 错误）。

### 后端选项

```cpp
struct NativeOptions
{
	bool readOnly = false;        // 所有写操作返回 READ_ONLY
	bool followSymlinks = true;   // false：解析后逃出根目录的路径一律拒绝（PERMISSION_DENIED）
	bool strictCase = false;      // true：核对真实文件名的大小写，不一致时输出警告（结果不变）
	bool exactCase = false;       // true：按字节匹配名字，大小写不敏感的文件系统上只差大小写视为不存在（见 mount-rules.md §1）
	bool createRoot = false;      // 根目录不存在时自动创建（mkdir -p）
};

struct MemoryOptions
{
	bool readOnly = false;        // 只读：只能通过 addFile 预填数据
};

struct ArchiveOptions
{
	std::vector<uint8_t> indexKey;   // 非空时必须 32 字节，覆盖归档中存储的索引密钥
	int64_t baseOffset = -1;         // 归档起点；-1 自动（偏移 0 处的 magic，否则看文件末尾的 footer）
};
```

本机后端的平台说明：

- **Windows 长路径**：UTF-8 路径转 UTF-16 时，长度达到 `MAX_PATH - 12`（`CreateDirectoryW` 的限制）就转换为绝对路径并加 `\\?\` 前缀（UNC 路径为 `\\?\UNC\server\share\...`），所有本机操作（打开、stat、列目录、mkdir、删除、改名）都走同一个转换，不依赖系统的 `LongPathsEnabled` 设置。仓库里的可执行文件（MSVC）另外嵌入了 `longPathAware` 清单（`cmake/longpath.manifest`，CMake 函数 `vfs_add_long_path_manifest`）。
- **Windows 共享与替换**：本机流以三种共享方式全开打开；删除、改名覆盖优先用 POSIX 语义，打开中的文件可以被删除或替换（详见 mount-rules.md §7、§9）。追加写（`APPEND`）用“写到文件末尾”的原子写，多个流并发追加不会互相覆盖。
- **大小写**：默认随文件系统，`exactCase` 可以强制按字节匹配（mount-rules.md §1）。

后端也可以单独创建再交给 `mount()`：

```cpp
#include "vfs/native/SingleFileBackend.h"

auto native  = NativeBackend::create("assets", NativeOptions{}, &err);
auto single  = SingleFileBackend::create("settings.ini", /*readOnly*/ false, &err);
auto memory  = MemoryBackend::create(MemoryOptions{});
memory->addFile("config/default.json", "{}");          // 自动创建父目录，只读模式下同样可用
memory->setReadOnly(true);

auto archive = ArchiveBackend::create("game.vfs", ArchiveOptions{}, &err);
auto inMem   = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(std::move(bytes)), ArchiveOptions{}, &err);
fs.mount("/res", archive, MountOptions{0, MountFlags::ReadOnly});
```

`ArchiveBackend::verifyAll()` 校验归档中所有文件的内容哈希（离线检查用，相当于把归档读一遍）。

## 3. 查询

```cpp
Error stat(path, Stat& out) const;   // Stat{type, size, mtime, mount, writable}
bool exists(path) const;
bool isFile(path) const;
bool isDir(path) const;
int64_t fileSize(path) const;        // 不是文件或不存在时返回 -1

Error list(path, const std::function<bool(const DirEntry&)>& cb) const;  // 回调返回 false 停止
std::vector<DirEntry> list(path, Error* err = nullptr) const;            // DirEntry{name, type}
Error walk(path, const std::function<bool(const std::string& fullPath, const DirEntry&)>& cb) const;
std::string realPath(path) const;    // 可见项对应的本机路径；不在本机后端上时返回空串
```

- `Stat::mtime` 为 Unix 秒，`0` 表示未知；归档中 mtime 为 0 的条目返回归档文件自身的修改时间。
- `Stat::mount` 是可见项所在的挂载 ID，合成的虚拟目录为 `0`。
- `list` 的结果按名字字节序排列；`walk` 深度优先、目录先于其子项回调，`fullPath` 是虚拟绝对路径。

## 4. 读写

```cpp
std::unique_ptr<IFileStream> openFileStream(path, IFileStream::Mode mode, Error* err = nullptr) const;

Error readFile(path, std::vector<uint8_t>& out) const;
Error readFile(path, std::string& out) const;
Error writeFile(path, const void* data, size_t size) const;   // WRITE 模式：创建或截断
Error writeFile(path, std::string_view data) const;
Error writeFileAtomic(path, const void* data, size_t size) const;   // 写 path.vfs-N.tmp 后 rename，失败时删除临时文件
```

- 读模式取可见项；写模式按 [写入目标规则](mount-rules.md#5-写入目标) 选择层，必要时 copy-up。
- 目录不能作为文件打开（`IS_DIR`）。
- 空文件也会返回有效的流（`size() == 0`）。

## 5. 修改

```cpp
Error createDir(path) const;                      // 已存在返回 EXISTS
Error createDirs(path) const;                     // mkdir -p
Error removeDir(path, bool recursive = false) const;
Error removeFile(path) const;
Error rename(from, to) const;                     // 同一挂载内原子改名
Error moveFile(from, to) const;                   // 跨挂载时文件退化为复制 + 删除
Error copyFile(from, to) const;                   // 复制到自身：可写时不做任何事，只读层时等于 copy-up
```

各操作的落点与错误码见 [挂载规则](mount-rules.md) §6–§9。

## 6. IFileStream

`vfs::IFileStream` 与 axmol 引擎的 `ax::IFileStream` 签名完全一致；定义 `VFS_WITH_AXMOL` 时它就是 `ax::IFileStream` 的别名。

```cpp
class IFileStream
{
public:
	enum class Mode { READ, WRITE, APPEND, OVERLAPPED };

	virtual bool open(std::string_view path, Mode mode) = 0;
	virtual int close() = 0;
	virtual int64_t seek(int64_t offset, int origin) const = 0;   // SEEK_SET / SEEK_CUR / SEEK_END
	virtual int read(void* buf, unsigned int size) const = 0;
	virtual int write(const void* buf, unsigned int size) const = 0;
	virtual int64_t tell() const = 0;
	virtual int64_t size() const = 0;
	virtual bool resize(int64_t size) const;          // 默认 errno = ENOTSUP 并返回 false
	virtual bool isOpen() const = 0;
	virtual osfhnd_t nativeHandle() const;            // 默认 -1；Windows 为 HANDLE，其他平台为 fd
	virtual operator bool() const { return isOpen(); }
};
```

### 打开模式

| 模式 | 文件不存在 | 截断 | 可读 | 写入位置 |
|---|---|---|---|---|
| `READ` | 失败（`NOT_FOUND`） | 否 | 是 | 不可写 |
| `WRITE` | 创建 | 是 | 是 | 当前位置 |
| `APPEND` | 创建 | 否 | 是 | 每次 `write` 都写到末尾 |
| `OVERLAPPED` | 创建 | 否 | 是 | 当前位置（随机读写） |

### 返回值与 errno

所有内置流（`NativeFileStream`、`MemoryFileStream`、`ArchiveFileStream`）和 VFS 返回的包装流遵循同一套约定，由 `tests/stream` 中的一致性测试覆盖：

- `read`/`write` 返回实际字节数，失败返回 `-1` 并设置 `errno`；`read` 到达 EOF 返回 `0`。
- `seek` 返回新位置；结果位置为负时返回 `-1`（`errno = EINVAL`），位置不变。
- 允许 `seek` 到 EOF 之后：随后 `read` 返回 `0`；`write` 会用 `0` 填充中间的空洞。
- `close` 成功返回 `0`。关闭之后所有方法返回 `-1`（或 `false`），`errno = EBADF`；`isOpen()` 返回 `false`。
- 在 `READ` 模式的流上 `write` 返回 `-1`。
- 单次 `read` 最多返回 `VFS_MAX_IO_CHUNK`（`0x7FFFF000`）字节，本机流的 `write` 同样按此截断；大块数据应循环读写，直到处理完或返回 `0`/`-1`。

### 各实现的差异

| 行为 | Native | Memory | Archive（未压缩） | Archive（LZ4） |
|---|---|---|---|---|
| `write` | 支持 | 支持（`READ` 模式 `EBADF`） | `-1`，`EROFS` | `-1`，`EBADF`（内存只读流） |
| `resize` | 支持 | 支持 | `false`，`ENOTSUP` | `false`，`EBADF` |
| `nativeHandle` | OS 句柄 | `-1` | `-1` | `-1` |
| 直接调用 `open()` | 支持 | `false`，`ENOTSUP` | `false`，`ENOTSUP` | `false`，`ENOTSUP` |

LZ4 压缩的归档文件在打开时整体解压到内存，之后的读取不再访问磁盘。

### 所有权与线程

- 流由 `std::unique_ptr` 独占，析构时自动关闭。
- 流持有所在后端，挂载被卸载后仍可继续使用（惰性卸载）。
- 单个流对象不是线程安全的；不同流对象可以在不同线程中并发使用。

## 7. 自定义后端

实现 `IBackend` 即可挂载任意数据源（网络缓存、数据库、加密容器……）：

```cpp
class IBackend
{
public:
	virtual uint32_t capabilities() const = 0;                       // BackendCaps::Read | Write | Rename | Resize
	virtual Error stat(std::string_view rel, Stat& out) const = 0;
	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) = 0;
	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const = 0;
	virtual Error createDir(std::string_view rel);                   // 默认 READ_ONLY
	virtual Error removeDir(std::string_view rel);                   // 默认 READ_ONLY
	virtual Error removeFile(std::string_view rel);                  // 默认 READ_ONLY
	virtual Error rename(std::string_view from, std::string_view to);// 默认 READ_ONLY
	virtual std::string realPath(std::string_view rel) const;        // 默认空串
	virtual const char* typeName() const = 0;
};
```

约定：

- `rel` 是已规范化的相对路径（无前导 `/`，无 `.`/`..`），后端根目录为空串 `""`。
- `stat("")` 必须返回目录。
- `list` 只列直接子项，不必排序，VFS 会合并排序。
- `removeDir` 只需删除空目录；递归删除、copy-up、跨层检查都由 VFS 完成。
- `open` 的写模式需要遵循上面的打开模式表；父目录存在性由 VFS 保证。
- 所有方法都可能被多个线程同时调用，后端必须自身线程安全。

## 8. 归档读写

运行时一般只需要 `mountArchive`。需要在程序中生成或检查归档时：

```cpp
#include "vfs/archive/ArchiveWriter.h"

ArchiveWriteOptions options;          // alignment=16, encryptIndex=true, dedup=true, compressionLevel=9 ...
ArchiveWriter writer(options);
writer.addFile("scripts/main.lua", "assets/scripts/main.lua", FileWriteOptions{false, true});  // 压缩
writer.addData("config.json", std::vector<uint8_t>(...), FileWriteOptions{true, false});       // XOR 加密
writer.addDir("empty");                                    // 只有最终为空的目录才会写入
ArchiveWriteStats stats;
Error e = writer.write("game.vfs", &stats);                // 先写 game.vfs.tmp 再 rename
if (e != Error::OK)
	std::puts(writer.lastError().c_str());
```

- `ArchiveWriteOptions`：`alignment`（2 的幂，1..65536）、`encryptIndex`、`indexKey`（空则确定性派生并存入文件；否则必须 32 字节）、`storeKey`、`fileKeyLength`（1..32）、`dedup`、`compressionLevel`（1..12）。
- `FileWriteOptions`：`encrypt`、`compress`（压缩后不变小时自动存原文）、`mtime`（默认 0，保证可复现）。
- 路径重复或仅大小写不同返回 `EXISTS`，选项非法返回 `UNSUPPORTED`，`lastError()` 给出具体说明。
- `ArchiveReader::open` 返回不可变的 `ArchiveIndex`；`ArchiveReader::readEntry` 读出单个条目的原始内容；`ArchiveReader::verify` 逐个校验内容哈希。

二进制格式见 [format.md](format.md)。

## 9. 日志与配置

```cpp
#include "vfs/Log.h"

enum class LogLevel { VERBOSE, INFO, WARN, FAIL };
using LogFunc = std::function<void(LogLevel, std::string_view)>;
void setLogger(LogFunc func);        // 传空函数关闭输出；默认不输出
```

编译期配置（`Config.h`，可在编译选项中覆盖）：

| 宏 | 默认值 | 说明 |
|---|---|---|
| `VFS_WITH_AXMOL` | `0` | 使用 axmol 的 `IFileStream` |
| `VFS_MAX_IO_CHUNK` | `0x7FFFF000` | 单次 read/write 的最大字节数 |
| `VFS_MAX_INDEX_SIZE` | 512 MB | 可接受的归档索引上限 |
| `VFS_DLL` | 空 | 导出宏（`Common.h`），构建或使用动态库时按平台定义 |
