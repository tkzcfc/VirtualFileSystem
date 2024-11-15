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

	std::unique_ptr<FileStream> openFileStream(const std::string_view& filePath, FileStream::Mode mode) const;

	void enumerate(const std::string_view& dir, const std::function<bool(const FileInfo& info)>& call) const;

	bool removeFile(const std::string_view& filePath) const;

	bool isFile(const std::string_view& filePath) const;

	bool isDir(const std::string_view& dirPath) const;

	bool createDir(const std::string_view& dirPath) const;

	bool copyFile(const std::string_view& srcFile, const std::string_view& dstFile) const;

private:

	bool isDir(FileSystem* fileSystem, const std::string_view& dirPath) const;

private:

	std::vector<FileSystem*> m_fileSystems;
	std::mutex m_mutex;
};

NS_VFS_END
