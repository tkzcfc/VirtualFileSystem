#include "PackFileSystem.h"
#include "PackUtils.h"
#include <fstream>
#include <set>
#include "../native/NativeFileStream.h"
#include "PackFileStream.h"

NS_VFS_BEGIN

static const char signature[] = { 'P', 'A', 'C', 'K' };

PackFileSystem::PackFileSystem(const std::string& archiveLocation, const std::string& mntpoint)
	: FileSystem(archiveLocation, mntpoint)
    , m_dataSecret(0)
{
    m_fileSystemType = FileSystemType::PackFile;
    setReadonly(true);
}

PackFileSystem::~PackFileSystem()
{}

bool PackFileSystem::init()
{
    if (m_archiveLocation.empty() || m_mntpoint.empty())
        return false;

    NativeFileStream fs;
    fs.open(m_archiveLocation, FileStream::Mode::READ);

    if (!fs.isOpen())
        return false;

    auto fileSize = static_cast<uint64_t>(fs.size());

    // 头部信息读取
    const std::streamsize header_length = 28;
    char headBuffer[header_length];

    fs.seek(0, FileStream::SeekOrigin::SET);
    if (fs.read(headBuffer, header_length) != header_length)
    {
        printf("mount error: failed to read head data");
        return false;
    }

    // 头部信息校验
    if (fileSize < header_length || memcmp(headBuffer, signature, sizeof(signature)) != 0)
    {
        printf("mount error: not a pack file");
        return false;
    }

    uint64_t offset = sizeof(signature);

    // version
    auto version = pack::readUint32InBigEndian(&headBuffer[offset]);
    offset += 4;

    // 索引加密秘钥
    auto indexSecret = pack::readUint32InBigEndian(&headBuffer[offset]);
    offset += 4;

    // 数据加密秘钥
    auto dataSecret = pack::readUint32InBigEndian(&headBuffer[offset]);
    offset += 4;

    // 索引偏移
    auto indexOffset = pack::readUint64InBigEndian(&headBuffer[offset]);
    offset += 8;

    // crc32校验码
    auto crc32 = pack::readUint32InBigEndian(&headBuffer[offset]);
    offset += 4;

    if (version != 0)
    {
        printf("mount error: unsupported version %d", version);
        return false;
    }

    m_dataSecret = dataSecret;

    // 重置偏移值
    offset = indexOffset;
    uint64_t length = fileSize;

    // 索引下标越界
    if (length < indexOffset)
    {
        printf("mount error: index out of bounds");
        return false;
    }

    // 空文件
    if (length == indexOffset)
    {
        printf("mount error: empty file");
        return true;
    }

    // 分配索引区域内存
    auto indexBufLength = length - indexOffset;
    std::unique_ptr<char[]> indexBuffer(new char[indexBufLength]);
    if (!indexBuffer)
    {
        printf("mount error: out of memory");
        return false;
    }

    memset(&indexBuffer[0], 0x0c, indexBufLength);

    // 读取索引数据
    if (fs.seek(indexOffset, FileStream::SeekOrigin::SET) != indexOffset)
    {
        int errsv = errno;
        printf("mount error: read index data failed, errno %d", errsv);
        return false;
    }
    if (fs.read(&indexBuffer[0], indexBufLength) != indexBufLength)
    {
        printf("mount error: read index data failed");
        return false;
    }

    PackFileInfo fileInfo;
    std::string filename;

#define CHECK_SIZE(num) if(offset + num > indexBufLength) { printf("mount error: not a pack file"); return false; }
    // 读取文件索引数据
    offset = 0;
    while (offset < indexBufLength)
    {
        CHECK_SIZE(8);
        fileInfo.offset = pack::readUint64InBigEndian(&indexBuffer[offset]);
        offset += 8;

        CHECK_SIZE(4);
        fileInfo.length = pack::readUint32InBigEndian(&indexBuffer[offset]);
        offset += 4;

        CHECK_SIZE(1);
        auto nameLength = (uint8_t)indexBuffer[offset];
        offset += 1;

        CHECK_SIZE(1);
        fileInfo.compressionType = (uint8_t)(indexBuffer[offset]);
        offset += 1;

        CHECK_SIZE(nameLength);
        pack::xorContent(indexSecret, &indexBuffer[offset], nameLength);
        filename.assign(&indexBuffer[offset], nameLength);
        offset += nameLength;

        m_packFiles.insert(std::make_pair(filename, fileInfo));
    }
#undef CHECK_SIZE

    return true;
}

void PackFileSystem::enumerate(const std::string& dirPath, const std::function<bool(const FileInfo&)>& call)
{
    if (m_packFiles.empty())
        return;

    std::set<std::string> fileSet;
    FileInfo info;
    for (auto it = m_packFiles.begin(); it != m_packFiles.end(); ++it)
    {
        if (dirPath.size() < it->first.size() && it->first.starts_with(dirPath))
        {
            auto s = it->first.substr(dirPath.size());
            auto p = s.find("/");
            if (p == std::string::npos)
            {
                info.flags = FileFlags::Read | FileFlags::File;
                info.filePath = m_mntpoint + dirPath + s;
                if (call(info))
                    break;
            }
            else
            {
                auto name = m_mntpoint + dirPath + s.substr(0, p);
                if (fileSet.count(name) == 0)
                {
                    info.flags = FileFlags::Read | FileFlags::Dir;
                    info.filePath = name;
                    if (call(info))
                        break;

                    fileSet.insert(name);
                }
            }
        }
    }
}

std::unique_ptr<FileStream> PackFileSystem::openFileStream(const std::string& filePath, FileStream::Mode mode)
{
    if (mode != FileStream::Mode::READ)
        return nullptr;

    auto it = m_packFiles.find(filePath);
    if (it == m_packFiles.end())
    {
        return nullptr;
    }
    
    auto fs = std::make_unique<PackFileStream>();
    return fs->open(m_archiveLocation, it->second, m_dataSecret) ? std::move(fs) : nullptr;
}

bool PackFileSystem::removeFile(const std::string& filePath)
{
	return false;
}

bool PackFileSystem::isFile(const std::string& filePath) const
{
	return m_packFiles.count(filePath);
}

bool PackFileSystem::isDir(const std::string& dirPath) const
{
    for (auto it = m_packFiles.begin(); it != m_packFiles.end(); ++it)
    {
        if (dirPath.size() < it->first.size() && it->first.starts_with(dirPath))
        {
            return true;
        }
    }
	return false;
}

bool PackFileSystem::createDir(const std::string& dirPath)
{
	return false;
}

const std::string& PackFileSystem::basePath() const
{
    return "";
}

NS_VFS_END
