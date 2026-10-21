// Block-based LZ4 archive entries: round trips for many sizes and block sizes, random access, incompressible blocks,
// encryption + compression, concurrent readers, corruption detection, reproducibility, and vfstool --block-size.
#include "TestUtils.h"
#include "core/Commands.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/archive/ArchiveFormat.h"
#include "vfs/archive/ArchiveReader.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/archive/ArchiveWriter.h"
#include "vfs/archive/ByteIO.h"
#include "vfs/crypto/Hash.h"
#include <atomic>
#include <random>
#include <sstream>
#include <thread>

using namespace vfs;

namespace
{

// Compressible but not trivially repetitive content
std::vector<uint8_t> textLike(size_t size, uint32_t seed)
{
	std::vector<uint8_t> v(size);
	std::mt19937 rng(seed);
	static const char* words[] = {"local ", "function ", "end\n", "return ", "player", ".health ", "= ", "42 ", "if ", "then\n"};
	size_t i = 0;
	while (i < size)
	{
		const char* w = words[rng() % 10];
		for (; *w && i < size; ++w)
			v[i++] = static_cast<uint8_t>(*w);
	}
	return v;
}

FileWriteOptions lz4(uint8_t log2 = 0, bool encrypt = false)
{
	FileWriteOptions o;
	o.compress = true;
	o.blockLog2 = log2;
	o.encrypt = encrypt;
	return o;
}

std::vector<uint8_t> pack(const std::vector<test::ArchiveFile>& files, const ArchiveWriteOptions& wo = {})
{
	auto mem = MemoryBackend::create();
	auto out = mem->open("a.vfs", IFileStream::Mode::WRITE, nullptr);
	REQUIRE(out);
	ArchiveWriter w(wo);
	for (const auto& f : files)
		REQUIRE(w.addData(f.path, f.content, f.options) == Error::OK);
	REQUIRE(w.write(*out) == Error::OK);
	out->seek(0, SEEK_SET);
	return test::readStream(*out);
}

std::shared_ptr<ArchiveBackend> mountData(const std::vector<uint8_t>& data)
{
	Error e = Error::OK;
	auto b = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(data), {}, &e);
	REQUIRE(e == Error::OK);
	REQUIRE(b);
	return b;
}

std::vector<uint8_t> readAllVia(ArchiveBackend& b, std::string_view rel)
{
	auto s = b.open(rel, IFileStream::Mode::READ, nullptr);
	REQUIRE(s);
	return test::readStream(*s);
}

// Random seek + read checks against the original
bool randomAccessMatches(IFileStream& s, const std::vector<uint8_t>& orig, int rounds, uint32_t seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> buf(200000);
	for (int r = 0; r < rounds; ++r)
	{
		const int64_t off = orig.empty() ? 0 : static_cast<int64_t>(rng() % (orig.size() + 10));
		const size_t len = 1 + rng() % (r % 4 == 0 ? buf.size() : 5000);
		if (s.seek(off, SEEK_SET) != off)
			return false;
		const int n = s.read(buf.data(), static_cast<unsigned int>(len));
		const int64_t expect = std::max<int64_t>(0, std::min<int64_t>(static_cast<int64_t>(len), static_cast<int64_t>(orig.size()) - off));
		if (n != expect)
			return false;
		if (n > 0 && std::memcmp(buf.data(), orig.data() + off, static_cast<size_t>(n)) != 0)
			return false;
		if (s.tell() != off + n)
			return false;
	}
	return true;
}

} // namespace

TEST_SUITE("archive")
{
	TEST_CASE("LZ4 blocks: round trip for edge sizes and every block-size boundary")
	{
		for (uint8_t log2 : {uint8_t(12), uint8_t(13), uint8_t(16), uint8_t(20), uint8_t(22)})
		{
			const size_t bs = size_t(1) << log2;
			std::vector<test::ArchiveFile> files;
			const std::vector<size_t> sizes = {0, 1, bs - 1, bs, bs + 1, 2 * bs, 3 * bs + 7};
			for (size_t sz : sizes)
				files.push_back({"f" + std::to_string(sz), textLike(sz, static_cast<uint32_t>(sz + log2)), lz4(log2)});
			auto b = mountData(pack(files));
			for (const auto& f : files)
			{
				CAPTURE(log2);
				CAPTURE(f.path);
				const ArchiveEntry* e = b->index()->findFile(f.path);
				REQUIRE(e);
				if (f.content.size() < 64)
					CHECK(e->compression == CompressionType::NONE);   // too small to shrink: stored as-is
				else
				{
					CHECK(e->compression == CompressionType::LZ4);
					CHECK(e->blockLog2 == log2);
					CHECK(e->blockCount() == (f.content.size() + bs - 1) / bs);
				}
				CHECK(readAllVia(*b, f.path) == f.content);
			}
			CHECK(b->verifyAll() == Error::OK);
		}
	}

	TEST_CASE("LZ4 blocks: multi-MB files, default block size, random seeks and reads")
	{
		const auto big = textLike(5 * 1024 * 1024 + 123, 9);
		auto b = mountData(pack({{"big.lua", big, lz4()}}));
		const ArchiveEntry* e = b->index()->findFile("big.lua");
		REQUIRE(e);
		CHECK(e->blockLog2 == ArchiveFormat::DEFAULT_BLOCK_LOG2);
		CHECK(e->storedSize < e->size / 2);
		auto s = b->open("big.lua", IFileStream::Mode::READ, nullptr);
		REQUIRE(s);
		CHECK(s->size() == static_cast<int64_t>(big.size()));
		CHECK(randomAccessMatches(*s, big, 400, 1));
		// SEEK_END / SEEK_CUR and reads crossing block boundaries
		CHECK(s->seek(-5, SEEK_END) == static_cast<int64_t>(big.size()) - 5);
		uint8_t tail[16];
		CHECK(s->read(tail, sizeof tail) == 5);
		CHECK(std::memcmp(tail, big.data() + big.size() - 5, 5) == 0);
		CHECK(s->read(tail, sizeof tail) == 0);
		CHECK(s->seek(65536 - 3, SEEK_SET) == 65533);
		CHECK(s->seek(0, SEEK_CUR) == 65533);
		CHECK(s->read(tail, 6) == 6);
		CHECK(std::memcmp(tail, big.data() + 65533, 6) == 0);
		CHECK(test::readStream(*s).size() == big.size() - 65539);
	}

	TEST_CASE("LZ4 blocks: incompressible blocks are stored raw inside a compressed entry")
	{
		// First half compressible, second half random: the entry is LZ4, the random blocks carry the raw flag
		auto mixed = textLike(256 * 1024, 3);
		const auto noise = test::randomBytes(256 * 1024, 4);
		mixed.insert(mixed.end(), noise.begin(), noise.end());
		const auto data = pack({{"mixed.bin", mixed, lz4(16)}, {"noise.bin", noise, lz4(16)}});
		auto b = mountData(data);
		const ArchiveEntry* e = b->index()->findFile("mixed.bin");
		REQUIRE(e);
		REQUIRE(e->compression == CompressionType::LZ4);
		int rawBlocks = 0;
		for (uint64_t i = 0; i < e->blockCount(); ++i)
			rawBlocks += (ByteIO::readU64LE(data.data() + e->offset + i * 8) & ArchiveFormat::BLOCK_RAW_FLAG) != 0;
		CHECK(rawBlocks == 4);
		CHECK(b->index()->findFile("noise.bin")->compression == CompressionType::NONE);   // not smaller overall
		auto s = b->open("mixed.bin", IFileStream::Mode::READ, nullptr);
		CHECK(randomAccessMatches(*s, mixed, 200, 5));
		CHECK(readAllVia(*b, "noise.bin") == noise);
	}

	TEST_CASE("LZ4 blocks: encrypted + compressed entries support random access")
	{
		const auto big = textLike(700000, 11);
		const auto data = pack({{"enc.lua", big, lz4(13, true)}});
		auto b = mountData(data);
		const ArchiveEntry* e = b->index()->findFile("enc.lua");
		REQUIRE(e);
		CHECK(e->cipher == CipherType::XOR);
		CHECK(e->compression == CompressionType::LZ4);
		// The block table is encrypted too: raw items do not look like increasing offsets
		CHECK(ByteIO::readU64LE(data.data() + e->offset) != ByteIO::readU64LE(data.data() + e->offset + 8) - 1);
		auto s = b->open("enc.lua", IFileStream::Mode::READ, nullptr);
		CHECK(randomAccessMatches(*s, big, 300, 7));
		CHECK(b->verifyAll() == Error::OK);
	}

	TEST_CASE("LZ4 blocks: many threads read the same compressed entry" * VFS_NEEDS_THREADS)
	{
		const auto big = textLike(2 * 1024 * 1024 + 5, 21);
		auto b = mountData(pack({{"shared.lua", big, lz4(14, true)}}));
		VirtualFileSystem fs;
		fs.mount("/", b);
		std::atomic<int> bad{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 8; ++t)
			threads.emplace_back([&, t] {
				for (int k = 0; k < 5; ++k)
				{
					auto s = fs.openFileStream("/shared.lua", IFileStream::Mode::READ);
					if (!s || !randomAccessMatches(*s, big, 40, static_cast<uint32_t>(t * 100 + k)))
						++bad;
				}
			});
		for (auto& th : threads)
			th.join();
		CHECK(bad == 0);
	}

	TEST_CASE("LZ4 blocks: corrupted blocks and tables are detected")
	{
		const auto text = textLike(300000, 31);
		const auto good = pack({{"t.lua", text, lz4(14)}}, [] {
			ArchiveWriteOptions o;
			o.encryptIndex = false;
			return o;
		}());
		const ArchiveEntry entry = *mountData(good)->index()->findFile("t.lua");
		const uint64_t table = entry.blockTableSize();

		auto expectBroken = [&](std::vector<uint8_t> bad) {
			auto b = mountData(bad);   // index untouched: open still succeeds
			CHECK(b->verifyAll() == Error::CORRUPTED);
			auto s = b->open("t.lua", IFileStream::Mode::READ, nullptr);
			REQUIRE(s);
			std::vector<uint8_t> buf(text.size());
			bool failed = false;
			while (true)
			{
				int n = s->read(buf.data(), static_cast<unsigned int>(buf.size()));
				if (n < 0)
				{
					failed = true;
					break;
				}
				if (n == 0)
					break;
			}
			// Either a read reports EIO, or the bytes differ (a flip inside literal data still decodes); never a crash
			if (!failed)
				CHECK(test::readStream(*b->open("t.lua", IFileStream::Mode::READ, nullptr)) != text);
		};

		// Flip a byte in the middle of the block data
		{
			auto bad = good;
			bad[entry.offset + table + (entry.storedSize - table) / 2] ^= 0x5A;
			expectBroken(bad);
		}
		// Table: non-monotonic end offset
		{
			auto bad = good;
			ByteIO::writeU64LE(bad.data() + entry.offset + 8, 1);
			expectBroken(bad);
		}
		// Table: last block does not end at the end of the data
		{
			auto bad = good;
			const size_t last = static_cast<size_t>(entry.offset + table - 8);
			ByteIO::writeU64LE(bad.data() + last, ByteIO::readU64LE(bad.data() + last) - 1);
			expectBroken(bad);
		}
		// Table: raw flag on a compressed block (length does not match the raw block length)
		{
			auto bad = good;
			const size_t first = static_cast<size_t>(entry.offset);
			ByteIO::writeU64LE(bad.data() + first, ByteIO::readU64LE(bad.data() + first) | ArchiveFormat::BLOCK_RAW_FLAG);
			expectBroken(bad);
		}
	}

	TEST_CASE("LZ4 blocks: index validation rejects bad block sizes")
	{
		const auto good = pack({{"t.lua", textLike(50000, 1), lz4(12)}}, [] {
			ArchiveWriteOptions o;
			o.encryptIndex = false;
			return o;
		}());
		const auto idx = mountData(good)->index();
		const uint64_t entryAt = idx->header.indexOffset;
		for (uint8_t v : {uint8_t(0), uint8_t(11), uint8_t(23), uint8_t(255)})
		{
			auto bad = good;
			bad[entryAt + ArchiveFormat::E_BLOCK_LOG2] = v;
			// Fix the index hash so the entry check itself is exercised
			const uint64_t h = xxh3_64(bad.data() + entryAt, idx->header.indexSize);
			ByteIO::writeU64LE(bad.data() + ArchiveFormat::H_INDEX_HASH, h);
			ByteIO::writeU64LE(bad.data() + ArchiveFormat::H_CHECKSUM, xxh3_64(bad.data(), ArchiveFormat::H_CHECKSUM));
			Error e = Error::OK;
			CHECK_FALSE(ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(bad), {}, &e));
			CHECK(e == Error::CORRUPTED);
		}
	}

	TEST_CASE("LZ4 blocks: packing is deterministic; block size is part of dedup")
	{
		const auto text = textLike(400000, 41);
		std::vector<test::ArchiveFile> files = {{"a.lua", text, lz4(12)}, {"b.lua", text, lz4(12)}, {"c.lua", text, lz4(20)}};
		const auto one = pack(files);
		const auto two = pack(files);
		CHECK(one == two);
		auto b = mountData(one);
		CHECK(b->index()->findFile("a.lua")->offset == b->index()->findFile("b.lua")->offset);
		CHECK(b->index()->findFile("a.lua")->offset != b->index()->findFile("c.lua")->offset);
		CHECK(readAllVia(*b, "c.lua") == text);
	}

	TEST_CASE("LZ4 blocks: writer rejects out-of-range block sizes")
	{
		auto mem = MemoryBackend::create();
		auto out = mem->open("a.vfs", IFileStream::Mode::WRITE, nullptr);
		ArchiveWriteOptions wo;
		wo.blockLog2 = 23;
		ArchiveWriter w(wo);
		REQUIRE(w.addData("x", textLike(100, 1), lz4()) == Error::OK);
		CHECK(w.write(*out) == Error::UNSUPPORTED);
		ArchiveWriter w2;
		REQUIRE(w2.addData("x", textLike(100, 1), lz4(11)) == Error::OK);
		CHECK(w2.write(*out) == Error::UNSUPPORTED);
	}
}

TEST_SUITE("tool")
{
	TEST_CASE("parseBlockSize / formatBlockSize")
	{
		uint8_t l = 0;
		CHECK((tool::parseBlockSize("64K", l) && l == 16));
		CHECK((tool::parseBlockSize("64KiB", l) && l == 16));
		CHECK((tool::parseBlockSize("1M", l) && l == 20));
		CHECK((tool::parseBlockSize("4m", l) && l == 22));
		CHECK((tool::parseBlockSize("4096", l) && l == 12));
		CHECK((tool::parseBlockSize("4K", l) && l == 12));
		for (const char* bad : {"", "K", "2K", "8M", "3K", "100000", "64Q", "-64K", "64 K"})
		{
			CAPTURE(bad);
			CHECK_FALSE(tool::parseBlockSize(bad, l));
		}
		CHECK(tool::formatBlockSize(65536) == "64K");
		CHECK(tool::formatBlockSize(1u << 20) == "1M");
	}

	TEST_CASE("pack --block-size: default, per-pattern overrides, list/info output, errors")
	{
		test::TempDir dir;
		dir.write("src/a.lua", test::str(textLike(300000, 1)));
		dir.write("src/music/x.ogg", test::str(textLike(300000, 2)));
		dir.write("src/music/y.txt", test::str(textLike(300000, 3)));
		tool::PackOptions po;
		po.inputDir = dir.sub("src");
		po.outputPath = dir.sub("o.vfs");
		po.key.useEnv = false;
		po.compressAll = true;
		po.blockSizes = {"16K", "*.ogg=1M", "music/**=256K", "music/x.ogg=4M"};
		std::ostringstream o, e;
		REQUIRE(tool::runPack(po, o, e) == 0);
		auto b = ArchiveBackend::create(dir.sub("o.vfs"));
		REQUIRE(b);
		CHECK(b->index()->findFile("a.lua")->blockLog2 == 14);
		CHECK(b->index()->findFile("music/x.ogg")->blockLog2 == 22);   // last matching rule wins
		CHECK(b->index()->findFile("music/y.txt")->blockLog2 == 18);

		tool::ListOptions lo;
		lo.archive.archive = dir.sub("o.vfs");
		lo.archive.key.useEnv = false;
		lo.longFormat = true;
		std::ostringstream lout, lerr;
		REQUIRE(tool::runList(lo, lout, lerr) == 0);
		CHECK(lout.str().find(" 16K ") != std::string::npos);
		CHECK(lout.str().find(" 4M ") != std::string::npos);
		tool::InfoOptions io;
		io.archive = lo.archive;
		std::ostringstream iout, ierr;
		REQUIRE(tool::runInfo(io, iout, ierr) == 0);
		CHECK(iout.str().find("block size") != std::string::npos);

		for (const char* bad : {"3K", "8M", "=64K", "*.ogg=", "*.ogg=abc"})
		{
			CAPTURE(bad);
			po.blockSizes = {bad};
			std::ostringstream o2, e2;
			CHECK(tool::runPack(po, o2, e2) == static_cast<int>(tool::ExitCode::USAGE));
			CHECK(e2.str().find("--block-size") != std::string::npos);
		}
	}
}
