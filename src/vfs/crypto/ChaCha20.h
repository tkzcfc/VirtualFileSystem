// This file: the in-tree ChaCha20 stream cipher (RFC 8439), used only to encrypt the .vfs index region.
// No third-party crypto library. Correctness is covered by the RFC 8439 test vectors in tests/crypto.
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

/**
 * ChaCha20 (RFC 8439: 32-byte key, 12-byte nonce, 32-bit block counter).
 * The object is stateful: successive process calls are equivalent to processing the concatenated input once.
 * Thread safety: a single object must not be used across threads. The destructor zeroes internal state.
 */
class VFS_DLL ChaCha20
{
public:
	static constexpr size_t KEY_SIZE = 32;
	static constexpr size_t NONCE_SIZE = 12;
	static constexpr size_t BLOCK_SIZE = 64;

	/**
	 * @param key     32-byte key
	 * @param nonce   12-byte nonce
	 * @param counter initial block counter (usually 1 when encrypting per RFC 8439)
	 */
	ChaCha20(const uint8_t* key, const uint8_t* nonce, uint32_t counter);

	~ChaCha20();

	ChaCha20(const ChaCha20&) = delete;

	ChaCha20& operator=(const ChaCha20&) = delete;

	/**
	 * In-place encrypt/decrypt (stream cipher; the two are the same).
	 */
	void process(uint8_t* data, size_t len);

	/**
	 * Compute one keystream block (the ChaCha20 block function in RFC 8439 section 2.3), for test vectors.
	 * @param out 64-byte output
	 */
	static void block(const uint8_t* key, uint32_t counter, const uint8_t* nonce, uint8_t* out);

private:
	// Generate the next keystream block from the current state and increment the counter.
	void nextBlock();

private:
	uint32_t m_state[16];       // Initial state: constants | key | counter | nonce
	uint8_t m_keystream[64];    // Keystream of the current block
	size_t m_used;              // Bytes already consumed from the current block
};

NS_VFS_END
