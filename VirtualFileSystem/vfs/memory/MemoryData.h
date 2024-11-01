#pragma once

#include "../Common.h"
#include <vector>
#include <mutex>

NS_VFS_BEGIN

class MemoryData
{
public:

    MemoryData();

    virtual ~MemoryData();

    uint64_t write(uint8_t* data, uint64_t len, uint64_t offset);

    uint64_t read(uint8_t* data, uint64_t len, uint64_t offset);

    uint64_t len();

    void getWriteLock();

    void releaseWriteLock();

    bool isWriting() { return m_writing.load(std::memory_order_relaxed); }

private:

    uint64_t read_impl(uint8_t* data, uint64_t len, uint64_t offset);

protected:
    std::mutex m_mutex;
    std::vector<uint8_t> m_data;   
    std::atomic<bool> m_writing;
};

NS_VFS_END
