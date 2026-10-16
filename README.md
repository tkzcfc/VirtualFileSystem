# VirtualFileSystem

面向游戏资源管理的分层虚拟文件系统，C++20 实现。

把本机目录、内存、`.vfs` 归档、单个文件或自定义后端挂载到任意虚拟路径，多层按优先级合并成一个统一视图；读写、建删目录都遵循确定性的规则；全部接口线程安全。文件流接口与 [axmol](https://github.com/axmolengine/axmol) 的 `IFileStream` 完全一致，可以直接接入引擎。

配套命令行工具 `vfstool` 负责打包、解包和检查 `.vfs` 归档。

## 特性

- **多种挂载**：本机目录、单个本机文件、内存目录树、`.vfs` 归档（来自文件或内存），以及实现 `IBackend` 的自定义后端。
- **分层合并视图**：按优先级叠加，同名文件高层优先、目录内容合并；支持遮蔽（`Opaque`）、只读（`ReadOnly`）、指定默认写入层（`WriteTarget`）。
- **确定性写入规则**：写入、copy-up、删除、`rmdir`、`rename` 的落点和错误都有明确定义，见 [挂载规则](docs/mount-rules.md)。
- **线程安全**：挂载表采用 copy-on-write 快照，查询不加锁；挂载、卸载可以与读取并发进行；卸载是惰性的，已打开的流可以读完。
- **`.vfs` 归档**：按文件可选 XOR 加密（支持随机 seek）和 LZ4 压缩，索引用 ChaCha20 加密，内容去重，数据对齐，header 与索引带损坏校验，**同一输入每次打包逐字节一致**。每个流独立持有句柄、按偏移读取，多线程读互不干扰。
- **跨平台**：Windows（UTF-8 路径自动转 UTF-16，支持长路径）、Linux、macOS；只依赖标准库和 `3rd/` 中的 doctest、xxHash、LZ4、CLI11。

## 快速上手

```cpp
#include "vfs/VirtualFileSystem.h"
#include <cstdio>

USING_NS_VFS;

int main()
{
	VirtualFileSystem fs;

	// 只读资源：归档在下，开发目录在上（同名文件以开发目录为准）
	fs.mountArchive("/res", "game.vfs");
	fs.mountNative("/res", "assets_dev", MountOptions{10});

	// 可写数据：单独挂在 /user
	NativeOptions userDir;
	userDir.createRoot = true;
	fs.mountNative("/user", "save", MountOptions{0, MountFlags::WriteTarget}, userDir);

	std::string config;
	if (fs.readFile("/res/config.json", config) != Error::OK)
		return 1;

	auto stream = fs.openFileStream("/res/scripts/main.lua", IFileStream::Mode::READ);
	fs.writeFileAtomic("/user/save.dat", "progress=3", 10);

	for (const auto& entry : fs.list("/res"))
		std::printf("%s%s\n", entry.name.c_str(), entry.type == FileType::DIR ? "/" : "");
	return 0;
}
```

打包资源：

```bash
vfstool pack assets -o game.vfs --compress "*.lua" --compress "*.json" --encrypt "**/*.png"
vfstool list game.vfs -l
vfstool verify game.vfs
```

更多示例见 [`examples/basic`](examples/basic/main.cpp) 与 [API 文档](docs/api.md)。

## 构建与测试

需要 CMake ≥ 3.16 和支持 C++20 的编译器（GCC、Clang、Apple Clang、MSVC 均可）。

```bash
cmake -S . -B build
cmake --build build -j
cd build && ctest --output-on-failure
```

| CMake 选项 | 默认值 | 说明 |
|---|---|---|
| `VFS_BUILD_TOOLS` | 作为顶层项目时 ON | 构建 `vfstool` |
| `VFS_BUILD_TESTS` | 作为顶层项目时 ON | 构建测试（会同时构建 `vfstool`） |
| `VFS_BUILD_EXAMPLES` | OFF | 构建 `examples/` |
| `VFS_WITH_AXMOL` | OFF | 使用 axmol 的 `ax::IFileStream`，见 [接入 axmol](docs/integration-axmol.md) |
| `VFS_WARNINGS_AS_ERRORS` | OFF | 把编译警告当作错误 |

作为子项目使用：

```cmake
add_subdirectory(VirtualFileSystem)
target_link_libraries(my_game PRIVATE vfs)   # 或 vfs::vfs
```

测试使用 doctest，按 suite 注册为 ctest 用例：`vfs.path`、`vfs.union`、`vfs.native`、`vfs.memory`、`vfs.stream`、`vfs.archive`、`vfs.crypto`、`vfs.tool`、`vfs.concurrency`，以及命令行端到端测试 `vfstool.e2e`。

## 目录结构

```
3rd/            第三方库（固定版本）：doctest、xxHash、LZ4、CLI11
cmake/          构建选项与编译器设置
docs/           文档
examples/       示例（默认不构建）
src/vfs/        运行时库
  native/       本机目录、单文件后端与 NativeFileStream
  memory/       内存后端
  archive/      .vfs 读写
  crypto/       ChaCha20、XXH3、XOR
tests/          单元测试与命令行端到端测试
tools/vfstool/  命令行工具
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 分层结构、挂载表、后端、流、线程模型 |
| [docs/mount-rules.md](docs/mount-rules.md) | 优先级、合并视图、写入目标、删除 / rmdir / rename 语义与错误码 |
| [docs/api.md](docs/api.md) | 主要公开 API、`IFileStream` 语义、自定义后端 |
| [docs/format.md](docs/format.md) | `.vfs` 二进制格式规范 |
| [docs/vfstool.md](docs/vfstool.md) | 命令行工具参考 |
| [docs/integration-axmol.md](docs/integration-axmol.md) | 接入 axmol 引擎 |
| [docs/style.md](docs/style.md) | 代码风格 |
