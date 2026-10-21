# 平台支持与构建

| 平台 | 本机后端 | 特有内容 | 验证方式 |
|---|---|---|---|
| Windows（MSVC、MinGW） | Win32 API，UTF-8 ↔ UTF-16，长路径 | — | MSVC Release 全部测试；MinGW 交叉编译 + wine |
| Linux（GCC、Clang、libc++） | POSIX | — | 全部测试，另有 TSAN 构建 |
| macOS | POSIX + `getattrlist`（大小写核对） | — | 全部测试 |
| iOS / iPadOS | 与 macOS 相同（应用包与沙盒目录都是普通文件） | 不需要专门的后端 | 真机（arm64）与模拟器编译零警告；真机（iPhone 14 Pro，iOS 26.5）与模拟器上运行全部测试 |
| Android（API 24+） | POSIX（内部存储、外部存储） | `AndroidAssetBackend`：挂载 APK 内的 `assets/` | NDK r27 交叉编译 arm64-v8a / x86_64 / armeabi-v7a 零警告；x86_64 模拟器（Android 14）上运行全部测试 |
| WebAssembly（Emscripten） | POSIX over Emscripten FS（MEMFS、NODEFS、NODERAWFS 等） | — | node 上运行全部测试（pthread 构建） |

平台判断统一使用 `src/vfs/Platform.h` 中的 `VFS_PLATFORM_WINDOWS / LINUX / ANDROID / MACOS / IOS / WASM`
以及 `VFS_PLATFORM_APPLE`、`VFS_PLATFORM_POSIX`（都定义为 0 或 1，用 `#if` 判断）。

## Android

### 构建

```bash
cmake -S . -B build-android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
```

在 Gradle 工程中用 `externalNativeBuild` 引入时，`add_subdirectory(VirtualFileSystem)` 后链接 `vfs` 即可；库会自动链接 `libandroid`。
最低 API 24：32 位 ABI 上库使用 64 位文件偏移（`_FILE_OFFSET_BITS=64`），对应的 `lseek64`/`pread64` 等在 API 24 起完整可用。

### 运行测试

测试程序是普通的命令行可执行文件，可以推到设备或模拟器上运行：

```bash
adb push build-android/tests/vfs_tests /data/local/tmp/
adb shell "cd /data/local/tmp && TMPDIR=/data/local/tmp ./vfs_tests"
```

`vfstool.e2e` 依赖宿主机运行 vfstool，交叉编译时不执行。

### APK 内的资源：AndroidAssetBackend

APK 的 `assets/` 不在文件系统中，`mountNative` 无法访问。`AndroidAssetBackend` 通过 `AAssetManager` 提供只读后端：

```cpp
#include "vfs/android/AndroidAssetBackend.h"

// 1. 设置 AAssetManager（二选一）
vfs::AndroidAssetBackend::setAssetManagerFromJava(env, javaAssetManager);   // JNI，内部持有全局引用
vfs::AndroidAssetBackend::setAssetManager(ax::FileUtilsAndroid::getAssetManager());   // 已接入 axmol

// 2. 挂载整个 assets/，或其中的子目录
auto assets = vfs::AndroidAssetBackend::create();             // 或 create({"res"}) 只挂载 assets/res/
fs.mount("/res", assets);

// 3. 也可以挂载 assets/ 中的 .vfs 归档（推荐：资源打包后只占一个 APK 条目）
fs.mount("/res", vfs::ArchiveBackend::create(assets->archiveSource("game.vfs")));
```

- **线程安全**：每个流持有自己的 `AAsset`，多个线程可以同时打开、读取同一个资源。
- **读取方式**：未压缩存放的资源用 `AAsset_openFileDescriptor64` 拿到描述符后按偏移读取（`pread`），随机读不需要 seek；
  压缩存放的资源用 `AAsset_read` / `AAsset_seek64`，随机读会反复解压，较慢。
  `.vfs` 归档请保持未压缩：Gradle 中设置 `androidResources { noCompress += "vfs" }`。
- **目录**：`AAssetManager` 只能列出目录中的文件，看不到子目录。默认的“探测模式”下，只含子目录的目录 `stat` 为 NOT_FOUND，
  `list` 也不含子目录。需要完整目录结构时提供清单：`AssetOptions::manifest = "assets.lst"`，清单每行一个文件路径（相对 `root`），
  可在构建时生成，例如 `cd app/src/main/assets && find . -type f | sed 's|^\./||' > assets.lst`。
  使用归档时不需要清单（归档自带完整索引）。
- **修改时间**：APK 资源没有 mtime，`Stat::mtime` 为 0。
- **测试**：后端逻辑与平台无关（`AssetBackend` + `IAssetProvider`），`tests/asset/` 用模拟的资源来源在所有平台上运行，
  覆盖两种目录模式、描述符 / 游标两种读取方式、并发读取与挂载归档；`AndroidAssetBackend` 只是 `AAssetManager` 的薄封装，需要 APK 才能端到端验证。

## iOS

不需要专门的后端：应用包（`[[NSBundle mainBundle] resourcePath]`）与沙盒目录（Documents、Library、tmp）都是普通的 POSIX 文件。

```bash
# 真机
cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_BUILD_TYPE=Release
# 模拟器：-DCMAKE_OSX_SYSROOT=iphonesimulator
cmake --build build-ios -j
# 在模拟器上运行测试
xcrun simctl spawn <device-udid> build-ios/tests/vfs_tests.app/vfs_tests
```

真机运行需要签名，用 Xcode 生成器：

```bash
cmake -S . -B build-ios-dev -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DVFS_BUILD_BENCH=ON -DCMAKE_XCODE_GENERATE_SCHEME=ON \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team-id> -DVFS_APPLE_BUNDLE_ID_PREFIX=com.yourname.vfs
xcodebuild -project build-ios-dev/VirtualFileSystem.xcodeproj -scheme vfs_tests -configuration Release \
    -destination "id=<device-udid>" -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
xcrun devicectl device install app --device <device-id> build-ios-dev/tests/Release-iphoneos/vfs_tests.app
xcrun devicectl device process launch --device <device-id> --console com.yourname.vfs.vfs-tests
```

每个可执行文件的包标识是 `VFS_APPLE_BUNDLE_ID_PREFIX` 加目标名（`_` 换成 `-`）。首次运行前要在 设置 > 通用 > VPN 与设备管理 里信任开发者证书。`vfs_bench` 在 iOS 上通过 UIKit 宿主入口启动（完整基准要跑几分钟，超过系统的启动看门狗时限），跑的时候保持屏幕常亮。

注意：iOS 模拟器的文件系统（宿主机 APFS）查找时区分大小写，但创建只差大小写的名字仍会冲突（EEXIST），与真机（区分大小写的 APFS）不同。

## WebAssembly（Emscripten）

```bash
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j
cd build-wasm && ctest            # 用 node 运行测试与 vfstool
```

- `VFS_WASM_THREADS`（默认 ON）：所有目标加 `-pthread`。库本身在无线程构建（OFF）下也能使用（锁退化为空操作），
  但测试、基准和多线程示例需要线程。
- 作为顶层项目构建时，可执行文件按 node 程序链接：`-sNODERAWFS=1`（本机后端直接访问宿主文件系统）、
  `-sPROXY_TO_PTHREAD`、固定 1.5 GiB 初始内存。作为子项目使用时这些选项不会加到你的程序上。
- 浏览器中：本机后端工作在 Emscripten 的虚拟文件系统上（默认 MEMFS；持久化可用 IDBFS / OPFS），
  用 `mountNative("/data", "/data")` 挂载其中的目录即可；资源也可以直接用内存后端或 `MemoryArchiveSource` 挂载下载得到的归档。
- Emscripten 的文件系统会把超过 255 字节的名字静默截断后创建，本机后端在 WebAssembly 上直接拒绝这类名字（INVALID_PATH）。
