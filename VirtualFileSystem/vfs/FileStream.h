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

    /**
     *  Open a file
     *  @param path file to open
     *  @param mode File open mode, being READ | WRITE | APPEND
     *  @return true if successful, false if not
     */
    virtual bool open(std::string path, FileStream::Mode mode) = 0;

    /**
     *  Close a file stream
     *  @return 0 if successful, -1 if not
     */
    virtual void close() = 0;

    /**
     *  Seek to position in a file stream
     *  @param offset how many bytes to move within the stream
     *  @param origin SEEK_SET | SEEK_CUR | SEEK_END
     *  @return 0 if successful, -1 if not
     */
    virtual uint64_t seek(uint64_t offset, SeekOrigin origin) = 0;

    /**
     *  Read data from file stream
     *  @param buf pointer to data
     *  @param size the amount of data to read in bytes
     *  @return amount of data read successfully, -1 if error
     */
    virtual uint64_t read(void* buf, uint64_t size) = 0;

    /**
     *  Write data to file stream
     *  @param buf pointer to data
     *  @param size the amount of data to write in bytes
     *  @return amount of data written successfully, -1 if error
     */
    virtual uint64_t write(const void* buf, uint64_t size) = 0;

    /**
     *  Get the current position in the file stream
     *  @return current position, -1 if error
     */
    virtual uint64_t tell() = 0;

    /**
     *  Get the size of the file stream
     *  @return stream size, -1 if error (Mode::WRITE and Mode::APPEND may return -1)
     */
    virtual uint64_t size() = 0;

    /**
     *  Get status of file stream
     *  @return true if open, false if closed
     */
    virtual bool isOpen() const = 0;

    virtual operator bool() const { return isOpen(); }

protected:
    FileStream() {};
};

NS_VFS_END
