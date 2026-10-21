# 接入 axmol

VFS 的流接口与 axmol 引擎的 `IFileStream` 逐字一致。打开 `VFS_WITH_AXMOL` 后，`vfs::IFileStream` 直接就是 `ax::IFileStream`，VFS 返回的流可以原样交给引擎；再写一个 `FileUtils` 子类把引擎的文件访问转发到 VFS，游戏代码和引擎模块就都能透明地读取归档和分层目录。

## 1. 编译

```cmake
set(VFS_WITH_AXMOL ON CACHE BOOL "" FORCE)
set(VFS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(VFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(VirtualFileSystem)

# vfs 需要能找到 axmol 的头文件（以及 AX_DLL 等宏），链接引擎核心库目标即可
target_link_libraries(vfs PUBLIC axmol)        # 目标名以所用引擎版本的 CMake 工程为准
target_link_libraries(${APP_NAME} PRIVATE vfs)
```

`VFS_WITH_AXMOL=1` 作为 PUBLIC 宏传给所有使用 `vfs` 的目标。`src/vfs/IFileStream.h` 按以下顺序查找引擎头文件：

| axmol 版本 | 头文件位置 | include 写法 |
|---|---|---|
| dev 分支（3.x） | `axmol/platform/IFileStream.h` | `"axmol/platform/IFileStream.h"` |
| 2.x | `core/platform/IFileStream.h` | `"platform/IFileStream.h"`（`core/` 在 include 路径中） |

前者存在时优先使用（`__has_include`），两个版本的 `IFileStream` 接口相同，VFS 代码无需任何改动。

## 2. FileUtils 子类

axmol 通过 `FileUtils::getInstance()` 访问文件，`FileUtils::setDelegate()` 可以替换这个实例。需要转发的虚函数：

| FileUtils 虚函数 | 转发到 | 说明 |
|---|---|---|
| `getContents` | `openFileStream` + `read` | **必须重写**：引擎默认实现直接构造本机 `FileStream`，不经过 `openFileStream` |
| `openFileStream` | `openFileStream` | 引擎的写文件函数（`writeStringToFile`、`writeDataToFile` 等）都通过它写入 |
| `isFileExistInternal` | `isFile` | 纯虚函数；搜索路径解析依赖它 |
| `isDirectoryExistInternal` | `isDir` | |
| `getFileSize` | `fileSize` | |
| `listFiles` / `listFilesRecursively` | `list` / `walk` | 默认实现用 `std::filesystem` 遍历本机目录 |
| `createDirectories` / `removeDirectory` / `removeFile` / `renameFile` | `createDirs` / `removeDir` / `removeFile` / `moveFile` | |
| `getWritablePath` / `getNativeWritableAbsolutePath` | — | 纯虚函数：前者返回虚拟可写目录，后者返回真实的本机目录 |

虚拟路径以 `/` 开头，axmol 的 `isAbsolutePath` 在所有平台上都把它视为绝对路径（Windows 上视为“当前盘符的绝对路径”），所以 `fullPathForFilename("/res/a.png")` 原样返回，不需要重写 `isAbsolutePath`。把 `/res/` 设为默认资源根目录后，相对路径 `"a.png"` 会经搜索路径解析成 `/res/a.png`。

下面以 dev 分支为例（2.x 的差异见代码后的说明）：

```cpp
// VfsFileUtils.h
#pragma once

#include "axmol/platform/FileUtils.h"
#include "vfs/VirtualFileSystem.h"

class VfsFileUtils : public ax::FileUtils
{
public:
	/**
	 * 创建并接管 FileUtils::getInstance()。
	 * @param fs             已经挂载好的 VFS，需要在整个程序生命周期内有效
	 * @param nativeWritable 真实的本机可写目录（供绕过 FileUtils 的模块使用）
	 */
	static VfsFileUtils* install(std::shared_ptr<vfs::VirtualFileSystem> fs, std::string nativeWritable)
	{
		auto* self = new VfsFileUtils(std::move(fs), std::move(nativeWritable));
		self->_defaultResRootPath = "/res/";
		self->_writablePath = "/user/";
		self->init();
		ax::FileUtils::setDelegate(self);   // 会删除之前的实例
		return self;
	}

	vfs::VirtualFileSystem& vfs() const { return *m_fs; }

	// ---------------------------------------------------------------- 读
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

	// 与引擎默认实现一致：返回完整路径，目录以 '/' 结尾
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

	// ---------------------------------------------------------------- 写
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

	// ---------------------------------------------------------------- 可写目录
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

启动时（例如 `AppDelegate::applicationDidFinishLaunching` 中，在加载任何资源之前）：

```cpp
auto fs = std::make_shared<vfs::VirtualFileSystem>();
const std::string nativeRes = ax::FileUtils::getInstance()->getDefaultResourceRootPath();
const std::string nativeUser = ax::FileUtils::getInstance()->getNativeWritableAbsolutePath();

fs->mountArchive("/res", nativeRes + "game.vfs");
#if defined(_DEBUG)
fs->mountNative("/res", "D:/project/assets", vfs::MountOptions{10});   // 开发时本机目录覆盖归档
#endif
vfs::NativeOptions userOptions;
userOptions.createRoot = true;
fs->mountNative("/user", nativeUser, vfs::MountOptions{0, vfs::MountFlags::WriteTarget}, userOptions);

VfsFileUtils::install(fs, nativeUser);
```

**2.x 的差异**：头文件为 `"platform/FileUtils.h"`；`ResizableBuffer` 的接口是 `resize(size_t)` / `buffer()` / `size()`，`getContents` 中改为先 `buffer->resize(size)`，再循环 `read` 到 `buffer->buffer()`，读取不足时 `resize` 到实际长度并返回 `Status::ReadFailed`。其余虚函数签名与 dev 分支相同。

## 3. 注意事项

- **在 Android 上**，安装包内的资源不在本机文件系统中，`mountNative` 无法挂载 APK 内的目录，改用 `AndroidAssetBackend`（见 [平台支持](platforms.md#apk-内的资源androidassetbackend)），直接复用引擎已经设置好的 `AAssetManager`：

  ```cpp
  #if VFS_PLATFORM_ANDROID
  #include "vfs/android/AndroidAssetBackend.h"
  vfs::AndroidAssetBackend::setAssetManager(ax::FileUtilsAndroid::getAssetManager());
  auto assets = vfs::AndroidAssetBackend::create();
  // 引擎的资源根目录在 Android 上是 "assets/"，nativeRes + "game.vfs" 不是文件系统路径，改为：
  fs->mount("/res", vfs::ArchiveBackend::create(assets->archiveSource("game.vfs")));
  #endif
  ```

  `.vfs` 在 APK 中要保持未压缩（Gradle：`androidResources { noCompress += "vfs" }`），这样归档内的文件按偏移直接读取。
  可写目录（`getNativeWritableAbsolutePath()`）是普通文件系统路径，照常用 `mountNative`。
- **绕过 FileUtils 的模块**（例如某些平台的音频解码、视频播放、按本机路径打开文件的第三方库）看不到 VFS 中的文件。对这类资源，要么在 VFS 外保留本机副本，要么通过 `realPath()` 取得本机路径（只对本机后端上的文件有效）。`getNativeWritableAbsolutePath()` 返回真实目录，就是为这类模块准备的。
- **全路径缓存**：`FileUtils` 会缓存 `fullPathForFilename` 的结果。挂载、卸载或 `remount` 之后调用 `FileUtils::purgeCachedEntries()`；可以用 `VirtualFileSystem::generation()` 判断是否发生了变化。
- **线程**：VFS 的所有接口线程安全，单个流对象只能在一个线程中使用。`FileUtils` 自身的搜索路径缓存不是线程安全的，后台线程加载资源时应传入已经解析好的完整路径（与引擎的一般约定一致）。
- **路径大小写**：VFS 区分大小写。Windows / macOS 上开发时可以给本机层打开 `NativeOptions::strictCase`，尽早发现与归档中的大小写不一致。
- **版本差异**：上面的代码按 axmol dev 分支与 2.x 的 `FileUtils.h` 编写。升级引擎时核对这些虚函数的签名，尤其是 `getContents` 与 `ResizableBuffer`。
