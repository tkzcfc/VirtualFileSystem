// Deadlock and reentrancy tests: every case runs under a watchdog; a timeout means a deadlock (or livelock), so the case fails and the process exits
// (a deadlocked thread cannot be joined, and continuing would hang the whole test run). Coverage:
// - unmount / strict unmount / remount while a stream is still open;
// - callbacks (list / walk callbacks, the log callback, a custom backend) reenter the VFS during an internal VFS call;
// - mount / unmount / alias changes concurrent with various I/O;
// - cross-backend rename, and opposing cross-mount moves;
// - nested locking: memory-backend directory locks (rename / remove / lookup crossing), and copy-up segment locks stacked with backend locks
#include "TestUtils.h"
#include "vfs/Log.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/memory/MemoryBackend.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <thread>

using namespace vfs;

namespace
{

// Run fn on its own thread and wait at most seconds; a timeout is treated as a deadlock: print the case name and exit the process immediately.
void runWithWatchdog(const char* name, const std::function<void()>& fn, int seconds = 120)
{
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	std::thread worker([&]()
	{
		fn();
		std::lock_guard<std::mutex> lock(m);
		done = true;
		cv.notify_all();
	});
	std::unique_lock<std::mutex> lock(m);
	if (!cv.wait_for(lock, std::chrono::seconds(seconds), [&]() { return done; }))
	{
		std::fprintf(stderr, "DEADLOCK: \"%s\" did not finish within %d seconds\n", name, seconds);
		std::fflush(stderr);
		std::_Exit(3);
	}
	lock.unlock();
	worker.join();
}

int rounds(int base)
{
	return base * test::scale();
}

/**
 * A backend that forwards calls to an inner backend and invokes a hook on stat / open / list (the hook may reenter the VFS),
 * simulating a custom backend that touches the VFS from a callback.
 */
class HookBackend : public IBackend
{
public:
	explicit HookBackend(std::shared_ptr<IBackend> inner) : m_inner(std::move(inner)) {}

	std::function<void(std::string_view op, std::string_view rel)> hook;

	virtual uint32_t capabilities() const override { return m_inner->capabilities(); }

	virtual Error stat(std::string_view rel, Stat& out) const override
	{
		call("stat", rel);
		return m_inner->stat(rel, out);
	}

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override
	{
		call("open", rel);
		return m_inner->open(rel, mode, err);
	}

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override
	{
		call("list", relDir);
		return m_inner->list(relDir, cb);
	}

	virtual Error createDir(std::string_view rel) override { return m_inner->createDir(rel); }
	virtual Error removeDir(std::string_view rel) override { return m_inner->removeDir(rel); }
	virtual Error removeFile(std::string_view rel) override { return m_inner->removeFile(rel); }
	virtual Error rename(std::string_view from, std::string_view to) override { return m_inner->rename(from, to); }
	virtual const char* typeName() const override { return "hook"; }

private:
	void call(std::string_view op, std::string_view rel) const
	{
		// Each thread reenters only one level, so a VFS call inside the hook does not fire the hook again and recurse forever.
		thread_local int depth = 0;
		if (!hook || depth > 0)
			return;
		++depth;
		hook(op, rel);
		--depth;
	}

	std::shared_ptr<IBackend> m_inner;
};

} // namespace

TEST_SUITE("deadlock" * VFS_NEEDS_THREADS)
{
	TEST_CASE("unmount / FailIfBusy / remount while streams of every backend are open and being read")
	{
		runWithWatchdog("unmount while streams open", []()
		{
			test::TempDir dir;
			const auto content = test::randomBytes(64 * 1024, 11);
			dir.write("n/data.bin", test::str(content));
			REQUIRE(test::buildArchive(dir.sub("a.vfs"), {{"data.bin", content, {}}}) == Error::OK);
			auto mem = MemoryBackend::create();
			mem->addFile("data.bin", std::vector<uint8_t>(content));
			std::vector<std::shared_ptr<IBackend>> backends = {
				mem, NativeBackend::create(dir.sub("n")), ArchiveBackend::create(dir.sub("a.vfs"))};
			for (auto& b : backends)
				REQUIRE(b);

			for (auto& backend : backends)
			{
				CAPTURE(backend->typeName());
				VirtualFileSystem fs;
				MountId id = fs.mount("/m", backend);
				// Single thread: an open stream can still be read to the end after a lazy unmount; strict unmount returns BUSY while a stream is open, and succeeds after it is closed.
				auto s1 = fs.openFileStream("/m/data.bin", IFileStream::Mode::READ);
				REQUIRE(s1);
				CHECK(fs.unmount(id, UnmountFlags::FailIfBusy) == Error::BUSY);
				CHECK(fs.remount(id, backend) == Error::OK);
				CHECK(fs.unmount(id, UnmountFlags::FailIfBusy) == Error::BUSY);   // A stream opened before remount still counts.
				CHECK(fs.unmount(id) == Error::OK);
				CHECK(test::readStream(*s1) == content);
				s1.reset();
				CHECK(!fs.exists("/m/data.bin"));

				// Multiple threads: reader threads repeatedly open / read to the end / close, while another thread repeatedly mounts, strict-unmounts, lazy-unmounts, and remounts.
				std::atomic<bool> stop{false};
				std::atomic<int> failures{0};
				std::atomic<int> reads{0};
				id = fs.mount("/m", backend);
				std::atomic<MountId> current{id};
				std::thread churn([&]()
				{
					for (int i = 0; i < rounds(400) || (reads.load() < 20 && i < 100000); ++i)
					{
						MountId cur = current.load();
						Error e = fs.unmount(cur, i % 2 ? static_cast<uint32_t>(UnmountFlags::FailIfBusy) : 0u);
						if (e == Error::BUSY)
						{
							if (fs.remount(cur, backend) != Error::OK)
								++failures;
							continue;
						}
						if (e != Error::OK)
							++failures;
						std::this_thread::yield();
						current = fs.mount("/m", backend);
					}
					stop = true;
				});
				std::vector<std::thread> readers;
				for (int t = 0; t < 6; ++t)
				{
					readers.emplace_back([&]()
					{
						while (!stop.load())
						{
							Error e = Error::OK;
							auto s = fs.openFileStream("/m/data.bin", IFileStream::Mode::READ, &e);
							if (!s)
							{
								if (e != Error::NOT_FOUND && e != Error::BUSY)
									++failures;
								continue;
							}
							if (test::readStream(*s) != content)
								++failures;
							++reads;
						}
					});
				}
				churn.join();
				for (auto& th : readers)
					th.join();
				CHECK(failures.load() == 0);
				CHECK(fs.unmount(current.load(), UnmountFlags::FailIfBusy) == Error::OK);
			}
		});
	}

	TEST_CASE("list / walk callbacks re-enter the VFS: queries, writes, mount and unmount from inside the callback")
	{
		runWithWatchdog("callbacks re-entering VFS", []()
		{
			test::TempDir dir;
			dir.write("n/a.txt", "a");
			dir.write("n/sub/b.txt", "b");
			VirtualFileSystem fs;
			std::shared_ptr<MemoryBackend> mem;
			fs.mountMemory("/", MountOptions{10, MountFlags::WriteTarget}, &mem);
			mem->addFile("x/1.txt", "1");
			mem->addFile("x/y/2.txt", "2");
			fs.mountNative("/n", dir.sub("n"));

			int calls = 0;
			for (const char* root : {"/", "/x", "/n"})
			{
				CAPTURE(root);
				Error e = fs.list(root, [&](const DirEntry& entry)
				{
					++calls;
					std::string p = std::string(root) + (std::string(root) == "/" ? "" : "/") + entry.name;
					Stat st;
					CHECK(fs.stat(p, st) == Error::OK);
					CHECK(fs.writeFile("/cb-" + std::to_string(calls) + ".txt", "z") == Error::OK);
					CHECK(fs.createDirs("/cbdir/" + std::to_string(calls)) == Error::OK);
					MountId id = fs.mountMemory("/tmp-" + std::to_string(calls));
					CHECK(id != 0);
					CHECK(fs.unmount(id) == Error::OK);
					fs.list(p, [](const DirEntry&) { return true; });
					return true;
				});
				CHECK(e == Error::OK);
			}
			Error e = fs.walk("/", [&](const std::string& full, const DirEntry& entry)
			{
				if (entry.type == FileType::FILE && full.rfind("/cb-", 0) == 0)
				{
					CHECK(fs.removeFile(full) == Error::OK);   // Delete a file in the directory currently being walked.
					return true;
				}
				if (full == "/x/y")
					CHECK(fs.rename("/x/y", "/x/z") == Error::OK);   // Rename a subdirectory that is about to be entered: the subdirectory disappearing is not an error.
				fs.addAlias("@w", "/x");
				fs.removeAlias("@w");
				return true;
			});
			CHECK(e == Error::OK);
			CHECK(calls > 0);
			CHECK(fs.isFile("/x/z/2.txt"));
		});
	}

	TEST_CASE("a backend and the logger re-enter the VFS while it is inside stat / open / list / copy-up")
	{
		runWithWatchdog("backend and logger re-entering VFS", []()
		{
			VirtualFileSystem fs;
			auto lowerMem = MemoryBackend::create();
			lowerMem->addFile("data/base.txt", "base");
			auto upperMem = MemoryBackend::create();
			auto lower = std::make_shared<HookBackend>(lowerMem);
			auto upper = std::make_shared<HookBackend>(upperMem);
			fs.mount("/", lower, MountOptions{0, MountFlags::ReadOnly});
			fs.mount("/", upper, MountOptions{10, MountFlags::WriteTarget});

			std::atomic<int> hooks{0};
			std::atomic<int> logs{0};
			setLogger([&](LogLevel, std::string_view)
			{
				// Touch the VFS from the log callback: read, write, and list a directory.
				++logs;
				fs.exists("/data/base.txt");
				fs.writeFile("/data/log.txt", "l");
				test::listNames(fs, "/data");
			});
			auto reenter = [&](std::string_view op, std::string_view rel)
			{
				++hooks;
				// The backend reenters during a VFS call (including inside the copy-up lock): query, open the same path for append, write another file, change the mount table, and log.
				fs.exists("/data/base.txt");
				if (op == "open" && rel == "data/base.txt")
				{
					auto s = fs.openFileStream("/data/base.txt", IFileStream::Mode::READ);
					CHECK(s);
				}
				fs.writeFile("/data/hook.txt", "h");
				MountId id = fs.mountMemory("/hook-mnt");
				fs.unmount(id);
				vfs::log(LogLevel::INFO, "hook");
			};
			lower->hook = reenter;
			upper->hook = reenter;

			CHECK(test::readAll(fs, "/data/base.txt") == "base");
			// APPEND a file from a read-only layer: copy-up reads the lower layer and writes the upper layer under the segment lock, and the backend hook reenters while that lock is held.
			{
				auto s = fs.openFileStream("/data/base.txt", IFileStream::Mode::APPEND);
				REQUIRE(s);
				CHECK(s->write("+", 1) == 1);
			}
			CHECK(test::readAll(fs, "/data/base.txt") == "base+");
			CHECK(test::listNames(fs, "/data").size() >= 3);
			// Several threads do the same thing at once.
			std::vector<std::thread> threads;
			for (int t = 0; t < 4; ++t)
			{
				threads.emplace_back([&, t]()
				{
					for (int i = 0; i < rounds(50); ++i)
					{
						auto s = fs.openFileStream("/data/f" + std::to_string(t) + ".txt", IFileStream::Mode::APPEND);
						if (s)
							s->write("x", 1);
						test::listNames(fs, "/data");
					}
				});
			}
			for (auto& th : threads)
				th.join();
			setLogger(nullptr);
			CHECK(hooks.load() > 0);
			CHECK(logs.load() > 0);
		});
	}

	TEST_CASE("mount / unmount / remount / alias churn concurrent with reads, writes, renames, removes, lists and walks")
	{
		runWithWatchdog("concurrent mount/unmount + IO", []()
		{
			test::TempDir dir;
			dir.write("n/keep.txt", "native");
			REQUIRE(test::buildArchive(dir.sub("a.vfs"), {{"keep.txt", test::bytes("archive"), {}}}) == Error::OK);
			auto archive = ArchiveBackend::create(dir.sub("a.vfs"));
			auto native = NativeBackend::create(dir.sub("n"));
			REQUIRE(archive);
			REQUIRE(native);

			VirtualFileSystem fs;
			fs.mountMemory("/", MountOptions{100, MountFlags::WriteTarget});
			fs.createDirs("/w");
			std::atomic<bool> stop{false};
			std::atomic<int> failures{0};
			std::thread churn([&]()
			{
				for (int i = 0; i < rounds(300); ++i)
				{
					MountId a = fs.mount("/w", archive, MountOptions{i % 3, 0});
					MountId n = fs.mount("/w", native, MountOptions{(i + 1) % 3, 0});
					fs.addAlias("@w", "/w");
					fs.remount(a, i % 2 ? archive : std::static_pointer_cast<IBackend>(MemoryBackend::create()));
					std::this_thread::yield();
					if (fs.unmount(a) != Error::OK)
						++failures;
					Error e = fs.unmount(n, UnmountFlags::FailIfBusy);   // BUSY while a reader thread holds a stream; fall back to a lazy unmount.
					if (e == Error::BUSY)
						e = fs.unmount(n);
					if (e != Error::OK)
						++failures;
					fs.removeAlias("@w");
				}
				stop = true;
			});
			std::vector<std::thread> workers;
			for (int t = 0; t < 6; ++t)
			{
				workers.emplace_back([&, t]()
				{
					const std::string mine = "/w/t" + std::to_string(t);
					int i = 0;
					while (!stop.load())
					{
						++i;
						fs.createDirs(mine + "/d");
						fs.writeFile(mine + "/d/f.txt", "data");
						fs.writeFileAtomic(mine + "/atomic.txt", "atomic", 6);
						fs.rename(mine + "/d/f.txt", mine + "/d/g.txt");
						auto s = fs.openFileStream("/w/keep.txt", IFileStream::Mode::READ);
						if (s)
						{
							std::string got = test::str(test::readStream(*s));
							// With no mount, APPEND creates an empty keep.txt on the top memory layer.
							if (got != "archive" && got != "native" && !got.empty())
								++failures;
						}
						fs.openFileStream("@w/keep.txt", IFileStream::Mode::APPEND);
						test::listNames(fs, "/w");
						fs.walk("/", [](const std::string&, const DirEntry&) { return true; });
						if (i % 4 == 0)
							fs.removeDir(mine, true);
					}
				});
			}
			churn.join();
			for (auto& th : workers)
				th.join();
			CHECK(failures.load() == 0);
		});
	}

	TEST_CASE("cross-backend rename and opposite-direction moves between memory and native mounts")
	{
		runWithWatchdog("cross-backend rename", []()
		{
			test::TempDir dir;
			VirtualFileSystem fs;
			fs.mountMemory("/mem");
			fs.mountNative("/nat", dir.path());
			const int files = 8;
			for (int i = 0; i < files; ++i)
			{
				REQUIRE(fs.writeFile("/mem/m" + std::to_string(i), "m" + std::to_string(i)) == Error::OK);
				REQUIRE(fs.writeFile("/nat/n" + std::to_string(i), "n" + std::to_string(i)) == Error::OK);
			}
			// A cross-mount rename is always CROSS_DEVICE and does not take the other backend's lock.
			CHECK(fs.rename("/mem/m0", "/nat/m0") == Error::CROSS_DEVICE);
			CHECK(fs.rename("/nat/n0", "/mem/n0") == Error::CROSS_DEVICE);

			std::atomic<int> failures{0};
			std::vector<std::thread> threads;
			// Two groups of threads move the same files in opposite directions: memory -> native, and native -> memory.
			for (int t = 0; t < 4; ++t)
			{
				threads.emplace_back([&, t]()
				{
					for (int i = 0; i < rounds(100); ++i)
					{
						const int k = (i + t) % files;
						for (const char* name : {"m", "n"})
						{
							const std::string a = "/mem/" + std::string(name) + std::to_string(k);
							const std::string b = "/nat/" + std::string(name) + std::to_string(k);
							Error e = (t % 2) ? fs.moveFile(a, b) : fs.moveFile(b, a);
							if (e != Error::OK && e != Error::NOT_FOUND)
							{
								std::fprintf(stderr, "moveFile: %s\n", errorName(e));
								++failures;
							}
							e = fs.rename(a, b);
							if (e != Error::CROSS_DEVICE && e != Error::NOT_FOUND)
							{
								std::fprintf(stderr, "rename: %s\n", errorName(e));
								++failures;
							}
						}
					}
				});
			}
			for (auto& th : threads)
				th.join();
			CHECK(failures.load() == 0);
			// Opposing moves are "copy + delete" and are not atomic: both sides may delete the file (the docs state that moveFile across mounts is not atomic).
			// The requirement here is only that there is no deadlock and that any file that remains has intact contents.
			for (int i = 0; i < files; ++i)
			{
				for (const char* name : {"m", "n"})
				{
					const std::string want = std::string(name) + std::to_string(i);
					for (const char* root : {"/mem/", "/nat/"})
					{
						if (fs.exists(root + want))
							CHECK(test::readAll(fs, root + want) == want);
					}
				}
			}
		});
	}

	TEST_CASE("nested locks: memory directory locks under crossing renames, removes, deep lookups and copy-up")
	{
		runWithWatchdog("nested locks", []()
		{
			VirtualFileSystem fs;
			auto lower = MemoryBackend::create();
			for (int i = 0; i < 4; ++i)
				lower->addFile("ro/f" + std::to_string(i) + ".txt", "ro");
			fs.mount("/", lower, MountOptions{0, MountFlags::ReadOnly});
			std::shared_ptr<MemoryBackend> mem;
			fs.mountMemory("/", MountOptions{10, MountFlags::WriteTarget}, &mem);
			for (const char* d : {"a/b/c", "a/x/y", "p/q/r", "p/s/t"})
				REQUIRE(fs.createDirs(std::string("/") + d) == Error::OK);

			std::atomic<bool> stop{false};
			std::vector<std::thread> threads;
			// Crossing renames: a/b <-> p/q, a/x/y <-> p/s/t/y, and between ancestor and descendant (a/b/c -> a/c, a/c -> a/b/c).
			const std::vector<std::pair<std::string, std::string>> moves = {
				{"/a/b", "/p/q2"}, {"/p/q2", "/a/b"}, {"/p/q", "/a/b2"}, {"/a/b2", "/p/q"},
				{"/a/x/y", "/p/s/t/y"}, {"/p/s/t/y", "/a/x/y"}, {"/a/b/c", "/a/c"}, {"/a/c", "/a/b/c"},
				{"/p/s/t", "/p/t"}, {"/p/t", "/p/s/t"}};
			for (int t = 0; t < 4; ++t)
			{
				threads.emplace_back([&, t]()
				{
					for (int i = 0; i < rounds(500); ++i)
					{
						const auto& m = moves[static_cast<size_t>(i * 4 + t) % moves.size()];
						fs.rename(m.first, m.second);
					}
				});
			}
			// Delete / create / deep lookup / list crossing with rename.
			threads.emplace_back([&]()
			{
				while (!stop.load())
				{
					fs.createDirs("/a/b/c/d/e");
					fs.writeFile("/a/b/c/d/e/f.txt", "f");
					fs.removeDir("/a/b/c/d", true);
					fs.removeDir("/p/s/t/y");
					fs.createDir("/p/s/t/y");
				}
			});
			threads.emplace_back([&]()
			{
				while (!stop.load())
				{
					fs.exists("/a/b/c/d/e/f.txt");
					fs.isDir("/p/s/t/y");
					test::listNames(fs, "/a");
					test::listNames(fs, "/p/s");
					fs.walk("/", [](const std::string&, const DirEntry&) { return true; });
				}
			});
			// Copy-up segment lock plus memory-backend directory locks: append read-only-layer files while renaming the parent directories that contain them.
			threads.emplace_back([&]()
			{
				for (int i = 0; !stop.load(); ++i)
				{
					auto s = fs.openFileStream("/ro/f" + std::to_string(i % 4) + ".txt", IFileStream::Mode::APPEND);
					if (s)
						s->write("+", 1);
					if (i % 8 == 0)
					{
						fs.rename("/ro", "/ro2");
						fs.rename("/ro2", "/ro");
					}
				}
			});
			for (int t = 0; t < 4; ++t)
				threads[static_cast<size_t>(t)].join();
			stop = true;
			for (size_t t = 4; t < threads.size(); ++t)
				threads[t].join();
			// The tree is intact: every directory can still be listed, and each subtree appears only once.
			std::set<std::string> seen;
			int duplicates = 0;
			Error we = fs.walk("/", [&](const std::string& p, const DirEntry&)
			{
				if (!seen.insert(p).second)
					++duplicates;
				return true;
			});
			CHECK(we == Error::OK);
			CHECK(duplicates == 0);
			CHECK(fs.isDir("/a"));
			CHECK(fs.isDir("/p"));
		});
	}
}
