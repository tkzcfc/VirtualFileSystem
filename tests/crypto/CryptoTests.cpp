// ChaCha20 (RFC 8439 test vectors), XOR, LZ4, XXH3, and ByteIO tests
#include "TestUtils.h"
#include "data/Rfc8439Vectors.h"
#include "vfs/archive/ByteIO.h"
#include "vfs/archive/Compression.h"
#include "vfs/crypto/ChaCha20.h"
#include "vfs/crypto/Hash.h"
#include "vfs/crypto/XorCipher.h"
#include <cstring>

using namespace vfs;

TEST_SUITE("crypto")
{
	TEST_CASE("ChaCha20: RFC 8439 block function test vectors (§2.3.2 + appendix A.1)")
	{
		const auto& vectors = rfc8439BlockVectors();
		REQUIRE(vectors.size() == 6);
		for (size_t i = 0; i < vectors.size(); ++i)
		{
			CAPTURE(i);
			const auto& v = vectors[i];
			uint8_t out[64];
			ChaCha20::block(v.key.data(), v.counter, v.nonce.data(), out);
			CHECK(std::vector<uint8_t>(out, out + 64) == v.keystream);

			// Encrypt 64 zero bytes through the stream API; the result is the keystream
			std::vector<uint8_t> zeros(64, 0);
			ChaCha20 cipher(v.key.data(), v.nonce.data(), v.counter);
			cipher.process(zeros.data(), zeros.size());
			CHECK(zeros == v.keystream);
		}
	}

	TEST_CASE("ChaCha20: RFC 8439 encryption test vectors (§2.4.2 + appendix A.2)")
	{
		const auto& vectors = rfc8439EncryptVectors();
		REQUIRE(vectors.size() == 4);
		for (size_t i = 0; i < vectors.size(); ++i)
		{
			CAPTURE(i);
			const auto& v = vectors[i];
			std::vector<uint8_t> data = v.plaintext;
			ChaCha20 enc(v.key.data(), v.nonce.data(), v.counter);
			enc.process(data.data(), data.size());
			CHECK(data == v.ciphertext);

			// Decryption is another encryption pass; irregular small chunks check that state carries across blocks
			ChaCha20 dec(v.key.data(), v.nonce.data(), v.counter);
			size_t pos = 0;
			size_t step = 1;
			while (pos < data.size())
			{
				size_t n = std::min(step, data.size() - pos);
				dec.process(data.data() + pos, n);
				pos += n;
				step = step * 3 % 97 + 1;
			}
			CHECK(data == v.plaintext);
		}
	}

	TEST_CASE("XOR: decrypting at any offset matches whole-buffer decryption (property test)")
	{
		const std::vector<uint8_t> plain = test::randomBytes(5000, 1);
		for (size_t keyLen : {1u, 7u, 16u, 31u, 32u})
		{
			CAPTURE(keyLen);
			std::vector<uint8_t> key = test::randomBytes(keyLen, 100 + keyLen);
			std::vector<uint8_t> cipher = plain;
			xorCipher(cipher.data(), cipher.size(), key.data(), key.size(), 0);
			CHECK(cipher != plain);

			uint64_t seed = 7;
			for (int iter = 0; iter < 2000; ++iter)
			{
				seed = seed * 6364136223846793005ull + 1442695040888963407ull;
				size_t off = static_cast<size_t>(seed >> 33) % plain.size();
				size_t len = static_cast<size_t>(seed >> 13) % (plain.size() - off + 1);
				std::vector<uint8_t> chunk(cipher.begin() + off, cipher.begin() + off + len);
				xorCipher(chunk.data(), chunk.size(), key.data(), key.size(), off);
				REQUIRE((len == 0 || std::memcmp(chunk.data(), plain.data() + off, len) == 0));
			}
		}
	}

	TEST_CASE("XOR: keyLen 0 leaves the data unchanged")
	{
		std::vector<uint8_t> data = test::bytes("abc");
		xorCipher(data.data(), data.size(), nullptr, 0, 5);
		CHECK(test::str(data) == "abc");
	}

	TEST_CASE("LZ4: compress/decompress round trip; corrupted data is rejected")
	{
		std::string text;
		for (int i = 0; i < 1000; ++i)
			text += "local value = " + std::to_string(i % 17) + "\n";
		std::vector<uint8_t> src = test::bytes(text);
		std::vector<uint8_t> packed;
		REQUIRE(Compression::lz4Compress(src.data(), src.size(), packed));
		CHECK(packed.size() < src.size() / 4);

		std::vector<uint8_t> out(src.size());
		CHECK(Compression::lz4Decompress(packed.data(), packed.size(), out.data(), out.size()));
		CHECK(out == src);

		// A wrong length or corrupt data must fail, and must not read past the buffer
		CHECK_FALSE(Compression::lz4Decompress(packed.data(), packed.size(), out.data(), out.size() - 1));
		std::vector<uint8_t> broken = packed;
		broken[broken.size() / 2] ^= 0xFF;
		broken.resize(broken.size() - 3);
		CHECK_FALSE(Compression::lz4Decompress(broken.data(), broken.size(), out.data(), out.size()));

		// Compression output is deterministic (required for reproducible builds)
		std::vector<uint8_t> packed2;
		REQUIRE(Compression::lz4Compress(src.data(), src.size(), packed2));
		CHECK(packed == packed2);
	}

	TEST_CASE("XXH3: one-shot and incremental results match")
	{
		std::vector<uint8_t> data = test::randomBytes(100000, 3);
		Hash128 whole = xxh3_128(data.data(), data.size());
		Xxh3Stream stream;
		size_t pos = 0;
		while (pos < data.size())
		{
			size_t n = std::min<size_t>(777, data.size() - pos);
			stream.update(data.data() + pos, n);
			pos += n;
		}
		CHECK(stream.digest() == whole);
		CHECK(xxh3_64(data.data(), data.size()) != 0);
		CHECK(xxh3_64(data.data(), data.size(), 1) != xxh3_64(data.data(), data.size(), 2));
		// Known XXH3-64 value: empty input, seed 0
		CHECK(xxh3_64("", 0) == 0x2D06800538D394C2ull);
	}

	TEST_CASE("ByteIO: always little-endian")
	{
		uint8_t buf[8];
		ByteIO::writeU32LE(buf, 0x11223344u);
		CHECK(buf[0] == 0x44);
		CHECK(buf[3] == 0x11);
		CHECK(ByteIO::readU32LE(buf) == 0x11223344u);
		ByteIO::writeU64LE(buf, 0x0102030405060708ull);
		CHECK(buf[0] == 0x08);
		CHECK(buf[7] == 0x01);
		CHECK(ByteIO::readU64LE(buf) == 0x0102030405060708ull);
		ByteIO::writeU16LE(buf, 0xABCD);
		CHECK(buf[0] == 0xCD);
		CHECK(ByteIO::readU16LE(buf) == 0xABCD);
	}
}
