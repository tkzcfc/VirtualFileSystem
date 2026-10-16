#include "Hash.h"

// header-only 方式使用 xxHash，所有符号都是 static inline，不会与宿主程序里的 xxHash 冲突
#define XXH_INLINE_ALL
#include "xxhash.h"

NS_VFS_BEGIN

uint64_t xxh3_64(const void* data, size_t len, uint64_t seed)
{
	return XXH3_64bits_withSeed(data, len, seed);
}

Hash128 xxh3_128(const void* data, size_t len, uint64_t seed)
{
	XXH128_hash_t h = XXH3_128bits_withSeed(data, len, seed);
	Hash128 result;
	result.low = h.low64;
	result.high = h.high64;
	return result;
}

Xxh3Stream::Xxh3Stream(uint64_t seed)
	: m_state(XXH3_createState())
{
	reset(seed);
}

Xxh3Stream::~Xxh3Stream()
{
	XXH3_freeState(static_cast<XXH3_state_t*>(m_state));
}

void Xxh3Stream::reset(uint64_t seed)
{
	XXH3_128bits_reset_withSeed(static_cast<XXH3_state_t*>(m_state), seed);
}

void Xxh3Stream::update(const void* data, size_t len)
{
	XXH3_128bits_update(static_cast<XXH3_state_t*>(m_state), data, len);
}

Hash128 Xxh3Stream::digest() const
{
	XXH128_hash_t h = XXH3_128bits_digest(static_cast<const XXH3_state_t*>(m_state));
	Hash128 result;
	result.low = h.low64;
	result.high = h.high64;
	return result;
}

NS_VFS_END
