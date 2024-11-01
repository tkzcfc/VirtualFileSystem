#include "VirtualFileSystem.h"
#include <set>

#if 1
#define LOCL_FILE_SYSTEMS_LIST void(0);
#else
#define LOCL_FILE_SYSTEMS_LIST std::lock_guard<std::mutex> lock(m_mutex);
#endif

NS_VFS_BEGIN

VirtualFileSystem::VirtualFileSystem()
{}

VirtualFileSystem::~VirtualFileSystem()
{
	for (auto& it : m_fileSystems)
	{
		delete it;
	}
	m_fileSystems.clear();
}

void VirtualFileSystem::mount(FileSystem* fs)
{
	LOCL_FILE_SYSTEMS_LIST
	m_fileSystems.push_back(fs);
}

void VirtualFileSystem::unmount(const std::string& archiveLocation)
{
	LOCL_FILE_SYSTEMS_LIST
	for (auto it = m_fileSystems.begin(); it != m_fileSystems.end(); ++it)
	{
		if ((*it)->archiveLocation() == archiveLocation)
		{
			delete (*it);
			m_fileSystems.erase(it);
			break;
		}
	}
}

std::unique_ptr<FileStream> VirtualFileSystem::openFileStream(const std::string& path, FileStream::Mode mode)
{
	auto filePath = simplifyPath(convertPathFormatToUnixStyle(path));
	if (filePath.empty() || filePath.back() == '/')
		return nullptr;

	LOCL_FILE_SYSTEMS_LIST

	std::vector<FileSystem*> fileSystems;
	fileSystems.reserve(m_fileSystems.size());

	for (auto& it : m_fileSystems)
	{
		if (filePath.starts_with(it->mntpoint()))
		{
			auto fullFilePath = it->basePath() + filePath.substr(it->mntpoint().size());
			if (it->isFile(fullFilePath))
			{
				if (mode != FileStream::Mode::READ && it->isReadonly())
					return nullptr;

				return it->openFileStream(fullFilePath, mode);
			}

			if (!it->isReadonly())
			{
				fileSystems.push_back(it);
			}
		}
	}

	if (mode == FileStream::Mode::READ)
		return nullptr;

	for (auto it : fileSystems)
	{
		auto fullFilePath = it->basePath() + filePath.substr(it->mntpoint().size());
		auto pFs = it->openFileStream(fullFilePath, mode);
		if (pFs)
		{
			return pFs;
		}
	}

	return nullptr;
}

void VirtualFileSystem::enumerate(const std::string& dir, const std::function<bool(const FileInfo& info)>& call)
{
	auto filePath = simplifyPath(convertDirPath(dir));

	LOCL_FILE_SYSTEMS_LIST
	std::set<std::string> pathSet;

	for (auto& it : m_fileSystems)
	{
		auto& mntpoint = it->mntpoint();

		if (mntpoint.starts_with(filePath))
		{
			if (mntpoint == filePath)
			{
				auto fullFilePath = it->basePath() + mntpoint.substr(filePath.size());
				it->enumerate(fullFilePath, [&call, &pathSet](const FileInfo& info) -> bool {
					if (pathSet.count(info.filePath) == 0)
					{
						pathSet.insert(info.filePath);
						return call(info);
					}
					else
					{
						return false;
					}
				});
			}
			else
			{
				FileInfo info;
				info.filePath = filePath + getFirstPart(mntpoint.substr(filePath.size()));
				info.flags = FileFlags::Dir | FileFlags::Read;
				if (pathSet.count(info.filePath) == 0)
				{
					pathSet.insert(info.filePath);
					if (call(info))
						break;
				}
			}
		}
		else if (filePath.starts_with(mntpoint))
		{
			auto fullFilePath = it->basePath() + filePath.substr(mntpoint.size());
			it->enumerate(fullFilePath, [&call, &pathSet](const FileInfo& info) -> bool {
				if (pathSet.count(info.filePath) == 0)
				{
					pathSet.insert(info.filePath);
					return call(info);
				}
				else
				{
					return false;
				}
			});
		}
	}
}

bool VirtualFileSystem::removeFile(const std::string& path)
{
	auto filePath = simplifyPath(convertPathFormatToUnixStyle(path));

	if (filePath.empty() || filePath.back() == '/')
		return false;

	LOCL_FILE_SYSTEMS_LIST
	for (auto& it : m_fileSystems)
	{
		if (filePath.starts_with(it->mntpoint()))
		{
			auto fullFilePath = it->basePath() + filePath.substr(it->mntpoint().size());
			if (it->isFile(fullFilePath))
			{
				return it->removeFile(fullFilePath);
			}
		}
	}
	return false;
}

bool VirtualFileSystem::isFile(const std::string& path) const
{
	auto filePath = simplifyPath(convertPathFormatToUnixStyle(path));

	if (filePath.empty() || filePath.back() == '/')
		return false;

	LOCL_FILE_SYSTEMS_LIST
	for (auto& it : m_fileSystems)
	{
		if (filePath.starts_with(it->mntpoint()))
		{
			auto fullFilePath = it->basePath() + filePath.substr(it->mntpoint().size());
			if (it->isFile(fullFilePath))
			{
				return true;
			}
		}
	}
	return false;
}

bool VirtualFileSystem::isDir(const std::string& dir) const
{
	auto filePath = simplifyPath(convertDirPath(dir));

	LOCL_FILE_SYSTEMS_LIST

	for (auto& it : m_fileSystems)
	{
		auto& mntpoint = it->mntpoint();

		if (mntpoint.starts_with(filePath))
		{
			if (mntpoint == filePath)
			{
				auto fullFilePath = it->basePath() + mntpoint.substr(filePath.size());
				if (it->isDir(fullFilePath))
					return true;
			}
			else
			{
				return true;
			}
		}
		else if (filePath.starts_with(mntpoint))
		{
			auto fullFilePath = it->basePath() + filePath.substr(mntpoint.size());
			if (it->isDir(fullFilePath))
				return true;
		}
	}
	return false;
}

bool VirtualFileSystem::createDir(const std::string& dir)
{
	auto filePath = simplifyPath(convertDirPath(dir));

	LOCL_FILE_SYSTEMS_LIST

	for (auto& it : m_fileSystems)
	{
		if (!it->isReadonly())
		{
			auto& mntpoint = it->mntpoint();

			if (mntpoint.starts_with(filePath))
			{
				auto fullFilePath = it->basePath() + mntpoint.substr(filePath.size());
				return it->createDir(fullFilePath);
			}
			else if (filePath.starts_with(mntpoint))
			{
				auto fullFilePath = it->basePath() + filePath.substr(mntpoint.size());
				return it->createDir(fullFilePath);
			}
		}
	}
	return false;
}

NS_VFS_END
