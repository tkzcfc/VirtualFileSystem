// 并发压力测试：多线程读归档、挂载/卸载与打开中的流并发、内存后端并发读写
#include "TestUtils.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/memory/MemoryBackend.h"
#include <atomic>
#include <cstring>
#include <thread>

using namespace vfs;

namespace
{

constexpr int THREADS = 16;

} // namespace

TEST_SUITE("concurrency")
{
	TEST_CASE("16 threads randomly read the same archive (each stream has its own handle and position)")
	{
		test::TempDir dir;
		std::vector<test::ArchiveFile> files;
		for (int i = 0; i < 20; ++i)
		{
			FileWriteOptions o;
			o.encrypt = i % 2 == 0;
			o.compress = i % 3 == 0;
			files.push_back({"f" + std::to_string(i) + ".bin", test::randomBytes(20000 + static_cast<size_t>(i) * 100, static_cast<uint64_t>(i)), o});
		}
		REQUIRE(test::buildArchive(dir.sub("c.vfs"), files) == Error::OK);
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/", dir.sub("c.vfs")) != 0);

		std::atomic<int> failures{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < THREADS; ++t)
		{
			threads.emplace_back([&, t]()
			{
				uint64_t x = static_cast<uint64_t>(t) + 1;
				for (int iter = 0; iter < 200; ++iter)
				{
					x = x * 6364136223846793005ull + 1442695040888963407ull;
					const auto& f = files[(x >> 33) % files.size()];
					auto s = fs.openFileStream("/" + f.path, IFileStream::Mode::READ);
					if (!s)
					{
						++failures;
						continue;
					}
					int64_t off = static_cast<int64_t>((x >> 20) % f.content.size());
					uint8_t buf[512];
					s->seek(off, SEEK_SET);
					int n = s->read(buf, sizeof(buf));
					size_t expect = std::min<size_t>(sizeof(buf), f.content.size() - static_cast<size_t>(off));
					if (n != static_cast<int>(expect) || std::memcmp(buf, f.content.data() + off, expect) != 0)
						++failures;
				}
			});
		}
		for (auto& th : threads)
			th.join();
		CHECK(failures.load() == 0);
	}

	TEST_CASE("mount/unmount concurrent with reads: open streams always read to the end, queries never crash")
	{
		test::TempDir dir;
		const auto content = test::randomBytes(100000, 5);
		REQUIRE(test::buildArchive(dir.sub("dlc.vfs"), {{"data.bin", content, {}}}) == Error::OK);
		auto backend = ArchiveBackend::create(dir.sub("dlc.vfs"));
		REQUIRE(backend);

		VirtualFileSystem fs;
		auto base = MemoryBackend::create();
		base->addFile("base.txt", "base");
		fs.mount("/", base);

		std::atomic<bool> stop{false};
		std::atomic<int> failures{0};
		std::atomic<int> streamsRead{0};

		// 一个线程不停地挂载、卸载
		std::thread mounter([&]()
		{
			// 至少 300 轮；如果读线程还没来得及读到（调度原因，Wine/慢机器上常见），继续挂载直到读到一些流
			for (int i = 0; i < 300 || (streamsRead.load() < 20 && i < 200000); ++i)
			{
				MountId id = fs.mount("/dlc", backend, MountOptions{i % 5, 0});
				std::this_thread::yield();
				if (fs.unmount(id) != Error::OK)
					++failures;
			}
			stop = true;
		});

		std::vector<std::thread> readers;
		for (int t = 0; t < THREADS - 1; ++t)
		{
			readers.emplace_back([&]()
			{
				while (!stop.load())
				{
					if (test::readAll(fs, "/base.txt") != "base")
						++failures;
					auto s = fs.openFileStream("/dlc/data.bin", IFileStream::Mode::READ);
					if (!s)
						continue;   // 恰好处于卸载状态
					// 打开之后即使被卸载也必须完整读完
					if (test::readStream(*s) != content)
						++failures;
					++streamsRead;
					test::listNames(fs, "/");
					fs.exists("/dlc");
				}
			});
		}
		mounter.join();
		for (auto& th : readers)
			th.join();
		CHECK(failures.load() == 0);
		CHECK(streamsRead.load() > 0);
		CHECK(fs.mounts().size() == 1);
	}

	TEST_CASE("memory backend: concurrent writes to different files, concurrent read/write of one file, concurrent create/remove")
	{
		VirtualFileSystem fs;
		std::shared_ptr<MemoryBackend> mem;
		fs.mountMemory("/", {}, &mem);
		REQUIRE(fs.createDir("/shared") == Error::OK);
		REQUIRE(fs.writeFile("/shared/log.txt", "") == Error::OK);

		std::atomic<int> failures{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < THREADS; ++t)
		{
			threads.emplace_back([&, t]()
			{
				const std::string own = "/t" + std::to_string(t);
				if (fs.createDirs(own + "/x/y") != Error::OK)
					++failures;
				for (int i = 0; i < 100; ++i)
				{
					const std::string p = own + "/x/y/" + std::to_string(i % 10);
					const std::string v = std::to_string(t * 1000 + i);
					if (fs.writeFile(p, v) != Error::OK)
						++failures;
					if (test::readAll(fs, p) != v)
						++failures;
					if (i % 10 == 9 && fs.removeFile(p) != Error::OK)
						++failures;

					// 所有线程都往同一个文件追加一行
					auto s = fs.openFileStream("/shared/log.txt", IFileStream::Mode::APPEND);
					if (!s || s->write("0123456789\n", 11) != 11)
						++failures;
					test::listNames(fs, "/shared");
				}
			});
		}
		for (auto& th : threads)
			th.join();
		CHECK(failures.load() == 0);
		CHECK(fs.fileSize("/shared/log.txt") == THREADS * 100 * 11);
		CHECK(test::listNames(fs, "/").size() == THREADS + 1);
	}

	TEST_CASE("native directory: threads write different files and create the same directory concurrently")
	{
		test::TempDir dir;
		VirtualFileSystem fs;
		fs.mountNative("/", dir.path());
		std::atomic<int> failures{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 8; ++t)
		{
			threads.emplace_back([&, t]()
			{
				for (int i = 0; i < 30; ++i)
				{
					if (fs.createDirs("/common/sub") != Error::OK)
						++failures;
					const std::string p = "/common/sub/" + std::to_string(t) + "_" + std::to_string(i);
					if (fs.writeFileAtomic(p, p.data(), p.size()) != Error::OK)
						++failures;
					if (test::readAll(fs, p) != p)
						++failures;
				}
			});
		}
		for (auto& th : threads)
			th.join();
		CHECK(failures.load() == 0);
		CHECK(test::listNames(fs, "/common/sub").size() == 8 * 30);
	}
}
