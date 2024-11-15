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

	void unmount(FileSystem* fs);

	std::vector<FileSystem*> getFileSystems() const;

	std::unique_ptr<FileStream> openFileStream(const std::string& filePath, FileStream::Mode mode) const;

	void enumerate(const std::string& dir, const std::function<bool(const FileInfo& info)>& call) const;

	bool removeFile(const std::string& filePath) const;

	bool isFile(const std::string& filePath) const;

	bool isDir(const std::string& dirPath) const;

	bool createDir(const std::string& dirPath) const;

	bool copyFile(const std::string& srcFile, const std::string& dstFile) const;

private:

	bool isDir(FileSystem* fileSystem, const std::string& dirPath) const;

private:

	std::vector<FileSystem*> m_fileSystems;
	std::mutex m_mutex;
};

NS_VFS_END
