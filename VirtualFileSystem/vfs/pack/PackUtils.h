#pragma once

#include "../Common.h"
#include <stdint.h>

NS_VFS_BEGIN

namespace pack 
{
    inline uint32_t readUint32InBigEndian(void* memory)
    {
        uint8_t* p = (uint8_t*)memory;
        return (((uint32_t)p[0]) << 24) |
            (((uint32_t)p[1]) << 16) |
            (((uint32_t)p[2]) << 8) |
            (((uint32_t)p[3]));
    }

    inline uint64_t readUint64InBigEndian(void* memory)
    {
        uint8_t* p = (uint8_t*)memory;
        return (((uint64_t)p[0]) << 56) |
            (((uint64_t)p[1]) << 48) |
            (((uint64_t)p[2]) << 40) |
            (((uint64_t)p[3]) << 32) |
            (((uint64_t)p[4]) << 24) |
            (((uint64_t)p[5]) << 16) |
            (((uint64_t)p[6]) << 8) |
            (((uint64_t)p[7]));
    }

    inline bool isBigEndian()
    {
        union {
            uint32_t i;
            char c[4];
        } bint = { 0x01000000 };

        return bint.c[0] == 1;
    }

    inline void xorContent(uint32_t s, char* buf, size_t len)
    {
        if (s == 0)
            return;
        auto p = reinterpret_cast<unsigned char*>(&s);

        char sBuf[4] = { 0 };
        if (isBigEndian())
        {
            sBuf[0] = p[3];
            sBuf[1] = p[2];
            sBuf[2] = p[1];
            sBuf[3] = p[0];
        }
        else
        {
            sBuf[0] = p[0];
            sBuf[1] = p[1];
            sBuf[2] = p[2];
            sBuf[3] = p[3];
        }

        for (size_t i = 0; i < len; ++i)
        {
            buf[i] ^= sBuf[i % sizeof(s)];
        }
    }

    char* decompressData(const char* inData, uint64_t inLen, uint64_t& outLen, int* errCode = nullptr);
}

NS_VFS_END

