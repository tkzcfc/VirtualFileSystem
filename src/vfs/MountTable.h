// 本文件：挂载表（决策 #3、#15、#17）。
// 挂载表是不可变快照：查询时拿到 shared_ptr<const MountTable> 后无锁使用；
// mount/unmount 时复制一份、修改、再发布新快照（copy-on-write）。
#pragma once

#include "IBackend.h"
#include <atomic>

NS_VFS_BEGIN

/**
 * 挂载标志。使用时必须带类型名限定，例如 MountFlags::ReadOnly。
 */
enum MountFlags : uint32_t
{
	ReadOnly = 1u << 0,     // 把这一层当作只读，即使后端可写
	Opaque = 1u << 1,       // 遮蔽：挂载点以下比它优先级低的层全部不可见（决策 #2）
	WriteTarget = 1u << 2,  // 默认写入层：在可写候选中优先（决策 #5，类似 PhysicsFS 的 setWriteDir）
};

/**
 * 卸载标志。
 */
enum UnmountFlags : uint32_t
{
	FailIfBusy = 1u << 0,   // 仍有打开的流时返回 BUSY，不卸载（决策 #18 严格模式）
};

/**
 * 挂载选项。
 */
struct MountOptions
{
	int priority = 0;     // 大的优先；相同时后挂载的优先（决策 #3）
	uint32_t flags = 0;   // MountFlags 位组合
};

/**
 * 对外暴露的挂载信息。
 */
struct MountInfo
{
	MountId id = 0;
	std::string mountPoint;
	int priority = 0;
	uint32_t flags = 0;
	std::shared_ptr<IBackend> backend;
};

/**
 * 每个挂载的运行时状态（在快照之间共享）。
 */
struct MountState
{
	std::atomic<int64_t> openStreams{0};   // 通过 VFS 打开、尚未关闭的流数量
};

/**
 * 挂载表中的一项。
 */
struct MountEntry
{
	MountId id = 0;
	std::string point;                    // 规范挂载点
	std::shared_ptr<IBackend> backend;
	int priority = 0;
	uint32_t flags = 0;
	std::shared_ptr<MountState> state;

	/**
	 * 这一层是否可写：没有 ReadOnly 标志，且后端有 Write 能力。
	 */
	bool writable() const
	{
		return !(flags & MountFlags::ReadOnly) && (backend->capabilities() & BackendCaps::Write);
	}
};

/**
 * 路径 P 的层栈中的一层。
 */
struct Layer
{
	const MountEntry* mount = nullptr;
	std::string_view rel;                 // P 相对该挂载点的路径（指向调用方的路径字符串）
};

/**
 * 挂载表快照。发布后不再修改。
 */
class VFS_DLL MountTable
{
public:
	std::vector<MountEntry> mounts;                                // 已按优先级排序：priority 降序，同优先级 id 降序
	std::vector<std::pair<std::string, std::string>> aliases;      // 前缀映射（决策 #30），按前缀长度降序

	/**
	 * 按决策 #3 排序。
	 */
	void sort();

	/**
	 * 收集路径 P 的层栈：所有挂载点是 P 前缀的挂载，按优先级从高到低；
	 * 遇到 Opaque 挂载后，更低的层不再加入（决策 #2）。
	 * @param path 规范路径（out 中的 rel 指向它，调用方必须保证它在使用期间有效）
	 */
	void collectLayers(std::string_view path, std::vector<Layer>& out) const;

	/**
	 * P 是否是合成的虚拟目录：根目录，或某个挂载点的真祖先（决策 #13、#15）。
	 */
	bool isVirtualDir(std::string_view path) const;

	/**
	 * P 是否正好是某个挂载点，或是某个挂载点的祖先（这类路径不能被删除或改名，决策 #15）。
	 */
	bool isMountPointOrAncestor(std::string_view path) const;

	/**
	 * P 是否正好是某个挂载点。
	 */
	bool isMountPoint(std::string_view path) const;

	const MountEntry* find(MountId id) const;

	/**
	 * 应用别名映射：原始路径以某个前缀开头时替换成目标路径（在规范化之前执行）。
	 */
	std::string applyAlias(std::string_view raw) const;
};

NS_VFS_END
