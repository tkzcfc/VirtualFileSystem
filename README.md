# VirtualFileSystem

[English](README.en.md) | 简体中文

面向游戏资源管理的分层虚拟文件系统，C++20 实现。

把本机目录、内存、`.vfs` 归档、单个文件或自定义后端挂载到任意虚拟路径，多层按优先级合并成一个统一视图；读写、建删目录都遵循确定性的规则；全部接口线程安全。文件流接口与 [axmol](https://github.com/axmolengine/axmol) 的 `IFileStream` 完全一致，可以直接接入引擎。

配套命令行工具 `vfstool` 负责打包、解包和检查 `.vfs` 归档。

**性能**：各后端与分层组合相对直接调用 C 文件 API 的开销（Linux、macOS、Windows，1 / 8 线程）以及选型建议见 [性能基准](docs/benchmark.md)。

## 特性

- **多种挂载**：本机目录、单个本机文件、内存目录树、`.vfs` 归档（来自文件或内存），以及实现 `IBackend` 的自定义后端。
- **分层合并视图**：按优先级叠加，同名文件高层优先、目录内容合并；支持遮蔽（`Opaque`）、只读（`ReadOnly`）、指定默认写入层（`WriteTarget`）。
- **确定性写入规则**：写入、copy-up、删除、`rmdir`、`rename` 的落点和错误都有明确定义，见 [挂载规则](docs/mount-rules.md)。
- **线程安全**：挂载表采用 copy-on-write 快照，查询不加锁；挂载、卸载可以与读取并发进行；卸载是惰性的，已打开的流可以读完。
- **`.vfs` 归档**：按文件可选 XOR 加密（支持随机 seek）和 LZ4 压缩，索引用 ChaCha20 加密，内容去重，数据对齐，header 与索引带损坏校验，**同一输入每次打包逐字节一致**。每个流独立持有句柄、按偏移读取，多线程读互不干扰。
- **跨平台**：Windows（UTF-8 路径自动转 UTF-16，支持长路径）、Linux、macOS、iOS、Android（含 APK 资源后端 `AndroidAssetBackend`）、WebAssembly（Emscripten）；只依赖标准库和 `3rd/` 中的 doctest、xxHash、LZ4、CLI11。

| 平台 | 状态 |
|---|---|
| Windows / Linux / macOS | 全部测试 |
| iOS | 真机与模拟器编译；模拟器运行全部测试 |
| Android（API 24+） | arm64-v8a / x86_64 / armeabi-v7a 交叉编译；x86_64 模拟器运行全部测试；APK 资源后端 |
| WebAssembly | Emscripten 构建，node 上运行全部测试（pthread 与无线程两种构建） |

各平台的构建方法与注意事项见 [平台支持](docs/platforms.md)。

## 性能

把资源打包成 `.vfs` 归档后，打开、`stat`、查找都由内存索引直接应答，通常比在同一台机器上用普通 C `stdio`（`fopen`/`fread`/`stat`）访问散文件快得多：

| 操作 | Windows | Linux | macOS | iOS | Android | WebAssembly |
|---|---:|---:|---:|---:|---:|---:|
| 读小文件（4 KiB） | **7.3x** / 4.4x | **2.5x** / 2.2x | **18.1x** / 10.3x | **164x** / 21.8x | **1.5x** / 1.2x | **3.9x** / 2.3x |
| 64 MiB 大文件随机读 | **0.9x** / 1.0x | **1.5x** / 1.0x | **2.4x** / 3.0x | **2.3x** / 3.1x | **2.1x** / 1.2x | **1.9x** / 2.1x |
| `stat` 文件 | **66.6x** / 47.4x | **4.4x** / 4.3x | **15.7x** / 25.8x | **285x** / 150x | **3.7x** / 1.2x | **58.1x** / 2.0x |
| 打开文件 | **26.3x** / 20.8x | **4.3x** / 3.1x | **45.6x** / 29.4x | **133x** / 30.2x | **5.6x** / 0.8x | **44.7x** / 1.3x |
| 查找不存在的文件 | **19.4x** / 17.9x | **2.6x** / 1.2x | **3.7x** / 9.4x | **68.6x** / 45.1x | **0.5x** / 0.4x | **27.3x** / 1.5x |

表中为普通归档后端（`vfs:archive`）相对 `base:stdio` 的加速比，**1 线程** / 8 线程，由 [`vfs_bench`](docs/benchmark.md) 以默认参数测得（热缓存，Release）。
小于 1x 表示比 stdio 慢。测试机器：Windows = Core i5-12400，Linux = 8 vCPU Xeon 虚拟机，macOS = Apple M4，iOS = iPhone 14 Pro，Android = Redmi K80（骁龙 8 Gen 3），WebAssembly = Emscripten -O3 pthread 构建，在同一台 Linux 虚拟机上用 node 24 运行。
Android 数据来自 `adb shell` 启动的命令行进程，没有前台应用的 CPU 提频。WebAssembly 下 stdio 和 native 调用都要经过 node 的文件系统 API，因此 1 线程倍数偏高、8 线程偏低。
所有后端（native、memory、LZ4 归档、多层挂载）、所有场景以及绝对数值见 [docs/benchmark.md](docs/benchmark.md)（英文）。

![Windows：相对 stdio 的速度](docs/images/bench/windows.svg)

### 性能提示：应避免的用法

以上六个平台的实测结果（相对 `base:stdio` 的倍数，1 线程 / 8 线程；详见 [docs/benchmark.md](docs/benchmark.md)）：

- **需要随机读或顺序流式读的大文件不要用 LZ4 压缩。** LZ4 条目按块（默认 64 KiB）存储，每次读取会解压所涉及的整个块。打开的开销与未压缩条目相同，但在 64 MiB 的 LZ4 文件里做 4 KiB 随机读，1 线程只有 stdio 的 0.06-0.21x（WebAssembly 为 0.82x），顺序读为 0.23-0.54x，256 KiB 文件为 0.44-1.07x。*建议：* 只压缩脚本、配置等小文本（`--compress "*.lua"`；LZ4 小文件 1 线程读取为 stdio 的 0.87-46x），大的二进制资源不压缩；需要随机读的压缩文件可用更小的 `--block-size`。
- **除非需要覆盖，不要在归档上叠加 native 目录。** 每次查找都先查 native 层，这是一次真实的系统调用：`archive+native` 的未命中查询为 stdio 的 0.27-1.34x（iOS 为 1.2-1.8x），而单独的归档直接由索引应答。*建议：* 开发用的覆盖目录只在开发构建中挂载，发布版不要挂载空目录。
- **层数要少。** 同一路径叠 9 层（`nested8`）时，打开为 stdio 的 0.26-1.25x，未命中为 0.18-0.96x（iOS 1 线程为 1.5x）。*建议：* 使用不同的挂载点，或把多层合并打包成一个归档。
- **native 目录列举比直接 `readdir` 慢。** VFS 要合并各层、去重、排序并转换文件名：`vfs:native` 的 `list` 为 0.56-0.92x（macOS/iOS 8 线程约 0.94-1.04x）。*建议：* 从归档列举（1.06-21x），或缓存需要重复使用的列表。
- **在 Windows 上，单个未压缩大文件内的随机读不要指望提速：** 归档为 stdio 的 0.88-1.0x（其他平台为 1.0-3.1x）。主要收益在打开、`stat`、查找和小文件读取。

## 快速上手

```cpp
#include "vfs/VirtualFileSystem.h"
#include <cstdio>

USING_NS_VFS;

// 假设 game.vfs 由 `vfstool pack assets -o game.vfs` 打出，含 config.json 和 scripts/main.lua；
// assets_dev/ 目录里只有 textures/hero.png
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

	// 别名：res://scripts/main.lua 等价于 /res/scripts/main.lua
	fs.addAlias("res://", "/res");

	std::string config;
	if (fs.readFile("res://config.json", config) != Error::OK)
		return 1;

	auto stream = fs.openFileStream("res://scripts/main.lua", IFileStream::Mode::READ);
	fs.writeFileAtomic("/user/save.dat", "progress=3", 10);

	for (const auto& entry : fs.list("res://"))
		std::printf("%s%s\n", entry.name.c_str(), entry.type == FileType::DIR ? "/" : "");
	// 输出（两层合并，按名字排序）：
	// config.json
	// scripts/
	// textures/
	return 0;
}
```

打包资源：

```bash
vfstool pack assets -o game.vfs --compress "*.lua" --compress "*.json" --encrypt "**/*.png"
vfstool list game.vfs -l
vfstool verify game.vfs
```

更多示例见 [`examples/`](examples/)（`-DVFS_BUILD_EXAMPLES=ON` 构建）：

| 示例 | 内容 |
|---|---|
| [`basic`](examples/basic/main.cpp) | 最小示例：内存中打包归档，与可写内存层叠加 |
| [`layers`](examples/layers/main.cpp) | 分层挂载、遮蔽、只读层与别名 |
| [`archive`](examples/archive/main.cpp) | 打包与读取归档：自定义索引密钥、XOR 加密、LZ4 压缩、去重 |
| [`custom_backend`](examples/custom_backend/main.cpp) | 实现 `IBackend` 挂载程序生成的内容 |
| [`multithread`](examples/multithread/main.cpp) | 8 线程并发读取，同时热更新（`remount`）归档 |
| [`stream`](examples/stream/main.cpp) | 流式 API：分块写入、seek、追加、分块读取 |

API 细节见 [API 文档](docs/api.md)。

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
| `VFS_BUILD_BENCH` | OFF | 构建性能基准 `vfs_bench`，见 [性能基准](docs/benchmark.md) |
| `VFS_WITH_AXMOL` | OFF | 使用 axmol 的 `ax::IFileStream`，见 [接入 axmol](docs/integration-axmol.md) |
| `VFS_WARNINGS_AS_ERRORS` | OFF | 把编译警告当作错误 |
| `VFS_WASM_THREADS` | ON | 仅 Emscripten：所有目标加 `-pthread`，见 [平台支持](docs/platforms.md#webassemblyemscripten) |

作为子项目使用：

```cmake
add_subdirectory(VirtualFileSystem)
target_link_libraries(my_game PRIVATE vfs)   # 或 vfs::vfs
```

测试使用 doctest，按 suite 注册为 ctest 用例：`vfs.path`、`vfs.union`、`vfs.native`、`vfs.memory`、`vfs.stream`、`vfs.archive`、`vfs.crypto`、`vfs.tool`、`vfs.concurrency`，以及命令行端到端测试 `vfstool.e2e`。另有四组覆盖面更广的测试：

| suite | 内容 |
|---|---|
| `vfs.matrix` | 同一组文件 / 目录 / 改名 / 流操作在 9 种后端与分层组合上逐一运行（本机、内存、嵌套挂载、归档在本机之下、内存叠在归档之上、优先级相同、三层叠加、只读本机目录等） |
| `vfs.stress` | 多线程压力：同目录与不同目录的并发增删改读、改名、读写竞争、并发追加 copy-up、列目录与修改并发、挂载 / 卸载 / remount 抖动、同一归档上的大量句柄 |
| `vfs.model` | 随机操作序列与参考模型逐步比对（错误码、合并视图、写入层内容） |
| `vfs.extreme` | 极端情况：空文件、海量条目、深路径与超长名字、Unicode 与大小写、外部修改、只读目录、归档逐区域损坏与伪造索引、故障注入、句柄耗尽、超过 4 GB 的稀疏文件 |

默认的 `ctest` 在一分钟内跑完。耗时长的用例默认跳过，用环境变量打开：

```bash
VFS_TEST_SLOW=1 ./build/tests/vfs_tests      # 包含慢速用例（10 万条目归档、>4 GB 稀疏文件、长时间模型比对）
VFS_TEST_SCALE=5 ./build/tests/vfs_tests     # 压力与随机测试的轮数放大 5 倍
./build/tests/vfs_tests --test-suite=stress  # 只跑某个 suite（doctest 的其他参数同样可用）
```

与平台能力有关的用例在运行时探测，而不是按操作系统写死：临时目录所在文件系统是否区分大小写（Windows/macOS 默认不区分，相关用例改为检查文档规定的不区分行为）、能否替换正被打开的文件（Windows 需要 NTFS 上的 POSIX rename 语义；不支持时用例放宽并输出一条 `MESSAGE`）。并发用例在失败时打印每种操作的错误码分布和前几条失败详情。在 Linux 上可以用 MinGW 交叉编译后通过 Wine 运行全部测试（Wine 不支持 POSIX rename 语义，会看到上述 `MESSAGE`）。

### 性能基准

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DVFS_BUILD_BENCH=ON -DVFS_BUILD_TESTS=OFF
cmake --build build-bench --target vfs_bench -j
./build-bench/bench/vfs_bench --csv bench.csv --md bench.md   # 全部场景；--help 查看 --scale / --threads / --filter 等选项
```

`vfs_bench` 对比 `std::fstream`、`FILE*`、POSIX `pread` / Win32 `ReadFile` 与各种后端和分层组合的读、写、元数据性能，输出吞吐、p50/p99 延迟和相对 stdio 的倍数。本机结果与分析见 [docs/benchmark.md](docs/benchmark.md)。

## 目录结构

```
3rd/            第三方库（固定版本）：doctest、xxHash、LZ4、CLI11
bench/          性能基准 vfs_bench（默认不构建）
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
| [docs/benchmark.md](docs/benchmark.md) | 性能基准：构建运行方法、本机结果与开销分析 |
| [docs/platforms.md](docs/platforms.md) | 平台支持：Android（APK 资源后端）、iOS、WebAssembly 的构建与注意事项 |
| [docs/integration-axmol.md](docs/integration-axmol.md) | 接入 axmol 引擎 |
| [docs/style.md](docs/style.md) | 代码风格 |
