#pragma once

#include "../FileStream.h"
#include <fstream>

NS_VFS_BEGIN

class NativeFileStream : public FileStream
{
public:

    virtual bool open(const std::string& path, FileStream::Mode mode) override;

    virtual void close() override;

    virtual uint64_t seek(uint64_t offset, SeekOrigin origin) override;

    virtual uint64_t read(void* buf, uint64_t size) override;

    virtual uint64_t write(const void* buf, uint64_t size) override;

    virtual uint64_t tell() override;

    virtual uint64_t size() override;

    virtual bool isOpen() const override;

protected:
    std::fstream m_fs;
};

NS_VFS_END
