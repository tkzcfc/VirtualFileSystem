// 本文件：只读“资源包”后端（AssetBackend）及其数据来源接口 IAssetProvider。
// Android 上由 AndroidAssetBackend 提供基于 AAssetManager 的实现（APK 内的 assets/）；
// 接口与平台无关，桌面上可以用模拟的 IAssetProvider 测试全部逻辑。
#pragma once

#include "../IBackend.h"
#include "../archive/ArchiveSource.h"
#include <unordered_map>
#include <unordered_set>

NS_VFS_BEGIN

/**
 * 一个打开的资源（语义与 AAsset 一致）。单个对象只在一个线程中使用，不需要线程安全。
 */
class VFS_DLL IAssetFile
{
public:
	virtual ~IAssetFile() = default;

	/// 从当前位置读取，返回读到的字节数，0 表示 EOF，< 0 表示出错
	virtual int read(void* buf, size_t size) = 0;

	/// 移动当前位置（whence 为 SEEK_SET/SEEK_CUR/SEEK_END），返回新位置，失败返回 -1
	virtual int64_t seek(int64_t offset, int whence) = 0;

	/// 资源总长度
	virtual int64_t length() const = 0;

	/**
	 * 资源未压缩存放时，返回一个可以按偏移读取的文件描述符（所有权交给调用方，需 close），
	 * 并输出资源在其中的起点与长度；压缩存放或不支持时返回 -1。
	 * 只在 POSIX 平台上使用（Windows 上的实现应返回 -1）。
	 */
	virtual int openFileDescriptor(int64_t& start, int64_t& length) = 0;
};

/**
 * 资源来源。所有方法都可能被多个线程并发调用，实现必须线程安全（AAssetManager 本身是线程安全的）。
 * 路径是相对资源根目录的规范路径，不带前导 '/'，空串表示根目录。
 */
class VFS_DLL IAssetProvider
{
public:
	virtual ~IAssetProvider() = default;

	/// 打开文件；不存在（或是目录）返回 nullptr
	virtual std::unique_ptr<IAssetFile> open(std::string_view path) = 0;

	/**
	 * 列出目录中的**文件**名（与 AAssetDir 一致：不包含子目录）。
	 * @return 目录中有文件时返回 true；目录不存在、为空或只含子目录时返回 false
	 */
	virtual bool listFiles(std::string_view dir, std::vector<std::string>& names) = 0;
};

/**
 * 资源后端选项。
 */
struct AssetOptions
{
	/**
	 * 资源内的子目录作为后端根目录，例如 "res" 表示只挂载 assets/res/。空串表示整个资源目录。
	 */
	std::string root;

	/**
	 * 可选的清单文件（相对 root 的资源路径，例如 "assets.lst"）：每行一个文件路径（相对 root，'/' 分隔，
	 * 可以带 "assets/" 之外的任意层级）。存在时目录结构完全来自清单，stat/list 是精确的，
	 * 包括只含子目录的目录；不存在或为空时退化为探测模式（见 AssetBackend 说明）。
	 */
	std::string manifest;
};

/**
 * 只读资源后端。
 *
 * 两种目录模式：
 * - 清单模式（AssetOptions::manifest 指向的清单存在）：挂载时读入清单建立目录树，stat/list 精确；
 * - 探测模式：文件通过 open 判断；目录通过“列出其中的文件是否非空”判断。AAssetManager 不能列出子目录，
 *   所以只含子目录的目录 stat 为 NOT_FOUND、list 也看不到子目录——需要完整目录结构时请使用清单。
 *
 * 读取：每个流持有自己的 IAssetFile，互不影响，可以多线程并发打开和读取同一资源。
 * POSIX 平台上，未压缩存放的资源改用 openFileDescriptor 得到的描述符按偏移读取（pread），
 * 不经过 AAsset 的游标；压缩存放的资源用 IAssetFile::read/seek。
 * 线程安全：后端本身只有不可变的清单索引，可以无限并发。
 */
class VFS_DLL AssetBackend : public IBackend, public std::enable_shared_from_this<AssetBackend>
{
public:

	/**
	 * @param provider 资源来源（不能为空）
	 * @param options  选项
	 * @param err      可选的错误输出：provider 为空返回 UNSUPPORTED；root 不合法返回 INVALID_PATH；清单含非法路径返回 CORRUPTED
	 * @return 失败返回 nullptr
	 */
	static std::shared_ptr<AssetBackend> create(std::shared_ptr<IAssetProvider> provider, const AssetOptions& options = {},
												Error* err = nullptr);

	AssetBackend(std::shared_ptr<IAssetProvider> provider, std::string root);

	virtual ~AssetBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual const char* typeName() const override { return "asset"; }

	/// 是否使用清单模式
	bool hasManifest() const { return m_hasManifest; }

	/**
	 * 把资源中的一个 .vfs 归档包装成归档来源，用于 ArchiveBackend::create（例如 APK 内的 assets/game.vfs）。
	 * POSIX 平台上资源未压缩存放时（Gradle：androidResources { noCompress += "vfs" }），来源持有一个描述符做位置读，
	 * 打开归档内的文件不需要再打开资源；否则每个流打开一次资源（压缩存放的资源随机读很慢，不推荐）。
	 * @return 资源不存在或是目录时返回 nullptr，err 为 NOT_FOUND / IS_DIR 等；后端不是由 shared_ptr 持有时返回 UNSUPPORTED
	 */
	std::shared_ptr<IArchiveSource> archiveSource(std::string_view rel, Error* err = nullptr);

private:
	// 后端相对路径 → 资源路径（加上 root 前缀）
	std::string assetPath(std::string_view rel) const;

	// 路径不存在时区分 NOT_FOUND 与 NOT_DIR（某一级父路径是文件）
	Error classifyMissing(std::string_view rel) const;

	// 探测模式：目录是否存在（根目录总是存在）
	bool probeDir(std::string_view rel) const;

	// 读入并解析清单
	Error loadManifest(std::string_view manifestPath);

	std::shared_ptr<IAssetProvider> m_provider;
	std::string m_root;   // 空串，或以 '/' 结尾的资源子目录
	bool m_hasManifest = false;
	std::unordered_set<std::string> m_files;                                  // 清单模式：所有文件
	std::unordered_map<std::string, std::vector<DirEntry>> m_dirs;            // 清单模式：目录 → 子项
};

NS_VFS_END
