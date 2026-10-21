// This file: little-endian serialization helpers. Everything is implemented with shifts, so the result is the same on machines of any endianness.
#pragma once

#include "../Common.h"
#include <cstring>

NS_VFS_BEGIN

namespace ByteIO
{

inline uint16_t readU16LE(const uint8_t* p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t readU32LE(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		   (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t readU64LE(const uint8_t* p)
{
	return static_cast<uint64_t>(readU32LE(p)) | (static_cast<uint64_t>(readU32LE(p + 4)) << 32);
}

inline void writeU16LE(uint8_t* p, uint16_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

inline void writeU32LE(uint8_t* p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
	p[2] = static_cast<uint8_t>(v >> 16);
	p[3] = static_cast<uint8_t>(v >> 24);
}

inline void writeU64LE(uint8_t* p, uint64_t v)
{
	writeU32LE(p, static_cast<uint32_t>(v));
	writeU32LE(p + 4, static_cast<uint32_t>(v >> 32));
}

} // namespace ByteIO

NS_VFS_END
