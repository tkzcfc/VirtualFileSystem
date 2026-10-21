// IFileStream conformance tests: the same cases run against every backend.
// Read-only cases cover every backend; write cases cover only writable backends.
#include "TestUtils.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/memory/MemoryBackend.h"
#include "vfs/native/NativeFileStream.h"
#include <cstring>

using namespace vfs;
using Mode = IFileStream::Mode;

namespace
{

// ---------------------------------------------------------------- writable backends

// Uses NativeFileStream directly.
struct NativeFx
{
	test::TempDir dir;

	std::string file() const { return dir.sub("目录/file.bin"); }

	void prepare(const std::vector<uint8_t>& content) { dir.write("目录/file.bin", content); }

	void removeFile()
	{
		std::error_code ec;
		std::filesystem::remove(test::u8(file()), ec);
		dir.mkdir("目录");
	}

	std::unique_ptr<IFileStream> open(Mode mode)
	{
		auto s = std::make_unique<NativeFileStream>();
		if (!s->open(file(), mode))
			return nullptr;
		return s;
	}

	std::vector<uint8_t> contents() { return test::readNative(file()); }
};

// Uses MemoryBackend directly.
struct MemoryFx
{
	std::shared_ptr<MemoryBackend> backend = MemoryBackend::create();

	void prepare(const std::vector<uint8_t>& content) { REQUIRE(backend->addFile("目录/file.bin", content) == Error::OK); }

	void removeFile()
	{
		backend->removeFile("目录/file.bin");
		backend->createDir("目录");
	}

	std::unique_ptr<IFileStream> open(Mode mode) { return backend->open("目录/file.bin", mode, nullptr); }

	std::vector<uint8_t> contents()
	{
		auto s = backend->open("目录/file.bin", Mode::READ, nullptr);
		REQUIRE(s);
		return test::readStream(*s);
	}
};

// Native directory through VirtualFileSystem (wrapped by TrackedFileStream).
struct VfsNativeFx
{
	test::TempDir dir;
	VirtualFileSystem fs;

	VfsNativeFx()
	{
		dir.mkdir("root/目录");
		REQUIRE(fs.mountNative("/mnt", dir.sub("root")) != 0);
	}

	void prepare(const std::vector<uint8_t>& content) { dir.write("root/目录/file.bin", content); }

	void removeFile() { fs.removeFile("/mnt/目录/file.bin"); }

	std::unique_ptr<IFileStream> open(Mode mode) { return fs.openFileStream("/mnt/目录/file.bin", mode); }

	std::vector<uint8_t> contents()
	{
		std::vector<uint8_t> out;
		REQUIRE(fs.readFile("/mnt/目录/file.bin", out) == Error::OK);
		return out;
	}
};

// Memory backend through VirtualFileSystem.
struct VfsMemoryFx
{
	VirtualFileSystem fs;
	std::shared_ptr<MemoryBackend> backend;

	VfsMemoryFx()
	{
		REQUIRE(fs.mountMemory("/", {}, &backend) != 0);
		backend->createDir("目录");
	}

	void prepare(const std::vector<uint8_t>& content) { REQUIRE(fs.writeFile("/目录/file.bin", content.data(), content.size()) == Error::OK); }

	void removeFile() { fs.removeFile("/目录/file.bin"); }

	std::unique_ptr<IFileStream> open(Mode mode) { return fs.openFileStream("/目录/file.bin", mode); }

	std::vector<uint8_t> contents()
	{
		std::vector<uint8_t> out;
		REQUIRE(fs.readFile("/目录/file.bin", out) == Error::OK);
		return out;
	}
};

// Single-file mount (mountFile).
struct SingleFileFx
{
	test::TempDir dir;
	VirtualFileSystem fs;

	void prepare(const std::vector<uint8_t>& content)
	{
		dir.write("one.bin", content);
		REQUIRE(fs.mountFile("/config/one.bin", dir.sub("one.bin")) != 0);
	}

	std::unique_ptr<IFileStream> open(Mode mode) { return fs.openFileStream("/config/one.bin", mode); }
};

// ---------------------------------------------------------------- archive backends

template <bool Encrypt, bool Compress, bool InMemory, bool ViaVfs>
struct ArchiveFx
{
	test::TempDir dir;
	std::shared_ptr<ArchiveBackend> backend;
	VirtualFileSystem fs;

	void prepare(const std::vector<uint8_t>& content)
	{
		FileWriteOptions fo;
		fo.encrypt = Encrypt;
		fo.compress = Compress;
		// Add a few more files so the target file is not at the start of the data region.
		std::vector<test::ArchiveFile> files = {
			{"a/first.txt", test::bytes("hello"), {}},
			{"目录/file.bin", content, fo},
			{"z/last.txt", test::bytes("world"), fo},
		};
		const std::string path = dir.sub("test.vfs");
		REQUIRE(test::buildArchive(path, files) == Error::OK);
		Error err = Error::OK;
		if (InMemory)
			backend = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(test::readNative(path)), {}, &err);
		else
			backend = ArchiveBackend::create(path, {}, &err);
		REQUIRE(err == Error::OK);
		REQUIRE(backend);
		if (ViaVfs)
			REQUIRE(fs.mount("/pkg", backend) != 0);
	}

	std::unique_ptr<IFileStream> open(Mode mode)
	{
		if (ViaVfs)
			return fs.openFileStream("/pkg/目录/file.bin", mode);
		return backend->open("目录/file.bin", mode, nullptr);
	}
};

using ArchivePlainFx = ArchiveFx<false, false, false, false>;
using ArchiveXorFx = ArchiveFx<true, false, false, false>;
using ArchiveLz4Fx = ArchiveFx<false, true, false, false>;
using ArchiveXorLz4Fx = ArchiveFx<true, true, false, false>;
using ArchiveMemoryFx = ArchiveFx<true, false, true, false>;
using ArchiveVfsFx = ArchiveFx<true, false, false, true>;

// Content that compresses but is not perfectly regular (so LZ4 actually compresses it).
std::vector<uint8_t> sampleContent(size_t size)
{
	std::vector<uint8_t> out = test::randomBytes(size, 42);
	for (size_t i = 0; i < size; ++i)
		if ((i / 64) % 2 == 0)
			out[i] = static_cast<uint8_t>('a' + (i % 26));
	return out;
}

} // namespace

#define VFS_READ_FIXTURES                                                                                             \
	NativeFx, MemoryFx, VfsNativeFx, VfsMemoryFx, SingleFileFx, ArchivePlainFx, ArchiveXorFx, ArchiveLz4Fx,          \
		ArchiveXorLz4Fx, ArchiveMemoryFx, ArchiveVfsFx

#define VFS_WRITE_FIXTURES NativeFx, MemoryFx, VfsNativeFx, VfsMemoryFx

TEST_SUITE("stream")
{
	TEST_CASE_TEMPLATE("read-only: size / read / EOF", Fx, VFS_READ_FIXTURES)
	{
		Fx fx;
		const auto content = sampleContent(10000);
		fx.prepare(content);
		auto s = fx.open(Mode::READ);
		REQUIRE(s);
		CHECK(s->isOpen());
		CHECK(static_cast<bool>(*s));
		CHECK(s->size() == 10000);
		CHECK(s->tell() == 0);

		CHECK(test::readStream(*s) == content);
		CHECK(s->tell() == 10000);

		// After EOF, read returns 0, not -1.
		char buf[16];
		CHECK(s->read(buf, sizeof(buf)) == 0);
		CHECK(s->read(buf, 0) == 0);
	}

	TEST_CASE_TEMPLATE("read-only: seek with all three origins, past EOF, negative positions", Fx, VFS_READ_FIXTURES)
	{
		Fx fx;
		const auto content = sampleContent(5000);
		fx.prepare(content);
		auto s = fx.open(Mode::READ);
		REQUIRE(s);

		CHECK(s->seek(100, SEEK_SET) == 100);
		CHECK(s->tell() == 100);
		CHECK(s->seek(50, SEEK_CUR) == 150);
		CHECK(s->seek(-10, SEEK_CUR) == 140);
		CHECK(s->seek(-1, SEEK_END) == 4999);
		uint8_t c = 0;
		CHECK(s->read(&c, 1) == 1);
		CHECK(c == content[4999]);

		// A negative resulting position: return -1, errno = EINVAL, position unchanged.
		CHECK(s->seek(10, SEEK_SET) == 10);
		errno = 0;
		CHECK(s->seek(-11, SEEK_CUR) == -1);
		CHECK(errno == EINVAL);
		CHECK(s->tell() == 10);
		CHECK(s->seek(-5001, SEEK_END) == -1);
		CHECK(s->tell() == 10);

		// Past EOF: allowed, read returns 0.
		CHECK(s->seek(6000, SEEK_SET) == 6000);
		CHECK(s->tell() == 6000);
		CHECK(s->read(&c, 1) == 0);
		CHECK(s->size() == 5000);
	}

	TEST_CASE_TEMPLATE("read-only: random seek + read matches the original", Fx, VFS_READ_FIXTURES)
	{
		Fx fx;
		const auto content = sampleContent(70000);
		fx.prepare(content);
		auto s = fx.open(Mode::READ);
		REQUIRE(s);

		uint64_t x = 12345;
		std::vector<uint8_t> buf;
		for (int i = 0; i < 300; ++i)
		{
			x = x * 6364136223846793005ull + 1442695040888963407ull;
			int64_t off = static_cast<int64_t>((x >> 33) % content.size());
			unsigned int len = static_cast<unsigned int>((x >> 17) % 9000);
			buf.assign(len, 0);
			REQUIRE(s->seek(off, SEEK_SET) == off);
			int n = s->read(buf.data(), len);
			int expect = static_cast<int>(std::min<int64_t>(len, static_cast<int64_t>(content.size()) - off));
			REQUIRE(n == expect);
			REQUIRE(std::memcmp(buf.data(), content.data() + off, static_cast<size_t>(n)) == 0);
			REQUIRE(s->tell() == off + n);
		}
	}

	TEST_CASE_TEMPLATE("read-only: empty file", Fx, VFS_READ_FIXTURES)
	{
		Fx fx;
		fx.prepare({});
		auto s = fx.open(Mode::READ);
		REQUIRE(s);
		CHECK(s->size() == 0);
		char c;
		CHECK(s->read(&c, 1) == 0);
		CHECK(s->seek(0, SEEK_END) == 0);
	}

	TEST_CASE_TEMPLATE("read-only: write fails in READ mode and content is unchanged", Fx, VFS_READ_FIXTURES)
	{
		Fx fx;
		const auto content = sampleContent(100);
		fx.prepare(content);
		auto s = fx.open(Mode::READ);
		REQUIRE(s);
		CHECK(s->write("xx", 2) == -1);
		CHECK_FALSE(s->resize(10));
		CHECK(s->size() == 100);
		CHECK(test::readStream(*s) == content);
	}

	TEST_CASE_TEMPLATE("every operation after close returns EBADF", Fx, VFS_READ_FIXTURES)
	{
		Fx fx;
		fx.prepare(sampleContent(10));
		auto s = fx.open(Mode::READ);
		REQUIRE(s);
		CHECK(s->close() == 0);
		CHECK_FALSE(s->isOpen());
		CHECK_FALSE(static_cast<bool>(*s));
		char c;
		errno = 0;
		CHECK(s->read(&c, 1) == -1);
		CHECK(errno == EBADF);
		CHECK(s->seek(0, SEEK_SET) == -1);
		CHECK(s->tell() == -1);
		CHECK(s->size() == -1);
		// Closing again is harmless.
		s->close();
	}

	TEST_CASE_TEMPLATE("multiple streams on one file have independent positions", Fx, VFS_READ_FIXTURES)
	{
		Fx fx;
		const auto content = sampleContent(1000);
		fx.prepare(content);
		auto a = fx.open(Mode::READ);
		auto b = fx.open(Mode::READ);
		REQUIRE(a);
		REQUIRE(b);
		uint8_t x[10], y[10];
		a->seek(500, SEEK_SET);
		CHECK(b->read(y, 10) == 10);
		CHECK(a->read(x, 10) == 10);
		CHECK(std::memcmp(x, content.data() + 500, 10) == 0);
		CHECK(std::memcmp(y, content.data(), 10) == 0);
	}

	TEST_CASE_TEMPLATE("write: WRITE creates new files and truncates existing ones", Fx, VFS_WRITE_FIXTURES)
	{
		Fx fx;
		fx.removeFile();
		{
			auto s = fx.open(Mode::WRITE);
			REQUIRE(s);
			CHECK(s->size() == 0);
			CHECK(s->write("hello", 5) == 5);
			CHECK(s->tell() == 5);
			CHECK(s->size() == 5);
			// WRITE mode is also readable.
			CHECK(s->seek(0, SEEK_SET) == 0);
			char buf[5];
			CHECK(s->read(buf, 5) == 5);
			CHECK(std::memcmp(buf, "hello", 5) == 0);
		}
		CHECK(test::str(fx.contents()) == "hello");

		fx.prepare(test::bytes("a much longer old content"));
		{
			auto s = fx.open(Mode::WRITE);
			REQUIRE(s);
			CHECK(s->size() == 0);
			CHECK(s->write("new", 3) == 3);
		}
		CHECK(test::str(fx.contents()) == "new");
	}

	TEST_CASE_TEMPLATE("write: APPEND keeps the content and every write appends at the end", Fx, VFS_WRITE_FIXTURES)
	{
		Fx fx;
		fx.prepare(test::bytes("abc"));
		{
			auto s = fx.open(Mode::APPEND);
			REQUIRE(s);
			CHECK(s->size() == 3);
			CHECK(s->write("12", 2) == 2);
			// Even after seeking to the start, writes still append.
			s->seek(0, SEEK_SET);
			CHECK(s->write("34", 2) == 2);
			CHECK(s->size() == 7);
			// APPEND is readable (on Windows the first read upgrades the handle); writes after a read still append.
			CHECK(s->seek(1, SEEK_SET) == 1);
			char buf[4] = {};
			CHECK(s->read(buf, 3) == 3);
			CHECK(std::string(buf, 3) == "bc1");
			CHECK(s->tell() == 4);
			CHECK(s->write("5", 1) == 1);
			CHECK(s->size() == 8);
		}
		CHECK(test::str(fx.contents()) == "abc12345");

		// APPEND creates the file when it does not exist.
		fx.removeFile();
		{
			auto s = fx.open(Mode::APPEND);
			REQUIRE(s);
			CHECK(s->write("x", 1) == 1);
		}
		CHECK(test::str(fx.contents()) == "x");
	}

	TEST_CASE_TEMPLATE("write: OVERLAPPED does not truncate, overwrites in place, readable", Fx, VFS_WRITE_FIXTURES)
	{
		Fx fx;
		fx.prepare(test::bytes("0123456789"));
		{
			auto s = fx.open(Mode::OVERLAPPED);
			REQUIRE(s);
			CHECK(s->size() == 10);
			CHECK(s->seek(3, SEEK_SET) == 3);
			CHECK(s->write("ab", 2) == 2);
			CHECK(s->tell() == 5);
			char c;
			CHECK(s->read(&c, 1) == 1);
			CHECK(c == '5');
		}
		CHECK(test::str(fx.contents()) == "012ab56789");

		fx.removeFile();
		auto s = fx.open(Mode::OVERLAPPED);
		REQUIRE(s);
		CHECK(s->size() == 0);
	}

	TEST_CASE_TEMPLATE("write: writing after seeking past EOF fills the hole with zeros", Fx, VFS_WRITE_FIXTURES)
	{
		Fx fx;
		fx.removeFile();
		{
			auto s = fx.open(Mode::WRITE);
			REQUIRE(s);
			CHECK(s->write("ab", 2) == 2);
			CHECK(s->seek(6, SEEK_SET) == 6);
			CHECK(s->size() == 2);   // Seeking alone does not change the size.
			CHECK(s->write("z", 1) == 1);
			CHECK(s->size() == 7);
		}
		const std::string expect("ab\0\0\0\0z", 7);
		CHECK(test::str(fx.contents()) == expect);
	}

	TEST_CASE_TEMPLATE("write: resize truncates and extends without moving the position", Fx, VFS_WRITE_FIXTURES)
	{
		Fx fx;
		fx.prepare(test::bytes("0123456789"));
		{
			auto s = fx.open(Mode::OVERLAPPED);
			REQUIRE(s);
			CHECK(s->seek(8, SEEK_SET) == 8);
			CHECK(s->resize(4));
			CHECK(s->size() == 4);
			CHECK(s->tell() == 8);
			CHECK(s->resize(6));
			CHECK(s->size() == 6);
			CHECK_FALSE(s->resize(-1));
		}
		CHECK(test::str(fx.contents()) == std::string("0123\0\0", 6));
	}

	TEST_CASE_TEMPLATE("write: large data written in chunks reads back intact", Fx, VFS_WRITE_FIXTURES)
	{
		Fx fx;
		fx.removeFile();
		const auto data = test::randomBytes(300000, 9);
		{
			auto s = fx.open(Mode::WRITE);
			REQUIRE(s);
			size_t pos = 0;
			while (pos < data.size())
			{
				unsigned int n = static_cast<unsigned int>(std::min<size_t>(4093, data.size() - pos));
				REQUIRE(s->write(data.data() + pos, n) == static_cast<int>(n));
				pos += n;
			}
			CHECK(s->size() == static_cast<int64_t>(data.size()));
		}
		CHECK(fx.contents() == data);
	}

	TEST_CASE_TEMPLATE("opening a missing file for READ fails", Fx, VFS_WRITE_FIXTURES)
	{
		Fx fx;
		fx.removeFile();
		CHECK(fx.open(Mode::READ) == nullptr);
	}
}

TEST_SUITE("stream")
{
	TEST_CASE("NativeFileStream: refuses to open directories; movable")
	{
		test::TempDir dir;
		dir.mkdir("d");
		NativeFileStream s;
		errno = 0;
		CHECK_FALSE(s.open(dir.sub("d"), Mode::READ));
		CHECK(errno == EISDIR);
		CHECK_FALSE(s.isOpen());

		dir.write("f", "abc");
		REQUIRE(s.open(dir.sub("f"), Mode::READ));
		CHECK(s.nativeHandle() != (osfhnd_t)-1);
		NativeFileStream moved(std::move(s));
		CHECK_FALSE(s.isOpen());
		CHECK(moved.isOpen());
		CHECK(moved.size() == 3);
	}

	TEST_CASE("calling open() directly on archive and memory streams returns ENOTSUP")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), {{"f", test::bytes("x"), {}}}) == Error::OK);
		auto backend = ArchiveBackend::create(dir.sub("a.vfs"));
		REQUIRE(backend);
		auto s = backend->open("f", Mode::READ, nullptr);
		REQUIRE(s);
		errno = 0;
		CHECK_FALSE(s->open("whatever", Mode::READ));
		CHECK(errno == ENOTSUP);
	}

	TEST_CASE("archives can only be opened for READ")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), {{"f", test::bytes("x"), {}}}) == Error::OK);
		auto backend = ArchiveBackend::create(dir.sub("a.vfs"));
		REQUIRE(backend);
		for (Mode m : {Mode::WRITE, Mode::APPEND, Mode::OVERLAPPED})
		{
			Error err = Error::OK;
			CHECK(backend->open("f", m, &err) == nullptr);
			CHECK(err == Error::READ_ONLY);
		}
	}
}
