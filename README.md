# VirtualFileSystem

面向游戏资源管理的分层虚拟文件系统（C++20）。可以把本机目录、内存、`.vfs` 归档、单个文件或自定义后端挂载到任意虚拟路径，多层按优先级合并成一个视图；读写、建删目录遵循确定性的规则；线程安全；文件流接口与 [axmol](https://github.com/axmolengine/axmol) 的 `IFileStream` 完全一致，可直接接入引擎。

配套命令行工具 `vfstool` 用于打包、解包、检查 `.vfs` 归档。

## 目录结构

```
3rd/            第三方库（固定版本）：doctest、xxHash、LZ4、CLI11
cmake/          构建选项与编译器设置
docs/           格式规范、CLI 参考、代码风格、与设计的出入、设计文档存档
examples/       示例（默认不构建）
src/vfs/        运行时库
  native/       本机目录、单文件后端，NativeFileStream
  memory/       内存后端
  archive/      .vfs 读写
  crypto/       ChaCha20、XXH3 封装、XOR
tests/          doctest 单元测试与命令行端到端测试
tools/vfstool/  命令行工具
```

## 架构

| 类 | 作用 |
|---|---|
| `VirtualFileSystem` | 入口：挂载管理、路径解析、合并视图上的 stat/list/open/读写/建删目录/rename |
| `MountTable` | 挂载表快照（copy-on-write，查询无锁拿快照，修改时复制后发布） |
| `IBackend` | 后端接口：`stat/open/list/createDir/removeDir/removeFile/rename`，后端只接收已规范化的相对路径 |
| `NativeBackend` / `SingleFileBackend` | 本机目录 / 单个本机文件 |
| `MemoryBackend` | 内存文件树（unlink 语义：删除打开中的文件不影响已打开的流） |
| `ArchiveBackend` | 只读 `.vfs` 归档；索引 `ArchiveIndex` 不可变、在所有流之间共享 |
| `IFileStream` | 与 axmol 逐字一致的流接口；实现有 `NativeFileStream`、`MemoryFileStream`、`ArchiveFileStream` |
| `ArchiveWriter` / `ArchiveReader` | 归档写入（可复现、去重、对齐、XOR、LZ4、ChaCha20 索引加密）/ 解析与校验 |
| `Path` | 路径规范化与可移植性检查 |

所有接口返回 `vfs::Error`（并设置 `errno`）；流方法按 POSIX 约定返回 -1。

## 挂载与分层规则

```cpp
#include "vfs/VirtualFileSystem.h"
USING_NS_VFS;

VirtualFileSystem fs;
fs.mountArchive("/res", "game.vfs");                                   // 只读资源
fs.mountNative("/res", "patch", MountOptions{10});                     // 补丁目录，优先级更高
fs.mountNative("/user", savePath, MountOptions{0, MountFlags::WriteTarget}, NativeOptions{false, true, false, true});
fs.mountMemory("/tmp");

std::string text;
fs.readFile("/res/config.json", text);
auto stream = fs.openFileStream("/res/scripts/main.lua", IFileStream::Mode::READ);
fs.writeFile("/user/save.dat", data, size);
fs.addAlias("res://", "/res");                                         // "res://a.png" → "/res/a.png"
```

- **路径**：`\` 视为 `/`，合并重复 `/`，去掉 `.`，按字面消解 `..`，越过根返回 `INVALID_PATH`；拒绝控制字符；区分大小写；UTF-8。本机后端另外拒绝 `:` 与 Windows 保留名（`CON`、`NUL`、`COM1`…）。
- **层栈**：路径 P 的层 = 所有挂载点是 P 前缀的挂载，**priority 大的在上，相同时后挂载的在上**，与挂载深度无关。`Opaque` 挂载屏蔽其下更低的层。挂载点的祖先显示为只读虚拟目录。
- **查找**：最上层命中即可见项。高层是文件、低层是目录时只显示文件，查找其子路径返回 `NOT_DIR`。
- **列目录**：合并所有层、按名字去重（高层决定类型）、按字节序排序；回调返回 `false` 停止。
- **写入目标**：可见项所在层可写 → 写在那里；否则写到覆盖 P 的最高优先级可写挂载（`WriteTarget` 标记优先）；没有 → `READ_ONLY`；可写层低于持有可见项的只读层（写了也读不到）→ `READ_ONLY`。
- **copy-up**：父目录只在只读层时，自动在写入目标中 `mkdir -p`；父目录在合并视图中不存在 → `NOT_FOUND`。以 `APPEND`/`OVERLAPPED` 打开只读层文件时先完整拷贝到写入目标（临时文件 + rename）；`WRITE` 直接新建。
- **删除**：只删可见项，可见项在只读层 → `READ_ONLY`；删掉覆盖文件后下层原版重新露出（没有 whiteout）。
- **rmdir**：任何其他层（只读层或第二个可写层）贡献了该目录或其子孙 → `READ_ONLY`，什么都不删；非递归要求为空；挂载点及其祖先 → `BUSY`。
- **rename**：源与目标在同一个可写后端实例上才成功（原子）；源在只读层 → `READ_ONLY`；跨后端 → `CROSS_DEVICE`，文件可用 `moveFile`（复制 + 删除）。
- **卸载**：惰性卸载，已打开的流继续可用直到关闭；`UnmountFlags::FailIfBusy` 时有打开的流返回 `BUSY`。
- **线程安全**：所有方法可并发调用；每个流同一时间只能被一个线程使用；VFS 不做文件锁。
- **推荐用法**：只读资源挂 `/res`（归档 + 开发目录 union），可写数据挂 `/user`（单一本机目录），避免读写混合的层叠。

与旧版本相比的行为变化：同优先级时**后挂载的优先**（旧版先挂载优先）；`list` 回调返回 `false` 停止（旧版返回 `true` 停止）；`WRITE` 模式一定截断；旧 pack 格式不再支持，请用 `vfstool` 重新打包。

## `.vfs` 格式摘要

完整规范见 [`docs/format.md`](docs/format.md)。

```
Header 128 B | Data（按 alignment 对齐） | Index（Entry[n] × 96 B + 路径池，可 ChaCha20 加密） | Footer 16 B
```

- 单一版本号，不一致直接拒绝；header 与索引用 XXH3-64 做快速损坏检查。
- 每文件可选 XOR 加密（16~32 字节 key 存在索引里，按文件内偏移取 key，支持随机 seek）。
- 每文件可选 LZ4 压缩（先压缩再加密，打开时整体解压到内存）。
- 索引可用 ChaCha20（自带实现，RFC 8439 向量验证）加密；密钥默认派生并存在文件中（只是混淆），也可以用外部密钥（不存入文件，读取时必须提供）。
- 内容相同的文件只存一份；默认 16 字节对齐；默认不保留空目录。
- **可复现**：同一输入每次输出逐字节一致（路径排序、mtime 默认 0、key/nonce 确定性派生）。
- 每个打开的流各自持有句柄、按偏移读取，多线程读互不干扰。

## vfstool

完整参考见 [`docs/vfstool.md`](docs/vfstool.md)。

```bash
vfstool pack assets -o game.vfs --compress "*.lua" --encrypt "**/*.png" --exclude "*.tmp"
vfstool info game.vfs                 # 版本、大小、加密/压缩/去重统计
vfstool list game.vfs -l "*.lua"      # 大小、存储大小、标志、哈希
vfstool cat game.vfs scripts/main.lua # 输出到 stdout
vfstool extract game.vfs scripts -o out
vfstool unpack game.vfs -o out        # 默认校验内容哈希
vfstool verify game.vfs               # header、索引、每个文件
vfstool keygen -o game.key            # 外部密钥：pack/读取时加 --key-file game.key
```

退出码：0 成功，1 用法错误，2 IO 错误，3 格式错误，4 完整性失败，5 密钥问题。

## 构建与测试

需要 CMake ≥ 3.16 与支持 C++20 的编译器（GCC 11+ / Clang 14+ / MSVC 2022）。

```bash
cmake -S . -B build                    # 作为顶层项目时默认构建工具和测试
cmake --build build -j
cd build && ctest --output-on-failure
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `VFS_BUILD_TOOLS` | 顶层 ON | 构建 vfstool |
| `VFS_BUILD_TESTS` | 顶层 ON | 构建测试（会同时打开 tools） |
| `VFS_BUILD_EXAMPLES` | OFF | 构建 `examples/` |
| `VFS_WITH_AXMOL` | OFF | 使用 axmol 的 `ax::IFileStream` 代替自带的同名接口 |
| `VFS_WARNINGS_AS_ERRORS` | OFF | 警告视为错误 |

作为子项目使用：`add_subdirectory(VirtualFileSystem)` 后链接 `vfs` 目标。

测试（doctest）按 suite 注册为 ctest 用例：`path`、`union`（分层规则逐条验证）、`native`、`memory`、`stream`（IFileStream 一致性，所有后端跑同一套用例）、`archive`（往返、可复现、对齐、去重、密钥、逐字节损坏检测、版本不匹配、随机变异）、`crypto`（RFC 8439 向量、XOR、LZ4）、`tool`、`concurrency`（多线程读、挂载/卸载与打开中的流并发），以及 `vfstool.e2e`（pack → info → list → cat → unpack 往返、两次打包哈希一致、错误码）。

## 文档

- [`docs/format.md`](docs/format.md)：`.vfs` 格式规范
- [`docs/vfstool.md`](docs/vfstool.md)：命令行参考
- [`docs/style.md`](docs/style.md)：代码风格
- [`docs/deviations.md`](docs/deviations.md)：实现与设计文档的出入
- [`docs/design/`](docs/design/)：设计争议与决策存档
