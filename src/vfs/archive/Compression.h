// 本文件：LZ4 压缩的薄封装（final-decisions #6）。LZ4 的头文件只在 Compression.cpp 中出现。
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

namespace Compression
{

/**
 * LZ4 HC 压缩（结果确定，保证可复现构建）。
 * @param src   原始数据
 * @param size  原始长度（必须小于 LZ4 的单块上限，约 2 GB）
 * @param out   输出压缩数据
 * @param level LZ4 HC 压缩级别（1..12）
 * @return 成功返回 true
 */
VFS_DLL bool lz4Compress(const uint8_t* src, size_t size, std::vector<uint8_t>& out, int level = 9);

/**
 * LZ4 解压。
 * @param src     压缩数据
 * @param size    压缩长度
 * @param dst     输出缓冲区
 * @param dstSize 原始长度（必须恰好解压出这么多字节）
 * @return 成功返回 true；数据损坏返回 false
 */
VFS_DLL bool lz4Decompress(const uint8_t* src, size_t size, uint8_t* dst, size_t dstSize);

/**
 * LZ4 单块能处理的最大输入。
 */
VFS_DLL size_t lz4MaxInputSize();

} // namespace Compression

NS_VFS_END
