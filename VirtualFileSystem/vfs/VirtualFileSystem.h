#pragma once

#include "Common.h"
#include "FileSystem.h"
#include <mutex>

NS_VFS_BEGIN

class VirtualFileSystem
{
public:

	VirtualFileSystem();

	~VirtualFileSystem();

	bool mount(FileSystem* fs);

	void unmount(const std::string& archiveLocation);

	std::unique_ptr<FileStream> openFileStream(const std::string& filePath, FileStream::Mode mode);

	void enumerate(const std::string& dir, const std::function<bool(const FileInfo& info)>& call);

	bool removeFile(const std::string& filePath);

	bool isFile(const std::string& filePath) const;

	bool isDir(const std::string& dirPath) const;

	bool createDir(const std::string& dirPath);

private:

	std::vector<FileSystem*> m_fileSystems;
	std::mutex m_mutex;
};

NS_VFS_END
