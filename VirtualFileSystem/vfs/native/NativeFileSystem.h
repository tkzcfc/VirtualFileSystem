#pragma once

#include "../FileSystem.h"

NS_VFS_BEGIN

class NativeFileSystem : public FileSystem
{
public:

    NativeFileSystem(const std::string_view& archiveLocation, const std::string_view& mntpoint);

    virtual ~NativeFileSystem();

    virtual bool init() override;

    virtual void enumerate(const std::string_view& dir, const std::function<bool(const FileInfo&)>& call) override;

    virtual std::unique_ptr<FileStream> openFileStream(const std::string_view& filePath, FileStream::Mode mode) override;

    virtual bool removeFile(const std::string_view& filePath) override;

    virtual bool isFile(const std::string_view& filePath) const override;

    virtual bool isDir(const std::string_view& dirPath) const override;

    virtual bool createDir(const std::string_view& dirPath) override;

    virtual const std::string_view& basePath() const override;
};

NS_VFS_END
