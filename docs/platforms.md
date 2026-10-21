# Platform Support and Builds

| Platform | Native backend | Platform-specific | How it is verified |
|---|---|---|---|
| Windows (MSVC, MinGW) | Win32 API, UTF-8 <-> UTF-16, long paths | — | All tests, MSVC Release; MinGW cross-compile + wine |
| Linux (GCC, Clang, libc++) | POSIX | — | All tests, plus a TSAN build |
| macOS | POSIX + `getattrlist` (case check) | — | All tests |
| iOS / iPadOS | Same as macOS (the app bundle and sandbox directories are ordinary files) | No dedicated backend | Device (arm64) and simulator builds with zero warnings; all tests on a device (iPhone 14 Pro, iOS 26.5) and on the simulator |
| Android (API 24+) | POSIX (internal storage, external storage) | `AndroidAssetBackend`: mount `assets/` inside the APK | NDK r27 cross-compile arm64-v8a / x86_64 / armeabi-v7a with zero warnings; all tests on an x86_64 emulator (Android 14) |
| WebAssembly (Emscripten) | POSIX over the Emscripten FS (MEMFS, NODEFS, NODERAWFS, and so on) | — | All tests under node (pthread build) |

Platform detection uses `VFS_PLATFORM_WINDOWS / LINUX / ANDROID / MACOS / IOS / WASM` from `src/vfs/Platform.h`,
plus `VFS_PLATFORM_APPLE` and `VFS_PLATFORM_POSIX` (each defined as 0 or 1, tested with `#if`).

## Android

### Build

```bash
cmake -S . -B build-android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
```

When a Gradle project pulls it in with `externalNativeBuild`, `add_subdirectory(VirtualFileSystem)` and link `vfs`; the library links `libandroid` itself.
Minimum API 24: on 32-bit ABIs the library uses 64-bit file offsets (`_FILE_OFFSET_BITS=64`). The matching `lseek64` / `pread64` and related calls are fully available from API 24.

### Running tests

The test program is an ordinary command-line executable and can be pushed to a device or emulator:

```bash
adb push build-android/tests/vfs_tests /data/local/tmp/
adb shell "cd /data/local/tmp && TMPDIR=/data/local/tmp ./vfs_tests"
```

`vfstool.e2e` needs the host to run vfstool, so it is not run for a cross build.

### Assets inside an APK: AndroidAssetBackend

APK `assets/` are not on the filesystem, so `mountNative` cannot reach them. `AndroidAssetBackend` is a read-only backend on top of `AAssetManager`:

```cpp
#include "vfs/android/AndroidAssetBackend.h"

// 1. Set the AAssetManager (either one)
vfs::AndroidAssetBackend::setAssetManagerFromJava(env, javaAssetManager);   // JNI; holds a global reference internally
vfs::AndroidAssetBackend::setAssetManager(ax::FileUtilsAndroid::getAssetManager());   // already wired into axmol

// 2. Mount all of assets/, or a subdirectory
auto assets = vfs::AndroidAssetBackend::create();             // or create({"res"}) to mount only assets/res/
fs.mount("/res", assets);

// 3. Or mount a .vfs archive inside assets/ (preferred: packed resources occupy a single APK entry)
fs.mount("/res", vfs::ArchiveBackend::create(assets->archiveSource("game.vfs")));
```

- **Thread safety**: each stream holds its own `AAsset`. Multiple threads can open and read the same asset at the same time.
- **How reads work**: uncompressed assets use `AAsset_openFileDescriptor64` and then read by offset (`pread`), so random reads do not seek.
  Compressed assets use `AAsset_read` / `AAsset_seek64`; random reads inflate the asset repeatedly and are slower.
  Keep `.vfs` archives uncompressed: in Gradle set `androidResources { noCompress += "vfs" }`.
- **Directories**: `AAssetManager` can list files in a directory but not subdirectories. In the default "probe" mode, a directory that contains only subdirectories `stat`s as NOT_FOUND,
  and `list` omits subdirectories. For a full tree, supply a manifest: `AssetOptions::manifest = "assets.lst"`. Each line is one file path (relative to `root`).
  Generate it at build time, for example `cd app/src/main/assets && find . -type f | sed 's|^\./||' > assets.lst`.
  An archive does not need a manifest (the archive already has a full index).
- **Modification time**: APK assets have no mtime, so `Stat::mtime` is 0.
- **Tests**: the backend logic is platform-independent (`AssetBackend` + `IAssetProvider`). `tests/asset/` runs a simulated asset source on every platform
  and covers both directory modes, descriptor and cursor reads, concurrent reads, and mounting an archive. `AndroidAssetBackend` is a thin wrapper over `AAssetManager` and needs an APK for an end-to-end check.

## iOS

No dedicated backend: the app bundle (`[[NSBundle mainBundle] resourcePath]`) and the sandbox directories (Documents, Library, tmp) are ordinary POSIX files.

```bash
# Device
cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_BUILD_TYPE=Release
# Simulator: -DCMAKE_OSX_SYSROOT=iphonesimulator
cmake --build build-ios -j
# Run tests on the simulator
xcrun simctl spawn <device-udid> build-ios/tests/vfs_tests.app/vfs_tests
```

Running on a device needs signing. Use the Xcode generator:

```bash
cmake -S . -B build-ios-dev -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DVFS_BUILD_BENCH=ON -DCMAKE_XCODE_GENERATE_SCHEME=ON \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team-id> -DVFS_APPLE_BUNDLE_ID_PREFIX=com.yourname.vfs
xcodebuild -project build-ios-dev/VirtualFileSystem.xcodeproj -scheme vfs_tests -configuration Release \
    -destination "id=<device-udid>" -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
xcrun devicectl device install app --device <device-id> build-ios-dev/tests/Release-iphoneos/vfs_tests.app
xcrun devicectl device process launch --device <device-id> --console com.yourname.vfs.vfs-tests
```

Each executable's bundle identifier is `VFS_APPLE_BUNDLE_ID_PREFIX` plus the target name (`_` replaced with `-`). Before the first run, trust the developer certificate under Settings > General > VPN & Device Management. On iOS, `vfs_bench` starts through a UIKit host entry (a full benchmark takes several minutes and exceeds the system launch watchdog), so keep the screen awake while it runs.

Note: the iOS simulator filesystem (host APFS) is case-sensitive for lookup, but creating a name that differs only in case still conflicts (EEXIST). That differs from a device (case-sensitive APFS).

## WebAssembly (Emscripten)

```bash
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j
cd build-wasm && ctest            # run tests and vfstool under node
```

- `VFS_WASM_THREADS` (default ON): every target gets `-pthread`. The library itself still works in a threadless build (OFF); locks become no-ops.
  Tests, the benchmark, and the multithreaded examples need threads.
- When built as the top-level project, executables are linked as node programs: `-sNODERAWFS=1` (the native backend talks to the host filesystem directly),
  `-sPROXY_TO_PTHREAD`, and a fixed 1.5 GiB initial memory. Used as a subdirectory, those options are not applied to your program.
- In the browser the native backend runs on Emscripten's virtual filesystem (MEMFS by default; IDBFS / OPFS for persistence).
  Mount a directory there with `mountNative("/data", "/data")`. Assets can also be mounted directly through the memory backend or `MemoryArchiveSource` from a downloaded archive.
- Emscripten's filesystem silently truncates names longer than 255 bytes and still creates the file. On WebAssembly the native backend rejects those names outright (INVALID_PATH).
