// This file: the mount table.
// The mount table is an immutable snapshot: a query takes shared_ptr<const MountTable> and uses it without a lock;
// mount/unmount copies it, edits the copy, and publishes a new snapshot (copy-on-write).
#pragma once

#include "IBackend.h"
#include <atomic>

NS_VFS_BEGIN

/**
 * Mount flags. Always qualify with the type name, for example MountFlags::ReadOnly.
 */
enum MountFlags : uint32_t
{
	ReadOnly = 1u << 0,     // treat this layer as read-only even if the backend can write
	Opaque = 1u << 1,       // masking: every lower-priority layer below this mount point is invisible
	WriteTarget = 1u << 2,  // default write layer: preferred among writable candidates
};

/**
 * Unmount flags.
 */
enum UnmountFlags : uint32_t
{
	FailIfBusy = 1u << 0,   // if streams are still open, return BUSY and do not unmount
};

/**
 * Mount options.
 */
struct MountOptions
{
	int priority = 0;     // larger wins; on a tie, the later mount wins
	uint32_t flags = 0;   // MountFlags bit combination
};

/**
 * Mount information exposed to callers.
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
 * Per-mount runtime state (shared across snapshots).
 */
struct MountState
{
	std::atomic<int64_t> openStreams{0};   // number of streams opened through the VFS and not yet closed (shared across remount)
	std::atomic<bool> closing{false};      // a FailIfBusy unmount is checking: new opens give up and retry, avoiding a race between the check and the count
};

/**
 * One entry in the mount table.
 */
struct MountEntry
{
	MountId id = 0;
	std::string point;                    // canonical mount point
	std::shared_ptr<IBackend> backend;
	int priority = 0;
	uint32_t flags = 0;
	std::shared_ptr<MountState> state;

	/**
	 * Whether this layer is writable: no ReadOnly flag, and the backend has the Write capability.
	 */
	bool writable() const
	{
		return !(flags & MountFlags::ReadOnly) && (backend->capabilities() & BackendCaps::Write);
	}
};

/**
 * One layer in the layer stack of path P.
 */
struct Layer
{
	const MountEntry* mount = nullptr;
	std::string_view rel;                 // path of P relative to this mount point (points into the caller's path string)
};

/**
 * Mount-table snapshot. Not modified after it is published.
 */
class VFS_DLL MountTable
{
public:
	std::vector<MountEntry> mounts;                                // sorted by priority: priority descending, then id descending
	std::vector<std::pair<std::string, std::string>> aliases;      // prefix mappings, sorted by prefix length descending

	/**
	 * Sort: higher priority first; on a tie, the later mount (larger id) first.
	 */
	void sort();

	/**
	 * Collect the layer stack of path P: every mount whose mount point is a prefix of P, highest priority first;
	 * after an Opaque mount, lower layers are not added.
	 * @param path canonical path (rel in out points into it; the caller must keep it alive while it is used)
	 */
	void collectLayers(std::string_view path, std::vector<Layer>& out) const;

	/**
	 * Whether P is a synthetic virtual directory: the root, or a true ancestor of some mount point.
	 */
	bool isVirtualDir(std::string_view path) const;

	/**
	 * Whether P is exactly some mount point, or an ancestor of some mount point (such paths cannot be removed or renamed).
	 */
	bool isMountPointOrAncestor(std::string_view path) const;

	/**
	 * Whether P is exactly some mount point.
	 */
	bool isMountPoint(std::string_view path) const;

	const MountEntry* find(MountId id) const;

	/**
	 * Apply alias mappings: if the raw path starts with a prefix, replace it with the target path (done before normalization).
	 */
	std::string applyAlias(std::string_view raw) const;
};

NS_VFS_END
