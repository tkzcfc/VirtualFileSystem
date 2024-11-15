#pragma once

#include "../native/NativeFileStream.h"

NS_VFS_BEGIN

struct PackFileInfo;
class PackFileStream : public FileStream
{
public:

    PackFileStream();

    virtual ~PackFileStream();

    virtual bool open(const std::string& path, FileStream::Mode mode) override;

    bool open(const std::string& path, const PackFileInfo& fileInfo, uint32_t dataSecret);

    virtual void close() override;

    virtual uint64_t seek(uint64_t offset, SeekOrigin origin) override;

    virtual uint64_t read(void* buf, uint64_t size) override;

    virtual uint64_t write(const void* buf, uint64_t size) override;

    virtual uint64_t tell() override;

    virtual uint64_t size() override;

    virtual bool isOpen() const override;

protected:
    NativeFileStream m_fs;
    int64_t m_offset;
    uint8_t* m_realData;
    uint64_t m_realDataLen;
    uint64_t m_rawOffset;
};

NS_VFS_END
