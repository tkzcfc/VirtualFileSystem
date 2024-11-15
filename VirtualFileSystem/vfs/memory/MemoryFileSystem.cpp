#include "MemoryFileSystem.h"
#include "MemoryFileStream.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

NS_VFS_BEGIN

MemoryFileSystem::MemoryFileSystem(const std::string_view& archiveLocation, const std::string_view& mntpoint)
	: FileSystem(archiveLocation, mntpoint)
{
	m_fileSystemType = FileSystemType::Memory;
	m_dirs.insert("/");
}

MemoryFileSystem::~MemoryFileSystem()
{}

bool MemoryFileSystem::init()
{
	return true;
}

void MemoryFileSystem::enumerate(const std::string_view& dir, const std::function<bool(const FileInfo&)>& call)
{
	uint8_t defaultFlgs = FileFlags::Read;
	if (!isReadonly())
		defaultFlgs |= FileFlags::Write;

	FileInfo info;
	{
		std::lock_guard<std::mutex> lock(m_dirMutex);
		if (m_dirs.count(std::string(dir)) == 0)
			return;

		for (auto& it : m_dirs)
		{
			if (it.size() > dir.size())
			{
				auto p1 = it.substr(dir.size());
				if (p1.find('/') == p1.size() - 1)
				{
					info.flags = defaultFlgs | FileFlags::Dir;
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
					info.flags = defaultFlgs | FileFlags::File;
					info.filePath = m_mntpoint + p1;
					if (call(info))
						break;
				}
			}
		}
	}
}

std::unique_ptr<FileStream> MemoryFileSystem::openFileStream(const std::string_view& filePath, FileStream::Mode mode)
{
	std::lock_guard<std::mutex> lock(m_fileMutex);

	auto it = m_files.find(std::string(filePath));
	if (it != m_files.end())
	{
		auto fs = std::make_unique<MemoryFileStream>();
		return fs->open(it->second, mode) ? std::move(fs) : nullptr;
	}

	if (mode == FileStream::Mode::READ)
		return nullptr;

	
	auto dirName(getFileDir(filePath));
	if (!isDir(dirName))
	{
		return nullptr;
	}

	auto data = std::make_shared<MemoryData>();
	m_files.insert(std::make_pair(filePath, data));

	auto fs = std::make_unique<MemoryFileStream>();
	return fs->open(data, mode) ? std::move(fs) : nullptr;
}

bool MemoryFileSystem::removeFile(const std::string_view& filePath)
{
	std::lock_guard<std::mutex> lock(m_fileMutex);

	auto it = m_files.find(std::string(filePath));
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

bool MemoryFileSystem::isFile(const std::string_view& filePath) const
{
	std::lock_guard<std::mutex> lock(m_fileMutex);
	return m_files.find(std::string(filePath)) != m_files.end();
}

bool MemoryFileSystem::isDir(const std::string_view& dirPath) const
{
	std::lock_guard<std::mutex> lock(m_dirMutex);
	return m_dirs.count(std::string(dirPath)) > 0;
}

bool MemoryFileSystem::createDir(const std::string_view& dirPath)
{
	std::lock_guard<std::mutex> lock(m_dirMutex);

	if (m_dirs.count(std::string(dirPath)) > 0)
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

				if (m_dirs.count(path) == 0)
				{
					m_dirs.insert(path);
				}
			}
		}
	}

	return true;
}

const std::string_view& MemoryFileSystem::basePath() const
{
	static const std::string_view basePath("/");
	return basePath;
}

NS_VFS_END
