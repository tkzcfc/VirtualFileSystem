// This file: a thin wrapper around LZ4 compression. The LZ4 headers appear only in Compression.cpp.
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

namespace Compression
{

/**
 * LZ4 HC compression (deterministic output, so builds stay reproducible).
 * @param src   raw data
 * @param size  raw length (must be below LZ4's single-block limit, about 2 GB)
 * @param out   compressed output
 * @param level LZ4 HC compression level (1..12)
 * @return true on success
 */
VFS_DLL bool lz4Compress(const uint8_t* src, size_t size, std::vector<uint8_t>& out, int level = 9);

/**
 * LZ4 decompression.
 * @param src     compressed data
 * @param size    compressed length
 * @param dst     output buffer
 * @param dstSize raw length (exactly this many bytes must come out)
 * @return true on success; false if the data is corrupt
 */
VFS_DLL bool lz4Decompress(const uint8_t* src, size_t size, uint8_t* dst, size_t dstSize);

/**
 * Largest input one LZ4 block can handle.
 */
VFS_DLL size_t lz4MaxInputSize();

/**
 * Worst-case compressed size of size input bytes (size must not exceed lz4MaxInputSize()).
 */
VFS_DLL size_t lz4CompressBound(size_t size);

} // namespace Compression

NS_VFS_END
