// Asset backend (AssetBackend) tests: a simulated IAssetProvider covers all of the logic on desktop platforms.
// Behavior follows AAssetManager semantics (opening a directory fails, listing a directory returns only files, an uncompressed asset can yield a descriptor).
// A real AAssetManager on Android needs an APK; see the notes in docs/integration-axmol.md.
#include "TestUtils.h"
#include "vfs/VirtualFileSystem.h"
#include "vfs/android/AssetBackend.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <thread>

#if VFS_PLATFORM_POSIX
#    include <fcntl.h>
#    include <unistd.h>
#endif

using namespace vfs;

namespace
{

struct MockAsset
{
	std::vector<uint8_t> data;
	bool compressed = false;
	int64_t blobOffset = -1;   // Position of an uncompressed asset inside the blob file
};

// Simulated AAsset: it has its own cursor.
class MockAssetFile : public IAssetFile
{
public:
	MockAssetFile(const MockAsset* asset, std::string blobPath, std::atomic<int>* fdCount)
		: m_asset(asset), m_blob(std::move(blobPath)), m_fdCount(fdCount) {}

	int read(void* buf, size_t size) override
	{
		size_t avail = m_pos < static_cast<int64_t>(m_asset->data.size()) ? m_asset->data.size() - static_cast<size_t>(m_pos) : 0;
		size_t n = std::min(avail, size);
		if (n)
			std::memcpy(buf, m_asset->data.data() + m_pos, n);
		m_pos += static_cast<int64_t>(n);
		return static_cast<int>(n);
	}

	int64_t seek(int64_t offset, int whence) override
	{
		int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? m_pos : length();
		if (base + offset < 0)
			return -1;
		m_pos = base + offset;
		return m_pos;
	}

	int64_t length() const override { return static_cast<int64_t>(m_asset->data.size()); }

	int openFileDescriptor(int64_t& start, int64_t& len) override
	{
#if VFS_PLATFORM_POSIX
		if (m_asset->compressed || m_blob.empty())
			return -1;
		int fd = ::open(m_blob.c_str(), O_RDONLY);
		if (fd >= 0)
			m_fdCount->fetch_add(1);
		start = m_asset->blobOffset;
		len = length();
		return fd;
#else
		(void)start;
		(void)len;
		return -1;
#endif
	}

private:
	const MockAsset* m_asset;
	std::string m_blob;
	std::atomic<int>* m_fdCount;
	int64_t m_pos = 0;
};

// Simulated AAssetManager: path -> asset; uncompressed assets are written in order into one blob file (garbage data is left in front to verify the start offset).
class MockProvider : public IAssetProvider
{
public:
	explicit MockProvider(std::map<std::string, MockAsset> assets, const test::TempDir* dir = nullptr)
		: m_assets(std::move(assets))
	{
		if (!dir)
			return;
		std::vector<uint8_t> blob(37, 0xCC);
		for (auto& [path, a] : m_assets)
			if (!a.compressed)
			{
				a.blobOffset = static_cast<int64_t>(blob.size());
				blob.insert(blob.end(), a.data.begin(), a.data.end());
				blob.insert(blob.end(), 11, 0xEE);
			}
		dir->write("blob.apk", blob);
		m_blob = dir->sub("blob.apk");
	}

	std::unique_ptr<IAssetFile> open(std::string_view path) override
	{
		opens.fetch_add(1);
		auto it = m_assets.find(std::string(path));
		return it == m_assets.end() ? nullptr : std::make_unique<MockAssetFile>(&it->second, m_blob, &fds);
	}

	bool listFiles(std::string_view dir, std::vector<std::string>& names) override
	{
		std::string prefix(dir);
		if (!prefix.empty())
			prefix.push_back('/');
		for (const auto& [path, a] : m_assets)
			if (path.compare(0, prefix.size(), prefix) == 0 && path.find('/', prefix.size()) == std::string::npos)
				names.push_back(path.substr(prefix.size()));
		return !names.empty();
	}

	std::atomic<int> opens{0};
	std::atomic<int> fds{0};

private:
	std::map<std::string, MockAsset> m_assets;
	std::string m_blob;
};

MockAsset asset(std::string_view text, bool compressed = false)
{
	return MockAsset{test::bytes(text), compressed, -1};
}

std::vector<std::string> names(const IBackend& b, std::string_view dir, Error* err = nullptr)
{
	std::vector<std::string> out;
	Error e = b.list(dir, [&](const DirEntry& d) {
		out.push_back(d.name + (d.type == FileType::DIR ? "/" : ""));
		return true;
	});
	if (err)
		*err = e;
	std::sort(out.begin(), out.end());
	return out;
}

std::map<std::string, MockAsset> sampleAssets()
{
	return {
		{"config.json", asset("{}")},
		{"scripts/main.lua", asset("print(1)", true)},
		{"scripts/lib/util.lua", asset("return {}")},
		{"deep/only/sub/file.txt", asset("deep")},   // deep/ and deep/only/ contain only subdirectories
		{"res/ui/title.txt", asset("Title")},
		{"res/a.txt", asset("A")},
		{"assets.lst", asset("# manifest\nconfig.json\nscripts/main.lua\r\nscripts/lib/util.lua\n\ndeep/only/sub/file.txt\nres/ui/title.txt\nres/a.txt\n")},
		{"res/res.lst", asset("ui/title.txt\na.txt\n")},
	};
}

}   // namespace

TEST_SUITE("asset")
{
	TEST_CASE("probe mode: stat, open and list follow AAssetManager semantics")
	{
		auto p = std::make_shared<MockProvider>(sampleAssets());
		Error err = Error::IO_ERROR;
		auto b = AssetBackend::create(p, {}, &err);
		REQUIRE(b);
		CHECK(err == Error::OK);
		CHECK_FALSE(b->hasManifest());
		CHECK(b->capabilities() == BackendCaps::Read);

		Stat st;
		REQUIRE(b->stat("config.json", st) == Error::OK);
		CHECK(st.type == FileType::FILE);
		CHECK(st.size == 2);
		REQUIRE(b->stat("scripts", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		REQUIRE(b->stat("", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		CHECK(b->stat("missing.txt", st) == Error::NOT_FOUND);
		CHECK(b->stat("config.json/x", st) == Error::NOT_DIR);
		// Known limitation: a directory that contains only subdirectories is invisible in probe mode.
		CHECK(b->stat("deep", st) == Error::NOT_FOUND);
		REQUIRE(b->stat("deep/only/sub", st) == Error::OK);

		Error e = Error::OK;
		CHECK_FALSE(b->open("scripts", IFileStream::Mode::READ, &e));
		CHECK(e == Error::IS_DIR);
		CHECK_FALSE(b->open("", IFileStream::Mode::READ, &e));
		CHECK(e == Error::IS_DIR);
		CHECK_FALSE(b->open("nope", IFileStream::Mode::READ, &e));
		CHECK(e == Error::NOT_FOUND);
		CHECK_FALSE(b->open("config.json/x", IFileStream::Mode::READ, &e));
		CHECK(e == Error::NOT_DIR);
		for (auto mode : {IFileStream::Mode::WRITE, IFileStream::Mode::APPEND, IFileStream::Mode::OVERLAPPED})
		{
			CHECK_FALSE(b->open("config.json", mode, &e));
			CHECK(e == Error::READ_ONLY);
		}

		CHECK(names(*b, "scripts", &e) == std::vector<std::string>{"main.lua"});   // The subdirectory lib is not visible.
		CHECK(e == Error::OK);
		CHECK(names(*b, "", &e) == std::vector<std::string>{"assets.lst", "config.json"});
		CHECK(names(*b, "config.json", &e).empty());
		CHECK(e == Error::NOT_DIR);
		CHECK(names(*b, "nope", &e).empty());
		CHECK(e == Error::NOT_FOUND);
		CHECK(b->createDir("x") == Error::READ_ONLY);
		CHECK(b->removeFile("config.json") == Error::READ_ONLY);
	}

	TEST_CASE("manifest mode: exact directory tree including directories that only contain directories")
	{
		auto p = std::make_shared<MockProvider>(sampleAssets());
		AssetOptions o;
		o.manifest = "assets.lst";
		auto b = AssetBackend::create(p, o);
		REQUIRE(b);
		CHECK(b->hasManifest());

		Stat st;
		REQUIRE(b->stat("deep", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		REQUIRE(b->stat("scripts/lib/util.lua", st) == Error::OK);
		CHECK(st.size == 9);
		CHECK(b->stat("assets.lst", st) == Error::NOT_FOUND);   // A file that is not in the manifest is invisible.
		CHECK(b->stat("config.json/x", st) == Error::NOT_DIR);
		CHECK(b->stat("nope/x", st) == Error::NOT_FOUND);

		Error e;
		CHECK(names(*b, "", &e) == std::vector<std::string>{"config.json", "deep/", "res/", "scripts/"});
		CHECK(names(*b, "scripts") == std::vector<std::string>{"lib/", "main.lua"});
		CHECK(names(*b, "deep/only") == std::vector<std::string>{"sub/"});
		CHECK(names(*b, "config.json", &e).empty());
		CHECK(e == Error::NOT_DIR);
		CHECK_FALSE(b->open("deep", IFileStream::Mode::READ, &e));
		CHECK(e == Error::IS_DIR);

		auto s = b->open("deep/only/sub/file.txt", IFileStream::Mode::READ, &e);
		REQUIRE(s);
		CHECK(test::str(test::readStream(*s)) == "deep");
	}

	TEST_CASE("manifest: missing manifest falls back to probing; invalid manifests are rejected")
	{
		AssetOptions o;
		o.manifest = "no-such.lst";
		auto b = AssetBackend::create(std::make_shared<MockProvider>(sampleAssets()), o);
		REQUIRE(b);
		CHECK_FALSE(b->hasManifest());

		for (const char* bad : {"a/../../x\n", "a.txt\na.txt/b\n", "a/b\na\n"})
		{
			CAPTURE(bad);
			Error err = Error::OK;
			o.manifest = "m.lst";
			CHECK_FALSE(AssetBackend::create(std::make_shared<MockProvider>(std::map<std::string, MockAsset>{{"m.lst", asset(bad)}}), o, &err));
			CHECK(err == Error::CORRUPTED);
		}
		Error err = Error::OK;
		CHECK_FALSE(AssetBackend::create(nullptr, {}, &err));
		CHECK(err == Error::UNSUPPORTED);
		o = {};
		o.root = "../x";
		CHECK_FALSE(AssetBackend::create(std::make_shared<MockProvider>(sampleAssets()), o, &err));
		CHECK(err == Error::INVALID_PATH);
	}

	TEST_CASE("root option mounts a subdirectory of the assets, with and without a manifest")
	{
		for (bool manifest : {false, true})
		{
			CAPTURE(manifest);
			AssetOptions o;
			o.root = "res";
			if (manifest)
				o.manifest = "res.lst";
			auto b = AssetBackend::create(std::make_shared<MockProvider>(sampleAssets()), o);
			REQUIRE(b);
			CHECK(b->hasManifest() == manifest);
			Stat st;
			REQUIRE(b->stat("ui/title.txt", st) == Error::OK);
			CHECK(st.size == 5);
			CHECK(b->stat("config.json", st) == Error::NOT_FOUND);
			CHECK(names(*b, "") == (manifest ? std::vector<std::string>{"a.txt", "ui/"} : std::vector<std::string>{"a.txt", "res.lst"}));
		}
	}

	TEST_CASE("streams: compressed assets use the AAsset cursor, uncompressed ones read the descriptor at an offset")
	{
		test::TempDir dir;
		auto big = test::randomBytes(200000, 7);
		std::map<std::string, MockAsset> assets{{"plain.bin", MockAsset{big, false, -1}}, {"packed.bin", MockAsset{big, true, -1}}};
		auto p = std::make_shared<MockProvider>(assets, &dir);
		auto b = AssetBackend::create(p);
		REQUIRE(b);
		for (const char* name : {"plain.bin", "packed.bin"})
		{
			CAPTURE(name);
			auto s = b->open(name, IFileStream::Mode::READ, nullptr);
			REQUIRE(s);
			CHECK(s->size() == static_cast<int64_t>(big.size()));
			CHECK(test::readStream(*s) == big);
			// Random positional read
			for (int64_t off : {int64_t(0), int64_t(1), int64_t(4095), int64_t(199990), int64_t(123457)})
			{
				REQUIRE(s->seek(off, SEEK_SET) == off);
				uint8_t buf[32];
				int n = s->read(buf, sizeof(buf));
				int expect = static_cast<int>(std::min<int64_t>(32, static_cast<int64_t>(big.size()) - off));
				REQUIRE(n == expect);
				CHECK(std::memcmp(buf, big.data() + off, static_cast<size_t>(n)) == 0);
				CHECK(s->tell() == off + n);
			}
			CHECK(s->seek(-1, SEEK_SET) == -1);
			CHECK(s->seek(10, SEEK_END) == static_cast<int64_t>(big.size()) + 10);
			uint8_t c;
			CHECK(s->read(&c, 1) == 0);
			CHECK(s->write("x", 1) == -1);
			CHECK(errno == EROFS);
			CHECK(s->close() == 0);
			CHECK_FALSE(s->isOpen());
			CHECK(s->read(&c, 1) == -1);
		}
#if VFS_PLATFORM_POSIX
		CHECK(p->fds.load() == 1);   // Only uncompressed assets use a descriptor.
#endif
	}

	TEST_CASE("concurrent reads: one asset object per stream, 8 threads read the same assets")
	{
		test::TempDir dir;
		std::map<std::string, MockAsset> assets;
		std::vector<std::vector<uint8_t>> content;
		for (int i = 0; i < 16; ++i)
		{
			content.push_back(test::randomBytes(4096 + static_cast<size_t>(i) * 97, static_cast<uint64_t>(i)));
			assets.emplace("f" + std::to_string(i), MockAsset{content.back(), i % 2 == 0, -1});
		}
		auto b = AssetBackend::create(std::make_shared<MockProvider>(assets, &dir));
		REQUIRE(b);
		VirtualFileSystem fs;
		REQUIRE(fs.mount("/res", b) != 0);
		std::atomic<int> bad{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 8; ++t)
			threads.emplace_back([&, t] {
				for (int round = 0; round < 50; ++round)
				{
					int i = (t + round) % 16;
					std::vector<uint8_t> out;
					if (fs.readFile("/res/f" + std::to_string(i), out) != Error::OK || out != content[static_cast<size_t>(i)])
						bad.fetch_add(1);
				}
			});
		for (auto& th : threads)
			th.join();
		CHECK(bad.load() == 0);
	}

	TEST_CASE("mounted under a writable memory layer: writes copy up, the asset stays untouched")
	{
		AssetOptions o;
		o.manifest = "assets.lst";
		auto b = AssetBackend::create(std::make_shared<MockProvider>(sampleAssets()), o);
		REQUIRE(b);
		VirtualFileSystem fs;
		REQUIRE(fs.mount("/res", b) != 0);
		REQUIRE(fs.mountMemory("/res", MountOptions{1, MountFlags::WriteTarget}) != 0);
		CHECK(test::readAll(fs, "/res/scripts/main.lua") == "print(1)");
		REQUIRE(fs.writeFile("/res/scripts/main.lua", "patched") == Error::OK);
		CHECK(test::readAll(fs, "/res/scripts/main.lua") == "patched");
		auto st = b->open("scripts/main.lua", IFileStream::Mode::READ, nullptr);
		REQUIRE(st);
		CHECK(test::str(test::readStream(*st)) == "print(1)");
		CHECK(test::listNames(fs, "/res/deep") == std::vector<std::string>{"only/"});
	}
}

TEST_SUITE("asset")
{
	TEST_CASE("a .vfs archive stored in the assets can be mounted through archiveSource (descriptor or stream)")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("g.vfs"), {{"scripts/main.lua", test::bytes("print(42)"), {}}, {"big.bin", test::randomBytes(100000, 3), {}}}) == Error::OK);
		auto archive = test::readNative(dir.sub("g.vfs"));
		for (bool compressed : {false, true})
		{
			CAPTURE(compressed);
			auto p = std::make_shared<MockProvider>(std::map<std::string, MockAsset>{{"game.vfs", MockAsset{archive, compressed, -1}}, {"dir/x", asset("x")}}, &dir);
			auto b = AssetBackend::create(p);
			REQUIRE(b);
			Error err = Error::OK;
			auto src = b->archiveSource("game.vfs", &err);
			REQUIRE(src);
			CHECK(err == Error::OK);
#if VFS_PLATFORM_POSIX
			CHECK(src->supportsReadAt() == !compressed);
#endif
			auto ab = ArchiveBackend::create(src, {}, &err);
			REQUIRE(ab);
			VirtualFileSystem fs;
			REQUIRE(fs.mount("/res", ab) != 0);
			CHECK(test::readAll(fs, "/res/scripts/main.lua") == "print(42)");
			std::vector<uint8_t> big;
			REQUIRE(fs.readFile("/res/big.bin", big) == Error::OK);
			CHECK(big == test::randomBytes(100000, 3));
			CHECK_FALSE(b->archiveSource("nope.vfs", &err));
			CHECK(err == Error::NOT_FOUND);
			CHECK_FALSE(b->archiveSource("dir", &err));
			CHECK(err == Error::IS_DIR);
		}
		AssetBackend raw(std::make_shared<MockProvider>(std::map<std::string, MockAsset>{{"game.vfs", MockAsset{archive, true, -1}}}), "");
		Error err = Error::OK;
		CHECK_FALSE(raw.archiveSource("game.vfs", &err));
		CHECK(err == Error::UNSUPPORTED);
	}
}
