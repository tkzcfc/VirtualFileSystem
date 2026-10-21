# Code style

Formatting is constrained by `.clang-format` and `.editorconfig` at the repository root. Format with clang-format before committing. Third-party code under `3rd/` stays as it is and is not subject to these rules.

## Formatting

- **Braces**: Allman. The `{` of a function, class, `if`, `for`, or `switch` is on its own line.
- **Indentation**: C++ uses tabs (width 4), column width 120; CMake files use 4 spaces; Markdown uses spaces.
- **Access specifiers**: `public:` / `private:` at column 0.
- **Constructor initializer lists**: leading comma, one item per line.
- **Single-statement `if`**: braces may be omitted when the branch is one simple statement (`return`, `break`, `continue`, assignment, and the like). Multiple statements require braces.
- **Pointers and references**: next to the type, `const std::string& name`, `IBackend* backend`.

## Naming

| Object | Rule | Example |
|---|---|---|
| Types, classes, structs | `PascalCase` | `VirtualFileSystem`, `MountOptions` |
| Methods, functions, local variables, parameters | `camelCase` | `openFileStream`, `mountPoint` |
| Member variables | `m_` prefix | `m_table` |
| Static members and file-scope static variables | `s_` prefix | `s_logFunc` |
| Fields of plain data structs | no prefix | `Stat::size` |
| `enum class` values | all capitals | `Error::NOT_FOUND`, `FileType::DIR` |
| Bit flags | unscoped enum, values in `PascalCase`, used with the type name | `MountFlags::ReadOnly` |
| Macros | all capitals, `VFS_` prefix | `VFS_MAX_IO_CHUNK` |
| File names | same name as the primary class | `ArchiveBackend.h` |

## Files and namespaces

- Headers always use `#pragma once`. Do not use it in `.cpp` files.
- The first line of each header is a one-sentence comment stating the file's responsibility (`// This file: ...`).
- A `.cpp` includes its own header first. In-library headers use a relative path and `""`; the standard library and system headers use `<>`. Tools and tests include library headers as `"vfs/..."`.
- All library code lives in namespace `vfs`, wrapped in `NS_VFS_BEGIN` / `NS_VFS_END`. `USING_NS_VFS` is used only in `.cpp` files, tools, tests, and examples.
- Exported classes and functions are marked `VFS_DLL`.
- Platform-specific code is concentrated in `src/vfs/native/` (except the tool's command-line entry) and distinguished with `#ifdef _WIN32`. Other directories do not call platform APIs directly.

## C++ usage

- C++20. Depend only on the standard library and libraries in `3rd/`.
- Virtual functions are both `virtual` and `override`.
- Ownership is expressed with `std::unique_ptr` / `std::shared_ptr`. Read-only parameters use `std::string_view` or `const&`.
- Do not report business errors with exceptions: return `vfs::Error` and set `errno`. Interfaces that return a pointer or a stream take an optional `Error* err` out-parameter.
- `IFileStream` methods return `-1` and set `errno` on failure, following POSIX.
- A single `read`/`write` does not exceed `VFS_MAX_IO_CHUNK`. Large transfers loop in chunks.
- Binary formats are little-endian throughout, read and written through `ByteIO`, not by `memcpy` of a struct.
- The code compiles without warnings on GCC, Clang (`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor`), and MSVC (`/W4`). Check with `VFS_WARNINGS_AS_ERRORS=ON`.

## Comments

- Write comments in English. Explain why, do not restate the code.
- Public APIs use `/** */` and spell out parameters, return values, error codes, thread safety, and ownership.
- Mark the offset where a binary-format field is read or written, for example `// [48] u64 indexOffset`.

## Encoding

- Every source file and document is UTF-8 without BOM, LF line endings, and a trailing newline. MSVC is compiled with `/utf-8`.
- Paths on the interface are always UTF-8 strings; on Windows they are converted to UTF-16 before the system call.

## Tests

- Use doctest, one test directory per module (`tests/union/` and so on). `TEST_SUITE("union")` is registered automatically as the ctest case `vfs.union`. A new suite must be added to the suite list in `tests/CMakeLists.txt`. Long-running cases add `* doctest::skip(!test::slowEnabled())` and run only when `VFS_TEST_SLOW=1`. Stress / random tests multiply their rounds by `test::scale()` (`VFS_TEST_SCALE`) so the default `ctest` finishes within a minute.
- Code comments are in English. **All user-facing text is in English**: returned error strings (`errorMessage`, `ArchiveWriter::lastError()`), logs, vfstool / vfs_bench output, test names, and assertion INFO/MESSAGE. That way nothing is garbled under any terminal code page.
- Test names describe behavior in English, for example `"removeDir: READ_ONLY and nothing removed when a read-only layer contributes descendants"`. Chinese paths and content in test data (used to verify UTF-8 handling) stay as they are.
- Tests write only to a temporary directory and clean it up when finished.
