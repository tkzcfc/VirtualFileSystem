// 本文件：XXH3 哈希的薄封装。xxHash 的头文件只在 Hash.cpp 中出现，不暴露给库外。
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

/**
 * 128 位哈希值。
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
 * XXH3-64。
 */
VFS_DLL uint64_t xxh3_64(const void* data, size_t len, uint64_t seed = 0);

/**
 * XXH3-128。
 */
VFS_DLL Hash128 xxh3_128(const void* data, size_t len, uint64_t seed = 0);

/**
 * 增量 XXH3-128，用于流式计算大文件的哈希。
 * 线程安全：单个对象不能跨线程使用。
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
	void* m_state;   // XXH3_state_t*，不在头文件中暴露 xxHash 的类型
};

NS_VFS_END
