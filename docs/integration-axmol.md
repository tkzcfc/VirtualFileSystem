# Integrating with axmol

VFS's stream interface is identical, word for word, to the axmol engine's `IFileStream`. With `VFS_WITH_AXMOL` on, `vfs::IFileStream` is `ax::IFileStream`, so a stream returned by VFS can be handed to the engine as-is. A `FileUtils` subclass then forwards the engine's file access to VFS, and both game code and engine modules read archives and layered directories transparently.

## 1. Build

```cmake
set(VFS_WITH_AXMOL ON CACHE BOOL "" FORCE)
set(VFS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(VFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(VirtualFileSystem)

# vfs must be able to see axmol's headers (and macros such as AX_DLL); link the engine core-library target
target_link_libraries(vfs PUBLIC axmol)        # target name depends on the CMake project of the engine version in use
target_link_libraries(${APP_NAME} PRIVATE vfs)
```

`VFS_WITH_AXMOL=1` is passed as a PUBLIC macro to every target that uses `vfs`. `src/vfs/IFileStream.h` looks for the engine header in this order:

| axmol version | Header location | Include |
|---|---|---|
| dev branch (3.x) | `axmol/platform/IFileStream.h` | `"axmol/platform/IFileStream.h"` |
| 2.x | `core/platform/IFileStream.h` | `"platform/IFileStream.h"` (`core/` is on the include path) |

The first one that exists is preferred (`__has_include`). The `IFileStream` interface is the same in both versions, so VFS code does not change.

## 2. FileUtils subclass

axmol reaches files through `FileUtils::getInstance()`, and `FileUtils::setDelegate()` replaces that instance. Virtual functions to forward:

| FileUtils virtual | Forwards to | Notes |
|---|---|---|
| `getContents` | `openFileStream` + `read` | **Must be overridden**: the engine's default implementation constructs a native `FileStream` directly and does not go through `openFileStream` |
| `openFileStream` | `openFileStream` | The engine's write functions (`writeStringToFile`, `writeDataToFile`, and so on) all write through it |
| `isFileExistInternal` | `isFile` | Pure virtual; search-path resolution depends on it |
| `isDirectoryExistInternal` | `isDir` | |
| `getFileSize` | `fileSize` | |
| `listFiles` / `listFilesRecursively` | `list` / `walk` | The default implementation walks native directories with `std::filesystem` |
| `createDirectories` / `removeDirectory` / `removeFile` / `renameFile` | `createDirs` / `removeDir` / `removeFile` / `moveFile` | |
| `getWritablePath` / `getNativeWritableAbsolutePath` | — | Pure virtual: the first returns the virtual writable directory, the second returns the real native directory |

A virtual path starts with `/`. axmol's `isAbsolutePath` treats that as absolute on every platform (on Windows, as "an absolute path on the current drive"), so `fullPathForFilename("/res/a.png")` returns the path unchanged and `isAbsolutePath` does not need to be overridden. After `/res/` is set as the default resource root, the relative path `"a.png"` is resolved through the search path to `/res/a.png`.

The dev branch is shown below (differences in 2.x are noted after the code):

```cpp
// VfsFileUtils.h
#pragma once

#include "axmol/platform/FileUtils.h"
#include "vfs/VirtualFileSystem.h"

class VfsFileUtils : public ax::FileUtils
{
public:
	/**
	 * Create the instance and take over FileUtils::getInstance().
	 * @param fs             a VFS that is already mounted; it must stay alive for the whole process
	 * @param nativeWritable the real native writable directory (for modules that bypass FileUtils)
	 */
	static VfsFileUtils* install(std::shared_ptr<vfs::VirtualFileSystem> fs, std::string nativeWritable)
	{
		auto* self = new VfsFileUtils(std::move(fs), std::move(nativeWritable));
		self->_defaultResRootPath = "/res/";
		self->_writablePath = "/user/";
		self->init();
		ax::FileUtils::setDelegate(self);   // deletes the previous instance
		return self;
	}

	vfs::VirtualFileSystem& vfs() const { return *m_fs; }

	// ---------------------------------------------------------------- read
	Status getContents(std::string_view filename, ax::ResizableBuffer* buffer) const override
	{
		if (filename.empty())
			return Status::NotExists;
		const std::string path = fullPathForFilename(filename);
		if (path.empty())
			return Status::NotExists;

		vfs::Error err = vfs::Error::OK;
		auto stream = m_fs->openFileStream(path, ax::IFileStream::Mode::READ, &err);
		if (!stream)
			return err == vfs::Error::NOT_FOUND ? Status::NotExists : Status::OpenFailed;
		const int64_t size = stream->size();
		if (size < 0)
			return Status::ObtainSizeFailed;
		if (size > UINT32_MAX)
			return Status::TooLarge;

		buffer->resize_and_overwrite(static_cast<size_t>(size), [&stream](void* out, size_t total)
		{
			size_t done = 0;
			while (done < total)
			{
				int n = stream->read(static_cast<char*>(out) + done, static_cast<unsigned int>(total - done));
				if (n <= 0)
					break;
				done += static_cast<size_t>(n);
			}
			return done;
		});
		return buffer->size_in_bytes() == static_cast<size_t>(size) ? Status::OK : Status::ReadFailed;
	}

	std::unique_ptr<ax::IFileStream> openFileStream(std::string_view filePath, ax::IFileStream::Mode mode) const override
	{
		return m_fs->openFileStream(filePath, mode);
	}

	bool isFileExistInternal(std::string_view path) const override { return m_fs->isFile(path); }

	bool isDirectoryExistInternal(std::string_view path) const override { return m_fs->isDir(path); }

	int64_t getFileSize(std::string_view filepath) const override
	{
		const std::string path = isAbsolutePath(filepath) ? std::string(filepath) : fullPathForFilename(filepath);
		return path.empty() ? -1 : m_fs->fileSize(path);
	}

	// Same as the engine default: return the full path, and end directories with '/'
	std::vector<std::string> listFiles(std::string_view dirPath) const override
	{
		std::vector<std::string> files;
		std::string dir = fullPathForDirectory(dirPath);
		if (!dir.empty() && dir.back() != '/')
			dir += '/';
		for (const auto& entry : m_fs->list(dir))
			files.push_back(dir + entry.name + (entry.type == vfs::FileType::DIR ? "/" : ""));
		return files;
	}

	void listFilesRecursively(std::string_view dirPath, std::vector<std::string>* files) const override
	{
		m_fs->walk(fullPathForDirectory(dirPath), [files](const std::string& fullPath, const vfs::DirEntry& entry)
		{
			files->push_back(entry.type == vfs::FileType::DIR ? fullPath + "/" : fullPath);
			return true;
		});
	}

	// ---------------------------------------------------------------- write
	bool createDirectories(std::string_view path) const override { return m_fs->createDirs(path) == vfs::Error::OK; }

	bool removeDirectory(std::string_view path) const override
	{
		return m_fs->removeDir(path, true) == vfs::Error::OK;
	}

	bool removeFile(std::string_view path) const override { return m_fs->removeFile(path) == vfs::Error::OK; }

	bool renameFile(std::string_view oldPath, std::string_view newPath) const override
	{
		return m_fs->moveFile(oldPath, newPath) == vfs::Error::OK;
	}

	bool renameFile(std::string_view dir, std::string_view oldName, std::string_view name) const override
	{
		std::string base(dir);
		if (!base.empty() && base.back() != '/')
			base += '/';
		return renameFile(base + std::string(oldName), base + std::string(name));
	}

	// ---------------------------------------------------------------- writable directory
	std::string getWritablePath() const override { return _writablePath; }

	std::string getNativeWritableAbsolutePath() const override { return m_nativeWritable; }

private:
	VfsFileUtils(std::shared_ptr<vfs::VirtualFileSystem> fs, std::string nativeWritable)
		: m_fs(std::move(fs))
		, m_nativeWritable(std::move(nativeWritable))
	{
	}

	std::shared_ptr<vfs::VirtualFileSystem> m_fs;
	std::string m_nativeWritable;
};
```

At startup (for example in `AppDelegate::applicationDidFinishLaunching`, before any resource is loaded):

```cpp
auto fs = std::make_shared<vfs::VirtualFileSystem>();
const std::string nativeRes = ax::FileUtils::getInstance()->getDefaultResourceRootPath();
const std::string nativeUser = ax::FileUtils::getInstance()->getNativeWritableAbsolutePath();

fs->mountArchive("/res", nativeRes + "game.vfs");
#if defined(_DEBUG)
fs->mountNative("/res", "D:/project/assets", vfs::MountOptions{10});   // during development, the native directory overrides the archive
#endif
vfs::NativeOptions userOptions;
userOptions.createRoot = true;
fs->mountNative("/user", nativeUser, vfs::MountOptions{0, vfs::MountFlags::WriteTarget}, userOptions);

VfsFileUtils::install(fs, nativeUser);
```

**Differences in 2.x**: the header is `"platform/FileUtils.h"`; `ResizableBuffer` is `resize(size_t)` / `buffer()` / `size()`. In `getContents`, `buffer->resize(size)` first, then loop `read` into `buffer->buffer()`; if the read is short, `resize` to the actual length and return `Status::ReadFailed`. The other virtual signatures match the dev branch.

## 3. Notes

- **On Android**, resources inside the package are not on the native filesystem, so `mountNative` cannot mount a directory inside the APK. Use `AndroidAssetBackend` instead (see [Platforms](platforms.md#assets-inside-an-apk-androidassetbackend)) and reuse the `AAssetManager` the engine has already set up:

  ```cpp
  #if VFS_PLATFORM_ANDROID
  #include "vfs/android/AndroidAssetBackend.h"
  vfs::AndroidAssetBackend::setAssetManager(ax::FileUtilsAndroid::getAssetManager());
  auto assets = vfs::AndroidAssetBackend::create();
  // On Android the engine's resource root is "assets/", so nativeRes + "game.vfs" is not a filesystem path. Use:
  fs->mount("/res", vfs::ArchiveBackend::create(assets->archiveSource("game.vfs")));
  #endif
  ```

  `.vfs` must stay uncompressed inside the APK (Gradle: `androidResources { noCompress += "vfs" }`) so files inside the archive are read by offset.
  The writable directory (`getNativeWritableAbsolutePath()`) is an ordinary filesystem path; keep using `mountNative` for it.
- **Modules that bypass FileUtils** (for example audio decode or video playback on some platforms, or a third-party library that opens a native path) cannot see files that exist only in VFS. For those assets, either keep a native copy outside VFS or obtain a native path with `realPath()` (valid only for files on a native backend). `getNativeWritableAbsolutePath()` returns the real directory for exactly these modules.
- **Full-path cache**: `FileUtils` caches the result of `fullPathForFilename`. After mount, unmount, or `remount`, call `FileUtils::purgeCachedEntries()`. `VirtualFileSystem::generation()` tells whether anything changed.
- **Threads**: every VFS API is thread-safe, and a single stream object may be used from only one thread. `FileUtils`'s own search-path cache is not thread-safe. A background thread that loads resources should be given an already resolved full path (the same convention the engine uses in general).
- **Path case**: VFS is case-sensitive. During development on Windows / macOS, turn on `NativeOptions::strictCase` for the native layer so a case mismatch with the archive shows up early.
- **Version differences**: the code above is written against `FileUtils.h` on the axmol dev branch and on 2.x. When upgrading the engine, check those virtual signatures, especially `getContents` and `ResizableBuffer`.
