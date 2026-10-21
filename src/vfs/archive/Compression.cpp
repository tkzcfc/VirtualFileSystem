#include "Compression.h"
#include "lz4.h"
#include "lz4hc.h"
#include <climits>

NS_VFS_BEGIN

namespace Compression
{

bool lz4Compress(const uint8_t* src, size_t size, std::vector<uint8_t>& out, int level)
{
	if (size > static_cast<size_t>(LZ4_MAX_INPUT_SIZE))
		return false;
	int bound = LZ4_compressBound(static_cast<int>(size));
	out.resize(static_cast<size_t>(bound));
	int n = LZ4_compress_HC(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(out.data()),
							static_cast<int>(size), bound, level);
	if (n <= 0)
	{
		out.clear();
		return false;
	}
	out.resize(static_cast<size_t>(n));
	return true;
}

bool lz4Decompress(const uint8_t* src, size_t size, uint8_t* dst, size_t dstSize)
{
	if (size > static_cast<size_t>(INT_MAX) || dstSize > static_cast<size_t>(INT_MAX))
		return false;
	// LZ4_decompress_safe does a full bounds check, so corrupted or malicious input is safe to feed it.
	int n = LZ4_decompress_safe(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst),
								static_cast<int>(size), static_cast<int>(dstSize));
	return n >= 0 && static_cast<size_t>(n) == dstSize;
}

size_t lz4MaxInputSize()
{
	return static_cast<size_t>(LZ4_MAX_INPUT_SIZE);
}

} // namespace Compression

NS_VFS_END
