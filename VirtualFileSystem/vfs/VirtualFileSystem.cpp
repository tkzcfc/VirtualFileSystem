#include "VirtualFileSystem.h"
#include <set>

#if 1
#define LOCL_FILE_SYSTEMS_LIST void(0)
#else
#define LOCL_FILE_SYSTEMS_LIST std::lock_guard<std::mutex> lock(m_mutex)
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

bool VirtualFileSystem::mount(FileSystem* fs)
{
	LOCL_FILE_SYSTEMS_LIST;
	if (fs->init())
	{
		m_fileSystems.push_back(fs);
		return true;
	}
	delete fs;
	return false;
}

void VirtualFileSystem::unmount(FileSystem* fs)
{
	LOCL_FILE_SYSTEMS_LIST;
	for (auto it = m_fileSystems.begin(); it != m_fileSystems.end(); ++it)
	{
		if (*it == fs)
		{
			delete fs;
			m_fileSystems.erase(it);
			break;
		}
	}
}

std::vector<FileSystem*> VirtualFileSystem::getFileSystems() const
{
	return m_fileSystems;
}

std::unique_ptr<FileStream> VirtualFileSystem::openFileStream(const std::string_view& path, FileStream::Mode mode) const
{
	std::string filePath = simplifyPath(convertPathFormatToUnixStyle(path));
	if (filePath.empty() || filePath.back() == '/')
		return nullptr;

	LOCL_FILE_SYSTEMS_LIST;


	std::vector<FileSystem*> fileSystems;
	fileSystems.reserve(m_fileSystems.size());

	std::string fullFilePath;

	for (auto& it : m_fileSystems)
	{
		if (filePath.starts_with(it->mntpoint()))
		{
			fullFilePath = it->basePath();
			fullFilePath += filePath.substr(it->mntpoint().size());
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
		fullFilePath = it->basePath();
		fullFilePath += filePath.substr(it->mntpoint().size());
		auto pFs = it->openFileStream(fullFilePath, mode);
		if (pFs)
		{
			return pFs;
		}
	}
	return nullptr;
}

void VirtualFileSystem::enumerate(const std::string_view& dir, const std::function<bool(const FileInfo& info)>& call) const
{
	auto filePath = simplifyPath(convertDirPath(dir));

	LOCL_FILE_SYSTEMS_LIST;
	std::set<std::string> pathSet;

	for (auto& it : m_fileSystems)
	{
		auto& mntpoint = it->mntpoint();

		if (mntpoint.starts_with(filePath))
		{
			if (mntpoint == filePath)
			{
				std::string fullFilePath(it->basePath());
				fullFilePath += mntpoint.substr(filePath.size());
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
				info.filePath = filePath + std::string(getFirstPart(mntpoint.substr(filePath.size())));

				if(!it->isReadonly() && info.filePath == mntpoint.substr(0, mntpoint.size() - 1))
					info.flags = FileFlags::Dir | FileFlags::Read | FileFlags::Write;
				else
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
			std::string fullFilePath(it->basePath());
			fullFilePath += filePath.substr(mntpoint.size());
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

bool VirtualFileSystem::removeFile(const std::string_view& path) const
{
	auto filePath = simplifyPath(convertPathFormatToUnixStyle(path));

	if (filePath.empty() || filePath.back() == '/')
		return false;

	LOCL_FILE_SYSTEMS_LIST;
	for (auto& it : m_fileSystems)
	{
		if (filePath.starts_with(it->mntpoint()))
		{
			std::string fullFilePath(it->basePath());
			fullFilePath += filePath.substr(it->mntpoint().size());
			if (it->isFile(fullFilePath))
			{
				return it->removeFile(fullFilePath);
			}
		}
	}
	return false;
}

bool VirtualFileSystem::isFile(const std::string_view& path) const
{
	auto filePath = simplifyPath(convertPathFormatToUnixStyle(path));

	if (filePath.empty() || filePath.back() == '/')
		return false;

	LOCL_FILE_SYSTEMS_LIST;
	for (auto& it : m_fileSystems)
	{
		if (filePath.starts_with(it->mntpoint()))
		{
			std::string fullFilePath(it->basePath());
			fullFilePath += filePath.substr(it->mntpoint().size());
			if (it->isFile(fullFilePath))
			{
				return true;
			}
		}
	}
	return false;
}

bool VirtualFileSystem::isDir(const std::string_view& dir) const
{
	auto dirPath = simplifyPath(convertDirPath(dir));

	LOCL_FILE_SYSTEMS_LIST;

	std::string_view dirPathSv = dirPath;
	for (auto& it : m_fileSystems)
	{
		if (isDir(it, dirPathSv))
			return true;
	}
	return false;
}

bool VirtualFileSystem::isDir(FileSystem* fileSystem, const std::string_view& dirPath) const
{
	auto& mntpoint = fileSystem->mntpoint();

	if (mntpoint.starts_with(dirPath))
	{
		if (mntpoint == dirPath)
		{
			std::string fulldirPath(fileSystem->basePath());
			fulldirPath += mntpoint.substr(dirPath.size());
			if (fileSystem->isDir(fulldirPath))
				return true;
		}
		else
		{
			return true;
		}
	}
	else if (dirPath.starts_with(mntpoint))
	{
		std::string fulldirPath(fileSystem->basePath());
		fulldirPath += dirPath.substr(mntpoint.size());
		if (fileSystem->isDir(fulldirPath))
			return true;
	}

	return false;
}

bool VirtualFileSystem::createDir(const std::string_view& dir) const
{
	auto filePath = simplifyPath(convertDirPath(dir));

	LOCL_FILE_SYSTEMS_LIST;

	for (auto& it : m_fileSystems)
	{
		if (!it->isReadonly())
		{
			auto& mntpoint = it->mntpoint();

			if (mntpoint.starts_with(filePath))
			{
				std::string fullFilePath(it->basePath());
				fullFilePath += mntpoint.substr(filePath.size());
				return it->createDir(fullFilePath);
			}
			else if (filePath.starts_with(mntpoint))
			{
				std::string fullFilePath(it->basePath());
				fullFilePath += filePath.substr(mntpoint.size());
				return it->createDir(fullFilePath);
			}
		}
	}
	return false;
}

bool VirtualFileSystem::copyFile(const std::string_view& srcFile, const std::string_view& dstFile) const
{
	auto srcFs = openFileStream(srcFile, FileStream::Mode::READ);
	if (!srcFs)
		return false;

	auto srcFileDataLen = srcFs->size();
	if (srcFileDataLen == 0)
	{
		return openFileStream(dstFile, FileStream::Mode::WRITE) != nullptr;
	}

	uint8_t* buf = (uint8_t*)malloc(srcFileDataLen);
	if (buf == nullptr)
		return false;

	if (srcFileDataLen != srcFs->read(buf, srcFileDataLen))
	{
		free(buf);
		return false;
	}

	auto dstFs = openFileStream(dstFile, FileStream::Mode::WRITE);
	if (dstFs == nullptr)
	{
		free(buf);
		return false;
	}

	if(srcFileDataLen != dstFs->write(buf, srcFileDataLen))
	{
		free(buf);
		return false;
	}

	free(buf);
	return true;
}

NS_VFS_END
