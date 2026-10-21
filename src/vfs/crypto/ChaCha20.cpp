#include "ChaCha20.h"
#include <cstring>

NS_VFS_BEGIN

namespace
{

inline uint32_t rotl32(uint32_t v, int c)
{
	return (v << c) | (v >> (32 - c));
}

inline uint32_t load32le(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		   (static_cast<uint32_t>(p[3]) << 24);
}

inline void store32le(uint8_t* p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
	p[2] = static_cast<uint8_t>(v >> 16);
	p[3] = static_cast<uint8_t>(v >> 24);
}

// RFC 8439 §2.1 quarter round
#define VFS_CHACHA_QR(a, b, c, d) \
	a += b;                       \
	d ^= a;                       \
	d = rotl32(d, 16);            \
	c += d;                       \
	b ^= c;                       \
	b = rotl32(b, 12);            \
	a += b;                       \
	d ^= a;                       \
	d = rotl32(d, 8);             \
	c += d;                       \
	b ^= c;                       \
	b = rotl32(b, 7);

void setupState(uint32_t state[16], const uint8_t* key, uint32_t counter, const uint8_t* nonce)
{
	// "expand 32-byte k"
	state[0] = 0x61707865u;
	state[1] = 0x3320646eu;
	state[2] = 0x79622d32u;
	state[3] = 0x6b206574u;
	for (int i = 0; i < 8; ++i)
		state[4 + i] = load32le(key + i * 4);
	state[12] = counter;
	state[13] = load32le(nonce + 0);
	state[14] = load32le(nonce + 4);
	state[15] = load32le(nonce + 8);
}

void blockFunction(const uint32_t in[16], uint8_t out[64])
{
	uint32_t x[16];
	std::memcpy(x, in, sizeof(x));
	// 20 rounds = 10 iterations (column round + diagonal round)
	for (int i = 0; i < 10; ++i)
	{
		VFS_CHACHA_QR(x[0], x[4], x[8], x[12])
		VFS_CHACHA_QR(x[1], x[5], x[9], x[13])
		VFS_CHACHA_QR(x[2], x[6], x[10], x[14])
		VFS_CHACHA_QR(x[3], x[7], x[11], x[15])
		VFS_CHACHA_QR(x[0], x[5], x[10], x[15])
		VFS_CHACHA_QR(x[1], x[6], x[11], x[12])
		VFS_CHACHA_QR(x[2], x[7], x[8], x[13])
		VFS_CHACHA_QR(x[3], x[4], x[9], x[14])
	}
	for (int i = 0; i < 16; ++i)
		store32le(out + i * 4, x[i] + in[i]);
}

#undef VFS_CHACHA_QR

} // namespace

ChaCha20::ChaCha20(const uint8_t* key, const uint8_t* nonce, uint32_t counter)
	: m_used(BLOCK_SIZE)
{
	setupState(m_state, key, counter, nonce);
	std::memset(m_keystream, 0, sizeof(m_keystream));
}

ChaCha20::~ChaCha20()
{
	// Zero the key material; a volatile pointer stops the compiler from optimizing the wipe away
	volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(m_state);
	for (size_t i = 0; i < sizeof(m_state); ++i)
		p[i] = 0;
	p = m_keystream;
	for (size_t i = 0; i < sizeof(m_keystream); ++i)
		p[i] = 0;
}

void ChaCha20::nextBlock()
{
	blockFunction(m_state, m_keystream);
	// 32-bit counter, wraps per RFC 8439 (one index is at most 256 GB, which does not happen in practice)
	++m_state[12];
	m_used = 0;
}

void ChaCha20::process(uint8_t* data, size_t len)
{
	while (len > 0)
	{
		if (m_used == BLOCK_SIZE)
			nextBlock();
		size_t n = BLOCK_SIZE - m_used;
		if (n > len)
			n = len;
		for (size_t i = 0; i < n; ++i)
			data[i] ^= m_keystream[m_used + i];
		m_used += n;
		data += n;
		len -= n;
	}
}

void ChaCha20::block(const uint8_t* key, uint32_t counter, const uint8_t* nonce, uint8_t* out)
{
	uint32_t state[16];
	setupState(state, key, counter, nonce);
	blockFunction(state, out);
}

NS_VFS_END
