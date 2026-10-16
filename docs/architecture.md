# 架构

## 分层

```
            应用 / 引擎（例如 axmol 的 FileUtils）
                          │
                 VirtualFileSystem        路径规范化、别名、层栈解析、合并视图、写入规则
                          │
                  MountTable 快照          挂载点 → 后端、优先级、标志（不可变，copy-on-write）
                          │
   ┌──────────────┬───────┴──────┬───────────────┬──────────────┐
NativeBackend  SingleFileBackend  MemoryBackend   ArchiveBackend   自定义 IBackend
   │                 │                │               │
NativeFileStream  NativeFileStream  MemoryFileStream  ArchiveFileStream / MemoryFileStream
```

- **VirtualFileSystem** 是唯一入口。它把调用方的路径规范化成 `/a/b` 形式，找出覆盖该路径的所有挂载（层栈），在合并视图上完成查询，并按 [挂载规则](mount-rules.md) 决定写入落在哪一层。
- **后端（IBackend）** 只负责自己的一棵目录树，接收的路径一律是**已规范化的相对路径**（不含前导 `/`、`.`、`..`），根目录是空串。后端不知道挂载点和其他层的存在。
- **流（IFileStream）** 由后端创建，经 VFS 包装后交给调用方。接口与 axmol 完全一致。

## 源码布局

| 目录 / 文件 | 内容 |
|---|---|
| `src/vfs/Common.h`、`Config.h` | 命名空间宏 `NS_VFS_BEGIN/END`、导出宏 `VFS_DLL`、编译期开关 |
| `src/vfs/Error.h` | 错误码 `vfs::Error` 与 errno 映射 |
| `src/vfs/Path.h` | 路径规范化、拆分、可移植性检查 |
| `src/vfs/IFileStream.h` | 流接口 |
| `src/vfs/IBackend.h`、`Stat.h`、`DirEntry.h` | 后端接口与元数据 |
| `src/vfs/MountTable.h` | 挂载表与层栈计算 |
| `src/vfs/VirtualFileSystem.h` | 入口类 |
| `src/vfs/Log.h` | 可注入的日志输出 |
| `src/vfs/native/` | `NativeBackend`、`SingleFileBackend`、`NativeFileStream`、平台路径工具 |
| `src/vfs/memory/` | `MemoryBackend`、`MemoryFileStream` |
| `src/vfs/archive/` | 格式常量、`ArchiveReader`、`ArchiveWriter`、`ArchiveBackend`、`ArchiveFileStream`、数据来源 |
| `src/vfs/crypto/` | ChaCha20（RFC 8439）、XXH3 封装、XOR |

## 挂载表

每次 `mount`/`unmount`/`remount`/`addAlias`/`removeAlias` 都会复制当前的 `MountTable`、修改副本、再整体替换（copy-on-write）。

- `MountTable::mounts` 中每一项（`MountEntry`）包含 ID、规范挂载点、后端、优先级、标志，以及记录打开流数量的 `MountState`。
- 挂载项按 **priority 降序、相同时 ID 降序**（后挂载在前）排好序。
- 查询路径 P 时，`collectLayers` 依次挑出挂载点是 P 前缀的挂载，得到 P 的层栈（`Layer` = 挂载项 + P 在该后端中的相对路径）；遇到 `Opaque` 挂载后不再加入更低的层。
- 挂载点的祖先路径（以及根目录 `/`）被合成为只读的虚拟目录。
- `aliases` 保存前缀别名（例如 `res://` → `/res`），按前缀长度降序匹配，在规范化之前替换。

`VirtualFileSystem` 持有指向当前快照的 `shared_ptr<const MountTable>`。一个互斥锁只保护这个指针的读取与替换；另一个互斥锁串行化修改操作。查询时只在锁内复制指针，之后整个操作都在这份不可变快照上进行，所以一次调用看到的挂载表始终一致，查询之间也不会互相阻塞。

## 后端

| 后端 | 能力 | 说明 |
|---|---|---|
| `NativeBackend` | 读、写、rename、resize | 本机目录。路径段额外拒绝 `:`、结尾的 `.`/空格和 Windows 保留名，保证各平台行为一致；可选只读、禁止跟随逃出根目录的符号链接、核对真实文件名大小写（`strictCase` 警告 / `exactCase` 强制按字节匹配）、自动创建根目录。不做缓存，外部修改立即可见。修改操作（mkdir/rmdir/unlink/rename/列目录）在 `NativeUtils` 中直接用系统调用实现（POSIX 系统调用 / Win32），不经过 `std::filesystem`，以便精确映射错误码、支持 Windows 长路径与 POSIX 语义 |
| `SingleFileBackend` | 读、写、resize | 把一个本机文件挂到虚拟路径上（类似文件 bind mount）；挂载点下的任何子路径都返回 `NOT_DIR` |
| `MemoryBackend` | 读、写、rename、resize | 内存目录树。删除打开中的文件采用 unlink 语义：名字立即消失，已打开的流继续持有数据。`addFile` 可在只读模式下预填数据 |
| `ArchiveBackend` | 读 | `.vfs` 归档。索引 `ArchiveIndex` 解析后不可变，由后端和所有流共享 |

后端的能力通过 `IBackend::capabilities()` 返回的 `BackendCaps` 位（`Read`、`Write`、`Rename`、`Resize`）声明。一层是否可写 = 后端有 `Write` 能力，且挂载时没有 `ReadOnly` 标志。

## 流

| 实现 | 来源 | 说明 |
|---|---|---|
| `NativeFileStream` | 本机文件 | POSIX 上用 `open/lseek/read/write`，Windows 上用 `CreateFileW`（UTF-8 转 UTF-16，长路径自动加 `\\?\` 前缀，共享方式读、写、删除全开以允许打开期间删除或替换文件；`APPEND` 用 OVERLAPPED 偏移 `0xFFFFFFFF` 原子追加；短暂的共享冲突有限重试）。拒绝打开目录 |
| `MemoryFileStream` | 内存缓冲区 | 内存后端的文件，以及归档中 LZ4 压缩的文件（打开时整体解压） |
| `ArchiveFileStream` | 归档中未压缩的文件 | 每个流持有一个独立的来源句柄，按偏移读取；XOR 文件按文件内偏移即时解密；顺序读时省去多余的 seek |

`VirtualFileSystem::openFileStream` 返回的流外面还包了一层跟踪包装：它持有后端的 `shared_ptr` 并维护所在挂载的打开流计数。因此：

- **惰性卸载**：`unmount` 立即把挂载从命名空间中移除，但已打开的流仍然持有后端（归档索引、内存数据随之保活），直到最后一个流关闭；
- `unmount(id, UnmountFlags::FailIfBusy)` 在仍有打开的流时返回 `BUSY`；
- `remount` 原子地替换后端，旧流继续读旧后端。

所有流的公开 `open()` 都只对 `NativeFileStream` 有效；其他流只能由后端创建，直接调用 `open()` 返回 `false`（`errno = ENOTSUP`）。

## 线程模型

- `VirtualFileSystem` 的所有方法都可以被多个线程同时调用，包括挂载、卸载与读写并发。
- 后端必须自身线程安全：内置后端中，本机后端依赖操作系统；内存后端用读写锁保护目录树和每个文件的缓冲区；归档后端只读共享索引，每个流独立句柄，读取路径上没有锁。
- **单个流对象不是线程安全的**，同一时间只能被一个线程使用；不同流对象之间完全独立。
- VFS 不做文件锁：多个流同时读写同一文件时，结果与操作系统上的行为一致（内存后端保证内存安全，内容由时序决定）。
- 不提供异步 API；需要异步时由调用方的线程池包装同步接口。

## 错误报告

所有返回 `vfs::Error` 的接口在失败时同时设置 `errno`（`toErrno()` 映射到最接近的 POSIX 值），便于与 POSIX 风格的代码混用。返回指针或流的接口另有可选的 `Error* err` 输出参数。流方法按 POSIX 约定返回 `-1` 并设置 `errno`。

库内的诊断信息通过 `vfs::setLogger()` 注入的回调输出（级别 `VERBOSE`、`INFO`、`WARN`、`FAIL`），默认不输出。
