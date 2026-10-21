#include "MountTable.h"
#include "Path.h"
#include <algorithm>

NS_VFS_BEGIN

void MountTable::sort()
{
	// Higher priority comes first; when priority is equal, the later mount (larger id) comes first, matching Linux and Godot
	std::stable_sort(mounts.begin(), mounts.end(), [](const MountEntry& a, const MountEntry& b)
	{
		if (a.priority != b.priority)
			return a.priority > b.priority;
		return a.id > b.id;
	});
}

void MountTable::collectLayers(std::string_view path, std::vector<Layer>& out) const
{
	out.clear();
	for (const auto& m : mounts)
	{
		std::string_view rel;
		if (!Path::isUnder(path, m.point, rel))
			continue;
		out.push_back(Layer{&m, rel});
		// Below an Opaque mount, every lower-priority layer is invisible
		if (m.flags & MountFlags::Opaque)
			break;
	}
}

bool MountTable::isVirtualDir(std::string_view path) const
{
	if (path.size() == 1)
		return true;
	for (const auto& m : mounts)
	{
		if (Path::isStrictAncestor(path, m.point))
			return true;
	}
	return false;
}

bool MountTable::isMountPointOrAncestor(std::string_view path) const
{
	if (path.size() == 1)
		return true;
	for (const auto& m : mounts)
	{
		std::string_view rel;
		if (Path::isUnder(m.point, path, rel))
			return true;
	}
	return false;
}

bool MountTable::isMountPoint(std::string_view path) const
{
	for (const auto& m : mounts)
	{
		if (m.point == path)
			return true;
	}
	return false;
}

const MountEntry* MountTable::find(MountId id) const
{
	for (const auto& m : mounts)
	{
		if (m.id == id)
			return &m;
	}
	return nullptr;
}

std::string MountTable::applyAlias(std::string_view raw) const
{
	for (const auto& alias : aliases)
	{
		if (raw.size() >= alias.first.size() && raw.compare(0, alias.first.size(), alias.first) == 0)
		{
			std::string result = alias.second;
			result.push_back('/');
			result.append(raw.substr(alias.first.size()));
			return result;
		}
	}
	return std::string(raw);
}

NS_VFS_END
