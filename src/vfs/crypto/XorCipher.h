// 本文件：单文件多字节 XOR 加密（final-decisions #4）。
// 第 i 个存储字节 = 原字节 ^ key[i % keyLen]，i 是该文件存储数据内的偏移，所以可以从任意位置开始解密，支持随机 seek。
// 注意：这只是混淆，提高提取资源的门槛，不是真正的安全加密（vfs-format F7）。
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

/**
 * 原地 XOR（加密和解密相同）。
 * @param data   数据
 * @param len    长度
 * @param key    密钥
 * @param keyLen 密钥长度；为 0 时什么也不做
 * @param offset data[0] 在文件存储数据中的偏移
 */
inline void xorCipher(uint8_t* data, size_t len, const uint8_t* key, size_t keyLen, uint64_t offset)
{
	if (keyLen == 0 || len == 0)
		return;
	size_t k = static_cast<size_t>(offset % keyLen);
	// 先处理到密钥边界，之后整段整段地异或，循环体简单，编译器可以自动向量化
	size_t head = keyLen - k;
	if (head > len)
		head = len;
	for (size_t i = 0; i < head; ++i)
		data[i] ^= key[k + i];
	data += head;
	len -= head;
	while (len >= keyLen)
	{
		for (size_t i = 0; i < keyLen; ++i)
			data[i] ^= key[i];
		data += keyLen;
		len -= keyLen;
	}
	for (size_t i = 0; i < len; ++i)
		data[i] ^= key[i];
}

NS_VFS_END
