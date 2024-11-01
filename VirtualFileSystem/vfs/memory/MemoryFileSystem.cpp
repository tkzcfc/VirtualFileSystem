#include "MemoryFileSystem.h"
#include "MemoryFileStream.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

NS_VFS_BEGIN

MemoryFileSystem::MemoryFileSystem(const std::string& archiveLocation, const std::string& mntpoint)
	: FileSystem("/", mntpoint)
{
	m_dirs.insert("/");
}

MemoryFileSystem::~MemoryFileSystem()
{}

void MemoryFileSystem::enumerate(const std::string& dir, const std::function<bool(const FileInfo&)>& call)
{
	FileInfo info;
	{
		std::lock_guard<std::mutex> lock(m_dirMutex);
		if (m_dirs.count(dir) == 0)
			return;

		for (auto& it : m_dirs)
		{
			if (it.size() > dir.size())
			{
				auto p1 = it.substr(dir.size());
				if (p1.find('/') == p1.size() - 1)
				{
					info.isDir = true;
					info.filePath = m_mntpoint + p1.substr(0, p1.size() - 1);
					if (call(info))
						break;
				}
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(m_fileMutex);
		for (auto& it : m_files)
		{
			if (it.first.starts_with(dir))
			{
				auto p1 = it.first.substr(dir.size());
				if (!p1.empty() && p1.find("/") == std::string::npos)
				{
					info.isDir = true;
					info.filePath = m_mntpoint + "/" + p1;
					if (call(info))
						break;
				}
			}
		}
	}
}

std::unique_ptr<FileStream> MemoryFileSystem::openFileStream(const std::string& filePath, FileStream::Mode mode)
{ 
	std::lock_guard<std::mutex> lock(m_fileMutex);

	auto it = m_files.find(filePath);
	if (it != m_files.end())
	{
		MemoryFileStream fs;
		return fs.open(it->second, mode) ? std::make_unique<MemoryFileStream>(std::move(fs)) : nullptr;
	}

	if (mode == FileStream::Mode::READ)
		return nullptr;

	auto dirName = getFileDir(filePath) + "/";
	if (!isDir(dirName))
	{
		return nullptr;
	}

	auto data = std::make_shared<MemoryData>();
	m_files.insert(std::make_pair(filePath, data));

	MemoryFileStream fs;
	return fs.open(data, mode) ? std::make_unique<MemoryFileStream>(std::move(fs)) : nullptr;
}

bool MemoryFileSystem::removeFile(const std::string& filePath)
{
	std::lock_guard<std::mutex> lock(m_fileMutex);
	auto it = m_files.find(filePath);
	if (it == m_files.end())
		return false;

	// unused
	if (it->second.use_count() == 1)
	{
		m_files.erase(it);
		return true;
	}
	// in use
	return false;
}

bool MemoryFileSystem::isFile(const std::string& filePath) const
{
	std::lock_guard<std::mutex> lock(m_fileMutex);
	return m_files.find(filePath) != m_files.end();
}

bool MemoryFileSystem::isDir(const std::string& dirPath) const
{
	std::lock_guard<std::mutex> lock(m_dirMutex);
	return m_dirs.count(dirPath) > 0;
}

bool MemoryFileSystem::createDir(const std::string& dirPath)
{
	std::lock_guard<std::mutex> lock(m_dirMutex);

	if (m_dirs.count(dirPath) > 0)
		return false;

	std::string path = "/";
	for (auto& part : splitString(dirPath, "/"))
	{
		if (!part.empty())
		{
			//if (part == "." || part == "..")
			//{
			//	return false;
			//}
			//else
			{
				path += part;
				path += "/";

				if (m_dirs.count(dirPath) == 0)
				{
					m_dirs.insert(path);
				}
			}
		}
	}

	return true;
}

NS_VFS_END
