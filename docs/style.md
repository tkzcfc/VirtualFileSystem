# 代码风格

格式由根目录的 `.clang-format` 和 `.editorconfig` 约束，提交前用 clang-format 格式化。`3rd/` 下的第三方代码保持原样，不受这些规则约束。

## 格式

- **大括号**：Allman，函数、类、`if`、`for`、`switch` 的 `{` 都单独占一行。
- **缩进**：C++ 用 Tab（宽度 4），列宽 120；CMake 文件用 4 个空格；Markdown 用空格。
- **访问修饰符**：`public:` / `private:` 顶格写。
- **构造函数初始化列表**：前置逗号，每项一行。
- **单语句 `if`**：分支只有一条简单语句（`return`、`break`、`continue`、赋值等）时可以省略大括号，多条语句必须加。
- **指针与引用**：贴近类型，`const std::string& name`、`IBackend* backend`。

## 命名

| 对象 | 规则 | 示例 |
|---|---|---|
| 类型、类、结构体 | `PascalCase` | `VirtualFileSystem`、`MountOptions` |
| 方法、函数、局部变量、参数 | `camelCase` | `openFileStream`、`mountPoint` |
| 成员变量 | `m_` 前缀 | `m_table` |
| 静态成员、文件内静态变量 | `s_` 前缀 | `s_logFunc` |
| 纯数据结构体字段 | 不加前缀 | `Stat::size` |
| `enum class` 值 | 全大写 | `Error::NOT_FOUND`、`FileType::DIR` |
| 位标志 | 无作用域 enum，值用 `PascalCase`，使用时带类型名 | `MountFlags::ReadOnly` |
| 宏 | 全大写，`VFS_` 前缀 | `VFS_MAX_IO_CHUNK` |
| 文件名 | 与主要类同名 | `ArchiveBackend.h` |

## 文件与命名空间

- 头文件一律 `#pragma once`，`.cpp` 中不使用。
- 每个头文件第一行用一句注释说明本文件的职责（`// 本文件：...`）。
- `.cpp` 先包含自己的头文件；库内头文件用相对路径加 `""`，标准库和系统头用 `<>`；工具与测试通过 `"vfs/..."` 引用库头文件。
- 所有库代码放在命名空间 `vfs` 中，用 `NS_VFS_BEGIN` / `NS_VFS_END` 包围；`USING_NS_VFS` 只在 `.cpp`、工具、测试和示例中使用。
- 导出的类和函数加 `VFS_DLL`。
- 平台相关代码集中在 `src/vfs/native/`（工具的命令行入口除外），用 `#ifdef _WIN32` 区分；其他目录不直接调用平台 API。

## C++ 用法

- C++20，只依赖标准库和 `3rd/` 中的库。
- 虚函数同时写 `virtual` 和 `override`。
- 所有权用 `std::unique_ptr` / `std::shared_ptr` 表达；只读参数用 `std::string_view` 或 `const&`。
- 不使用异常报告业务错误：返回 `vfs::Error` 并设置 `errno`；返回指针或流的接口提供可选的 `Error* err` 输出参数。
- `IFileStream` 的方法按 POSIX 约定失败时返回 `-1` 并设置 `errno`。
- 单次 `read`/`write` 不超过 `VFS_MAX_IO_CHUNK`，大块数据分块循环处理。
- 二进制格式统一小端，通过 `ByteIO` 读写，不直接 `memcpy` 结构体。
- 代码在 GCC、Clang（`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor`）和 MSVC（`/W4`）上编译无警告，可用 `VFS_WARNINGS_AS_ERRORS=ON` 检查。

## 注释

- 注释用中文，说明“为什么”，不复述代码。
- 公开 API 用 `/** */` 写清参数、返回值、错误码、线程安全与所有权。
- 二进制格式的字段读写处标注偏移，例如 `// [48] u64 indexOffset`。

## 编码

- 所有源文件和文档使用 UTF-8 无 BOM、LF 换行、文件末尾保留换行；MSVC 编译时加 `/utf-8`。
- 路径在接口上一律是 UTF-8 字符串；Windows 上进入系统调用前再转 UTF-16。

## 测试

- 使用 doctest，每个模块一个测试目录（`tests/union/` 等）；`TEST_SUITE("union")` 自动注册为 ctest 用例 `vfs.union`。新 suite 需要加到 `tests/CMakeLists.txt` 的 suite 列表中。耗时长的用例加 `* doctest::skip(!test::slowEnabled())`，只在 `VFS_TEST_SLOW=1` 时运行；压力 / 随机测试的轮数乘以 `test::scale()`（`VFS_TEST_SCALE`），保证默认 `ctest` 在一分钟内完成。
- 测试名用中文描述行为，例如 `"removeDir：只读层贡献了子孙时返回 READ_ONLY，什么都不删"`。
- 测试只写入临时目录，结束时清理。
