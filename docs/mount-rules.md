# 挂载与分层规则

本文定义 `VirtualFileSystem` 在多层挂载下的查找、列目录、写入、删除和改名行为。所有规则都是确定性的：同样的挂载表和同样的调用，总是得到同样的结果。

## 1. 路径

- 虚拟路径使用 `/` 分隔，UTF-8 编码，**区分大小写**（按字节比较）。
- 入口统一规范化：`\` 视为 `/`；合并重复的 `/`；去掉 `.`；按字面消解 `..`，**越过根目录返回 `INVALID_PATH`**；结尾的 `/` 被忽略。规范形式为 `/a/b`，根为 `/`。不带前导 `/` 的路径按根目录下的路径处理（`a/b` 等同 `/a/b`）。
- 含 NUL 或其他控制字符（`0x00-0x1F`、`0x7F`）的路径返回 `INVALID_PATH`。
- 本机后端另外拒绝含 `:` 的名字、以 `.` 或空格结尾的名字，以及 Windows 保留名（`CON`、`PRN`、`AUX`、`NUL`、`COM1`-`COM9`、`LPT1`-`LPT9`，不区分大小写，带扩展名同样拒绝），在所有平台上一致返回 `INVALID_PATH`。
- **大小写与本机文件系统**：内存后端、归档后端按字节匹配名字。本机后端默认把查找交给操作系统，所以结果随文件系统而定：Linux（ext4 等）区分大小写；Windows NTFS 和 macOS APFS/HFS+ 默认**不区分**大小写（保留原样），`/res/A.png` 能打开磁盘上的 `a.png`，写 `case.txt` 会覆盖已有的 `Case.txt`。这一差异同样适用于本机目录作为只读下层的情况（例如内存层在上、本机目录在下）。需要所有平台行为一致时，给本机层打开 `NativeOptions::exactCase`：
  - 每级路径都与磁盘上的真实名字按字节比较，只差大小写（或 Unicode 规范化形式不同，如 macOS 上 NFC/NFD）的名字一律视为不存在：`stat`/`exists`/读取/列目录/删除/改名源返回 `NOT_FOUND`。
  - 在不区分大小写的文件系统上，写入、`createDir` 或改名的目标与一个已有条目只差大小写时返回 `EXISTS`，而不是悄悄写到那个已有文件上（区分大小写的文件系统上它们本来就是不同的文件，正常创建）。只改大小写的改名（`a.txt` → `A.txt`）允许。
  - 代价是每次访问对每一级路径多一次系统调用（Windows 上 `FindFirstFileExW`，macOS 上 `getattrlist`；Linux 上是一次 `lstat`）。默认关闭；`strictCase` 只在不一致时输出警告，不改变结果。
  - 推荐：开发期在 Windows/macOS 上对资源目录打开 `exactCase`（或至少 `strictCase`），可以尽早发现发布到归档后才会暴露的大小写错误。
- 别名在规范化之前按最长前缀替换：`addAlias("res://", "/res")` 之后，`res://img/a.png` 等同 `/res/img/a.png`。

## 2. 挂载与层栈

```cpp
MountId mount(mountPoint, backend, MountOptions{priority, flags});
```

- 对路径 P，**所有挂载点是 P 前缀（按路径段比较）的挂载**构成 P 的层栈，与挂载点的深度无关。
- 排序：**priority 大的在上；priority 相同时后挂载的在上**。
- `MountFlags::Opaque`：在该挂载的挂载点以下，比它低的层全部不可见（相当于 Linux 的 mount 遮蔽）。比它高的层仍然可见。
- `MountFlags::ReadOnly`：即使后端可写，这一层也按只读处理。
- `MountFlags::WriteTarget`：在可写层中优先作为写入目标（见 §5）。
- 挂载点可以建在合并视图中不存在的路径上，它的祖先路径会显示为**只读的虚拟目录**。根目录 `/` 始终存在。
- 挂载点的祖先路径**始终是目录**：低层在同一位置上有同名文件时，该文件被遮蔽（`stat`/`list` 显示为目录，`removeFile`/`writeFile` 返回 `IS_DIR`），保证挂载点可达。
- 挂载点和它的祖先不能被 `removeDir` 删除，也不能被 `rename`，返回 `BUSY`。

推荐用法：只读资源挂在 `/res`（归档与开发目录叠加），可写数据挂在 `/user`（单一本机目录）。把只读层和可写层叠在同一个挂载点上，适合“补丁覆盖”这类场景。

## 3. 查找

- P 的**可见项**是层栈中自上而下第一个包含 P 的层中的那一项；`Stat::mount` 给出它所在的挂载 ID，`Stat::writable` 表示该层是否可写。
- **类型冲突**：上层是文件、下层是同名目录时，只看到文件，查找其子路径返回 `NOT_DIR`。上层是目录、下层是同名文件时，按目录处理。
- 路径中间某一段在合并视图中是文件时返回 `NOT_DIR`。

## 4. 列目录

- 汇总层栈中所有层在该目录下的子项，**按名字去重，同名时上层决定类型**；类型冲突时按 §3 处理。
- 挂载点的祖先目录会列出下一级的挂载路径段。
- 结果**按名字字节序排序**。
- 回调形式 `bool(const DirEntry&)`，返回 `false` 停止遍历。另有返回 `std::vector<DirEntry>` 的重载和深度优先的 `walk`。
- 目标是文件返回 `NOT_DIR`，不存在返回 `NOT_FOUND`。
- 某一层列目录时返回 `NOT_FOUND`/`NOT_DIR`/`INVALID_PATH` 以外的错误（例如 `IO_ERROR`、`PERMISSION_DENIED`）时，整个 `list` 返回该错误，不会把出错的层当作空目录。

## 5. 写入目标

以写模式（`WRITE`、`APPEND`、`OVERLAPPED`）打开、`writeFile`、`createDir` 等写操作按以下顺序决定落在哪一层：

1. 找到 P 的可见项。
2. 可见项所在的层可写 → 写在那一层。
3. 否则（P 不存在，或可见项在只读层）→ 写到**覆盖 P 的可写层**中：带 `WriteTarget` 标志的优先，其次是层栈中最高的可写层。没有可写层 → `READ_ONLY`。
4. 如果选中的可写层**低于**持有可见项的只读层，写入后会被只读层挡住、读不到，此时返回 `READ_ONLY`，不写入。

### 父目录（目录 copy-up）

写入 `/a/b/new.txt` 时，只要父目录 `/a/b` 在合并视图中存在（任意层），就在写入层中自动创建缺失的父目录链。父目录在合并视图中不存在 → `NOT_FOUND`（与 `open(O_CREAT)` 一致，不会凭空创建父目录）；父路径是文件 → `NOT_DIR`。

### 修改只读层中的文件（文件 copy-up）

- `WRITE`（截断）：直接在写入层新建文件，遮蔽下层，不复制旧内容。
- `APPEND`、`OVERLAPPED`：先把下层文件完整复制到写入层（先写唯一命名的临时文件 `path.vfs-copyup-N.tmp` 再改名，失败不会留下半个文件），再打开副本。copy-up 在 VFS 内串行执行，并发追加同一个只读层文件时只复制一次，各线程的追加内容都会保留。大文件 copy-up 有相应的复制开销。

## 6. 创建目录

- `createDir(P)`：P 已存在（无论是目录还是文件）→ `EXISTS`；父目录必须在合并视图中存在，否则 `NOT_FOUND`；父路径是文件 → `NOT_DIR`。目录创建在按 §5 选出的写入层中，只存在于只读层的中间目录会自动复制到写入层。
- `createDirs(P)`：`mkdir -p` 语义，已存在的目录（包括其他线程并发创建的）不算错误；路径中某一段是文件 → `NOT_DIR`，最后一段是文件 → `EXISTS`。其他线程在中途删掉了上级目录时从头重试（最多 16 次），持续被删除才返回 `NOT_FOUND`。
- 并发创建、删除同一组目录时，本机后端只报告上面列出的错误码：父目录正被删除时各系统给出的 errno 不同（Linux `ENOENT`，macOS 可能是其他值），`mkdir` 失败后会重新确认父目录，已不存在则报告 `NOT_FOUND`。
- 在虚拟目录下创建、且没有覆盖它的可写层 → `READ_ONLY`。

## 7. 删除文件

- 只删除可见项。可见项在只读层 → `READ_ONLY`；是目录 → `IS_DIR`；不存在 → `NOT_FOUND`。
- 删除可写层中的覆盖文件后，**下层的同名文件会重新可见**。这是有意的设计：删除覆盖即“恢复原版”。运行时不写任何 whiteout 标记，只读层中的文件无法被删除。
- 单文件挂载的挂载点本身不能删除（`BUSY`）。
- 打开中的文件被删除：内存后端采用 unlink 语义，流继续可用；本机后端遵循操作系统。Windows 上同样允许：流以 `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` 打开，删除使用 POSIX 语义（`FileDispositionInfoEx`，Windows 10 1709+ 的 NTFS），名字立即消失，已打开的流继续可用；不支持 POSIX 语义的文件系统（FAT/exFAT、部分网络盘、Wine）退回传统删除，名字在最后一个句柄关闭后才消失，期间在同一位置创建同名文件会失败。

## 8. 删除目录

`removeDir(P, recursive = false)`：

1. P 不存在 → `NOT_FOUND`；是文件 → `NOT_DIR`。
2. P 是挂载点或挂载点的祖先（包括根目录）→ `BUSY`。
3. **先检查、再执行**：统计层栈中有多少层把 P 提供为目录。多于一层（其他层可能贡献了子孙），或唯一的那一层只读 → `READ_ONLY`，什么都不删。
4. 非递归：P 不为空 → `NOT_EMPTY`。
5. 递归：检查通过后删除该层中的整棵子树。

例：归档挂在 `/` 提供 `/a/b/c.txt`，本机可写目录也挂在 `/` 并含 `/a/mine.txt`。`removeDir("/a")` 返回 `READ_ONLY`；`/a/mine.txt` 可以单独删除，`/a/b/c.txt` 始终存在。

## 9. 改名与移动

- `rename(from, to)`：**源的可见项和目标都落在同一个可写挂载上**时，调用该后端的原生 rename（原子）。目标已存在且所在层可写时，目标落在那一层；否则按 §5 选择写入层，目标的父目录必须存在。目标已存在时的覆盖行为由后端按 POSIX 规则处理（文件覆盖文件，目录覆盖空目录）。
- 错误：源不存在 → `NOT_FOUND`；源或目标是挂载点或其祖先 → `BUSY`；源在只读层 → `READ_ONLY`；源目录由多个层合并而成（其他层也有同名目录）→ `READ_ONLY`（与 §8 的 `removeDir` 一致，否则只有写入层中的部分会被移走）；把目录移到自己的子树里 → `INVALID_PATH`；文件改名到已有目录 → `IS_DIR`；目录改名到已有文件 → `NOT_DIR`；源和目标落在不同挂载 → `CROSS_DEVICE`；后端不支持 rename → `UNSUPPORTED`。`from` 与 `to` 相同时直接成功。
- 改名后，源路径下层的同名文件可能重新可见（同 §7）。
- 本机后端在 Windows 上按 `rename(2)` 的语义实现：文件覆盖文件先用 `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`，目标正被其他句柄打开而失败时改用 POSIX 语义的 `FileRenameInfoEx`（同样要求 Windows 10 1709+ 的 NTFS），这样可以替换打开中的文件，旧句柄继续读到旧内容。不支持 POSIX 语义时，目标被打开期间替换返回 `PERMISSION_DENIED`（`writeFileAtomic` 同理）。目录覆盖空目录在 Windows 上分两步（先删目标再改名），不是原子的。
- 并发竞争时报告与原子 `rename(2)` 一致的错误码（Windows 上的预检查观察到的冲突会重新执行确认，避免把竞争中的瞬时状态报告成 `NOT_EMPTY`/`NOT_DIR`/`IS_DIR`）：源被其他线程移走或删除 → `NOT_FOUND`（而不是 Windows 返回的 `ACCESS_DENIED`）；目标是非空目录 → `NOT_EMPTY`（macOS/BSD 的 `rename`/`rmdir` 在这种情况下返回 `EEXIST`，同样映射为 `NOT_EMPTY`）。Windows 上杀毒软件、索引服务短暂占用文件造成的 `ACCESS_DENIED`/`SHARING_VIOLATION` 会有限重试（`ACCESS_DENIED` 约 63ms；`SHARING_VIOLATION`/`LOCK_VIOLATION` 一定由其他句柄造成，最多约 255ms）；真正没有权限时只多花约 63ms。
- `moveFile(from, to)`：先尝试 `rename`；跨挂载时对**文件**退化为复制加删除（不保证原子性）；目录跨挂载移动返回 `CROSS_DEVICE`。
- `copyFile(from, to)`：读取可见项，按 §5 写入目标路径。`from` 与 `to` 相同时：可见项可写则什么都不做（不会把文件截断为空）；可见项在只读层则复制到写入层，相当于显式 copy-up；是目录返回 `IS_DIR`。
- `writeFileAtomic(path, data, size)`：先写同目录下唯一命名的临时文件 `path.vfs-N.tmp`（N 为进程内递增序号，并发写同一路径互不干扰），再改名为 `path`；写入失败时删除临时文件；临时文件与目标落在不同层时退化为直接写入。

## 10. 卸载与替换

- `unmount(id)`：**惰性卸载**，挂载立即从命名空间消失，已打开的流继续可用，后端在最后一个流关闭后释放。
- `unmount(id, UnmountFlags::FailIfBusy)`：仍有通过 VFS 打开的流时返回 `BUSY`，不卸载。
- `remount(id, backend)`：原子地替换某个挂载的后端，挂载点、优先级和标志不变；旧流继续读旧后端。
- `generation()`：挂载表变化或任何写操作后递增，上层可据此让缓存失效。

## 11. 错误码

| `vfs::Error` | errno | 典型场景 |
|---|---|---|
| `OK` | 0 | 成功 |
| `NOT_FOUND` | `ENOENT` | 路径不存在；写入时父目录不存在；本机根目录不存在 |
| `EXISTS` | `EEXIST` | `createDir` 目标已存在；打包时路径重复 |
| `NOT_DIR` | `ENOTDIR` | 路径中间是文件；对文件 `list`/`removeDir`；单文件挂载的子路径 |
| `IS_DIR` | `EISDIR` | 对目录读文件、`removeFile`、以文件方式打开 |
| `NOT_EMPTY` | `ENOTEMPTY` | 非递归删除非空目录 |
| `READ_ONLY` | `EROFS` | 只读层；没有可写层；写入后会被遮挡；`removeDir` / `rename` 的目录涉及其他层 |
| `CROSS_DEVICE` | `EXDEV` | 跨挂载 `rename`；目录跨挂载 `moveFile` |
| `BUSY` | `EBUSY` | 删除或改名挂载点；`FailIfBusy` 卸载时仍有打开的流 |
| `INVALID_PATH` | `EINVAL` | 越过根目录、控制字符、本机不可移植的名字 |
| `PERMISSION_DENIED` | `EACCES` | 操作系统拒绝；禁止跟随的符号链接逃出根目录 |
| `IO_ERROR` | `EIO` | 读写失败 |
| `UNSUPPORTED` | `ENOTSUP` | 后端不支持该操作；参数不受支持（例如非法的对齐值） |
| `CORRUPTED` | `EIO` | 归档损坏 |
| `VERSION_MISMATCH` | `EIO` | 归档版本不受支持 |
| `KEY_REQUIRED` | `EACCES` | 归档索引已加密但没有提供密钥 |
| `KEY_INVALID` | `EACCES` | 密钥长度不对或密钥错误 |

`errorName(e)` 返回枚举名，`errorMessage(e)` 返回英文说明，`toErrno(e)` 返回上表中的 errno。
