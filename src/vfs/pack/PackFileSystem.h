#pragma once

#include "../FileSystem.h"
#include <mutex>

NS_VFS_BEGIN

enum PackFileCompressionType
{
    None,
    Gzip,
    Unknown
};

struct PackFileInfo
{
    uint64_t offset;
    uint32_t length;// file max size 4GB
    uint8_t compressionType;
};

class PackFileSystem : public FileSystem
{
public:

    PackFileSystem(const std::string& archiveLocation, const std::string& mntpoint);

    virtual ~PackFileSystem();

    virtual bool init() override;

    virtual void enumerate(const std::string& dir, const std::function<bool(const FileInfo&)>& call) override;

    virtual std::unique_ptr<FileStream> openFileStream(const std::string& filePath, FileStream::Mode mode) override;

    virtual bool removeFile(const std::string& filePath) override;

    virtual bool isFile(const std::string& filePath) const override;

    virtual bool isDir(const std::string& dirPath) const override;

    virtual bool createDir(const std::string& dirPath) override;

    bool isReadonly() const { return true; }

    virtual const std::string& basePath() const override;

private:
    std::unordered_map<std::string, PackFileInfo> m_packFiles;
    uint32_t m_dataSecret;
};

NS_VFS_END
