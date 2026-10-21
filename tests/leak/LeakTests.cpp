// Leak-focused tests. Lifetimes are checked with std::weak_ptr (and, on Linux, /proc/self/fd).
// ASan/LSan is the backstop; a passing weak_ptr check means the backend or index was destroyed
// even if a sanitizer is not watching that allocation.
#include "TestUtils.h"
#include "extreme/FaultBackend.h"
#include "vfs/android/AssetBackend.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/archive/ArchiveReader.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/native/NativeBackend.h"
#include "vfs/native/SingleFileBackend.h"
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#    include <filesystem>
#endif

using namespace vfs;

namespace
{

int countOpenFds()
{
#if defined(__linux__)
	int n = 0;
	std::error_code ec;
	for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end; it.increment(ec))
		++n;
	return n;
#else
	return -1;
#endif
}

void expectFdStable(int before)
{
	if (before < 0)
		return;
	CHECK(countOpenFds() == before);
}

std::vector<uint8_t> repeating(size_t size)
{
	static const char phrase[] = "local function player.health = 42\n";
	std::vector<uint8_t> body;
	body.reserve(size);
	while (body.size() < size)
		body.insert(body.end(), phrase, phrase + sizeof(phrase) - 1);
	body.resize(size);
	return body;
}

FileWriteOptions lz4Xor(bool encrypt, bool compress)
{
	FileWriteOptions opt;
	opt.encrypt = encrypt;
	opt.compress = compress;
	opt.blockLog2 = 12;
	return opt;
}

// In-memory .vfs with a multi-block LZ4 entry, an XOR entry, and a plain entry.
std::vector<uint8_t> packSample(std::vector<uint8_t>* lz4BodyOut)
{
	const std::vector<uint8_t> lz4Body = repeating(24 * 1024);
	auto mem = MemoryBackend::create();
	auto out = mem->open("a.vfs", IFileStream::Mode::WRITE, nullptr);
	REQUIRE(out);
	ArchiveWriter writer;
	REQUIRE(writer.addData("both.bin", lz4Body, lz4Xor(true, true)) == Error::OK);
	REQUIRE(writer.addData("lz4.bin", lz4Body, lz4Xor(false, true)) == Error::OK);
	REQUIRE(writer.addData("xor.bin", std::vector<uint8_t>(192, 0x5A), lz4Xor(true, false)) == Error::OK);
	REQUIRE(writer.addData("plain.txt", test::bytes("plain"), lz4Xor(false, false)) == Error::OK);
	REQUIRE(writer.write(*out) == Error::OK);
	REQUIRE(out->seek(0, SEEK_SET) == 0);
	std::vector<uint8_t> bytes = test::readStream(*out);
	out->close();
	if (lz4BodyOut)
		*lz4BodyOut = lz4Body;
	return bytes;
}

bool memoryDirEmpty(MemoryBackend& backend)
{
	bool any = false;
	const Error err = backend.list("", [&any](const DirEntry&)
	{
		any = true;
		return false;
	});
	return err == Error::OK && !any;
}

void readSome(IFileStream& stream)
{
	uint8_t buf[128];
	const int64_t at = stream.seek(50, SEEK_SET);
	CHECK(at == 50);
	CHECK(stream.read(buf, sizeof(buf)) > 0);
	CHECK(stream.seek(0, SEEK_SET) == 0);
	CHECK(stream.read(buf, 16) > 0);
}

class BytesFile : public IAssetFile
{
public:
	explicit BytesFile(std::vector<uint8_t> data) : m_data(std::move(data)) {}

	int read(void* buf, size_t size) override
	{
		if (m_pos >= static_cast<int64_t>(m_data.size()) || size == 0)
			return 0;
		const size_t n = std::min(size, m_data.size() - static_cast<size_t>(m_pos));
		std::memcpy(buf, m_data.data() + static_cast<size_t>(m_pos), n);
		m_pos += static_cast<int64_t>(n);
		return static_cast<int>(n);
	}

	int64_t seek(int64_t offset, int whence) override
	{
		const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? m_pos : static_cast<int64_t>(m_data.size());
		if (base + offset < 0)
			return -1;
		m_pos = base + offset;
		return m_pos;
	}

	int64_t length() const override { return static_cast<int64_t>(m_data.size()); }

	int openFileDescriptor(int64_t& start, int64_t& len) override
	{
		start = 0;
		len = 0;
		return -1;
	}

private:
	std::vector<uint8_t> m_data;
	int64_t m_pos = 0;
};

class BytesProvider : public IAssetProvider
{
public:
	BytesProvider(std::string name, std::vector<uint8_t> data) : m_name(std::move(name)), m_data(std::move(data)) {}

	std::unique_ptr<IAssetFile> open(std::string_view path) override
	{
		if (path == m_name)
			return std::make_unique<BytesFile>(m_data);
		return nullptr;
	}

	bool listFiles(std::string_view, std::vector<std::string>& names) override
	{
		names.clear();
		return false;
	}

private:
	std::string m_name;
	std::vector<uint8_t> m_data;
};

} // namespace

TEST_SUITE("leak")
{
	TEST_CASE("mount, unmount, and remount release native, memory, and archive backends")
	{
		test::TempDir dir;
		dir.write("a.txt", "native-bytes");
		dir.mkdir("sub");
		dir.write("sub/b.txt", "bb");
		const std::string archivePath = dir.sub("pack.vfs");
		REQUIRE(test::buildArchive(archivePath, {
			{"both.bin", repeating(12 * 1024), lz4Xor(true, true)},
			{"xor.bin", std::vector<uint8_t>(64, 3), lz4Xor(true, false)},
			{"plain.txt", test::bytes("p"), lz4Xor(false, false)},
		}) == Error::OK);

		const int fds = countOpenFds();
		for (int i = 0; i < 20; ++i)
		{
			VirtualFileSystem fs;
			auto mem = MemoryBackend::create();
			REQUIRE(mem->addFile("m.txt", "mem") == Error::OK);
			std::weak_ptr<MemoryBackend> weakMem = mem;
			auto native = NativeBackend::create(dir.path());
			REQUIRE(native);
			std::weak_ptr<NativeBackend> weakNative = native;
			Error err = Error::OK;
			auto arch = ArchiveBackend::create(archivePath, {}, &err);
			REQUIRE(err == Error::OK);
			REQUIRE(arch->index()->findFile("both.bin")->compression == CompressionType::LZ4);
			std::weak_ptr<ArchiveBackend> weakArch = arch;
			std::weak_ptr<const ArchiveIndex> weakIndex = arch->index();

			const MountId memId = fs.mount("/mem", mem);
			const MountId nativeId = fs.mount("/native", native);
			const MountId archId = fs.mount("/arch", arch);
			REQUIRE(memId != 0);
			REQUIRE(nativeId != 0);
			REQUIRE(archId != 0);
			mem.reset();
			native.reset();
			arch.reset();
			CHECK_FALSE(weakMem.expired());
			CHECK_FALSE(weakNative.expired());
			CHECK_FALSE(weakIndex.expired());

			REQUIRE(fs.unmount(memId) == Error::OK);
			REQUIRE(fs.unmount(nativeId) == Error::OK);
			REQUIRE(fs.unmount(archId) == Error::OK);
			CHECK(weakMem.expired());
			CHECK(weakNative.expired());
			CHECK(weakArch.expired());
			CHECK(weakIndex.expired());
		}

		for (int i = 0; i < 10; ++i)
		{
			VirtualFileSystem fs;
			auto first = MemoryBackend::create();
			auto second = MemoryBackend::create();
			REQUIRE(first->addFile("a.txt", "one") == Error::OK);
			REQUIRE(second->addFile("a.txt", "two") == Error::OK);
			std::weak_ptr<MemoryBackend> weakFirst = first;
			std::weak_ptr<MemoryBackend> weakSecond = second;
			const MountId id = fs.mount("/m", first);
			REQUIRE(id != 0);
			first.reset();
			REQUIRE(fs.remount(id, second) == Error::OK);
			CHECK(weakFirst.expired());
			second.reset();
			CHECK_FALSE(weakSecond.expired());
			REQUIRE(fs.unmount(id) == Error::OK);
			CHECK(weakSecond.expired());
		}
		expectFdStable(fds);
	}

	TEST_CASE("open and close streams on every built-in backend, then owners expire")
	{
		test::TempDir dir;
		dir.write("a.txt", std::string(4096, 'n'));
		const std::string archivePath = dir.sub("pack.vfs");
		const std::vector<uint8_t> body = repeating(16 * 1024);
		REQUIRE(test::buildArchive(archivePath, {
			{"both.bin", body, lz4Xor(true, true)},
			{"xor.bin", std::vector<uint8_t>(128, 9), lz4Xor(true, false)},
		}) == Error::OK);
		dir.write("one.txt", "single");

		const int fds = countOpenFds();
		std::weak_ptr<MemoryBackend> weakMem;
		std::weak_ptr<NativeBackend> weakNative;
		std::weak_ptr<ArchiveBackend> weakArch;
		std::weak_ptr<const ArchiveIndex> weakIndex;
		std::weak_ptr<SingleFileBackend> weakFile;
		{
			VirtualFileSystem fs;
			auto mem = MemoryBackend::create();
			REQUIRE(mem->addFile("m.txt", std::string(2048, 'm')) == Error::OK);
			weakMem = mem;
			auto native = NativeBackend::create(dir.path());
			REQUIRE(native);
			weakNative = native;
			Error err = Error::OK;
			auto arch = ArchiveBackend::create(archivePath, {}, &err);
			REQUIRE(arch);
			REQUIRE(arch->index()->findFile("both.bin")->compression == CompressionType::LZ4);
			weakArch = arch;
			weakIndex = arch->index();
			auto single = SingleFileBackend::create(dir.sub("one.txt"), true, &err);
			REQUIRE(single);
			weakFile = single;

			const MountId memId = fs.mount("/mem", mem);
			const MountId nativeId = fs.mount("/native", native);
			const MountId archId = fs.mount("/arch", arch);
			const MountId fileId = fs.mount("/one.txt", single);
			REQUIRE(memId != 0);
			REQUIRE(nativeId != 0);
			REQUIRE(archId != 0);
			REQUIRE(fileId != 0);
			mem.reset();
			native.reset();
			arch.reset();
			single.reset();

			for (int i = 0; i < 25; ++i)
			{
				Error openErr = Error::OK;
				auto memStream = fs.openFileStream("/mem/m.txt", IFileStream::Mode::READ, &openErr);
				auto nativeStream = fs.openFileStream("/native/a.txt", IFileStream::Mode::READ, &openErr);
				auto archStream = fs.openFileStream("/arch/both.bin", IFileStream::Mode::READ, &openErr);
				auto xorStream = fs.openFileStream("/arch/xor.bin", IFileStream::Mode::READ, &openErr);
				auto fileStream = fs.openFileStream("/one.txt", IFileStream::Mode::READ, &openErr);
				REQUIRE(memStream);
				REQUIRE(nativeStream);
				REQUIRE(archStream);
				REQUIRE(xorStream);
				REQUIRE(fileStream);
				readSome(*archStream);
				readSome(*xorStream);
				uint8_t buf[32];
				CHECK(memStream->read(buf, sizeof(buf)) == 32);
				CHECK(nativeStream->read(buf, sizeof(buf)) == 32);
				CHECK(fileStream->read(buf, 6) == 6);
				auto missing = fs.openFileStream("/native/missing.txt", IFileStream::Mode::READ, &openErr);
				CHECK_FALSE(missing);
				CHECK(memStream->close() == 0);
				CHECK(nativeStream->close() == 0);
				CHECK(archStream->close() == 0);
				CHECK(xorStream->close() == 0);
				CHECK(fileStream->close() == 0);
			}
			// After lazy unmount, VFS close() drops the backend while the wrapper object is still alive.
			{
				auto held = fs.openFileStream("/arch/both.bin", IFileStream::Mode::READ, nullptr);
				REQUIRE(held);
				REQUIRE(fs.unmount(archId) == Error::OK);
				CHECK_FALSE(weakArch.expired());
				CHECK_FALSE(weakIndex.expired());
				CHECK(held->close() == 0);
				CHECK(weakArch.expired());
				CHECK(weakIndex.expired());
			}
			REQUIRE(fs.unmount(memId) == Error::OK);
			REQUIRE(fs.unmount(nativeId) == Error::OK);
			REQUIRE(fs.unmount(fileId) == Error::OK);
			CHECK(weakMem.expired());
			CHECK(weakNative.expired());
			CHECK(weakFile.expired());
		}
		expectFdStable(fds);
	}

	TEST_CASE("lazy unmount keeps the backend until the last stream is destroyed")
	{
		test::TempDir dir;
		const std::string archivePath = dir.sub("pack.vfs");
		REQUIRE(test::buildArchive(archivePath, {
			{"both.bin", repeating(8 * 1024), lz4Xor(true, true)},
		}) == Error::OK);
		const int fds = countOpenFds();

		VirtualFileSystem fs;
		Error err = Error::OK;
		auto arch = ArchiveBackend::create(archivePath, {}, &err);
		REQUIRE(arch);
		std::weak_ptr<ArchiveBackend> weakArch = arch;
		std::weak_ptr<const ArchiveIndex> weakIndex = arch->index();
		const MountId id = fs.mount("/a", arch);
		REQUIRE(id != 0);
		arch.reset();

		auto first = fs.openFileStream("/a/both.bin", IFileStream::Mode::READ, &err);
		auto second = fs.openFileStream("/a/both.bin", IFileStream::Mode::READ, &err);
		REQUIRE(first);
		REQUIRE(second);
		REQUIRE(fs.unmount(id) == Error::OK);
		CHECK_FALSE(weakArch.expired());
		CHECK_FALSE(weakIndex.expired());
		uint8_t buf[16];
		CHECK(first->read(buf, sizeof(buf)) == 16);
		first.reset();
		CHECK_FALSE(weakArch.expired());
		CHECK_FALSE(weakIndex.expired());
		second.reset();
		CHECK(weakArch.expired());
		CHECK(weakIndex.expired());

		auto mem = MemoryBackend::create();
		REQUIRE(mem->addFile("t.txt", "lazy") == Error::OK);
		std::weak_ptr<MemoryBackend> weakMem = mem;
		const MountId memId = fs.mount("/m", mem);
		mem.reset();
		auto live = fs.openFileStream("/m/t.txt", IFileStream::Mode::READ, nullptr);
		REQUIRE(live);
		REQUIRE(fs.unmount(memId) == Error::OK);
		CHECK_FALSE(weakMem.expired());
		CHECK(live->close() == 0);
		CHECK(weakMem.expired());
		expectFdStable(fds);
	}

	TEST_CASE("copy-up and copyFile failures remove temps and do not pin backends")
	{
		const std::string payload(3000, 'q');
		for (int mode = 0; mode < 4; ++mode)
		{
			for (int n = 0; n < 8; ++n)
			{
				VirtualFileSystem fs;
				auto lower = MemoryBackend::create();
				REQUIRE(lower->addFile("a.txt", payload) == Error::OK);
				auto upper = MemoryBackend::create();
				std::weak_ptr<MemoryBackend> weakUpper = upper;
				test::FaultConfig cfg;
				if (mode == 0)
					cfg.writeFailAt = 8;
				else if (mode == 1)
				{
					cfg.match = ".vfs-copyup-";
					cfg.openError = Error::IO_ERROR;
				}
				else if (mode == 2)
				{
					cfg.match = ".vfs-copyup-";
					cfg.renameError = Error::IO_ERROR;
				}
				else
					cfg.closeError = EIO;
				auto fault = std::make_shared<test::FaultBackend>(upper, cfg);
				std::weak_ptr<IBackend> weakFault = fault;
				REQUIRE(fs.mount("/g", lower, MountOptions{0, MountFlags::ReadOnly}) != 0);
				const MountId upperId = fs.mount("/g", fault, MountOptions{10, MountFlags::WriteTarget});
				REQUIRE(upperId != 0);
				fault.reset();
				Error err = Error::OK;
				auto stream = fs.openFileStream("/g/a.txt", IFileStream::Mode::APPEND, &err);
				CHECK_FALSE(stream);
				CHECK(err != Error::OK);
				CHECK(cfg.injected.load() > 0);
				CHECK(memoryDirEmpty(*upper));

				// writeFailAt and closeError hit every stream. The open/rename faults are limited to copy-up temps,
				// so a normal copyFile would succeed and is not part of those two iterations.
				if (mode == 0 || mode == 3)
				{
					err = fs.copyFile("/g/a.txt", "/g/b.txt");
					CHECK(err != Error::OK);
					CHECK(memoryDirEmpty(*upper));
				}

				upper.reset();
				lower.reset();
				REQUIRE(fs.unmount(upperId) == Error::OK);
				CHECK(weakUpper.expired());
				CHECK(weakFault.expired());
			}
		}
	}

	TEST_CASE("corrupt and truncated archives fail open and release the source")
	{
		test::TempDir dir;
		const std::vector<uint8_t> good = packSample(nullptr);
		REQUIRE(good.size() > 64);
		test::writeNative(dir.sub("good.vfs"), good);
		std::vector<uint8_t> half = good;
		half.resize(good.size() / 2);
		test::writeNative(dir.sub("half.vfs"), half);
		test::writeNative(dir.sub("junk.vfs"), test::bytes("this is not a vfs archive"));
		test::writeNative(dir.sub("tiny.vfs"), std::vector<uint8_t>(8, 0xAB));

		const int fds = countOpenFds();
		const char* names[] = {"half.vfs", "junk.vfs", "tiny.vfs", "missing.vfs"};
		for (int i = 0; i < 15; ++i)
		{
			for (const char* name : names)
			{
				auto src = std::make_shared<FileArchiveSource>(dir.sub(name));
				std::weak_ptr<IArchiveSource> weakSrc = src;
				Error err = Error::OK;
				auto backend = ArchiveBackend::create(std::move(src), {}, &err);
				CHECK_FALSE(backend);
				CHECK(err != Error::OK);
				CHECK(weakSrc.expired());

				VirtualFileSystem fs;
				CHECK(fs.mountArchive("/a", dir.sub(name), {}, {}, &err) == 0);
				CHECK(err != Error::OK);
			}
			auto memSrc = std::make_shared<MemoryArchiveSource>(std::vector<uint8_t>(half));
			std::weak_ptr<IArchiveSource> weakMem = memSrc;
			Error err = Error::OK;
			auto idx = ArchiveReader::open(std::move(memSrc), {}, &err);
			CHECK_FALSE(idx);
			CHECK(weakMem.expired());
		}

		// Index parsed, then the file is truncated under the live stream.
		{
			VirtualFileSystem fs;
			Error err = Error::OK;
			auto backend = ArchiveBackend::create(dir.sub("good.vfs"), {}, &err);
			REQUIRE(backend);
			std::weak_ptr<ArchiveBackend> weakBackend = backend;
			std::weak_ptr<const ArchiveIndex> weakIndex = backend->index();
			const MountId id = fs.mount("/g", backend);
			REQUIRE(id != 0);
			backend.reset();
			auto stream = fs.openFileStream("/g/both.bin", IFileStream::Mode::READ, &err);
			REQUIRE(stream);
			test::writeNative(dir.sub("good.vfs"), std::vector<uint8_t>(good.begin(), good.begin() + 80));
			uint8_t buf[32];
			CHECK(stream->read(buf, sizeof(buf)) < 0);
			stream.reset();
			REQUIRE(fs.unmount(id) == Error::OK);
			CHECK(weakBackend.expired());
			CHECK(weakIndex.expired());
		}
		expectFdStable(fds);
	}

	TEST_CASE("LZ4 block and XOR streams survive open, seek, read, and close loops")
	{
		std::vector<uint8_t> expect;
		const std::vector<uint8_t> packed = packSample(&expect);
		auto src = std::make_shared<MemoryArchiveSource>(packed);
		std::weak_ptr<IArchiveSource> weakSrc = src;
		Error err = Error::OK;
		auto backend = ArchiveBackend::create(std::move(src), {}, &err);
		REQUIRE(err == Error::OK);
		REQUIRE(backend);
		const ArchiveEntry* both = backend->index()->findFile("both.bin");
		const ArchiveEntry* lz4 = backend->index()->findFile("lz4.bin");
		REQUIRE(both);
		REQUIRE(lz4);
		REQUIRE(both->compression == CompressionType::LZ4);
		REQUIRE(lz4->compression == CompressionType::LZ4);
		REQUIRE(both->cipher == CipherType::XOR);
		std::weak_ptr<ArchiveBackend> weakBackend = backend;
		std::weak_ptr<const ArchiveIndex> weakIndex = backend->index();

		std::vector<uint8_t> buf(512);
		for (int i = 0; i < 30; ++i)
		{
			auto stream = backend->open("both.bin", IFileStream::Mode::READ, &err);
			REQUIRE(stream);
			CHECK(stream->seek(100, SEEK_SET) == 100);
			CHECK(stream->read(buf.data(), 64) == 64);
			CHECK(std::memcmp(buf.data(), expect.data() + 100, 64) == 0);
			const int64_t tail = stream->seek(-20, SEEK_END);
			CHECK(tail == static_cast<int64_t>(expect.size()) - 20);
			CHECK(stream->read(buf.data(), 64) == 20);
			CHECK(stream->seek(static_cast<int64_t>(expect.size()) + 10, SEEK_SET) > 0);
			CHECK(stream->read(buf.data(), 8) == 0);
			// Cross a 4 KiB block boundary.
			CHECK(stream->seek(4090, SEEK_SET) == 4090);
			CHECK(stream->read(buf.data(), 32) == 32);
			CHECK(std::memcmp(buf.data(), expect.data() + 4090, 32) == 0);
			CHECK(stream->close() == 0);

			auto other = backend->open(i % 2 == 0 ? "lz4.bin" : "xor.bin", IFileStream::Mode::READ, &err);
			REQUIRE(other);
			CHECK(other->read(buf.data(), 32) == 32);
			other.reset();
		}
		CHECK_FALSE(weakIndex.expired());
		// A closed stream that is still alive must not pin the index or the source.
		auto kept = backend->open("both.bin", IFileStream::Mode::READ, &err);
		REQUIRE(kept);
		CHECK(kept->read(buf.data(), 16) == 16);
		CHECK(kept->close() == 0);
		CHECK(kept->size() == -1);
		backend.reset();
		CHECK(weakBackend.expired());
		CHECK(weakIndex.expired());
		CHECK(weakSrc.expired());
		kept.reset();

		VirtualFileSystem fs;
		auto again = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(packed), {}, &err);
		std::weak_ptr<const ArchiveIndex> weakAgain = again->index();
		REQUIRE(fs.mount("/a", again) != 0);
		again.reset();
		for (int i = 0; i < 10; ++i)
		{
			auto stream = fs.openFileStream("/a/both.bin", IFileStream::Mode::READ, &err);
			REQUIRE(stream);
			CHECK(stream->seek(5000, SEEK_SET) == 5000);
			CHECK(stream->read(buf.data(), 16) == 16);
			stream.reset();
		}
		REQUIRE(fs.unmount(fs.mounts().front().id) == Error::OK);
		CHECK(weakAgain.expired());
	}

	TEST_CASE("alias add and remove churn does not retain mount snapshots")
	{
		const int fds = countOpenFds();
		for (int round = 0; round < 10; ++round)
		{
			VirtualFileSystem fs;
			auto mem = MemoryBackend::create();
			REQUIRE(mem->addFile("cfg/a.txt", "alias") == Error::OK);
			std::weak_ptr<MemoryBackend> weakMem = mem;
			const MountId id = fs.mount("/res", mem);
			REQUIRE(id != 0);
			mem.reset();
			for (int i = 0; i < 40; ++i)
			{
				REQUIRE(fs.addAlias("res://", "/res") == Error::OK);
				REQUIRE(fs.addAlias("cfg://", "/res/cfg") == Error::OK);
				REQUIRE(fs.removeAlias("res://") == Error::OK);
				REQUIRE(fs.removeAlias("cfg://") == Error::OK);
			}
			CHECK_FALSE(weakMem.expired());
			auto stream = fs.openFileStream("/res/cfg/a.txt", IFileStream::Mode::READ, nullptr);
			REQUIRE(stream);
			stream.reset();
			REQUIRE(fs.unmount(id) == Error::OK);
			CHECK(weakMem.expired());
		}
		expectFdStable(fds);
	}

	TEST_CASE("threads that open streams and exit do not retain backends" * VFS_NEEDS_THREADS)
	{
		test::TempDir dir;
		dir.write("a.txt", std::string(8192, 't'));
		const std::string archivePath = dir.sub("pack.vfs");
		REQUIRE(test::buildArchive(archivePath, {
			{"both.bin", repeating(16 * 1024), lz4Xor(true, true)},
			{"xor.bin", std::vector<uint8_t>(256, 1), lz4Xor(true, false)},
		}) == Error::OK);
		const int fds = countOpenFds();

		VirtualFileSystem fs;
		auto mem = MemoryBackend::create();
		REQUIRE(mem->addFile("m.txt", std::string(1024, 'm')) == Error::OK);
		std::weak_ptr<MemoryBackend> weakMem = mem;
		auto native = NativeBackend::create(dir.path());
		std::weak_ptr<NativeBackend> weakNative = native;
		Error err = Error::OK;
		auto arch = ArchiveBackend::create(archivePath, {}, &err);
		REQUIRE(arch);
		std::weak_ptr<ArchiveBackend> weakArch = arch;
		std::weak_ptr<const ArchiveIndex> weakIndex = arch->index();
		const MountId memId = fs.mount("/mem", mem);
		const MountId nativeId = fs.mount("/native", native);
		const MountId archId = fs.mount("/arch", arch);
		REQUIRE(memId != 0);
		REQUIRE(nativeId != 0);
		REQUIRE(archId != 0);
		mem.reset();
		native.reset();
		arch.reset();

		auto worker = [&fs](const char* path)
		{
			for (int n = 0; n < 8; ++n)
			{
				auto stream = fs.openFileStream(path, IFileStream::Mode::READ, nullptr);
				REQUIRE(stream);
				uint8_t buf[64];
				CHECK(stream->seek(10, SEEK_SET) == 10);
				CHECK(stream->read(buf, sizeof(buf)) > 0);
			}
		};
		{
			std::vector<std::thread> threads;
			threads.emplace_back(worker, "/mem/m.txt");
			threads.emplace_back(worker, "/native/a.txt");
			threads.emplace_back(worker, "/arch/both.bin");
			threads.emplace_back(worker, "/arch/xor.bin");
			for (auto& th : threads)
				th.join();
		}
		CHECK_FALSE(weakArch.expired());
		REQUIRE(fs.unmount(memId) == Error::OK);
		REQUIRE(fs.unmount(nativeId) == Error::OK);
		REQUIRE(fs.unmount(archId) == Error::OK);
		CHECK(weakMem.expired());
		CHECK(weakNative.expired());
		CHECK(weakArch.expired());
		CHECK(weakIndex.expired());
		expectFdStable(fds);
	}

	TEST_CASE("asset archiveSource does not cycle with the asset backend")
	{
		const std::vector<uint8_t> packed = packSample(nullptr);
		auto provider = std::make_shared<BytesProvider>("pack.vfs", packed);
		std::weak_ptr<IAssetProvider> weakProvider = provider;
		Error err = Error::OK;
		auto asset = AssetBackend::create(provider, {}, &err);
		REQUIRE(err == Error::OK);
		REQUIRE(asset);
		provider.reset();
		CHECK_FALSE(weakProvider.expired());
		std::weak_ptr<AssetBackend> weakAsset = asset;

		for (int i = 0; i < 15; ++i)
		{
			auto source = asset->archiveSource("pack.vfs", &err);
			REQUIRE(err == Error::OK);
			REQUIRE(source);
			std::weak_ptr<IArchiveSource> weakSource = source;
			auto arch = ArchiveBackend::create(source, {}, &err);
			REQUIRE(arch);
			REQUIRE(arch->index()->findFile("both.bin")->compression == CompressionType::LZ4);
			std::weak_ptr<ArchiveBackend> weakArch = arch;
			source.reset();
			CHECK_FALSE(weakSource.expired());
			auto stream = arch->open("both.bin", IFileStream::Mode::READ, &err);
			REQUIRE(stream);
			uint8_t buf[8];
			CHECK(stream->read(buf, sizeof(buf)) == 8);
			stream.reset();
			arch.reset();
			CHECK(weakArch.expired());
			CHECK(weakSource.expired());
			CHECK_FALSE(weakAsset.expired());
		}

		auto source = asset->archiveSource("pack.vfs", &err);
		REQUIRE(source);
		auto arch = ArchiveBackend::create(std::move(source), {}, &err);
		REQUIRE(arch);
		std::weak_ptr<ArchiveBackend> weakArch = arch;
		asset.reset();
		// The archive source owns the AssetBackend; the asset backend does not own the archive.
		CHECK_FALSE(weakAsset.expired());
		CHECK_FALSE(weakProvider.expired());
		arch.reset();
		CHECK(weakArch.expired());
		CHECK(weakAsset.expired());
		CHECK(weakProvider.expired());

		// Not owned by shared_ptr: archiveSource must fail and must not leak a source.
		AssetBackend stackBackend(std::make_shared<BytesProvider>("pack.vfs", packed), std::string());
		CHECK_FALSE(stackBackend.archiveSource("pack.vfs", &err));
		CHECK(err == Error::UNSUPPORTED);
	}
}
