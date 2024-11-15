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

    PackFileSystem(const std::string_view& archiveLocation, const std::string_view& mntpoint);

    virtual ~PackFileSystem();

    virtual bool init() override;

    virtual void enumerate(const std::string_view& dir, const std::function<bool(const FileInfo&)>& call) override;

    virtual std::unique_ptr<FileStream> openFileStream(const std::string_view& filePath, FileStream::Mode mode) override;

    virtual bool removeFile(const std::string_view& filePath) override;

    virtual bool isFile(const std::string_view& filePath) const override;

    virtual bool isDir(const std::string_view& dirPath) const override;

    virtual bool createDir(const std::string_view& dirPath) override;

    bool isReadonly() const { return true; }

    virtual const std::string_view& basePath() const override;

private:
    std::unordered_map<std::string, PackFileInfo> m_packFiles;
    uint32_t m_dataSecret;
};

NS_VFS_END
