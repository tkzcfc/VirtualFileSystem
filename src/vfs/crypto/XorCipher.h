// This file: multi-byte XOR encryption for a single file.
// Stored byte i = original byte ^ key[i % keyLen], where i is the offset within that file's stored data, so decryption can start at any position and random seek is supported.
// Note: this is only obfuscation. It raises the cost of extracting assets; it is not real cryptographic security.
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

/**
 * In-place XOR (encryption and decryption are the same).
 * @param data   Data
 * @param len    Length
 * @param key    Key
 * @param keyLen Key length; when 0, do nothing
 * @param offset Offset of data[0] within the file's stored data
 */
inline void xorCipher(uint8_t* data, size_t len, const uint8_t* key, size_t keyLen, uint64_t offset)
{
	if (keyLen == 0 || len == 0)
		return;
	size_t k = static_cast<size_t>(offset % keyLen);
	// First advance to a key boundary, then XOR whole key-sized chunks. The loop body stays simple so the compiler can auto-vectorize it
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
