#pragma once

#include "FileStream.h"
#include "Utils.h"

NS_VFS_BEGIN

enum FileFlags: uint8_t
{
	// file type
	File = 0x01,
	Dir = 0x02,

	// op permissions
	Read = 0x08,
	Write = 0x04,
};

struct FileInfo
{
	std::string filePath;
	uint8_t flags;
};

enum FileSystemType : uint8_t
{
	Native,
	Memory,
	PackFile
};

class FileSystem
{
public:

	FileSystem(const std::string_view& archiveLocation, const std::string_view& mntpoint)
		: m_archiveLocation(archiveLocation)
		, m_mntpoint(convertDirPath(mntpoint))
		, m_readonly(false)
	{
	}

	virtual ~FileSystem() {}

	virtual void enumerate(const std::string_view& dir, const std::function<bool(const FileInfo& info)>& call) = 0;

	virtual std::unique_ptr<FileStream> openFileStream(const std::string_view& filePath, FileStream::Mode mode) = 0;

    virtual bool removeFile(const std::string_view& filePath) = 0;

    virtual bool isFile(const std::string_view& filePath) const = 0;

    virtual bool isDir(const std::string_view& dirPath) const = 0;

	virtual bool createDir(const std::string_view& dirPath) = 0;

	virtual bool init() = 0;

	virtual const std::string_view& basePath() const = 0;

	const std::string& archiveLocation() { return m_archiveLocation; }

	const std::string& mntpoint() { return m_mntpoint; }

	bool isReadonly() const { return m_readonly; }

	void setReadonly(bool value) { m_readonly = value; }

	FileSystemType getFileSystemType() { return m_fileSystemType; }

protected:
	bool m_readonly;
	FileSystemType m_fileSystemType;
	std::string m_archiveLocation;
	std::string m_mntpoint;
};

NS_VFS_END
