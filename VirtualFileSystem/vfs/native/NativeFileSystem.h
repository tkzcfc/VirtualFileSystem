#pragma once

#include "../FileSystem.h"

NS_VFS_BEGIN

class NativeFileSystem : public FileSystem
{
public:

    NativeFileSystem(const std::string& archiveLocation, const std::string& mntpoint);

    virtual ~NativeFileSystem();

    virtual void enumerate(const std::string& dir, const std::function<bool(const FileInfo&)>& call) override;

    virtual std::unique_ptr<FileStream> openFileStream(const std::string& filePath, FileStream::Mode mode) override;

    virtual bool removeFile(const std::string& filePath) override;

    virtual bool isFile(const std::string& filePath) const override;

    virtual bool isDir(const std::string& dirPath) const override;

    virtual bool createDir(const std::string& dirPath) override;

protected:
};

NS_VFS_END
