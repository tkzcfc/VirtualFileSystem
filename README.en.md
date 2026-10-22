# VirtualFileSystem

English | [简体中文](README.md)

A layered virtual file system for game asset management, implemented in C++20.

Mount a native directory, memory, a `.vfs` archive, a single file, or a custom backend at any virtual path. Layers merge by priority into one view; reads, writes, and directory create/delete follow deterministic rules; every API is thread-safe. The file-stream interface matches [axmol](https://github.com/axmolengine/axmol)'s `IFileStream` exactly, so it can be wired into the engine directly.

The companion command-line tool `vfstool` packs, unpacks, and inspects `.vfs` archives.

**Performance**: overhead of each backend and layer combination versus calling the C file API directly (Linux, macOS, Windows, iOS, Android, WebAssembly; 1 / 8 threads), plus selection advice, is in [Benchmarks](docs/benchmark.md).

## Features

- **Several mount types**: a native directory, a single native file, an in-memory directory tree, a `.vfs` archive (from a file or from memory), and a custom backend that implements `IBackend`.
- **Layered merged view**: stacked by priority, a higher layer wins for the same file name, and directory contents are merged; supports occlusion (`Opaque`), read-only (`ReadOnly`), and a designated default write layer (`WriteTarget`).
- **Deterministic write rules**: where a write, copy-up, delete, `rmdir`, or `rename` lands, and which error it returns, is defined in [Mount rules](docs/mount-rules.md).
- **Thread safety**: the mount table is a copy-on-write snapshot, so lookups take no lock; mount and unmount can run concurrently with reads; unmount is lazy, and a stream that is already open can finish reading.
- **`.vfs` archives**: optional per-file XOR encryption (random seek is supported) and LZ4 compression, ChaCha20-encrypted index, content dedup, data alignment, and corruption checks on the header and index. **The same input packs to the same bytes every time.** Each stream holds its own handle and reads by offset, so concurrent readers do not interfere.
- **Cross-platform**: Windows (UTF-8 paths converted to UTF-16, long paths supported), Linux, macOS, iOS, Android (including the APK asset backend `AndroidAssetBackend`), and WebAssembly (Emscripten). Dependencies are the standard library plus doctest, xxHash, LZ4, and CLI11 under `3rd/`.

| Platform | Status |
|---|---|
| Windows / Linux / macOS | Full test suite |
| iOS | Device and simulator builds; full test suite on the simulator |
| Android (API 24+) | Cross-compile for arm64-v8a / x86_64 / armeabi-v7a; full test suite on an x86_64 emulator; APK asset backend |
| WebAssembly | Emscripten build; full test suite under node (pthread and threadless builds) |

How to build on each platform, and the caveats, are in [Platforms](docs/platforms.md).

## Performance

Shipping assets in a `.vfs` archive is usually much faster than opening loose files with plain C `stdio` (`fopen`/`fread`/`stat`) on the same machine, because opens, `stat`s, and lookups are answered from an in-memory index:

| Operation | Windows | Linux | macOS | iOS | Android | WebAssembly |
|---|---:|---:|---:|---:|---:|---:|
| Read a small file (4 KiB) | **7.3x** / 4.4x | **2.5x** / 2.2x | **18.1x** / 10.3x | **164x** / 21.8x | **1.5x** / 1.2x | **3.9x** / 2.3x |
| Random reads in a 64 MiB file | **0.9x** / 1.0x | **1.5x** / 1.0x | **2.4x** / 3.0x | **2.3x** / 3.1x | **2.1x** / 1.2x | **1.9x** / 2.1x |
| `stat` a file | **66.6x** / 47.4x | **4.4x** / 4.3x | **15.7x** / 25.8x | **285x** / 150x | **3.7x** / 1.2x | **58.1x** / 2.0x |
| Open a file | **26.3x** / 20.8x | **4.3x** / 3.1x | **45.6x** / 29.4x | **133x** / 30.2x | **5.6x** / 0.8x | **44.7x** / 1.3x |
| Look up a missing file | **19.4x** / 17.9x | **2.6x** / 1.2x | **3.7x** / 9.4x | **68.6x** / 45.1x | **0.5x** / 0.4x | **27.3x** / 1.5x |

Speedup of the plain archive backend (`vfs:archive`) over `base:stdio`, **1 thread** / 8 threads, measured with [`vfs_bench`](docs/benchmark.md) at default settings (warm cache, Release).
Values below 1x mean slower than stdio. Machines: Windows = Core i5-12400, Linux = 8-vCPU Xeon VM, macOS = Apple M4, iOS = iPhone 14 Pro, Android = Redmi K80 (Snapdragon 8 Gen 3), WebAssembly = Emscripten -O3 pthread build run under node 24 on the same Linux VM.
Android ran as a command-line process from `adb shell`, which gets no foreground-app CPU boost. Under WebAssembly every stdio and native call goes through node's file system API, so the 1-thread ratios are high and the 8-thread ones low.
Every backend (native, memory, LZ4 archives, layered mounts), every scenario, and the absolute numbers are in [docs/benchmark.md](docs/benchmark.md).

![Windows: speed relative to stdio](docs/images/bench/windows.svg)

### Performance tips: what to avoid

Measured on the six platforms above (ratios vs `base:stdio`, 1 thread / 8 threads; details in [docs/benchmark.md](docs/benchmark.md)):

- **Don't LZ4-compress large files you read randomly or stream.** LZ4 entries are split into blocks (64 KiB by default), and a read decodes the whole block it touches. Opening is as cheap as an uncompressed entry, but 4 KiB random reads inside a 64 MiB LZ4 file run at 0.06-0.21x of stdio on 1 thread (0.82x under WebAssembly), sequential reads of that file at 0.23-0.54x, and 256 KiB files at 0.44-1.07x. *Instead:* compress small text such as scripts and config (`--compress "*.lua"`; small LZ4 files read 0.87-46x as fast as stdio on 1 thread), leave large binary assets uncompressed, or use a smaller `--block-size` for compressed files that are read randomly.
- **Don't stack a native directory over an archive unless you need overrides.** Every lookup checks the native layer first, which is a real system call: `archive+native` misses are 0.27-1.34x of stdio (iOS 1.2-1.8x), while the plain archive answers misses from its index. *Instead:* mount development override directories only in development builds, never an empty one in release.
- **Keep layer stacks shallow.** With 9 layers on one path (`nested8`), opens are 0.26-1.25x and misses 0.18-0.96x of stdio (iOS 1 thread: 1.5x). *Instead:* use separate mount points, or pack layers into one archive.
- **Native directory listing is slower than a raw `readdir`.** The VFS merges layers, deduplicates, sorts, and converts names: `vfs:native` `list` is 0.56-0.92x (about 0.94-1.04x on macOS/iOS at 8 threads). *Instead:* list from an archive (1.06-21x) or cache listings you reuse.
- **Don't expect gains on random reads inside one large uncompressed file on Windows:** the archive is 0.88-1.0x of stdio there (1.0-3.1x elsewhere). The big wins are opens, `stat`, lookups, and small files.
- **On Android, don't serve many loose files through `AAssetManager`, and don't mount `AndroidAssetBackend` without a manifest.** Measured in the bench APK on a Redmi K80: small reads through `AAssetManager` (raw or `vfs:asset`) are 0.55-0.82x of stdio on extracted files on 1 thread and 0.08-0.11x on 8 threads; deflated loose assets drop to 0.3x / 0.07x for small files and 0.11-0.13x for a 64 MiB sequential read, and random reads inside a deflated 64 MiB asset did not finish a single run in 9 minutes. Without `assets.lst` the backend probes the APK: `stat` is 0.03x / 0.002x, misses 0.01x / 0.001x, and listing a 10000-file directory takes about 3 s (0.002x). With the manifest, `stat` is 0.98x, misses 2.3x / 2.0x and listing 1.9x / 0.9x. *Instead:* pack assets into a `.vfs` stored uncompressed in the APK (Gradle `noCompress += "vfs"`) and mount it with `archiveSource`: 3.4x / 3.0x for small reads, 7.7x / 6.5x for `stat`; if loose assets are needed, always ship `assets.lst` and keep large or randomly read assets uncompressed.

## Quick start

```cpp
#include "vfs/VirtualFileSystem.h"
#include <cstdio>

USING_NS_VFS;

// Assume game.vfs was produced by `vfstool pack assets -o game.vfs` and contains config.json and scripts/main.lua;
// assets_dev/ contains only textures/hero.png
int main()
{
	VirtualFileSystem fs;

	// Read-only assets: archive below, dev directory above (the dev directory wins for the same name)
	fs.mountArchive("/res", "game.vfs");
	fs.mountNative("/res", "assets_dev", MountOptions{10});

	// Writable data: mounted on its own at /user
	NativeOptions userDir;
	userDir.createRoot = true;
	fs.mountNative("/user", "save", MountOptions{0, MountFlags::WriteTarget}, userDir);

	// Alias: res://scripts/main.lua is the same as /res/scripts/main.lua
	fs.addAlias("res://", "/res");

	std::string config;
	if (fs.readFile("res://config.json", config) != Error::OK)
		return 1;

	auto stream = fs.openFileStream("res://scripts/main.lua", IFileStream::Mode::READ);
	fs.writeFileAtomic("/user/save.dat", "progress=3", 10);

	for (const auto& entry : fs.list("res://"))
		std::printf("%s%s\n", entry.name.c_str(), entry.type == FileType::DIR ? "/" : "");
	// Output (the two layers merged, sorted by name):
	// config.json
	// scripts/
	// textures/
	return 0;
}
```

Pack assets:

```bash
vfstool pack assets -o game.vfs --compress "*.lua" --compress "*.json" --encrypt "**/*.png"
vfstool list game.vfs -l
vfstool verify game.vfs
```

More samples are in [`examples/`](examples/) (build with `-DVFS_BUILD_EXAMPLES=ON`):

| Example | Contents |
|---|---|
| [`basic`](examples/basic/main.cpp) | Minimal sample: pack an archive in memory and stack a writable memory layer on it |
| [`layers`](examples/layers/main.cpp) | Layered mounts, occlusion, a read-only layer, and aliases |
| [`archive`](examples/archive/main.cpp) | Pack and read an archive: custom index key, XOR encryption, LZ4 compression, dedup |
| [`custom_backend`](examples/custom_backend/main.cpp) | Implement `IBackend` to mount content generated by the program |
| [`multithread`](examples/multithread/main.cpp) | Eight threads reading concurrently while the archive is hot-updated (`remount`) |
| [`stream`](examples/stream/main.cpp) | Stream API: chunked write, seek, append, chunked read |

API details are in [API reference](docs/api.md).

## Build and test

Requires CMake >= 3.16 and a C++20 compiler (GCC, Clang, Apple Clang, or MSVC).

```bash
cmake -S . -B build
cmake --build build -j
cd build && ctest --output-on-failure
```

| CMake option | Default | Description |
|---|---|---|
| `VFS_BUILD_TOOLS` | ON when this is the top-level project | Build `vfstool` |
| `VFS_BUILD_TESTS` | ON when this is the top-level project | Build tests (also builds `vfstool`) |
| `VFS_BUILD_EXAMPLES` | OFF | Build `examples/` |
| `VFS_BUILD_BENCH` | OFF | Build the `vfs_bench` benchmark; see [Benchmarks](docs/benchmark.md) |
| `VFS_WITH_AXMOL` | OFF | Use axmol's `ax::IFileStream`; see [Integrating with axmol](docs/integration-axmol.md) |
| `VFS_WARNINGS_AS_ERRORS` | OFF | Treat compiler warnings as errors |
| `VFS_WASM_THREADS` | ON | Emscripten only: add `-pthread` to every target; see [Platforms](docs/platforms.md#webassembly-emscripten) |

Use it as a subdirectory:

```cmake
add_subdirectory(VirtualFileSystem)
target_link_libraries(my_game PRIVATE vfs)   # or vfs::vfs
```

Tests use doctest and are registered as ctest cases by suite: `vfs.path`, `vfs.union`, `vfs.native`, `vfs.memory`, `vfs.stream`, `vfs.archive`, `vfs.crypto`, `vfs.tool`, `vfs.concurrency`, plus the command-line end-to-end suite `vfstool.e2e`. Four broader suites:

| suite | Contents |
|---|---|
| `vfs.matrix` | The same file / directory / rename / stream operations, run once on each of 9 backend and layer combinations (native, memory, nested mounts, archive under native, memory stacked on an archive, equal priority, three layers, a read-only native directory, and so on) |
| `vfs.stress` | Multithreaded stress: concurrent create/delete/update/read in the same directory and in different directories, rename, read/write races, concurrent append copy-up, listing concurrent with mutation, mount / unmount / remount jitter, many handles on one archive |
| `vfs.model` | Random operation sequences checked step by step against a reference model (error codes, merged view, write-layer contents) |
| `vfs.extreme` | Edge cases: empty files, huge entry counts, deep paths and very long names, Unicode and case, external modification, read-only directories, per-region archive corruption and forged indexes, fault injection, handle exhaustion, sparse files over 4 GB |

The default `ctest` finishes within a minute. Long cases are skipped unless an environment variable enables them:

```bash
VFS_TEST_SLOW=1 ./build/tests/vfs_tests      # include slow cases (100k-entry archive, sparse files >4 GB, long model comparisons)
VFS_TEST_SCALE=5 ./build/tests/vfs_tests     # multiply stress and random-test rounds by 5
./build/tests/vfs_tests --test-suite=stress  # run one suite (other doctest arguments work the same way)
```

Cases that depend on platform capabilities probe at runtime instead of hard-coding the OS: whether the filesystem of the temp directory is case-sensitive (Windows/macOS are not, by default, and those cases check the documented case-insensitive behavior), and whether a file that is open can be replaced (Windows needs POSIX rename semantics on NTFS; when that is unavailable the case is relaxed and prints one `MESSAGE`). On failure, concurrency cases print the error-code distribution of each operation and the first few failure details. On Linux the full suite can be cross-compiled with MinGW and run under Wine (Wine does not support POSIX rename semantics, so the `MESSAGE` above appears).

### Benchmarks

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DVFS_BUILD_BENCH=ON -DVFS_BUILD_TESTS=OFF
cmake --build build-bench --target vfs_bench -j
./build-bench/bench/vfs_bench --csv bench.csv --md bench.md   # every scenario; --help lists --scale / --threads / --filter and the rest
```

`vfs_bench` compares `std::fstream`, `FILE*`, POSIX `pread` / Win32 `ReadFile` with the backends and layer combinations for read, write, and metadata performance, and reports throughput, p50/p99 latency, and the multiple relative to stdio. Local results and analysis are in [docs/benchmark.md](docs/benchmark.md).

## Layout

```
3rd/            third-party libraries (pinned versions): doctest, xxHash, LZ4, CLI11
bench/          vfs_bench performance benchmark (not built by default)
cmake/          build options and compiler settings
docs/           documentation
examples/       samples (not built by default)
src/vfs/        runtime library
  native/       native directory, single-file backend, and NativeFileStream
  memory/       memory backend
  archive/      .vfs read and write
  crypto/       ChaCha20, XXH3, XOR
tests/          unit tests and command-line end-to-end tests
tools/vfstool/  command-line tool
```

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Layer structure, mount table, backends, streams, threading model |
| [docs/mount-rules.md](docs/mount-rules.md) | Priority, merged view, write target, delete / rmdir / rename semantics and error codes |
| [docs/api.md](docs/api.md) | Main public API, `IFileStream` semantics, custom backends |
| [docs/format.md](docs/format.md) | `.vfs` binary format specification |
| [docs/vfstool.md](docs/vfstool.md) | Command-line tool reference |
| [docs/benchmark.md](docs/benchmark.md) | Benchmarks: how to build and run, local results, and overhead analysis |
| [docs/platforms.md](docs/platforms.md) | Platforms: building and caveats for Android (APK asset backend), iOS, and WebAssembly |
| [docs/integration-axmol.md](docs/integration-axmol.md) | Integrating with the axmol engine |
| [docs/style.md](docs/style.md) | Code style |
