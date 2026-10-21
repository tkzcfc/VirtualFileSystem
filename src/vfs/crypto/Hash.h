// This file: a thin wrapper around XXH3. The xxHash header appears only in Hash.cpp and is not exposed outside the library.
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

/**
 * 128-bit hash value.
 */
struct Hash128
{
	uint64_t low = 0;
	uint64_t high = 0;

	bool operator==(const Hash128& other) const { return low == other.low && high == other.high; }

	bool operator!=(const Hash128& other) const { return !(*this == other); }

	bool operator<(const Hash128& other) const
	{
		return high != other.high ? high < other.high : low < other.low;
	}
};

/**
 * XXH3-64.
 */
VFS_DLL uint64_t xxh3_64(const void* data, size_t len, uint64_t seed = 0);

/**
 * XXH3-128.
 */
VFS_DLL Hash128 xxh3_128(const void* data, size_t len, uint64_t seed = 0);

/**
 * Incremental XXH3-128, for streaming the hash of a large file.
 * Thread safety: a single object must not be used across threads.
 */
class VFS_DLL Xxh3Stream
{
public:

	explicit Xxh3Stream(uint64_t seed = 0);

	~Xxh3Stream();

	Xxh3Stream(const Xxh3Stream&) = delete;

	Xxh3Stream& operator=(const Xxh3Stream&) = delete;

	void reset(uint64_t seed = 0);

	void update(const void* data, size_t len);

	Hash128 digest() const;

private:
	void* m_state;   // XXH3_state_t*; the xxHash type is not exposed in the header
};

NS_VFS_END
