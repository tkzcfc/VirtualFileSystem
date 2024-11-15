#pragma once

#include "../FileStream.h"
#include "MemoryData.h"

NS_VFS_BEGIN

class MemoryFileStream : public FileStream
{
public:

    MemoryFileStream();

    virtual ~MemoryFileStream();

    virtual bool open(std::string_view path, FileStream::Mode mode) override;

    bool open(std::shared_ptr<MemoryData> data, FileStream::Mode mode);

    virtual void close() override;

    virtual uint64_t seek(uint64_t offset, SeekOrigin origin) override;

    virtual uint64_t read(void* buf, uint64_t size) override;

    virtual uint64_t write(const void* buf, uint64_t size) override;

    virtual uint64_t tell() override;

    virtual uint64_t size() override;

    virtual bool isOpen() const override;

protected:
    int64_t m_offset;
    FileStream::Mode m_mode;
    std::shared_ptr<MemoryData> m_data;
};

NS_VFS_END
