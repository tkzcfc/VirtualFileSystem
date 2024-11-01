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

class FileSystem
{
public:

	FileSystem(const std::string& archiveLocation, const std::string& mntpoint)
		: m_archiveLocation(convertDirPath(archiveLocation))
		, m_mntpoint(convertDirPath(mntpoint))
		, m_readonly(false)
	{
	}

	virtual ~FileSystem() {}

	virtual void enumerate(const std::string& dir, const std::function<bool(const FileInfo& info)>& call) = 0;

	virtual std::unique_ptr<FileStream> openFileStream(const std::string& filePath, FileStream::Mode mode) = 0;

    virtual bool removeFile(const std::string& filePath) = 0;

    virtual bool isFile(const std::string& filePath) const = 0;

    virtual bool isDir(const std::string& dirPath) const = 0;

	virtual bool createDir(const std::string& dirPath) = 0;

	virtual bool init() { return !m_archiveLocation.empty() && !m_mntpoint.empty(); }

	const std::string& archiveLocation() { return m_archiveLocation; }

	const std::string& basePath() { return m_archiveLocation; }

	const std::string& mntpoint() { return m_mntpoint; }

	bool isReadonly() const { return m_readonly; }

	void setReadonly(bool value) { m_readonly = value; }

protected:
	bool m_readonly;
	std::string m_archiveLocation;
	std::string m_mntpoint;
};

NS_VFS_END