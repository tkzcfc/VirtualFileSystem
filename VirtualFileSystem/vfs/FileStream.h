#pragma once

#include "Common.h"
#include <stdint.h>

NS_VFS_BEGIN

class FileStream
{
public:
    enum class Mode
    {
        READ,
        WRITE,
        APPEND,
    };


    enum class SeekOrigin
    {
        CUR,
        SET,
        END,
    };

    virtual ~FileStream() = default;

    virtual bool open(std::string_view path, FileStream::Mode mode) = 0;

    virtual void close() = 0;

    virtual uint64_t seek(uint64_t offset, SeekOrigin origin) = 0;

    virtual uint64_t read(void* buf, uint64_t size) = 0;

    virtual uint64_t write(const void* buf, uint64_t size) = 0;

    virtual uint64_t tell() = 0;

    virtual uint64_t size() = 0;

    virtual bool isOpen() const = 0;

    virtual operator bool() const { return isOpen(); }

protected:
    FileStream() {};
};

NS_VFS_END
