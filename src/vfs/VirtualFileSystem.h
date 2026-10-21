// 本文件：虚拟文件系统核心。负责挂载表、分层（union）查找、写入目标选择、copy-up、mkdir/rmdir/rename 规则。
// 分层规则详见 docs/mount-rules.md。
#pragma once

#include "Common.h"
#include "Error.h"
#include "IBackend.h"
#include "IFileStream.h"
#include "MountTable.h"
#include "native/NativeBackend.h"
#include "memory/MemoryBackend.h"
#include "archive/ArchiveBackend.h"
#include <atomic>
#include <mutex>

NS_VFS_BEGIN

/**
 * 虚拟文件系统。
 *
 * 核心模型：
 * - 单一虚拟命名空间：绝对路径、'/' 分隔、UTF-8、大小写敏感。
 * - 挂载 = (挂载点, 后端, 优先级, 标志)。对路径 P，所有挂载点是 P 前缀的挂载构成 P 的层栈，按优先级从高到低排序。
 * - 读是 union（overlay 风格）；写只落到唯一确定的一层；删除只作用于可写层；运行时不做 whiteout。
 *
 * 线程安全：所有方法都可以被多个线程并发调用。挂载表是 copy-on-write 快照，查询不持锁。
 * 返回的流对象归调用方所有，不能跨线程同时使用。
 */
class VFS_DLL VirtualFileSystem
{
public:

	VirtualFileSystem();

	~VirtualFileSystem();

	VirtualFileSystem(const VirtualFileSystem&) = delete;

	VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

	// ------------------------------------------------------------ 挂载管理

	/**
	 * 挂载后端。
	 * @param mountPoint 虚拟挂载点（会被规范化；可以是合并视图中不存在的路径，祖先会显示为虚拟目录）
	 * @param backend    后端（共享所有权）
	 * @param options    优先级与标志
	 * @param err        可选的错误输出
	 * @return 挂载 ID；失败返回 0
	 */
	MountId mount(std::string_view mountPoint, std::shared_ptr<IBackend> backend, const MountOptions& options = {},
				  Error* err = nullptr);

	/**
	 * 挂载本机目录。
	 */
	MountId mountNative(std::string_view mountPoint, std::string_view nativeDir, const MountOptions& options = {},
						const NativeOptions& nativeOptions = {}, Error* err = nullptr);

	/**
	 * 挂载一个新的空内存后端。
	 * @param backendOut 可选：输出创建的后端，便于 addFile 预填数据
	 */
	MountId mountMemory(std::string_view mountPoint, const MountOptions& options = {},
						std::shared_ptr<MemoryBackend>* backendOut = nullptr, Error* err = nullptr);

	/**
	 * 挂载 .vfs 归档（只读）。
	 */
	MountId mountArchive(std::string_view mountPoint, std::string_view nativePath,
						 const ArchiveOptions& archiveOptions = {}, const MountOptions& options = {},
						 Error* err = nullptr);

	/**
	 * 挂载单个本机文件。
	 */
	MountId mountFile(std::string_view mountPoint, std::string_view nativeFile, const MountOptions& options = {},
					  Error* err = nullptr);

	/**
	 * 卸载。默认惰性卸载：立即从命名空间移除，已打开的流继续可用。
	 * @param flags UnmountFlags::FailIfBusy 时，仍有打开的流则返回 BUSY
	 * @return OK；ID 不存在返回 NOT_FOUND
	 */
	Error unmount(MountId id, uint32_t flags = 0);

	/**
	 * 在一次快照交换内原子地替换某个挂载的后端（热更新）。旧流继续读旧后端。
	 */
	Error remount(MountId id, std::shared_ptr<IBackend> backend);

	/**
	 * 当前所有挂载（按优先级从高到低）。
	 */
	std::vector<MountInfo> mounts() const;

	/**
	 * 添加前缀别名，例如 addAlias("res://", "/res")。
	 */
	Error addAlias(std::string_view prefix, std::string_view target);

	Error removeAlias(std::string_view prefix);

	/**
	 * 变更计数：mount、unmount、remount 或任何写操作时递增，上层可据此让缓存失效。
	 */
	uint64_t generation() const { return m_generation.load(std::memory_order_acquire); }

	// ------------------------------------------------------------ 查询

	/**
	 * 查询合并视图中的可见项。
	 * @return OK / NOT_FOUND / NOT_DIR（某一级被更高层的文件遮蔽）/ INVALID_PATH
	 */
	Error stat(std::string_view path, Stat& out) const;

	bool exists(std::string_view path) const;

	bool isFile(std::string_view path) const;

	bool isDir(std::string_view path) const;

	/**
	 * 文件大小；不存在或不是文件返回 -1。
	 */
	int64_t fileSize(std::string_view path) const;

	/**
	 * 列目录：汇总所有层的子项，按名字去重（高层决定类型），按名字字节序排序。
	 * @param cb 回调，返回 false 停止遍历
	 */
	Error list(std::string_view path, const std::function<bool(const DirEntry&)>& cb) const;

	std::vector<DirEntry> list(std::string_view path, Error* err = nullptr) const;

	/**
	 * 递归遍历（深度优先、每层有序）。回调参数为完整虚拟路径和目录项，返回 false 停止。
	 */
	Error walk(std::string_view path, const std::function<bool(const std::string& fullPath, const DirEntry& entry)>& cb) const;

	/**
	 * 可见项的本机路径；只有本机后端返回非空。
	 */
	std::string realPath(std::string_view path) const;

	// ------------------------------------------------------------ 流

	/**
	 * 打开文件流。按写入目标规则选择落点；父目录只在其他层存在时自动在写入层创建；APPEND/OVERLAPPED 修改只读层文件时先 copy-up。
	 * @param path 虚拟路径
	 * @param mode 打开模式（语义见 IFileStream.h）
	 * @param err  可选的错误输出（同时设置 errno）
	 * @return 成功返回流（所有权交给调用方）；失败返回 nullptr
	 */
	std::unique_ptr<IFileStream> openFileStream(std::string_view path, IFileStream::Mode mode,
												Error* err = nullptr) const;

	// ------------------------------------------------------------ 修改

	/**
	 * 创建单级目录：已存在返回 EXISTS；父目录必须在合并视图中存在。
	 */
	Error createDir(std::string_view path) const;

	/**
	 * mkdir -p：已存在的目录不算错误。
	 */
	Error createDirs(std::string_view path) const;

	/**
	 * 删除目录。只要有其他层（只读层或非写入层）贡献了该目录或其子孙，返回 READ_ONLY 且什么都不删；
	 * 非递归时合并视图非空返回 NOT_EMPTY；挂载点及其祖先返回 BUSY。
	 */
	Error removeDir(std::string_view path, bool recursive = false) const;

	/**
	 * 删除文件：只删可见项；可见项在只读层返回 READ_ONLY；删除后下层同名文件会重新露出。
	 */
	Error removeFile(std::string_view path) const;

	/**
	 * 改名：仅当源和目标落在同一个挂载上时成功；跨挂载返回 CROSS_DEVICE；源在只读层返回 READ_ONLY；
	 * 源目录由多个层合并而成时同样返回 READ_ONLY（与 removeDir 一致）。
	 */
	Error rename(std::string_view from, std::string_view to) const;

	/**
	 * 移动文件：先尝试 rename，跨挂载时退化为 copy + remove（不保证原子性）。
	 */
	Error moveFile(std::string_view from, std::string_view to) const;

	/**
	 * 复制文件（目标按 WRITE 模式打开，会截断）。复制到自身时：可见项可写则什么都不做，
	 * 可见项在只读层则复制到写入层（相当于显式 copy-up）。
	 */
	Error copyFile(std::string_view from, std::string_view to) const;

	// ------------------------------------------------------------ 便利函数

	Error readFile(std::string_view path, std::vector<uint8_t>& out) const;

	Error readFile(std::string_view path, std::string& out) const;

	Error writeFile(std::string_view path, const void* data, size_t size) const;

	Error writeFile(std::string_view path, std::string_view data) const;

	/**
	 * 原子保存：先写同目录下唯一命名的临时文件（path.vfs-N.tmp），再 rename 为 path。
	 * 失败时删除临时文件；并发写同一路径时最终内容是某一次完整写入。
	 */
	Error writeFileAtomic(std::string_view path, const void* data, size_t size) const;

private:

	// 当前挂载表快照
	std::shared_ptr<const MountTable> snapshot() const;

	// 别名 + 规范化
	Error normalize(const MountTable& table, std::string_view raw, std::string& out) const;

	// 在给定快照上解析路径的可见项。visible 为层下标，-1 表示合成的虚拟目录
	Error resolve(const MountTable& table, std::string_view path, std::vector<Layer>& layers, int& visible,
				  Stat& st) const;

	// 选择写入层：优先 WriteTarget 标志，其次最高优先级的可写层；没有返回 -1
	static int chooseWriteTarget(const std::vector<Layer>& layers);

	// 除第 keep 层以外，没有其他层把该路径提供为目录：OK；有则 READ_ONLY；无法确定时返回那一层的错误
	static Error checkSingleLayerDir(const std::vector<Layer>& layers, int keep);

	// 把 in 剩下的内容全部写入 out
	static Error copyStream(IFileStream& in, IFileStream& out);

	// 在写入层中创建相对父目录链
	static Error ensureParents(const Layer& layer, std::string_view relParent);

	// copy-up：把可见层 src 中的文件完整复制到写入层 dst，可能时写临时文件再 rename
	static Error copyUp(const Layer& src, const Layer& dst);

	// openFileStream 的一次尝试；与 FailIfBusy 卸载竞争时由 openFileStream 重试
	std::unique_ptr<IFileStream> openOnce(std::string_view path, IFileStream::Mode mode, Error* err) const;

	// 包装后端返回的流：统计打开数量、保活后端。挂载正在被 FailIfBusy 卸载时返回空并要求重试
	std::unique_ptr<IFileStream> track(const MountEntry& mount, std::unique_ptr<IFileStream> stream) const;

	Error createDirIn(const MountTable& table, const std::string& path) const;

	Error listIn(const MountTable& table, const std::string& path, const std::function<bool(const DirEntry&)>& cb) const;

	Error walkIn(const MountTable& table, const std::string& path,
				 const std::function<bool(const std::string&, const DirEntry&)>& cb, bool& stop) const;

	// 修改一个挂载表：复制、修改、发布
	template <typename Fn>
	Error modifyTable(Fn&& fn);

	void bump() const { m_generation.fetch_add(1, std::memory_order_acq_rel); }

private:
	// 快照分片：每个分片持有当前快照的一个独立引用（各自的控制块）和一把锁，线程按固定分片读取，
	// 多线程查询时引用计数与锁都不在线程之间共享缓存行。定义在 .cpp
	struct SnapshotShard;

	std::mutex m_modifyMutex;                      // 串行化挂载表的修改；m_table 只在它保护下读写
	mutable std::mutex m_copyUpMutex;              // 串行化 copy-up，避免并发追加同一文件时互相覆盖
	std::shared_ptr<const MountTable> m_table;     // 当前快照（发布用的主引用）
	std::unique_ptr<SnapshotShard[]> m_shards;     // 读取用的分片引用
	std::atomic<MountId> m_nextId;                 // 下一个挂载 ID
	mutable std::atomic<uint64_t> m_generation;    // 变更计数
};

NS_VFS_END
