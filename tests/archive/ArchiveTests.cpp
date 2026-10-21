// .vfs 归档测试：往返、可复现、对齐、去重、空目录、密钥、损坏检测、版本不匹配、附加到其他文件、写入端错误
#include "TestUtils.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/archive/ArchiveFormat.h"
#include "vfs/archive/ArchiveReader.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/archive/ByteIO.h"
#include "vfs/crypto/Hash.h"
#include "vfs/memory/MemoryBackend.h"
#include <algorithm>
#include <cstring>
#include <set>

using namespace vfs;
using Names = std::vector<std::string>;

namespace
{

FileWriteOptions opt(bool encrypt, bool compress)
{
	FileWriteOptions o;
	o.encrypt = encrypt;
	o.compress = compress;
	return o;
}

// 可压缩的文本内容
std::vector<uint8_t> textContent(size_t lines, int seed)
{
	std::string s;
	for (size_t i = 0; i < lines; ++i)
		s += "line " + std::to_string((i * 7 + static_cast<size_t>(seed)) % 50) + " of some lua script\n";
	return test::bytes(s);
}

// 一组覆盖各种组合的样例文件
std::vector<test::ArchiveFile> sampleFiles()
{
	return {
		{"scripts/main.lua", textContent(500, 1), opt(false, true)},
		{"scripts/util.lua", textContent(300, 2), opt(true, true)},
		{"images/bg.png", test::randomBytes(20000, 3), opt(true, false)},
		{"images/icon.png", test::randomBytes(777, 4), opt(false, false)},
		{"中文/配置.json", test::bytes("{\"语言\":\"中文\"}"), opt(true, false)},
		{"empty.txt", {}, opt(true, true)},
		{"random-compress.bin", test::randomBytes(5000, 5), opt(false, true)},   // 压不小，存原文
		{"a/b/c/d/deep.txt", test::bytes("deep"), opt(false, false)},
	};
}

std::shared_ptr<ArchiveBackend> openOk(const std::string& path, const ArchiveOptions& o = {})
{
	Error err = Error::OK;
	auto b = ArchiveBackend::create(path, o, &err);
	CHECK(err == Error::OK);
	REQUIRE(b);
	return b;
}

Error openErr(const std::vector<uint8_t>& data, const ArchiveOptions& o = {})
{
	Error err = Error::OK;
	auto b = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(data), o, &err);
	if (b)
		return Error::OK;
	CHECK(err != Error::OK);
	return err;
}

std::vector<uint8_t> readEntryVia(ArchiveBackend& b, std::string_view rel)
{
	auto s = b.open(rel, IFileStream::Mode::READ, nullptr);
	REQUIRE(s);
	return test::readStream(*s);
}

void fixHeaderChecksum(std::vector<uint8_t>& data, size_t base = 0)
{
	uint64_t c = xxh3_64(data.data() + base, ArchiveFormat::H_CHECKSUM);
	ByteIO::writeU64LE(data.data() + base + ArchiveFormat::H_CHECKSUM, c);
}

} // namespace

TEST_SUITE("archive")
{
	TEST_CASE("round trip: content, sizes and listings are correct for every encryption/compression combination")
	{
		test::TempDir dir;
		const auto files = sampleFiles();
		ArchiveWriteStats stats;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), files, {}, &stats) == Error::OK);
		CHECK(stats.fileCount == files.size());
		CHECK(stats.encryptedCount == 3);   // 空文件即使要求加密也不加密（没有数据）
		CHECK(stats.compressedCount == 2);   // main.lua、util.lua；random 和 empty 不压缩
		CHECK(stats.archiveSize == test::readNative(dir.sub("a.vfs")).size());

		auto b = openOk(dir.sub("a.vfs"));
		for (const auto& f : files)
		{
			CAPTURE(f.path);
			Stat st;
			REQUIRE(b->stat(f.path, st) == Error::OK);
			CHECK(st.type == FileType::FILE);
			CHECK(st.size == static_cast<int64_t>(f.content.size()));
			CHECK_FALSE(st.writable);
			CHECK(readEntryVia(*b, f.path) == f.content);

			std::vector<uint8_t> direct;
			const ArchiveEntry* e = b->index()->findFile(f.path);
			REQUIRE(e);
			REQUIRE(ArchiveReader::readEntry(*b->index(), *e, direct) == Error::OK);
			CHECK(direct == f.content);
		}

		VirtualFileSystem fs;
		REQUIRE(fs.mount("/", b) != 0);
		CHECK(test::listNames(fs, "/") == Names{"a/", "empty.txt", "images/", "random-compress.bin", "scripts/", "中文/"});
		CHECK(test::listNames(fs, "/a/b/c") == Names{"d/"});
		CHECK(fs.isDir("/a/b"));
		CHECK(fs.writeFile("/new.txt", "x") == Error::READ_ONLY);
		CHECK(fs.removeFile("/empty.txt") == Error::READ_ONLY);

		CHECK(b->verifyAll() == Error::OK);
		CHECK((b->capabilities() & BackendCaps::Write) == 0);
		Error err = Error::OK;
		CHECK(b->open("missing", IFileStream::Mode::READ, &err) == nullptr);
		CHECK(err == Error::NOT_FOUND);
		CHECK(b->open("images", IFileStream::Mode::READ, &err) == nullptr);
		CHECK(err == Error::IS_DIR);
		CHECK(b->open("empty.txt/x", IFileStream::Mode::READ, &err) == nullptr);
		CHECK(err == Error::NOT_DIR);
	}

	TEST_CASE("reproducible: same input (any add order) produces byte-identical files")
	{
		test::TempDir dir;
		auto files = sampleFiles();
		REQUIRE(test::buildArchive(dir.sub("1.vfs"), files) == Error::OK);
		std::reverse(files.begin(), files.end());
		REQUIRE(test::buildArchive(dir.sub("2.vfs"), files) == Error::OK);
		const auto a = test::readNative(dir.sub("1.vfs"));
		const auto b = test::readNative(dir.sub("2.vfs"));
		CHECK(a.size() > 0);
		CHECK(a == b);

		// 内容变化一个字节，文件就不同
		files[0].content.push_back('!');
		REQUIRE(test::buildArchive(dir.sub("3.vfs"), files) == Error::OK);
		CHECK(test::readNative(dir.sub("3.vfs")) != a);
	}

	TEST_CASE("alignment: every data block starts aligned; 1 means unaligned; non-powers of two are rejected")
	{
		test::TempDir dir;
		const auto files = sampleFiles();
		uint64_t prevSize = 0;
		for (uint32_t align : {1u, 16u, 4096u})
		{
			CAPTURE(align);
			ArchiveWriteOptions o;
			o.alignment = align;
			ArchiveWriteStats stats;
			const std::string path = dir.sub("a" + std::to_string(align) + ".vfs");
			REQUIRE(test::buildArchive(path, files, o, &stats) == Error::OK);
			auto b = openOk(path);
			CHECK(b->index()->header.alignment == align);
			for (const auto& e : b->index()->entries)
				if (e.type == EntryType::FILE && e.storedSize > 0)
					CHECK(e.offset % align == 0);
			CHECK(stats.archiveSize >= prevSize);
			prevSize = stats.archiveSize;
			CHECK(b->verifyAll() == Error::OK);
		}

		for (uint32_t bad : {0u, 3u, 100u, 131072u})
		{
			ArchiveWriteOptions o;
			o.alignment = bad;
			CHECK(test::buildArchive(dir.sub("bad.vfs"), files, o) == Error::UNSUPPORTED);
		}
	}

	TEST_CASE("dedup: identical content is stored once; different encryption is not shared; can be disabled")
	{
		test::TempDir dir;
		const auto content = test::randomBytes(10000, 11);
		std::vector<test::ArchiveFile> files = {
			{"a.bin", content, opt(false, false)},
			{"b.bin", content, opt(false, false)},
			{"c/copy.bin", content, opt(false, false)},
			{"enc.bin", content, opt(true, false)},
			{"enc2.bin", content, opt(true, false)},
		};
		ArchiveWriteStats stats;
		REQUIRE(test::buildArchive(dir.sub("d.vfs"), files, {}, &stats) == Error::OK);
		CHECK(stats.dedupCount == 3);
		auto b = openOk(dir.sub("d.vfs"));
		const auto* a = b->index()->findFile("a.bin");
		const auto* bb = b->index()->findFile("b.bin");
		const auto* e = b->index()->findFile("enc.bin");
		const auto* e2 = b->index()->findFile("enc2.bin");
		CHECK(a->offset == bb->offset);
		CHECK(a->offset != e->offset);
		CHECK(e->offset == e2->offset);
		for (const auto& f : files)
			CHECK(readEntryVia(*b, f.path) == content);
		CHECK(b->verifyAll() == Error::OK);

		ArchiveWriteOptions o;
		o.dedup = false;
		ArchiveWriteStats stats2;
		REQUIRE(test::buildArchive(dir.sub("nd.vfs"), files, o, &stats2) == Error::OK);
		CHECK(stats2.dedupCount == 0);
		CHECK(stats2.archiveSize > stats.archiveSize + 3 * content.size() - 100);
	}

	TEST_CASE("empty directories: dropped by default, kept when added with addDir; addDir of non-empty dirs is merged")
	{
		test::TempDir dir;
		ArchiveWriteStats stats;
		REQUIRE(test::buildArchive(dir.sub("e.vfs"),
								   {{"keep/empty/", {}, {}}, {"a/", {}, {}}, {"a/x.txt", test::bytes("x"), {}}}, {},
								   &stats) == Error::OK);
		CHECK(stats.dirCount == 1);
		CHECK(stats.fileCount == 1);
		auto b = openOk(dir.sub("e.vfs"));
		CHECK(b->index()->entries.size() == 2);
		Stat st;
		REQUIRE(b->stat("keep/empty", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		REQUIRE(b->stat("keep", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		bool any = false;
		CHECK(b->list("keep/empty", [&any](const DirEntry&) { return any = true; }) == Error::OK);
		CHECK_FALSE(any);
	}

	TEST_CASE("an empty archive is valid")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("empty.vfs"), {}) == Error::OK);
		auto b = openOk(dir.sub("empty.vfs"));
		CHECK(b->index()->entries.empty());
		Stat st;
		CHECK(b->stat("", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		CHECK(b->verifyAll() == Error::OK);
	}

	TEST_CASE("index key: derived and stored by default; external unstored keys must be supplied; wrong keys give KEY_INVALID")
	{
		test::TempDir dir;
		const auto files = sampleFiles();
		const std::vector<uint8_t> key = test::randomBytes(32, 77);

		// 默认：加密索引，密钥存于文件，打开不需要密钥
		REQUIRE(test::buildArchive(dir.sub("default.vfs"), files) == Error::OK);
		auto b = openOk(dir.sub("default.vfs"));
		CHECK((b->index()->header.flags & ArchiveFlags::IndexEncrypted) != 0);
		CHECK((b->index()->header.flags & ArchiveFlags::KeyStored) != 0);
		// 索引被加密：文件名不以明文出现
		const auto raw = test::readNative(dir.sub("default.vfs"));
		const std::string name = "scripts/main.lua";
		CHECK(std::search(raw.begin(), raw.end(), name.begin(), name.end()) == raw.end());

		// 外部密钥，不存储
		ArchiveWriteOptions o;
		o.indexKey = key;
		REQUIRE(test::buildArchive(dir.sub("keyed.vfs"), files, o) == Error::OK);
		const auto keyed = test::readNative(dir.sub("keyed.vfs"));
		CHECK(openErr(keyed) == Error::KEY_REQUIRED);
		ArchiveOptions ro;
		ro.indexKey = test::randomBytes(32, 78);
		CHECK(openErr(keyed, ro) == Error::KEY_INVALID);
		ro.indexKey = std::vector<uint8_t>(16, 1);
		CHECK(openErr(keyed, ro) == Error::KEY_INVALID);
		ro.indexKey = key;
		CHECK(openErr(keyed, ro) == Error::OK);
		auto kb = openOk(dir.sub("keyed.vfs"), ro);
		CHECK(readEntryVia(*kb, "images/bg.png") == files[2].content);
		// 文件头里不应该出现密钥
		CHECK(std::search(keyed.begin(), keyed.end(), key.begin(), key.end()) == keyed.end());

		// 外部密钥并存储
		o.storeKey = true;
		REQUIRE(test::buildArchive(dir.sub("stored.vfs"), files, o) == Error::OK);
		CHECK(openErr(test::readNative(dir.sub("stored.vfs"))) == Error::OK);

		// 明文索引
		ArchiveWriteOptions plain;
		plain.encryptIndex = false;
		REQUIRE(test::buildArchive(dir.sub("plain.vfs"), files, plain) == Error::OK);
		auto pb = openOk(dir.sub("plain.vfs"));
		CHECK((pb->index()->header.flags & ArchiveFlags::IndexEncrypted) == 0);
		const auto praw = test::readNative(dir.sub("plain.vfs"));
		CHECK(std::search(praw.begin(), praw.end(), name.begin(), name.end()) != praw.end());

		// 写入端：密钥长度不对
		ArchiveWriteOptions badKey;
		badKey.indexKey = std::vector<uint8_t>(31, 0);
		CHECK(test::buildArchive(dir.sub("bad.vfs"), files, badKey) == Error::KEY_INVALID);
	}

	TEST_CASE("XOR: encrypted file data contains no plaintext; fileKeyLength is honoured")
	{
		test::TempDir dir;
		const auto content = textContent(100, 9);
		ArchiveWriteOptions o;
		o.fileKeyLength = 7;
		REQUIRE(test::buildArchive(dir.sub("x.vfs"), {{"f.txt", content, opt(true, false)}}, o) == Error::OK);
		const auto raw = test::readNative(dir.sub("x.vfs"));
		const std::string needle = "of some lua script";
		CHECK(std::search(raw.begin(), raw.end(), needle.begin(), needle.end()) == raw.end());
		auto b = openOk(dir.sub("x.vfs"));
		CHECK(b->index()->findFile("f.txt")->keyLength == 7);
		CHECK(b->index()->findFile("f.txt")->cipher == CipherType::XOR);
		CHECK(readEntryVia(*b, "f.txt") == content);

		for (uint32_t bad : {0u, 33u})
		{
			o.fileKeyLength = bad;
			CHECK(test::buildArchive(dir.sub("y.vfs"), {{"f.txt", content, opt(true, false)}}, o) != Error::OK);
		}
	}

	TEST_CASE("LZ4: compressed entries have a smaller storedSize; incompressible data is stored raw")
	{
		test::TempDir dir;
		const auto text = textContent(2000, 3);
		const auto noise = test::randomBytes(3000, 4);
		REQUIRE(test::buildArchive(dir.sub("z.vfs"), {{"t.txt", text, opt(false, true)}, {"n.bin", noise, opt(false, true)}}) ==
				Error::OK);
		auto b = openOk(dir.sub("z.vfs"));
		const auto* t = b->index()->findFile("t.txt");
		const auto* n = b->index()->findFile("n.bin");
		CHECK(t->compression == CompressionType::LZ4);
		CHECK(t->storedSize < t->size / 4);
		CHECK(n->compression == CompressionType::NONE);
		CHECK(n->storedSize == n->size);
		CHECK(readEntryVia(*b, "t.txt") == text);
		CHECK(readEntryVia(*b, "n.bin") == noise);
	}

	TEST_CASE("addFile streams from a native file (large, encrypted) with the same result as addData")
	{
		test::TempDir dir;
		const auto big = test::randomBytes(3 * 1024 * 1024 + 17, 21);
		dir.write("big.bin", big);
		ArchiveWriter w1;
		REQUIRE(w1.addFile("big.bin", dir.sub("big.bin"), opt(true, false)) == Error::OK);
		REQUIRE(w1.addFile("big-plain.bin", dir.sub("big.bin")) == Error::OK);
		REQUIRE(w1.write(dir.sub("s.vfs")) == Error::OK);
		ArchiveWriter w2;
		REQUIRE(w2.addData("big.bin", big, opt(true, false)) == Error::OK);
		REQUIRE(w2.addData("big-plain.bin", big) == Error::OK);
		REQUIRE(w2.write(dir.sub("m.vfs")) == Error::OK);
		CHECK(test::readNative(dir.sub("s.vfs")) == test::readNative(dir.sub("m.vfs")));

		auto b = openOk(dir.sub("s.vfs"));
		CHECK(readEntryVia(*b, "big.bin") == big);
		CHECK(b->verifyAll() == Error::OK);

		// 本机文件在 write 时才打开：不存在时 write 失败，且不留下输出文件
		ArchiveWriter w3;
		CHECK(w3.addFile("x", dir.sub("missing.bin")) == Error::OK);
		CHECK(w3.write(dir.sub("missing.vfs")) == Error::NOT_FOUND);
		CHECK_FALSE(dir.exists("missing.vfs"));
	}

	TEST_CASE("write to an IFileStream and mount from an in-memory source")
	{
		auto mem = MemoryBackend::create();
		auto out = mem->open("a.vfs", IFileStream::Mode::WRITE, nullptr);
		REQUIRE(out);
		ArchiveWriter w;
		REQUIRE(w.addData("x.txt", test::bytes("in memory")) == Error::OK);
		REQUIRE(w.write(*out) == Error::OK);
		out->seek(0, SEEK_SET);
		auto data = test::readStream(*out);
		VirtualFileSystem fs;
		auto b = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(data));
		REQUIRE(b);
		fs.mount("/", b);
		CHECK(test::readAll(fs, "/x.txt") == "in memory");
	}

	TEST_CASE("writer errors: duplicates, case conflicts, file/directory conflicts, invalid paths")
	{
		test::TempDir dir;
		CHECK(test::buildArchive(dir.sub("1.vfs"), {{"a.txt", {}, {}}, {"a.txt", {}, {}}}) == Error::EXISTS);
		CHECK(test::buildArchive(dir.sub("2.vfs"), {{"Readme.md", {}, {}}, {"README.md", {}, {}}}) == Error::EXISTS);
		CHECK(test::buildArchive(dir.sub("3.vfs"), {{"a", {}, {}}, {"a/b", {}, {}}}) == Error::NOT_DIR);
		CHECK(test::buildArchive(dir.sub("4.vfs"), {{"../x", {}, {}}}) == Error::INVALID_PATH);
		CHECK(test::buildArchive(dir.sub("5.vfs"), {{"", {}, {}}}) == Error::INVALID_PATH);
		ArchiveWriter w;
		CHECK(w.addData("a", {}) == Error::OK);
		CHECK(w.addData("a", {}) == Error::OK);   // 延迟到 write 时检查
		CHECK(w.write(dir.sub("6.vfs")) == Error::EXISTS);
		CHECK_FALSE(w.lastError().empty());
		CHECK_FALSE(dir.exists("6.vfs"));
		for (const auto& e : std::filesystem::directory_iterator(test::u8(dir.path())))
			CHECK(e.path().filename().string().rfind("6.vfs", 0) != 0);   // 也没有留下 6.vfs.<pid>-<n>.tmp
		// 前导 '/' 与反斜杠会被规范化
		CHECK(test::buildArchive(dir.sub("7.vfs"), {{"/x\\y.txt", test::bytes("1"), {}}}) == Error::OK);
		CHECK(openOk(dir.sub("7.vfs"))->index()->findFile("x/y.txt") != nullptr);
	}

	TEST_CASE("appended to another file: located via the footer or an explicit baseOffset")
	{
		test::TempDir dir;
		const auto files = sampleFiles();
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), files) == Error::OK);
		auto data = test::readNative(dir.sub("a.vfs"));
		std::vector<uint8_t> exe = test::bytes("MZ fake executable header .....");
		const size_t base = exe.size();
		exe.insert(exe.end(), data.begin(), data.end());
		test::writeNative(dir.sub("game.exe"), exe);

		auto b = openOk(dir.sub("game.exe"));
		CHECK(b->index()->baseOffset == static_cast<int64_t>(base));
		CHECK(readEntryVia(*b, "images/bg.png") == files[2].content);
		CHECK(readEntryVia(*b, "scripts/util.lua") == files[1].content);
		CHECK(b->verifyAll() == Error::OK);

		ArchiveOptions o;
		o.baseOffset = static_cast<int64_t>(base);
		CHECK(openErr(exe, o) == Error::OK);
		o.baseOffset = static_cast<int64_t>(base) + 1;
		CHECK(openErr(exe, o) == Error::CORRUPTED);

		// 归档后面还有多余数据（footer 不在末尾）时，起点 0 的仍可打开
		auto tail = data;
		tail.insert(tail.end(), 100, 0xAB);
		CHECK(openErr(tail) == Error::OK);
	}

	TEST_CASE("version mismatch: only one version number is checked")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), sampleFiles()) == Error::OK);
		auto data = test::readNative(dir.sub("a.vfs"));
		for (uint32_t v : {0u, 2u, 0xFFFFFFFFu})
		{
			auto copy = data;
			ByteIO::writeU32LE(copy.data() + ArchiveFormat::H_VERSION, v);
			CHECK(openErr(copy) == Error::VERSION_MISMATCH);
			fixHeaderChecksum(copy);
			CHECK(openErr(copy) == Error::VERSION_MISMATCH);
		}
	}

	TEST_CASE("corruption detection: any flipped header/index byte fails open; flipped data fails verify")
	{
		test::TempDir dir;
		ArchiveWriteOptions o;
		o.alignment = 1;   // 无填充字节，数据区每个字节都属于某个文件
		std::vector<test::ArchiveFile> files = {
			{"a.txt", test::bytes("hello world"), opt(false, false)},
			{"b.lua", textContent(20, 1), opt(true, true)},
			{"c.bin", test::randomBytes(300, 2), opt(true, false)},
		};
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), files, o) == Error::OK);
		const auto data = test::readNative(dir.sub("a.vfs"));
		auto good = ArchiveReader::open(std::make_shared<MemoryArchiveSource>(data));
		REQUIRE(good);
		const ArchiveHeader h = good->header;
		const size_t indexEnd = static_cast<size_t>(h.indexOffset + h.indexSize);
		REQUIRE(indexEnd + ArchiveFormat::FOOTER_SIZE == data.size());
		REQUIRE(h.dataOffset == ArchiveFormat::HEADER_SIZE);

		int openFailures = 0, verifyFailures = 0, harmless = 0;
		for (size_t i = 0; i < data.size(); ++i)
		{
			CAPTURE(i);
			auto copy = data;
			copy[i] ^= 0x01;
			Error err = Error::OK;
			auto index = ArchiveReader::open(std::make_shared<MemoryArchiveSource>(copy), {}, &err);
			if (i < ArchiveFormat::HEADER_SIZE || (i >= h.indexOffset && i < indexEnd))
			{
				REQUIRE(index == nullptr);
				CHECK((err == Error::CORRUPTED || err == Error::VERSION_MISMATCH));
				++openFailures;
			}
			else if (i < h.indexOffset)
			{
				REQUIRE(index != nullptr);
				Error v = ArchiveReader::verify(*index);
				if (v != Error::CORRUPTED)
				{
					// 极少数情况：翻转 LZ4 匹配偏移后恰好复制出相同的内容，解出的原文没变，不算损坏
					for (const auto& f : files)
					{
						std::vector<uint8_t> out;
						REQUIRE(ArchiveReader::readEntry(*index, *index->findFile(f.path), out) == Error::OK);
						REQUIRE(out == f.content);
					}
					++harmless;
					continue;
				}
				++verifyFailures;
			}
			else
			{
				// footer：起点 0 有 magic 时不依赖 footer
				CHECK(index != nullptr);
			}
		}
		CHECK(openFailures == static_cast<int>(ArchiveFormat::HEADER_SIZE + h.indexSize));
		CHECK(verifyFailures + harmless == static_cast<int>(h.dataSize));
		CHECK(harmless <= 2);
	}

	TEST_CASE("corruption detection: truncation to any length fails safely")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), sampleFiles()) == Error::OK);
		const auto data = test::readNative(dir.sub("a.vfs"));
		auto good = ArchiveReader::open(std::make_shared<MemoryArchiveSource>(data));
		REQUIRE(good);
		const size_t indexEnd = static_cast<size_t>(good->header.indexOffset + good->header.indexSize);
		for (size_t len = 0; len < data.size(); len += (len < 300 ? 1 : 97))
		{
			CAPTURE(len);
			std::vector<uint8_t> cut(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(len));
			Error err = Error::OK;
			auto index = ArchiveReader::open(std::make_shared<MemoryArchiveSource>(cut), {}, &err);
			// 只少了 footer 时仍可打开（magic 在起点）；否则必须失败
			if (len < indexEnd)
				CHECK(index == nullptr);
		}
	}

	TEST_CASE("random mutation (lightweight fuzzing instead of libFuzzer): no crash, no out-of-bounds")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), sampleFiles()) == Error::OK);
		const auto data = test::readNative(dir.sub("a.vfs"));
		uint64_t x = 0xC0FFEE;
		auto next = [&x]()
		{
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			return x;
		};
		int opened = 0;
		for (int iter = 0; iter < 400; ++iter)
		{
			auto copy = data;
			int flips = 1 + static_cast<int>(next() % 8);
			for (int k = 0; k < flips; ++k)
				copy[next() % copy.size()] = static_cast<uint8_t>(next());
			// 一部分用例重新计算 header 校验和，让解析继续往下走
			if (iter % 2 == 0)
				fixHeaderChecksum(copy);
			if (iter % 3 == 0)
			{
				// 也重新计算索引哈希，迫使条目解析处理垃圾数据（明文索引）
				ArchiveHeader h;
				if (h.parse(copy.data()) == Error::OK && h.indexOffset + h.indexSize <= copy.size())
				{
					uint64_t ih = xxh3_64(copy.data() + h.indexOffset, static_cast<size_t>(h.indexSize));
					ByteIO::writeU64LE(copy.data() + ArchiveFormat::H_INDEX_HASH, ih);
					fixHeaderChecksum(copy);
				}
			}
			auto b = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(copy));
			if (!b)
				continue;
			++opened;
			for (const auto& e : b->index()->entries)
			{
				if (e.type != EntryType::FILE)
					continue;
				auto s = b->open(e.path, IFileStream::Mode::READ, nullptr);
				if (s)
					test::readStream(*s);
			}
			b->verifyAll();
		}
		CHECK(opened > 0);
	}

	TEST_CASE("ArchiveHeader: serialize/parse round trip; unknown flags are rejected")
	{
		ArchiveHeader h;
		h.flags = ArchiveFlags::IndexEncrypted;
		h.entryCount = 3;
		h.dataOffset = 128;
		h.dataSize = 100;
		h.indexOffset = 228;
		h.indexSize = 3 * 96 + 10;
		h.keyCheck = 0x12345678;
		h.indexHash = 0xABCDEF;
		uint8_t buf[128];
		h.serialize(buf);
		CHECK(std::memcmp(buf, ArchiveFormat::MAGIC, 8) == 0);
		ArchiveHeader p;
		REQUIRE(p.parse(buf) == Error::OK);
		CHECK(p.entryCount == 3);
		CHECK(p.indexSize == h.indexSize);
		CHECK(p.keyCheck == h.keyCheck);
		CHECK(p.headerChecksum == h.headerChecksum);

		ByteIO::writeU32LE(buf + ArchiveFormat::H_FLAGS, 0x80000000u);
		std::vector<uint8_t> v(buf, buf + 128);
		fixHeaderChecksum(v);
		CHECK(p.parse(v.data()) == Error::CORRUPTED);
	}

	TEST_CASE("verify callback reports every file; entry hash is the low 64 bits of XXH3-128")
	{
		test::TempDir dir;
		const auto files = sampleFiles();
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), files) == Error::OK);
		auto index = ArchiveReader::open(dir.sub("a.vfs"));
		REQUIRE(index);
		std::set<std::string> seen;
		int bad = 0;
		const Error result = ArchiveReader::verify(*index, [&seen, &bad](const ArchiveEntry& e, Error r)
		{
			if (r != Error::OK)
				++bad;
			seen.insert(e.path);
		});
		CHECK(result == Error::OK);
		CHECK(bad == 0);
		CHECK(seen.size() == files.size());
		for (const auto& f : files)
			CHECK(index->findFile(f.path)->hash == xxh3_128(f.content.data(), f.content.size()).low);
	}

	TEST_CASE("mtime: 0 by default for reproducibility; written to the index when specified")
	{
		test::TempDir dir;
		FileWriteOptions o;
		o.mtime = 1700000000;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), {{"t.txt", test::bytes("x"), o}, {"z.txt", test::bytes("y"), {}}}) ==
				Error::OK);
		auto b = openOk(dir.sub("a.vfs"));
		Stat st;
		REQUIRE(b->stat("t.txt", st) == Error::OK);
		CHECK(st.mtime == 1700000000);
		// 索引中存 0；stat 时 0 表示未知，回退为归档文件自身的 mtime
		CHECK(b->index()->findFile("z.txt")->mtime == 0);
		REQUIRE(b->stat("z.txt", st) == Error::OK);
		CHECK(st.mtime == b->index()->sourceMtime);
	}
}
