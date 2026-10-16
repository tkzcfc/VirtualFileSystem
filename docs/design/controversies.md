# VirtualFileSystem 重写：设计争议与决策文档

> 状态：重写前的设计调研（还没写代码）。日期：2026-10-16
> 参考对象：tkzcfc/VirtualFileSystem（HEAD `5a1f75f`）、axmol（`/workspace/axmol`，HEAD `7da7078ad`，2026-10-15）、Godot master（`core/io/file_access_pack.*`、`project_settings.cpp`）、Linux overlayfs 文档、PhysicsFS（`physfs.h`）、Unreal Pak、Bevy AssetSource。
> 写法：每条争议依次给出 **问题 → 业界做法 → 选项 → 决策**。决策部分直接定下来，按此实现即可。

---

## (a) 现有项目概要

### 功能
- 可以把**本机目录**（`NativeFileSystem`）、**内存**（`MemoryFileSystem`）、**pack 文件**（`PackFileSystem`，格式来自 tkzcfc/paktool）挂载到任意虚拟路径。
- 支持读文件、写文件（WRITE/APPEND）、`removeFile`、`createDir`、`isFile`、`isDir`、`enumerate`（列目录，多个挂载点合并去重）、`copyFile`。
- 没有 rmdir、rename、stat（size/mtime）、卸载保护、whiteout。

### 架构与主要类（src/vfs，约 2000 行，C++20）
| 类 | 职责 |
|---|---|
| `FileStream` | 抽象流。`Mode{READ,WRITE,APPEND}`，`SeekOrigin{CUR,SET,END}`，接口有 `open/close/seek/read/write/tell/size/isOpen`，全部用 `uint64_t`。和 axmol 的 IFileStream **不一致**。 |
| `FileSystem` | 后端基类，字段有 `archiveLocation`、`mntpoint`、`readonly`、`type`。虚函数：`enumerate/openFileStream/removeFile/isFile/isDir/createDir/init/basePath`。 |
| `VirtualFileSystem` | 持有 `vector<FileSystem*>`，按挂载顺序线性遍历，用 `starts_with(mntpoint)` 匹配路径。 |
| `NativeFileSystem/NativeFileStream` | 基于 `std::filesystem` 和 `std::fstream`。 |
| `MemoryFileSystem/MemoryFileStream/MemoryData` | 文件表是 `unordered_map<path, shared_ptr<MemoryData>>`，目录表是 `set<dir/>`。 |
| `PackFileSystem/PackFileStream/PackUtils` | 解析 pack 头和索引，每次打开都新建一个 `NativeFileStream`，按偏移读取。 |

### 挂载语义（现状）
- **优先级**：`mount()` 用 `push_back`，查找时从前往后遍历，所以**先挂载的优先**（相当于 PhysicsFS 的 append）。不能显式指定优先级。
- **读**：按顺序遍历，取第一个 `isFile` 为真的挂载。
- **写**：如果某个挂载里已有这个文件，就在那里打开（只读挂载直接失败）；否则依次尝试所有“覆盖该路径的可写挂载”，用第一个打开成功的。Memory 后端要求父目录存在，Native 后端在目录不存在时 open 失败，所以实际落到哪个挂载取决于“碰巧谁成功”。
- **createDir**：落到第一个覆盖该路径的可写挂载，并且**不管成败都直接 return**。
- **removeFile**：只删最上层可见的那份。下层有同名文件时会重新露出来。
- **enumerate**：用 `set` 按完整路径去重；挂载点的祖先目录会被合成为虚拟目录项；结果不排序；file/dir 类型冲突没有处理。

### Pack 格式（v0，大端）
```
Header(28B): "PACK" | u32 version(=0) | u32 indexSecret | u32 dataSecret | u64 indexOffset | u32 crc32(读出但从不校验)
Data ...
Index(到文件尾): 重复 { u64 offset | u32 length | u8 nameLen | u8 compression(0=None,1=Gzip) | name(XOR indexSecret) }
```
- 单文件最大 4 GB，文件名最长 255 字节，“加密”只是 4 字节 XOR，可选 gzip。
- 如果设置了 XOR 或 gzip，open 时会把**整个文件读进内存**；无压缩、无加密时才走“每次打开开一个新句柄 + 按偏移读”的路径（这正是用户要的模型）。

### 线程模型
- `VirtualFileSystem` 的互斥锁被 `#if 1` 宏**关掉了**，所以 mount/unmount 和查询并发时线程不安全。
- 每个流都是独立对象，pack 每次打开都开新 OS 句柄，所以多线程读不同流是安全的。
- `MemoryData` 用“写者计数 + 无写者时不加锁读”，实现有数据竞争：`load+store` 不是原子自增；读线程检查完计数后，写线程可能正好 resize vector。

### 主要缺陷（重写的理由）
1. `PackFileSystem::basePath()` 返回指向临时 `""` 的 `const std::string&`，是**悬垂引用（UB）**。另外 `isReadonly()` 在子类中被非虚函数遮蔽。
2. `unmount` 直接 `delete` 后端，没有引用计数；所有权靠裸指针。
3. `std::fstream` 用窄字符路径，**Windows 上非 ASCII 路径会失败**。WRITE 模式不截断已存在的文件，和 axmol 的 `O_TRUNC` 语义相反。`write` 失败时也返回 size。seek 只用 `seekg`。
4. Seek 用 `uint64_t` 偏移，CUR 无法向后退；END 的偏移方向和 Native 实现相反（Memory/Pack 是“减”，Native 是“加”）。
5. 写入目标不确定（见上文）；createDir 的分支逻辑有误；没有 rmdir/rename/stat。
6. Pack 的 `isDir`、`enumerate` 是 O(N) 全表扫描；目录树没有预先建立；crc 不校验；有 4 GB 和 255 字节的限制。
7. 用 printf/cerr 输出日志；错误只有 bool，无法区分 NotFound、ReadOnly、NotEmpty 等情况。
8. 路径大小写、Windows 保留名、symlink 逃逸都没处理。

---

## (b) axmol IFileStream 接口（原文）与适配问题

### `axmol/core/platform/IFileStream.h` 原文（逐字复制）
```cpp
// Copyright (c) 2019-present Axmol Engine contributors (see AUTHORS.md)
#pragma once

#include <errno.h>
#include <stdint.h>
#include <string>

#include "platform/PlatformMacros.h"

namespace ax
{

/**
 * @addtogroup platform
 * @{
 */

#if defined(_WIN32)
using osfhnd_t = void*;
#else
using osfhnd_t = int;
#endif

class AX_DLL IFileStream
{
public:
    virtual ~IFileStream() = default;

    enum class Mode
    {
        READ,
        WRITE,
        APPEND,
        OVERLAPPED,
    };

    /**
     *  Open a file
     *  @param path file to open
     *  @param mode File open mode, being READ, WRITE, APPEND
     *  @return true if successful, false if not
     */
    virtual bool open(std::string_view path, IFileStream::Mode mode) = 0;

    /**
     *  Close a file stream
     *  @return 0 if successful, -1 if not
     */
    virtual int close() = 0;

    /**
     *  Seek to position in a file stream
     *  @param offset how many bytes to move within the stream
     *  @param origin SEEK_SET, SEEK_CUR, SEEK_END
     *  @return offset from file begining
     */
    virtual int64_t seek(int64_t offset, int origin) const = 0;

    /**
     *  Read data from file stream
     *  @param buf pointer to data
     *  @param size the amount of data to read in bytes
     *  @return amount of data read successfully, -1 if error
     */
    virtual int read(void* buf, unsigned int size) const = 0;

    /**
     *  Write data to file stream
     *  @param buf pointer to data
     *  @param size the amount of data to write in bytes
     *  @return amount of data written successfully, -1 if error
     */
    virtual int write(const void* buf, unsigned int size) const = 0;

    /**
     *  Get the current position in the file stream
     *  @return current position, -1 if error
     */
    virtual int64_t tell() const = 0;

    /**
     *  Get the size of the file stream
     *  @return stream size, -1 if error (Mode::WRITE and Mode::APPEND may return -1)
     */
    virtual int64_t size() const = 0;

    /*
     * Resize file
     */
    virtual bool resize(int64_t /*size*/) const
    {
        errno = ENOTSUP;
        return false;
    }

    /**
     *  Get status of file stream
     *  @return true if open, false if closed
     */
    virtual bool isOpen() const = 0;

    /*
     * Get native handle if support
     * @returns -1 if not a local disk file or file not open
     *   Windows: HANDLE
     *   Other: file descriptor
     */
    virtual osfhnd_t nativeHandle() const { return (osfhnd_t)-1; }

    virtual operator bool() const { return isOpen(); }

protected:
    IFileStream(){};
};

// end of support group
/** @} */

}
```

### 关键语义（来自 `FileStream.cpp` 的实现）
| Mode | POSIX 标志 | Win32 | 含义 |
|---|---|---|---|
| READ | `O_RDONLY` | `GENERIC_READ, OPEN_EXISTING` | 只读，文件必须存在 |
| WRITE | `O_CREAT\|O_RDWR\|O_TRUNC` | `CREATE_ALWAYS` | 创建或**截断**，可读可写 |
| APPEND | `O_APPEND\|O_CREAT\|O_RDWR` | `OPEN_ALWAYS`，每次写前 seek 到 END | 追加，可读 |
| OVERLAPPED | `O_CREAT\|O_RDWR` | `OPEN_ALWAYS` | 不存在就创建，**不截断**，随机读写（UserDefault 的 mmap、Downloader 的 md5 断点续传用它） |

- 共享模式：Win32 是 `FILE_SHARE_READ|FILE_SHARE_WRITE`，**没有 `FILE_SHARE_DELETE`**，所以文件打开期间不能删除或替换。
- `seek(offset, SEEK_SET/CUR/END)` 直接转发给 `lseek64`/`SetFilePointerEx`，返回新位置，出错返回 -1。允许 seek 到 EOF 之后。
- `read/write` 一次最多 `unsigned int` 字节，返回 `int`，所以**单次调用上限约 2 GB**，出错返回 -1。
- `size()` 在 POSIX 上通过 seek_guard 两次 lseek 得到，不改变当前位置。
- `close()` 返回 int；析构时自动 close。`FileStream` 只能移动，不能拷贝。
- **工厂**：`virtual std::unique_ptr<IFileStream> FileUtils::openFileStream(std::string_view filePath, IFileStream::Mode mode) const;`，默认实现是 `FileStream fs; fs.open(...) ? make_unique<FileStream>(move(fs)) : nullptr`。**每次打开都产生一个独立的流对象**，所有权通过 unique_ptr 交给调用方。
- Android：相对路径会走 OBB（`ZipEntryInfo`）或 `AAssetManager`（只读，write/resize 返回 -1）。axmol 内部只有一个 `FileStream` 类，用 `PXIoF` 函数表实现多态；**没有** PosixFileStream、ZipFileStream、ResourceFileStream 这些独立类。
- 相关 FileUtils API：`getContents`、`isFileExistInternal`、`isDirectoryExistInternal`、`createDirectories`、`removeDirectory`（**递归删除**，nftw）、`removeFile`、`renameFile`、`getFileSize`、`listFiles`、`listFilesRecursively`、`writeDataToFile`（经 `openFileStream(WRITE)`）。

### 适配问题与决策
1. **接口逐字一致**。`vfs::IFileStream` 与 `ax::IFileStream` 的签名、枚举顺序、返回值约定完全相同，只是 namespace 和导出宏不同。用 `VFS_WITH_AXMOL` 宏切换到 `namespace vfs { using IFileStream = ax::IFileStream; using osfhnd_t = ax::osfhnd_t; }`，移植时一行不改。
2. **`const` 方法会改变状态**（位置）。axmol 本身也这样（OS 句柄内部有状态）。自有实现的游标用 `mutable int64_t _pos`。**单个流对象不是线程安全的**，不能跨线程同时使用；不同流对象可以完全并发。
3. **`open(path, mode)` 在接口上**。对 Native 流是真实的 open。对 Pack/Memory 流，由后端工厂通过私有的 `openEntry(...)` 构造，公开的 `open(path, mode)` 返回 false 并设 `errno=ENOTSUP`。用户永远应通过 `vfs.openFileStream()` 拿流，不要自己 new 流再 open。
4. **单次读写上限约 2 GB**：保持原样，`readAll` 等辅助函数分块循环读写。
5. **`size()` 允许返回 -1**：自有实现在所有模式下都返回真实大小。
6. **`nativeHandle()`**：只有 NativeFileStream 返回句柄。PackFileStream 一律返回 -1（底层虽有句柄，但偏移不是 0，交出去会被误用，比如 UserDefault 用它做 mmap）。
7. **`resize()`**：Native 和 Memory 支持；Pack 返回 ENOTSUP。
8. **errno 风格**：流层沿用 -1 + errno；VFS 层另用 `vfs::Error`（见争议 22）。
9. **axmol 的坑**：`FileUtils::getContents` **直接构造 `FileStream`，没有调用 `openFileStream`**。所以接入 VFS 时必须同时 override `getContents`（或者给 axmol 提 PR 让它改走 `openFileStream`），否则读取会绕过 VFS。
10. 读 pack 时建议给 Native 打开加上 `FILE_SHARE_DELETE`，这样热更新时可以 rename 或替换 pack（axmol 目前不加；自有的 NativeFileStream 加上它）。

---

## (c) 设计争议清单与决策

### 核心模型（先定下来，后面各条都基于它）
- **单一虚拟命名空间**：绝对路径、`/` 分隔、UTF-8、大小写敏感。
- **挂载（Mount）** = (mountPoint, backend, priority, flags)，其中 flags 包括 ReadOnly、Shadow 等。
- 对路径 P，所有满足“mountPoint 是 P 的前缀”的挂载构成 P 的**层栈**，按 priority 从高到低排序。**读**是 union（overlay 风格）；**写**只落到唯一确定的一层；**删除**只作用于可写层；**运行时不做 whiteout**。
- 后端（Backend）只负责自己的相对路径，完全不知道分层的存在；分层逻辑全部在 VFS 核心里。

---

### 1. 【争议 1】pack 和可写目录都挂在 `/` 时，能否删除 `/a`？VFS 要不要 mkdir/rmdir？
**问题**：pack 挂在 `/`，里面有 `a/b/c.txt`；本地可写目录也挂在 `/`，里面有 `a/`，新文件写进本地 `a/`。用户想 `rmdir /a`。

**业界做法**
- **Godot**：根本不让这种情况出现。`res://` 加载 pack 后，`DirAccess` 默认实现被换成 `DirAccessPack`（见 `project_settings.cpp` 中的 `make_default<DirAccessPack>`），它的 `make_dir/rename/remove` 一律返回 `ERR_UNAVAILABLE`；`FileAccess::open` 只在非 WRITE 模式下优先查 pack。可写数据走独立的 `user://`。`load_resource_pack(pack, replace_files=true)` 只决定同名文件是否被后加载的 pack 覆盖，属于纯只读层之间的覆盖。删除只能由 pack 自身的 `PACK_FILE_REMOVAL` 条目在加载时完成（`remove_path`）。
- **Linux overlayfs**：rmdir 一个 merged 目录时，要求**合并视图为空**，否则返回 ENOTEMPTY。目录如果在 lower 中存在，就在 upper 中留一个 whiteout（0/0 字符设备或 xattr）；之后再 mkdir 同名目录时，会建成 opaque 目录（`trusted.overlay.opaque=y`），不再合并 lower。
- **普通只读 FS**：EROFS。
- **PhysicsFS**：`PHYSFS_delete` 和 `PHYSFS_mkdir` 只作用于 write dir，“A directory must be empty”。删掉 write dir 里的文件后，search path 里的同名文件会重新可见，也没有 whiteout。

**选项**：A) 完全禁止合并只读层和可写层（Godot）；B) 实现 whiteout（overlayfs）；C) 不做 whiteout，删除只作用于可写层，只读层的内容删不掉（PhysicsFS）。

**决策（C + Godot 式推荐用法）**
1. **VFS 要提供 `createDir` 和 `removeDir`**。axmol FileUtils 有 `createDirectories/removeDirectory`，存档、缓存、下载目录都需要。
2. `removeDir(path, recursive=false)` 的规则：
   - 先找到可写层中的该目录。如果**任何只读层**（或任何不属于本次写入目标的层）在合并视图里**贡献了 path 或其子孙** → 返回 `Error::ReadOnly`（EROFS），**什么都不删**（先检查，再执行）。
   - 非递归：合并视图不为空 → `Error::NotEmpty`。
   - 递归：先检查；检查通过后只删可写层里的内容。
   - 回到例子：pack 提供了 `/a/b/c.txt`，所以 **`rmdir /a` 失败，返回 EROFS**。用户在本地 `a/` 里创建的文件可以逐个删除，`/a/b/c.txt` 始终存在。
3. 推荐用法（写进文档）：参照 Godot，把只读资源挂在 `/res`（pack 和开发目录做 union），可写数据挂在 `/user`（单一 Native 目录），这样从根本上避开这个问题。同点 union 的读写混合只用于“mod/补丁覆盖”这类场景。
4. 运行时永远不写 whiteout（理由见争议 4）。

### 2. Union（overlay）还是 Shadow（Linux mount 遮蔽）？
**问题**：同一挂载点（或嵌套挂载点）上有多个后端时，下层是合并显示还是被完全遮住？
**业界做法**：Linux `mount` 会完全遮蔽挂载点原有内容；overlayfs、PhysicsFS、Godot 多 pack、Unreal 多 pak 都是 union。
**决策**：**默认 Union**。每个 mount 可选 `MountFlags::Opaque`：它所在的挂载点以下，比它优先级低的层全部不可见，相当于 Linux mount 的遮蔽效果，也相当于 overlay 的 opaque 目录。

### 3. 挂载优先级 / 顺序
**业界做法**：PhysicsFS 用 `appendToPath` 决定 append（低优先级）还是 prepend；Godot 是后加载的 pack 在 `replace_files=true` 时覆盖前面的；Unreal 用 PakOrder 数值，`_P` 补丁包优先级更高；Linux 是后挂载的在上层。
**决策**：`mount(point, backend, MountOptions{priority=0, flags})` 返回 `MountId`。排序规则：**priority 大的优先；priority 相同时后挂载的优先**（和 Linux、Godot 一致；现有项目是先挂载优先，**行为反转了，要写进迁移说明**）。不按挂载点深度排序，见争议 15。

### 4. 要不要 whiteout / opaque？
**问题**：没有 whiteout 时，删除只读层中的文件做不到，删掉可写层的覆盖文件后原文件会“复活”。
**决策**：
- **运行时没有 whiteout**。原因：可写层是普通本机目录，whiteout 标记文件会污染用户目录；跨平台无法用字符设备或 xattr；会显著增加列目录和查找的复杂度。
- 删除可写层文件后下层同名文件重新露出，这是**特性**，含义是“撤销覆盖，恢复原版”，与 PhysicsFS 一致，需要写进文档。
- **pack 格式预留 `ENTRY_REMOVAL` 标志位**（对应 Godot `PACK_FILE_REMOVAL`），作为只读的、构建期的 whiteout，供补丁包删除旧文件用。v1 只解析并忽略，v1.1 再实现查找时的屏蔽。

### 5. 写入落到哪里（write target selection）
**决策**（确定性规则，没有“试到成功为止”）：
1. 先找 P 在合并视图中的**可见项**，即最高层命中。
2. 如果可见项所在的层可写 → 就写在那里。
3. 否则（不存在，或在只读层）→ 写入**覆盖 P 的最高优先级可写挂载**（以下称 write target）。如果没有这样的挂载 → `Error::ReadOnly`。
4. 如果 write target 的优先级**低于**持有可见项的只读层，那么写入之后的内容会被只读层挡住、读不到。为避免这种情况，此时返回 `Error::ReadOnly`（Shadowed），不允许“写了却读不到”。
5. 可以显式用 `MountFlags::WriteTarget` 标记默认写入层（类似 PhysicsFS 的 `setWriteDir`）。被标记的挂载在可写候选中优先。

### 6. 父目录不存在于可写层（copy-up 目录）
**问题**：例子中如果本地没有 `a/`，往 `/a/new.txt` 写入该怎么办？
**业界做法**：overlayfs 在 upper 中**隐式创建父目录链**（copy-up 目录）。
**决策**：只要父目录在**合并视图**中存在（任意层），就在 write target 中自动 `mkdir -p` 出相对路径链。父目录在合并视图中不存在 → `Error::NotFound`，和 POSIX 一致：open(O_CREAT) 不会创建父目录。

### 7. 修改只读层中的文件（copy-up 文件）
**决策**：
- `WRITE`（截断）：不需要拷贝，直接在 write target 中创建新文件，遮蔽下层。
- `APPEND`/`OVERLAPPED`：先**把下层内容完整拷贝**到 write target（overlay 的 copy-up），再打开。拷贝时写临时文件再 rename，保证原子性。
- 大文件 copy-up 有开销，在文档中说明。

### 8. 删除文件（removeFile）
**决策**：只删可见项。如果可见项在只读层 → `Error::ReadOnly`。如果在可写层 → 删除，并且**允许下层同名文件重新露出**（见争议 4）。`stat` 返回 `mountId`，调用方可以判断删除后是否还有原版。打开中的文件被删除，见争议 18。

### 9. rename / move
**业界做法**：Linux 跨文件系统 rename 返回 EXDEV；overlayfs 在没有 `redirect_dir` 时对 lower 中的目录 rename 也返回 EXDEV；Godot 的 pack rename 返回 ERR_UNAVAILABLE。
**决策**：`rename(from, to)` 只在**源可见项和目标都落在同一个可写 backend 实例上**时成功（调用 backend 的原生 rename，原子）。其他情况：源在只读层 → `ReadOnly`；跨后端 → `CrossDevice`（EXDEV）。另外提供辅助函数 `moveFile()`：仅对文件生效，实现为 copy + remove，不保证原子性。目录跨后端移动不支持。rename 之后源路径的下层同名文件可能露出，规则同争议 8。

### 10. mkdir 语义
**决策**：
- `createDir(p)`：p 在合并视图中已是目录 → `Error::Exists`；已是文件 → `Error::Exists`（NotDir）；父目录必须在合并视图中存在。
- `createDirs(p)`：mkdir -p 语义；已存在的目录不算错误，对应 axmol 的 `createDirectories`。
- 所有目录都创建在 write target（按争议 5 的规则）中。仅存在于只读层的中间目录会被自动 copy-up 到可写层（争议 6）。

### 11. 路径规范化与 `..` 逃逸
**决策**：
- 入口统一规范化：`\` 转成 `/`；合并重复的 `/`；去掉 `.`；按字面消解 `..`。**消解后越过根 → `Error::InvalidPath`**（不做截断处理，也不报静默错误），现有项目也是这样处理的。
- 结尾的 `/` 只用来提示“这是目录”，不影响查找；内部规范形式是 `/a/b`（根是 `/`）。
- 拒绝 NUL、控制字符；Native 后端额外拒绝 `:` 以及 Windows 保留名（CON、NUL、COM1 等），参考 PhysicsFS 禁止 `:`。
- 后端收到的路径是**相对路径、已规范化、不含 `..`**，Native 后端拼接 root 之后不可能逃逸（symlink 情况见争议 20）。

### 12. 大小写敏感
**问题**：Windows/macOS 的本机 FS 不区分大小写，pack 和 Linux 区分，导致开发期能跑，发布后找不到文件。
**决策**：VFS **区分大小写**（字节比较），与 PhysicsFS 和 pack 一致。pack 打包工具检查**大小写冲突**，冲突则报错。Native 后端在 Debug 构建下可选 `strictCase` 检查（核对真实文件名的大小写，不一致时打警告），帮助开发期提前发现问题。

### 13. 目录列表的合并与去重
**决策**：
- `list(dir)` 汇总所有层的子项，**按名字去重，同名时高层决定类型和属性**。
- **类型冲突**：高层是文件、低层是目录时，只显示文件，并且查找 `/x/y` 返回 NotDir（overlay 规则：upper 中的非目录项会遮蔽 lower）。高层是目录、低层是文件时，按目录合并。
- 挂载点的祖先路径合成为只读虚拟目录（例如挂载在 `/a/b`，那么列 `/` 时能看到 `a`）。
- 结果**按名字字节序排序**，保证确定性。
- Opaque 挂载以下不合并更低层（见争议 2）。
- 回调形式 `bool(const DirEntry&)`，返回 false 停止遍历（现有项目是返回 true 停止，**语义反转，要在文档中说明**）。另提供返回 `vector` 的便利重载。

### 14. 打开模式与截断
**决策**：完全沿用 axmol 的 4 种 Mode（见 (b) 的表格），在所有后端上语义一致：WRITE 截断（**修正**现有项目的 bug）；APPEND 每次 write 都写到末尾；OVERLAPPED 不截断、可读写、文件不存在则创建。READ 打开不存在的文件返回 null，errno 为 ENOENT。只读层上的写模式按争议 5 和 7 处理。暂不增加 EXCLUSIVE/CREATE_NEW 模式（以免偏离 axmol）；需要时用 `exists()` 先检查。

### 15. 嵌套挂载（先挂 `/a`，再挂 `/a/b`）
**业界做法**：Linux 中更深的挂载点必然在上面（路径解析会穿过挂载点）；PhysicsFS 的挂载点只是前缀，优先级按 search path 顺序。
**决策**：层栈 = **所有挂载点是 P 前缀的挂载，按 priority 排序**，与挂载深度无关。如果希望更深的挂载完全接管其子树，给它加 `Opaque` 标志即可。挂载点本身不能被 `removeDir` 删除（返回 `Busy`，相当于 EBUSY），也不能被 rename。可以把挂载点建在合并视图中并不存在的路径上，它的祖先目录会以虚拟目录形式显示出来。

### 16. 挂载单个文件
**决策**：支持。`mount("/config.json", SingleFileBackend("D:/x.json"))`，相当于 Linux 的文件 bind mount。实现上它是一个只含一个条目的后端，查找规则和其他后端一样。用途：覆盖单个配置或调试文件。挂载点路径下的子路径一律返回 NotDir。

### 17. 挂载表变更的线程安全
**决策**：挂载表采用 **copy-on-write 快照**：`std::atomic<std::shared_ptr<const MountTable>>`（C++20），或者 `shared_ptr` 配合 mutex 只保护指针交换。查询时无锁拿到快照；mount/unmount 时在锁内复制、修改、发布新快照。每个操作在单一快照上完成，保证一致性。不使用递归锁。要求后端自身线程安全（见争议 21）。

### 18. 卸载时仍有打开的流 / 删除打开中的文件
**业界做法**：Linux `umount` 默认返回 EBUSY，`umount -l`（MNT_DETACH）是惰性卸载，已打开的 fd 继续可用；PhysicsFS 卸载返回 `PHYSFS_ERR_FILES_STILL_OPEN`；Unix 上 unlink 打开中的文件后，fd 仍然可用。
**决策**：
- **惰性卸载**：unmount 立即从命名空间移除；流持有 `shared_ptr<Backend>`（pack 索引、内存数据随之保活），直到最后一个流关闭才真正释放。游戏卸载 DLC 时，加载线程可以读完正在读的文件。
- 提供 `unmount(id, UnmountFlags::FailIfBusy)` 可选严格模式。
- Memory 后端删除打开中的文件：采用**unlink 语义**，名字立即移除，数据由流继续持有（现有项目在这种情况下删除失败，改掉）。
- Native 后端删除打开中的文件：遵循 OS 行为；Windows 上因为 axmol 不带 `FILE_SHARE_DELETE` 会失败，自有的 NativeFileStream 加上它。

### 19. 同一文件的并发读写
**决策**：VFS **不做文件锁**，行为和 OS 一致。
- 同一个流对象不能跨线程使用；不同流对象可以并发。
- Pack 内容不可变，并发读天然安全。
- Memory 后端每个文件节点用 `std::shared_mutex` 保证**内存安全**（不会崩溃），但读和写交错时内容由时序决定。
- Native 后端由 OS 决定。
- 需要原子保存时，使用辅助函数 `writeFileAtomic()`：写 `xxx.tmp` 再 rename。

### 20. 符号链接
**决策**：VFS 本身**不支持 symlink**，没有 `symlink()` API，`stat` 也不报告链接类型。Native 后端选项 `followSymlinks`，默认 `true`（开发方便）；设为 `false` 时（沙箱、处理下载内容），对 canonical 路径做检查，逃出 root 则拒绝（PhysicsFS 默认不跟随 symlink）。pack 格式里没有链接。

### 21. 自定义挂载类型接口
**决策**：
```cpp
class IBackend {               // 线程安全要求：所有方法可被多线程并发调用
public:
  virtual ~IBackend() = default;
  virtual uint32_t capabilities() const = 0;           // Read|Write|Rename|Resize|...
  virtual Error stat(std::string_view rel, Stat& out) const = 0;
  virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode m, Error* err) = 0;
  virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const = 0;
  virtual Error createDir(std::string_view rel) { return Error::ReadOnly; }
  virtual Error removeDir(std::string_view rel) { return Error::ReadOnly; }   // 仅空目录
  virtual Error removeFile(std::string_view rel) { return Error::ReadOnly; }
  virtual Error rename(std::string_view from, std::string_view to) { return Error::ReadOnly; }
};
```
- 相对路径不带前导 `/`，空串表示后端根目录。
- union、copy-up、递归删除都由核心组合这些原语实现，后端只需提供原语。
- 内置后端：`NativeBackend`、`MemoryBackend`、`PackBackend`、`SingleFileBackend`；后续可加 `AndroidAssetBackend`。
- 后端在构造或 `create()` 时完成初始化，失败返回 null 和错误码；不再有现有项目那种 `init()` 失败后 `delete` 的写法。

### 22. 错误报告风格
**决策**：
- 不用异常。VFS API 返回 `vfs::Error` 枚举：`Ok, NotFound, Exists, NotDir, IsDir, NotEmpty, ReadOnly, CrossDevice, Busy, InvalidPath, PermissionDenied, IoError, Unsupported`，提供 `toErrno()`。
- 返回流的 API 用 `unique_ptr` 加可选的 `Error*` 输出参数，同时设置 errno，与 axmol 兼容。
- 日志通过可注入的 `setLogger(fn)` 输出，库内不出现 printf。

### 23. 要不要异步 API
**业界做法**：Bevy 的 `AssetReader` 是 async 的；Godot、PhysicsFS 是同步的；Unreal 有 AsyncIO。
**决策**：核心**只提供同步、线程安全的 API**；需要异步时由引擎的线程池（axmol 的 `AsyncTaskPool`、`performOperationOffthread`）包装。不内置线程池，不依赖协程。

### 24. pack 文件格式（全新的 v1，不兼容 paktool v0）
**决策**：
```
Header (64B, little-endian):
  magic "VFSPAK\0\1" (8) | u32 version=1 | u32 flags | u64 indexOffset | u64 indexSize
  | u64 dataOffset | u32 entryCount | u32 alignment | u64 indexHash(xxh64) | reserved(8)
Data: 每个文件起点按 alignment 对齐（默认 16，可配置 4096 以便 mmap/direct I/O）
Index (文件尾):
  Entry[entryCount] 按 pathHash 排序: u64 pathHash(xxh64 of 规范化路径) | u64 offset | u64 size
                     | u32 nameOffset | u16 nameLen | u16 flags(DIR / REMOVAL预留 / ...) | u64 contentHash(可选,0=无)
  StringTable: UTF-8 路径（规范化、无前导/）
```
- 64 位 offset 和 size，**去掉 4 GB 与 255 字节的限制**。
- **不压缩、不加密**（用户明确要求，可完全随机访问）。flags 预留“加密”位；将来要加密，只允许可随机访问的方式（如 AES-CTR，或按 offset 生成密钥流的 XOR），禁止用块链模式。
- 挂载时一次性读入索引，构建 `unordered_map<hash, idx>` 和目录树，`isDir`/`list` 都是 O(1) 或 O(子项数)。哈希命中后再用字符串比较一次，防止碰撞。
- 允许 `DIR` 条目表示空目录（现有格式无法表示空目录）。
- `indexHash` 在挂载时校验；`contentHash` 只在 `verify()` 中按需校验（Godot 存 md5，但运行时不校验）。
- **mount 支持 `baseOffset`**：pack 可以拼接在 exe 或其他文件后面（Godot `load_resource_pack` 的 offset 参数）。也可以在文件尾放一个 footer 指向 header，以便自动定位。
- 打包工具（写在同一仓库中）先写 data，最后写 index，再回填 header。

### 25. pack 的多线程读取策略
**决策**：
- 默认：**每次打开新建一个 OS 句柄**（`NativeFileStream`，READ 模式），seek 到 `base+offset`，读取时把范围限制在 `[0,size)` 内，满足用户要求，零锁。
- 可选优化 `PackOptions::sharedHandle`：所有流共享一个句柄，用 `pread`/Windows `ReadFile+OVERLAPPED.Offset` 按绝对偏移读取，没有 seek 竞争，可以省掉大量 open 开销（大量小文件时 open 是瓶颈）。v1 先实现默认方案，接口预留这个选项。
- 句柄上限：每个打开的流占一个 fd，Android/iOS 的 fd 上限需要考虑，在文档中提醒。
- Android：APK assets 中的 pack 必须**不压缩存储**（gradle `noCompress`），通过 `AAsset_openFileDescriptor64` 取得 fd + start 后按同样方式读取。

### 26. 内存挂载语义
**决策**：`MemoryBackend` 是内存中的树（节点为目录，或带 `shared_ptr<Buffer>` 的文件），可读写，支持 mkdir/rmdir/rename/resize。每个节点有 `shared_mutex`，采用 unlink 语义（见争议 18）。可以用 `addFile(path, bytes)` 预填数据，或设置成只读。**另外**，`PackBackend` 的数据源抽象为 `IPackSource`（文件路径或内存 span），这样嵌入二进制里的 pack 也能挂载。mtime 记录写入时间。

### 27. 元数据（stat）
**决策**：`Stat { Type type; int64_t size; int64_t mtime /*unix秒, 0=未知*/; MountId mount; bool writable; }`。
- Native：取真实值。
- Pack：size 用条目大小，mtime **统一使用 pack 文件本身的 mtime**，保证稳定，pack 更新后会变化，可以用于缓存失效判断。Godot 这里返回 0。
- Memory：使用写入时间。
- 合成的虚拟目录：mtime 为 0。
- 不提供权限位和 atime。

### 28. Windows 上的 UTF-8 路径
**决策**：所有 API 用 UTF-8 `std::string_view`。Native 后端在 Windows 上转成 UTF-16 再调用 `CreateFileW` 系列；axmol 用 `ntcvt::from_chars`，自有实现用 `MultiByteToWideChar` 或同等方式。路径超过 260 时自动加 `\\?\` 前缀。**禁止使用窄字符版 `std::fstream`、`fopen`**，这是现有项目的 bug。`std::filesystem` 仅在内部使用，并且一律通过 `u8path` 或 `char8_t` 构造。

### 29. 命名
**决策**：
- 流接口：**`IFileStream`**，与 axmol 一致，不改成 FileStream。axmol 中 `FileStream` 是本机实现类，如果把接口叫 FileStream，移植时会冲突。
- 实现类：`NativeFileStream`（移植自 axmol `FileStream` 的 lowio 实现）、`PackFileStream`、`MemoryFileStream`。
- 核心类：`VirtualFileSystem`；后端：`IBackend` / `NativeBackend` / `MemoryBackend` / `PackBackend` / `SingleFileBackend`。
- 命名空间 `vfs`，不用宏（去掉 `NS_VFS_BEGIN`）。

### 30. 路径 scheme（res:// 和 user://）
**决策**：核心**不支持 scheme**，只处理纯路径。另外提供一个 `alias("res://", "/res")` 前缀映射辅助，方便迁移 Godot 风格代码和接入 axmol。推荐约定：`/res` 为只读 union，`/user` 为可写，`/tmp` 为 Memory 后端。

### 31. 目录存在性与“空目录”
**决策**：在任一层中是目录，就算目录。pack 中的目录由文件路径推导，加上显式的 DIR 条目。mkdir 后的空目录只存在于可写层。合并视图中“目录只有在有子项时才存在”的歧义，靠 DIR 条目和可写层的实体目录消除。

### 32. 缓存与外部修改
**决策**：Pack 和 Memory 的索引本身就在内存里，无需缓存。Native 后端**不做任何缓存**，每次都 stat，因为外部程序和热重载会修改文件；也不做负缓存（缓存“不存在”的结果）。如果有性能需求，由上层（axmol 的 fullPathCache）自行缓存，VFS 提供 `generation()` 计数，mount、unmount 或写操作时递增，上层据此让缓存失效。

### 33. seek 越界与 EOF 语义
**决策**：所有实现与 POSIX 一致：seek 结果为负 → -1/EINVAL；允许 seek 到 EOF 之后。写模式下之后的 write 会扩展文件并在中间填零（Memory 后端同样处理）；读模式下 read 返回 0。Pack 流也允许 seek 越界，读返回 0。

### 34. 热更新时替换已挂载的 pack
**决策**：提供 `remount(id, newBackend)`，在一次快照交换内原子替换，旧流继续读旧句柄（见争议 18）。Windows 上替换文件需要 `FILE_SHARE_DELETE`，否则应先下载到新文件名再挂载新文件。不支持就地修改 pack。

### 35. 接入 axmol 的方式（移植时用）
**决策**：实现 `FileUtilsVFS : FileUtils`，override `isFileExistInternal`、`isDirectoryExistInternal`、`openFileStream`、**`getContents`**（必须，见 (b)-9）、`listFiles(Recursively)`、`createDirectories`、`removeDirectory`、`removeFile`、`renameFile`、`getFileSize`、`isAbsolutePath`。search path 改成 VFS 虚拟路径。依赖真实路径或 `nativeHandle()` 的模块（UserDefault 的 mmap、视频播放、部分第三方库会直接 fopen）需要放到 `/user` 的 Native 后端上，或者通过 `realPath(vpath)` API 取得本机路径；只有 Native 后端会返回非空结果。

---

## 决策速查表
| # | 争议 | 决策 |
|---|---|---|
| 1 | 同点 pack+可写目录能否删 `/a` | 不能。只读层有贡献就返回 EROFS，先检查后执行；保留 mkdir/rmdir；推荐 /res 与 /user 分离 |
| 2 | union vs shadow | 默认 union，可选 Opaque 遮蔽 |
| 3 | 优先级 | priority 大的优先，相同则后挂载优先 |
| 4 | whiteout | 运行时不做；pack 预留 REMOVAL 位 |
| 5 | 写入目标 | 可见项在可写层就写原处，否则写最高的可写挂载；写了会被挡住则拒绝 |
| 6 | 父目录 | 合并视图中存在就在可写层自动 mkdir -p |
| 7 | 改只读文件 | WRITE 直接遮蔽；APPEND/OVERLAPPED 先 copy-up |
| 8 | 删除文件 | 只删可写层，下层可重新露出 |
| 9 | rename | 仅同一可写后端内；否则 EXDEV/EROFS；提供 moveFile 辅助 |
| 10 | mkdir | createDir 严格，createDirs 等同 mkdir -p |
| 11 | 路径 | 统一规范化，`..` 越根报错，后端只收相对路径 |
| 12 | 大小写 | 区分；打包工具查冲突；Debug 下可选严格检查 |
| 13 | 列目录 | 合并、去重、高层决定类型、按名字排序 |
| 14 | 打开模式 | 完全沿用 axmol 的 4 种 Mode，WRITE 截断 |
| 15 | 嵌套挂载 | 按 priority 排序，不按深度；挂载点不可删 |
| 16 | 单文件挂载 | 支持 |
| 17 | 挂载表线程安全 | COW 快照，查询无锁 |
| 18 | 卸载时有打开的流 | 惰性卸载，流持有后端；unlink 语义 |
| 19 | 并发读写 | 不加锁，保证内存安全；提供 writeFileAtomic |
| 20 | symlink | VFS 不支持；Native 后端可选禁止跟随 |
| 21 | 自定义后端 | 使用 IBackend 原语接口，分层逻辑在核心 |
| 22 | 错误 | Error 枚举；流层沿用 -1+errno；可注入 logger |
| 23 | 异步 | 不提供，由上层包装 |
| 24 | pack 格式 | 新 v1：小端、64 位、对齐、哈希索引、无压缩无加密、支持 baseOffset |
| 25 | pack 多线程 | 默认每次打开一个新句柄；预留共享句柄 + pread |
| 26 | 内存挂载 | 可写树，shared_mutex，unlink 语义；pack 可从内存加载 |
| 27 | 元数据 | type/size/mtime/mount/writable；pack 的 mtime 用 pack 文件的 |
| 28 | Windows UTF-8 | 统一 UTF-8 转 UTF-16 调 W API，禁用窄字符 fstream |
| 29 | 命名 | IFileStream 与 axmol 一致，实现类加前缀 |
| 30 | scheme | 核心不支持，提供 alias 辅助 |
| 31 | 空目录 | 支持 DIR 条目，可写层有实体目录 |
| 32 | 缓存 | Native 不缓存；提供 generation 计数 |
| 33 | seek/EOF | 与 POSIX 一致 |
| 34 | 热替换 pack | remount 原子替换 |
| 35 | 接入 axmol | 写 FileUtilsVFS 子类，必须 override getContents |
