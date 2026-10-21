// Multi-thread stress tests: concurrent create, write, read, delete, and rename in the same directory and in different directories, mount-table churn, and many concurrent handles.
// Every case checks results with invariants, and not only that nothing crashed. Iteration counts can be scaled with VFS_TEST_SCALE.
#include "matrix/Envs.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <type_traits>

using namespace vfs;
using Mode = IFileStream::Mode;

namespace
{

const int THREADS = 8;

// Several combinations of a writable root at /s
struct StressMemory
{
	test::TempDir dir;
	VirtualFileSystem fs;
	StressMemory() { REQUIRE(fs.mountMemory("/s") != 0); }
};

struct StressNative
{
	test::TempDir dir;
	VirtualFileSystem fs;
	StressNative()
	{
		dir.mkdir("root");
		REQUIRE(fs.mountNative("/s", dir.sub("root")) != 0);
	}
};

// Archive below, memory above; the archive provides /s/base/*.txt
struct StressMemoryOverArchive
{
	test::TempDir dir;
	VirtualFileSystem fs;
	StressMemoryOverArchive()
	{
		std::vector<test::ArchiveFile> files;
		for (int i = 0; i < 16; ++i)
			files.push_back({"base/" + std::to_string(i) + ".txt", test::bytes("base" + std::to_string(i)), {}});
		REQUIRE(test::buildArchive(dir.sub("b.vfs"), files) == Error::OK);
		REQUIRE(fs.mountArchive("/s", dir.sub("b.vfs"), {}, env::prio(0)) != 0);
		REQUIRE(fs.mountMemory("/s", env::prio(1)) != 0);
	}
};

// Archive below, a native directory above
struct StressNativeOverArchive
{
	test::TempDir dir;
	VirtualFileSystem fs;
	StressNativeOverArchive()
	{
		std::vector<test::ArchiveFile> files;
		for (int i = 0; i < 16; ++i)
			files.push_back({"base/" + std::to_string(i) + ".txt", test::bytes("base" + std::to_string(i)), {}});
		REQUIRE(test::buildArchive(dir.sub("b.vfs"), files) == Error::OK);
		REQUIRE(fs.mountArchive("/s", dir.sub("b.vfs"), {}, env::prio(0)) != 0);
		dir.mkdir("upper");
		REQUIRE(fs.mountNative("/s", dir.sub("upper"), env::prio(1)) != 0);
	}
};

template <typename Fn>
void runThreads(int n, Fn fn)
{
	std::vector<std::thread> threads;
	for (int t = 0; t < n; ++t)
		threads.emplace_back([&fn, t]() { fn(t); });
	for (auto& th : threads)
		th.join();
}

// Content determined by (thread, index), so it can be checked
std::string payload(int t, int i)
{
	std::string s = "T" + std::to_string(t) + "#" + std::to_string(i) + ":";
	s.append(static_cast<size_t>((t * 131 + i * 17) % 3000), static_cast<char>('a' + t % 26));
	return s;
}

} // namespace

#define STRESS_ENVS StressMemory, StressNative, StressMemoryOverArchive, StressNativeOverArchive

TEST_SUITE("stress")
{
	TEST_CASE_TEMPLATE("same directory: each thread creates / writes / reads / overwrites / removes its own files while another lists", E, STRESS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDirs("/s/shared") == Error::OK);
		const int rounds = 120 * test::scale();
		test::FailureLog failures;
		std::atomic<bool> done{false};

		std::thread lister([&]()
		{
			while (!done.load())
			{
				std::vector<std::string> names;
				Error err = e.fs.list("/s/shared", [&](const DirEntry& d)
				{
					names.push_back(d.name);
					return true;
				});
				if (err != Error::OK || !std::is_sorted(names.begin(), names.end()) ||
					std::adjacent_find(names.begin(), names.end()) != names.end())
					failures.add(std::string("list -> ") + errorName(err) + ", " + std::to_string(names.size()) + " entries");
			}
		});

		runThreads(THREADS, [&](int t)
		{
			for (int i = 0; i < rounds; ++i)
			{
				const std::string path = "/s/shared/t" + std::to_string(t) + "_" + std::to_string(i % 10);
				const std::string data = payload(t, i);
				failures.expectOk(e.fs.writeFile(path, data), "writeFile " + path);
				failures.expectEq(test::readAll(e.fs, path), data, "readAll " + path);
				if (i % 3 == 0)
					failures.expectOk(e.fs.removeFile(path), "removeFile " + path);
			}
		});
		done = true;
		lister.join();
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		// Final state: among each thread's 10 slots, files not deleted in the last round have the correct content
		for (int t = 0; t < THREADS; ++t)
		{
			for (int slot = 0; slot < 10; ++slot)
			{
				int last = -1;
				for (int i = 0; i < rounds; ++i)
					if (i % 10 == slot)
						last = i;
				const std::string path = "/s/shared/t" + std::to_string(t) + "_" + std::to_string(slot);
				if (last % 3 == 0)
					CHECK_FALSE(e.fs.exists(path));
				else
					CHECK(test::readAll(e.fs, path) == payload(t, last));
			}
		}
	}

	TEST_CASE_TEMPLATE("separate directories: each thread runs createDirs / write / recursive removeDir in its own tree", E, STRESS_ENVS)
	{
		E e;
		const int rounds = 40 * test::scale();
		test::FailureLog failures;
		runThreads(THREADS, [&](int t)
		{
			const std::string root = "/s/own" + std::to_string(t);
			for (int i = 0; i < rounds; ++i)
			{
				const std::string d = root + "/a" + std::to_string(i % 4) + "/b/c";
				failures.expectOk(e.fs.createDirs(d), "createDirs " + d);
				failures.expectOk(e.fs.writeFile(d + "/f.txt", payload(t, i)), "writeFile " + d + "/f.txt");
				failures.expectEq(test::readAll(e.fs, d + "/f.txt"), payload(t, i), "readAll " + d + "/f.txt");
				if (i % 5 == 4)
					failures.expectOk(e.fs.removeDir(root, true), "removeDir(recursive) " + root);
			}
		});
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
	}

	TEST_CASE_TEMPLATE("concurrent renames: files move randomly between 4 directories, each ends up in exactly one place intact", E, STRESS_ENVS)
	{
		E e;
		const int FILES = 40;
		for (int d = 0; d < 4; ++d)
			REQUIRE(e.fs.createDirs("/s/mv/d" + std::to_string(d)) == Error::OK);
		for (int f = 0; f < FILES; ++f)
			REQUIRE(e.fs.writeFile("/s/mv/d0/f" + std::to_string(f), "content" + std::to_string(f)) == Error::OK);

		const int rounds = 300 * test::scale();
		test::FailureLog failures;
		std::atomic<int> moved{0};
		test::ErrorHistogram codes;
		runThreads(THREADS, [&](int t)
		{
			std::mt19937 rng(static_cast<unsigned>(t * 7 + 1));
			for (int i = 0; i < rounds; ++i)
			{
				const std::string name = "f" + std::to_string(rng() % FILES);
				const std::string from = "/s/mv/d" + std::to_string(rng() % 4) + "/" + name;
				const std::string to = "/s/mv/d" + std::to_string(rng() % 4) + "/" + name;
				Error err = e.fs.rename(from, to);
				codes.add(err);
				if (err == Error::OK)
					++moved;
				else if (err != Error::NOT_FOUND)
					failures.add("rename " + from + " -> " + to + ": " + errorName(err) + test::osErrorSuffix());
			}
		});
		INFO("rename codes: " << codes.str());
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		CHECK(moved.load() > 0);
		std::map<std::string, int> seen;
		for (int d = 0; d < 4; ++d)
		{
			for (const auto& n : test::listNames(e.fs, "/s/mv/d" + std::to_string(d)))
			{
				++seen[n];
				CHECK(test::readAll(e.fs, "/s/mv/d" + std::to_string(d) + "/" + n) == "content" + n.substr(1));
			}
		}
		CHECK(seen.size() == static_cast<size_t>(FILES));
		for (const auto& kv : seen)
			CHECK(kv.second == 1);
	}

	TEST_CASE_TEMPLATE("concurrent directory renames: a tree rotates between names, exactly one copy remains", E, STRESS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDirs("/s/rd/n0/inner") == Error::OK);
		REQUIRE(e.fs.writeFile("/s/rd/n0/inner/x.txt", "X") == Error::OK);
		const int rounds = 200 * test::scale();
		test::FailureLog failures;
		test::ErrorHistogram codes;
		runThreads(THREADS, [&](int t)
		{
			std::mt19937 rng(static_cast<unsigned>(t + 100));
			for (int i = 0; i < rounds; ++i)
			{
				Error err = e.fs.rename("/s/rd/n" + std::to_string(rng() % 3), "/s/rd/n" + std::to_string(rng() % 3));
				codes.add(err);
				if (err != Error::OK && err != Error::NOT_FOUND)
					failures.add(std::string("rename: ") + errorName(err) + test::osErrorSuffix());
			}
		});
		auto names = test::listNames(e.fs, "/s/rd");
		INFO("rename codes: " << codes.str());
		INFO("final directories: " << doctest::toString(names));
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		REQUIRE(names.size() == 1);
		CHECK(test::readAll(e.fs, "/s/rd/" + names[0] + "inner/x.txt") == "X");
	}

	TEST_CASE_TEMPLATE("concurrent mkdir / rmdir on the same directories: only expected error codes, consistent view afterwards", E, STRESS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDir("/s/md") == Error::OK);
		const int rounds = 200 * test::scale();
		test::FailureLog failures;
		// Count return codes separately for each operation, and print the whole set on failure
		test::ErrorHistogram createDirsCodes, createDirCodes, removeDirCodes, removeTreeCodes;
		// Failure details include the operation, path, error code, and the raw OS error (errno / GetLastError)
		auto expect = [&](test::ErrorHistogram& h, const std::string& what, Error err, Error a, Error b, Error c)
		{
			h.add(err);
			if (err != a && err != b && err != c)
				failures.add("unexpected code " + what + " -> " + errorName(err) + test::osErrorSuffix());
		};
		runThreads(THREADS, [&](int t)
		{
			std::mt19937 rng(static_cast<unsigned>(t + 1000));
			for (int i = 0; i < rounds; ++i)
			{
				const std::string d = "/s/md/x" + std::to_string(rng() % 3) + "/y" + std::to_string(rng() % 3);
				switch (rng() % 4)
				{
				case 0:
					// createDirs: already exists (including a concurrent create) counts as success; if an intermediate directory is deleted concurrently, it retries from the start,
					// and NOT_FOUND is returned only if deletion keeps winning past the retry limit (see mount-rules.md section 6). There are no files here, so EXISTS does not occur
					expect(createDirsCodes, "createDirs " + d, e.fs.createDirs(d), Error::OK, Error::NOT_FOUND,
						   Error::OK);
					break;
				case 1:
					expect(createDirCodes, "createDir " + d, e.fs.createDir(d), Error::OK, Error::NOT_FOUND,
						   Error::EXISTS);
					break;
				case 2:
					expect(removeDirCodes, "removeDir " + d, e.fs.removeDir(d), Error::OK, Error::NOT_FOUND,
						   Error::NOT_EMPTY);
					break;
				default:
					expect(removeTreeCodes, "removeDir(recursive) " + d.substr(0, d.rfind('/')),
						   e.fs.removeDir(d.substr(0, d.rfind('/')), true), Error::OK,
						   Error::NOT_FOUND, Error::NOT_EMPTY);
					break;
				}
			}
		});
		INFO("createDirs codes: " << createDirsCodes.str());
		INFO("createDir codes: " << createDirCodes.str());
		INFO("removeDir codes: " << removeDirCodes.str());
		INFO("removeDir(recursive) codes: " << removeTreeCodes.str());
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		// Consistent view: every item walk reaches can be stat'ed successfully and has a matching type
		e.fs.walk("/s/md", [&](const std::string& full, const DirEntry& d)
		{
			Stat st;
			CHECK(e.fs.stat(full, st) == Error::OK);
			CHECK(st.type == d.type);
			return true;
		});
	}

	TEST_CASE_TEMPLATE("read/write one file: writers replace / append, readers only see bytes from some write", E, STRESS_ENVS)
	{
		E e;
		REQUIRE(e.fs.writeFile("/s/hot.bin", std::string(1000, 'A')) == Error::OK);
		std::atomic<bool> done{false};
		test::FailureLog failures;
		std::atomic<int> reads{0};
		const int rounds = 300 * test::scale();

		std::vector<std::thread> readers;
		for (int r = 0; r < 4; ++r)
		{
			readers.emplace_back([&]()
			{
				while (!done.load())
				{
					auto s = e.fs.openFileStream("/s/hot.bin", Mode::READ);
					if (!s)
						continue;   // it may be missing for an instant during replace (native WRITE truncates first)
					char buf[700];
					while (true)
					{
						int n = s->read(buf, sizeof(buf));
						if (n < 0)
						{
							failures.add("read returned " + std::to_string(n));
							break;
						}
						if (n == 0)
							break;
						for (int i = 0; i < n; ++i)
							if ((buf[i] < 'A' || buf[i] > 'H') && buf[i] != 0)   // the part grown by resize is 0
								failures.add("unexpected byte " + std::to_string(static_cast<int>(static_cast<unsigned char>(buf[i]))));
					}
					++reads;
				}
			});
		}
		runThreads(4, [&](int t)
		{
			for (int i = 0; i < rounds; ++i)
			{
				const char c = static_cast<char>('A' + (t * 2 + i % 2));
				if (i % 4 == 0)
				{
					Error oe = Error::OK;
					auto s = e.fs.openFileStream("/s/hot.bin", Mode::APPEND, &oe);
					if (!s)
						failures.add(std::string("open APPEND -> ") + errorName(oe));
					else if (int n = s->write(std::string(100, c).data(), 100); n != 100)
						failures.add("APPEND write returned " + std::to_string(n));
				}
				else if (i % 4 == 1)
				{
					Error oe = Error::OK;
					auto s = e.fs.openFileStream("/s/hot.bin", Mode::OVERLAPPED, &oe);
					if (!s)
						failures.add(std::string("open OVERLAPPED -> ") + errorName(oe));
					else if (!s->resize(500 + i % 700))
						failures.add("resize failed");
				}
				else
					failures.expectOk(e.fs.writeFile("/s/hot.bin", std::string(1000 + i % 500, c)), "writeFile hot.bin");
			}
		});
		done = true;
		for (auto& th : readers)
			th.join();
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		CHECK(reads.load() > 0);
	}

	TEST_CASE_TEMPLATE("concurrent APPEND to one read-only-layer file (copy-up race): original kept, every append appears exactly once", E,
					   StressMemoryOverArchive, StressNativeOverArchive)
	{
		E e;
		const int perThread = 20;
		test::FailureLog failures;
		runThreads(THREADS, [&](int t)
		{
			for (int i = 0; i < perThread; ++i)
			{
				const std::string base = "/s/base/" + std::to_string(i % 16) + ".txt";
				Error oe = Error::OK;
				auto s = e.fs.openFileStream(base, Mode::APPEND, &oe);
				const std::string tag = "<" + std::to_string(t) + "." + std::to_string(i) + ">";
				if (!s)
					failures.add("open APPEND " + base + " -> " + errorName(oe));
				else if (int n = s->write(tag.data(), static_cast<unsigned>(tag.size()));
						 n != static_cast<int>(tag.size()))
					failures.add("APPEND write " + base + " returned " + std::to_string(n));
			}
		});
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		for (int f = 0; f < 16; ++f)
		{
			const std::string content = test::readAll(e.fs, "/s/base/" + std::to_string(f) + ".txt");
			CAPTURE(content);
			CHECK(content.rfind("base" + std::to_string(f), 0) == 0);
			for (int t = 0; t < THREADS; ++t)
			{
				for (int i = f; i < perThread; i += 16)
				{
					const std::string tag = "<" + std::to_string(t) + "." + std::to_string(i) + ">";
					size_t pos = content.find(tag);
					CHECK(pos != std::string::npos);
					if (pos != std::string::npos)
						CHECK(content.find(tag, pos + 1) == std::string::npos);
				}
			}
		}
		// Leave no copy-up temporary files
		for (const auto& n : test::listNames(e.fs, "/s/base"))
			CHECK(n.find("tmp") == std::string::npos);
	}

	TEST_CASE_TEMPLATE("concurrent writeFileAtomic on one path: final content is one complete write, no temp files left", E, STRESS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDir("/s/atomic") == Error::OK);
		test::FailureLog failures;
		std::atomic<bool> done{false};
		std::thread reader([&]()
		{
			while (!done.load())
			{
				std::string s;
				if (e.fs.readFile("/s/atomic/cfg", s) == Error::OK)
				{
					// What was read must be one complete write: the first character repeated len times
					if (s.empty() || s.find_first_not_of(s[0]) != std::string::npos)
						failures.add("incomplete content: " + std::to_string(s.size()) + " bytes");
				}
			}
		});
		const int rounds = 60 * test::scale();
		// A memory layer can always replace; a native layer depends on the platform
		const bool replaceOpen = std::is_same_v<E, StressMemory> || std::is_same_v<E, StressMemoryOverArchive> ||
								 test::nativeReplaceOpenFile();
		test::ErrorHistogram atomicCodes;
		runThreads(THREADS, [&](int t)
		{
			for (int i = 0; i < rounds; ++i)
			{
				const std::string data(2000 + 37 * t, static_cast<char>('a' + t));
				Error err = e.fs.writeFileAtomic("/s/atomic/cfg", data.data(), data.size());
				atomicCodes.add(err);
				// When the platform cannot replace a file that is open (see test::nativeReplaceOpenFile), a replace while a reader holds the handle
				// fails with PERMISSION_DENIED: that is a platform limit and is allowed, but the other invariants (intact content, no temp files) are still checked
				if (err == Error::PERMISSION_DENIED && !replaceOpen)
					continue;
				failures.expectOk(err, "writeFileAtomic cfg");
			}
		});
		done = true;
		reader.join();
		INFO("writeFileAtomic codes: " << atomicCodes.str() << " (can replace open files: " << replaceOpen << ")");
		INFO("failure details: " << failures.str());
		if (!replaceOpen)
			MESSAGE("native file system cannot replace open files (no POSIX rename semantics); writeFileAtomic checks relaxed");
		// There must be at least one successful write
		CHECK(atomicCodes.count(Error::OK) > 0);
		CHECK(failures.load() == 0);
		CHECK(test::listNames(e.fs, "/s/atomic") == std::vector<std::string>{"cfg"});
		const std::string final = test::readAll(e.fs, "/s/atomic/cfg");
		REQUIRE_FALSE(final.empty());
		CHECK(final.size() == static_cast<size_t>(2000 + 37 * (final[0] - 'a')));
	}

	TEST_CASE("remount churn: readers always see the complete content of one of the two versions")
	{
		test::TempDir dir;
		const auto v1 = test::randomBytes(50000, 1);
		const auto v2 = test::randomBytes(70000, 2);
		FileWriteOptions lz;
		lz.compress = true;
		REQUIRE(test::buildArchive(dir.sub("v1.vfs"), {{"data.bin", v1, {}}}) == Error::OK);
		REQUIRE(test::buildArchive(dir.sub("v2.vfs"), {{"data.bin", v2, lz}}) == Error::OK);
		auto a1 = ArchiveBackend::create(dir.sub("v1.vfs"));
		auto a2 = ArchiveBackend::create(dir.sub("v2.vfs"));
		REQUIRE(a1);
		REQUIRE(a2);
		VirtualFileSystem fs;
		MountId id = fs.mount("/dlc", a1);
		REQUIRE(id != 0);

		std::atomic<bool> done{false};
		test::FailureLog failures;
		std::atomic<int> reads{0};
		std::vector<std::thread> readers;
		for (int r = 0; r < THREADS - 1; ++r)
		{
			readers.emplace_back([&]()
			{
				while (!done.load())
				{
					auto s = fs.openFileStream("/dlc/data.bin", Mode::READ);
					if (!s)
					{
						++failures;
						continue;
					}
					auto got = test::readStream(*s);
					if (got != v1 && got != v2)
						++failures;
					++reads;
				}
			});
		}
		for (int i = 0; i < 2000 * test::scale() || reads.load() < 50; ++i)
		{
			failures.expectOk(fs.remount(id, (i % 2) ? a1 : a2), "remount");
			if (i > 2000000)
				break;
		}
		done = true;
		for (auto& th : readers)
			th.join();
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		CHECK(reads.load() >= 50);
	}

	TEST_CASE("mount / unmount churn: random layers at one mount point, readers only see one layer's complete content")
	{
		VirtualFileSystem fs;
		std::vector<std::shared_ptr<MemoryBackend>> layers;
		for (int i = 0; i < 4; ++i)
		{
			auto m = MemoryBackend::create();
			m->addFile("f.txt", std::string(1000 + i, static_cast<char>('0' + i)));
			m->setReadOnly(true);
			layers.push_back(m);
		}
		REQUIRE(fs.mount("/x", layers[0], env::prio(-100)) != 0);   // keep the bottom layer present the whole time

		std::atomic<bool> done{false};
		test::FailureLog failures;
		std::vector<std::thread> readers;
		for (int r = 0; r < 4; ++r)
		{
			readers.emplace_back([&]()
			{
				while (!done.load())
				{
					std::string s = test::readAll(fs, "/x/f.txt");
					if (s.size() < 1000 || s.size() > 1003 || s.find_first_not_of(s[0]) != std::string::npos ||
						s.size() != 1000u + static_cast<size_t>(s[0] - '0'))
						++failures;
					test::listNames(fs, "/x");
					fs.mounts();
				}
			});
		}
		runThreads(3, [&](int t)
		{
			std::mt19937 rng(static_cast<unsigned>(t));
			for (int i = 0; i < 1000 * test::scale(); ++i)
			{
				MountId id = fs.mount("/x", layers[1 + rng() % 3], env::prio(static_cast<int>(rng() % 3)));
				if (id == 0)
					++failures;
				std::this_thread::yield();
				failures.expectOk(fs.unmount(id), "unmount");
			}
		});
		done = true;
		for (auto& th : readers)
			th.join();
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		CHECK(fs.mounts().size() == 1);
	}

	TEST_CASE("1024 streams on one archive, 16 threads verify random seek / read")
	{
		test::TempDir dir;
		std::vector<test::ArchiveFile> files;
		FileWriteOptions xorOpt;
		xorOpt.encrypt = true;
		FileWriteOptions lzOpt;
		lzOpt.compress = true;
		for (int i = 0; i < 8; ++i)
			files.push_back({"f" + std::to_string(i), test::randomBytes(20000 + i * 1000, i), i % 3 == 0 ? xorOpt : (i % 3 == 1 ? lzOpt : FileWriteOptions{})});
		REQUIRE(test::buildArchive(dir.sub("h.vfs"), files) == Error::OK);
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("h.vfs")) != 0);

		const int perThread = 64;
		test::FailureLog failures;
		runThreads(16, [&](int t)
		{
			std::vector<std::unique_ptr<IFileStream>> streams;
			for (int i = 0; i < perThread; ++i)
			{
				auto s = fs.openFileStream("/a/f" + std::to_string((t + i) % 8), Mode::READ);
				if (!s)
				{
					++failures;
					return;
				}
				streams.push_back(std::move(s));
			}
			std::mt19937 rng(static_cast<unsigned>(t));
			for (int k = 0; k < 2000; ++k)
			{
				int i = static_cast<int>(rng() % perThread);
				const auto& content = files[(t + i) % 8].content;
				int64_t off = static_cast<int64_t>(rng() % content.size());
				uint8_t buf[64];
				if (streams[i]->seek(off, SEEK_SET) != off)
					++failures;
				int n = streams[i]->read(buf, sizeof(buf));
				int expect = static_cast<int>(std::min<int64_t>(64, static_cast<int64_t>(content.size()) - off));
				if (n != expect || std::memcmp(buf, content.data() + off, static_cast<size_t>(n)) != 0)
					++failures;
			}
		});
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
	}

	TEST_CASE("walk / stat / exists concurrent with modifications: no crash, consistent results")
	{
		VirtualFileSystem fs;
		REQUIRE(fs.mountMemory("/m") != 0);
		std::atomic<bool> done{false};
		test::FailureLog failures;
		std::thread walker([&]()
		{
			while (!done.load())
			{
				fs.walk("/m", [&](const std::string& full, const DirEntry&)
				{
					if (full.rfind("/m/", 0) != 0)
						++failures;
					return true;
				});
			}
		});
		const uint64_t g0 = fs.generation();
		runThreads(THREADS, [&](int t)
		{
			uint64_t last = 0;
			for (int i = 0; i < 200 * test::scale(); ++i)
			{
				const std::string d = "/m/t" + std::to_string(t) + "/" + std::to_string(i % 7);
				fs.createDirs(d);
				fs.writeFile(d + "/f", "x");
				fs.exists(d + "/f");
				if (i % 2)
					fs.removeDir("/m/t" + std::to_string(t), true);
				uint64_t g = fs.generation();
				if (g < last)
					++failures;   // generation increases monotonically
				last = g;
			}
		});
		done = true;
		walker.join();
		INFO("failure details: " << failures.str());
		CHECK(failures.load() == 0);
		CHECK(fs.generation() > g0);
	}
}
