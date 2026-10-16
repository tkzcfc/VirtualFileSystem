# 代码风格

完整规则见 `design/vfs-format-draft.md` §1，这里是速查。格式由根目录的 `.clang-format` 与 `.editorconfig` 约束。

- **大括号**：Allman（函数、类、if、for 的 `{` 都单独占一行）。
- **缩进**：Tab，宽度 4；列宽 120。CMake 文件沿用原仓库的 4 空格。
- **命名**：类型 `PascalCase`；方法与局部变量 `camelCase`；成员 `m_xxx`，静态成员 `s_xxx`；纯数据结构体字段不加前缀；`enum class` 值全大写（`Error::NOT_FOUND`）；位标志用无作用域 enum + PascalCase 值（`MountFlags::ReadOnly`），使用时带类型名限定；宏全大写并加 `VFS_` 前缀。
- **命名空间**：`NS_VFS_BEGIN` / `NS_VFS_END` / `USING_NS_VFS`（定义于 `Common.h`）。
- **头文件**：一律 `#pragma once`（.cpp 中禁止）；自己的头文件放第一行，库内用相对路径，系统头用 `<>`。
- **虚函数**：`virtual` 与 `override` 同时写。
- **构造函数初始化列表**：前置逗号，每项一行。
- **单行 if**：只有 `return/break/continue` 这类单语句可以省略大括号。
- **注释**：中文；每个头文件开头说明职责；公开 API 用 `/** */` 写清参数、返回值、错误码、线程安全与所有权；二进制格式字段读写处标注偏移（`// [48] hash`）；引用设计决策处标注 `决策 #N`。
- **编码**：UTF-8 无 BOM，MSVC 加 `/utf-8`。
- **错误**：返回 `vfs::Error`，同时设置 `errno`；`IFileStream` 方法按 POSIX 约定返回 -1 并设置 `errno`。
