#pragma once

#include "../FileSystem.h"
#include "MemoryData.h"
#include <unordered_map>
#include <set>

NS_VFS_BEGIN

class MemoryFileSystem : public FileSystem
{
public:

    MemoryFileSystem(const std::string_view& archiveLocation, const std::string_view& mntpoint);

    virtual ~MemoryFileSystem();

    virtual bool init() override;

    virtual void enumerate(const std::string_view& dir, const std::function<bool(const FileInfo&)>& call) override;

    virtual std::unique_ptr<FileStream> openFileStream(const std::string_view& filePath, FileStream::Mode mode) override;

    virtual bool removeFile(const std::string_view& filePath) override;

    virtual bool isFile(const std::string_view& filePath) const override;

    virtual bool isDir(const std::string_view& dirPath) const override;

    virtual bool createDir(const std::string_view& dirPath) override;

    virtual const std::string_view& basePath() const override;

protected:
    mutable std::mutex m_dirMutex;
    std::set<std::string> m_dirs;

    mutable std::mutex m_fileMutex;
    std::unordered_map<std::string, std::shared_ptr<MemoryData>> m_files;
};

NS_VFS_END
