// Game-style layered scenarios: base.vfs (archive with LZ4 block + XOR entries) + patch.vfs (higher priority) + a native
// "mods" directory + a memory hot-fix layer, with aliases, lazy unmount, remount, copy-up, Opaque, and a concurrent variant
// where readers never see torn content while another thread swaps layers and writes overrides.
#include "TestUtils.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/memory/MemoryBackend.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

using namespace vfs;

namespace
{

FileWriteOptions fo(bool encrypt, bool compress, uint8_t blockLog2 = 0)
{
	FileWriteOptions o;
	o.encrypt = encrypt;
	o.compress = compress;
	o.blockLog2 = blockLog2;
	return o;
}

// Self-describing content: "<tag>|" followed by bytes derived from the tag, so any mix of two versions is detectable.
std::vector<uint8_t> versioned(const std::string& tag, size_t size)
{
	std::vector<uint8_t> v(tag.begin(), tag.end());
	v.push_back('|');
	uint32_t h = 2166136261u;
	for (char c : tag)
		h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
	while (v.size() < size)
	{
		h = h * 1103515245u + 12345u;
		v.push_back(static_cast<uint8_t>('a' + (h >> 16) % 26));
	}
	return v;
}

// Returns the tag if content is a complete, consistent versioned() blob; empty string otherwise.
std::string checkVersioned(const std::vector<uint8_t>& data)
{
	auto bar = std::find(data.begin(), data.end(), '|');
	if (bar == data.end())
		return {};
	std::string tag(data.begin(), bar);
	return versioned(tag, data.size()) == data ? tag : std::string();
}

struct Game
{
	test::TempDir dir;
	std::string base, patch, patch2;

	Game()
	{
		// base.vfs: scripts LZ4 (small blocks so reads span several), textures XOR, a large LZ4+XOR level
		REQUIRE(test::buildArchive(base = dir.sub("base.vfs"), {
			{"scripts/main.lua", versioned("base-main", 20000), fo(false, true, 12)},
			{"scripts/ai.lua", versioned("base-ai", 9000), fo(true, true, 12)},
			{"textures/hero.png", versioned("base-hero", 30000), fo(true, false)},
			{"levels/1.map", versioned("base-l1", 300000), fo(true, true)},
			{"levels/2.map", versioned("base-l2", 5000), fo(false, false)},
			{"config.json", versioned("base-config", 300), fo(false, false)},
		}) == Error::OK);
		// patch.vfs: overrides main.lua and level 1, adds level 3 and a nested dlc tree
		REQUIRE(test::buildArchive(patch = dir.sub("patch.vfs"), {
			{"scripts/main.lua", versioned("patch-main", 25000), fo(false, true, 13)},
			{"levels/1.map", versioned("patch-l1", 200000), fo(false, true)},
			{"levels/3.map", versioned("patch-l3", 7000), fo(true, false)},
			{"dlc/pack1/items/sword.json", versioned("patch-sword", 500), fo(false, false)},
		}) == Error::OK);
		REQUIRE(test::buildArchive(patch2 = dir.sub("patch2.vfs"), {
			{"scripts/main.lua", versioned("patch2-main", 26000), fo(false, true)},
			{"levels/4.map", versioned("patch2-l4", 4000), fo(false, false)},
		}) == Error::OK);
		dir.write("mods/textures/hero.png", versioned("mod-hero", 31000));
		dir.write("mods/levels/2.map", versioned("mod-l2", 5100));
		dir.mkdir("save");
	}
};

std::string tagOf(const VirtualFileSystem& fs, const std::string& path)
{
	std::vector<uint8_t> data;
	if (fs.readFile(path, data) != Error::OK)
		return "<missing>";
	return checkVersioned(data);
}

struct Stack
{
	MountId base = 0, patch = 0, mods = 0, hot = 0, save = 0;
	std::shared_ptr<MemoryBackend> hotfix;
};

Stack mountStack(VirtualFileSystem& fs, const Game& g)
{
	Stack s;
	s.base = fs.mountArchive("/res", g.base, {}, MountOptions{0, 0});
	s.patch = fs.mountArchive("/res", g.patch, {}, MountOptions{10, 0});
	s.mods = fs.mountNative("/res", g.dir.sub("mods"), MountOptions{20, MountFlags::ReadOnly});
	s.hot = fs.mountMemory("/res", MountOptions{30, MountFlags::WriteTarget}, &s.hotfix);
	s.save = fs.mountNative("/save", g.dir.sub("save"));
	REQUIRE((s.base && s.patch && s.mods && s.hot && s.save));
	fs.addAlias("res://", "/res");
	return s;
}

void runWithWatchdog(const char* name, const std::function<void()>& fn, int seconds = 180)
{
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	std::thread worker([&] {
		fn();
		std::lock_guard<std::mutex> lock(m);
		done = true;
		cv.notify_all();
	});
	std::unique_lock<std::mutex> lock(m);
	if (!cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return done; }))
	{
		std::fprintf(stderr, "DEADLOCK: \"%s\" did not finish within %d seconds\n", name, seconds);
		std::fflush(stderr);
		std::_Exit(3);
	}
	lock.unlock();
	worker.join();
}

} // namespace

TEST_SUITE("scenario")
{
	TEST_CASE("layered game assets: topmost version wins for read, stat, list and walk")
	{
		Game g;
		VirtualFileSystem fs;
		Stack s = mountStack(fs, g);
		s.hotfix->addFile("config.json", versioned("hot-config", 320));

		CHECK(tagOf(fs, "/res/scripts/main.lua") == "patch-main");
		CHECK(tagOf(fs, "/res/scripts/ai.lua") == "base-ai");
		CHECK(tagOf(fs, "/res/textures/hero.png") == "mod-hero");
		CHECK(tagOf(fs, "/res/levels/1.map") == "patch-l1");
		CHECK(tagOf(fs, "/res/levels/2.map") == "mod-l2");
		CHECK(tagOf(fs, "/res/levels/3.map") == "patch-l3");
		CHECK(tagOf(fs, "/res/config.json") == "hot-config");
		CHECK(tagOf(fs, "res://dlc/pack1/items/sword.json") == "patch-sword");

		Stat st;
		REQUIRE(fs.stat("/res/levels/1.map", st) == Error::OK);
		CHECK(st.size == 200000);
		CHECK(st.mount == s.patch);
		REQUIRE(fs.stat("res://textures/hero.png", st) == Error::OK);
		CHECK(st.size == 31000);
		CHECK(st.mount == s.mods);
		CHECK_FALSE(st.writable);
		REQUIRE(fs.stat("/res/config.json", st) == Error::OK);
		CHECK((st.size == 320 && st.mount == s.hot && st.writable));

		CHECK(test::listNames(fs, "/res/levels") == std::vector<std::string>{"1.map", "2.map", "3.map"});
		CHECK(test::listNames(fs, "/res") == std::vector<std::string>{"config.json", "dlc/", "levels/", "scripts/", "textures/"});
		std::set<std::string> walked;
		size_t files = 0;
		fs.walk("res://", [&](const std::string& p, const DirEntry& e) {
			CHECK(walked.insert(p).second);   // no duplicates across layers
			files += e.type == FileType::FILE;
			return true;
		});
		CHECK(files == 8);   // 6 base files + 2 new in the patch; overrides are not duplicated
		CHECK(walked.count("/res/dlc/pack1/items/sword.json") == 1);

		// Large LZ4+XOR file streamed in odd-sized chunks through the stack
		auto f = fs.openFileStream("/res/scripts/ai.lua", IFileStream::Mode::READ);
		REQUIRE(f);
		CHECK(checkVersioned(test::readStream(*f)) == "base-ai");
	}

	TEST_CASE("equal priority: the later mount wins; unmount reveals the earlier one")
	{
		Game g;
		VirtualFileSystem fs;
		MountId a = fs.mountArchive("/res", g.patch, {}, MountOptions{5, 0});
		MountId b = fs.mountArchive("/res", g.patch2, {}, MountOptions{5, 0});
		REQUIRE((a && b));
		CHECK(tagOf(fs, "/res/scripts/main.lua") == "patch2-main");
		CHECK(fs.unmount(b) == Error::OK);
		CHECK(tagOf(fs, "/res/scripts/main.lua") == "patch-main");
	}

	TEST_CASE("unmount patch: base versions return; streams opened from the patch stay readable (lazy unmount)")
	{
		Game g;
		VirtualFileSystem fs;
		Stack s = mountStack(fs, g);
		auto open = fs.openFileStream("/res/levels/1.map", IFileStream::Mode::READ);
		REQUIRE(open);
		std::vector<uint8_t> head(1000);
		REQUIRE(open->read(head.data(), 1000) == 1000);

		CHECK(fs.unmount(s.patch, UnmountFlags::FailIfBusy) == Error::BUSY);
		CHECK(fs.unmount(s.patch) == Error::OK);
		CHECK(tagOf(fs, "/res/levels/1.map") == "base-l1");
		CHECK(tagOf(fs, "/res/scripts/main.lua") == "base-main");
		CHECK_FALSE(fs.exists("/res/levels/3.map"));
		CHECK_FALSE(fs.exists("/res/dlc"));
		CHECK(test::listNames(fs, "/res/levels") == std::vector<std::string>{"1.map", "2.map"});

		// The old stream still reads the patch version to the end
		auto rest = test::readStream(*open);
		head.insert(head.end(), rest.begin(), rest.end());
		CHECK(checkVersioned(head) == "patch-l1");
	}

	TEST_CASE("remount patch with a new archive swaps content atomically; old streams keep the old version")
	{
		Game g;
		VirtualFileSystem fs;
		Stack s = mountStack(fs, g);
		auto old = fs.openFileStream("/res/scripts/main.lua", IFileStream::Mode::READ);
		REQUIRE(old);
		CHECK(fs.remount(s.patch, ArchiveBackend::create(g.patch2)) == Error::OK);
		CHECK(tagOf(fs, "/res/scripts/main.lua") == "patch2-main");
		CHECK(fs.exists("/res/levels/4.map"));
		CHECK_FALSE(fs.exists("/res/levels/3.map"));
		CHECK(tagOf(fs, "/res/levels/1.map") == "base-l1");
		CHECK(checkVersioned(test::readStream(*old)) == "patch-main");
	}

	TEST_CASE("writes go to the write target with copy-up of the visible version; removing the override reveals the lower one")
	{
		Game g;
		VirtualFileSystem fs;
		Stack s = mountStack(fs, g);

		// WRITE: truncates, no copy
		{
			auto w = fs.openFileStream("/res/levels/2.map", IFileStream::Mode::WRITE);
			REQUIRE(w);
			CHECK(w->write("new", 3) == 3);
		}
		CHECK(test::readAll(fs, "/res/levels/2.map") == "new");
		CHECK(s.hotfix->open("levels/2.map", IFileStream::Mode::READ, nullptr) != nullptr);

		// APPEND: copy-up of the visible (patch, LZ4) version, then append
		{
			auto w = fs.openFileStream("res://scripts/main.lua", IFileStream::Mode::APPEND);
			REQUIRE(w);
			CHECK(w->write("--tail", 6) == 6);
		}
		std::vector<uint8_t> appended;
		REQUIRE(fs.readFile("/res/scripts/main.lua", appended) == Error::OK);
		REQUIRE(appended.size() == 25006);
		CHECK(checkVersioned(std::vector<uint8_t>(appended.begin(), appended.end() - 6)) == "patch-main");

		// OVERLAPPED: copy-up of the native mod version, then patch bytes in place
		{
			auto w = fs.openFileStream("/res/textures/hero.png", IFileStream::Mode::OVERLAPPED);
			REQUIRE(w);
			CHECK(w->size() == 31000);
			CHECK(w->seek(30000, SEEK_SET) == 30000);
			CHECK(w->write("XY", 2) == 2);
		}
		std::vector<uint8_t> hero;
		REQUIRE(fs.readFile("/res/textures/hero.png", hero) == Error::OK);
		auto expected = versioned("mod-hero", 31000);
		expected[30000] = 'X';
		expected[30001] = 'Y';
		CHECK(hero == expected);
		// The native mod file itself is untouched
		CHECK(checkVersioned(test::readNative(g.dir.sub("mods/textures/hero.png"))) == "mod-hero");

		// New file in a directory that exists only in the patch archive: parents are created on the write layer
		CHECK(fs.writeFile("/res/dlc/pack1/items/shield.json", "{}") == Error::OK);
		CHECK(test::listNames(fs, "/res/dlc/pack1/items") == std::vector<std::string>{"shield.json", "sword.json"});

		// Removing overrides reveals the lower versions; read-only layers cannot be deleted
		CHECK(fs.removeFile("/res/scripts/main.lua") == Error::OK);
		CHECK(tagOf(fs, "/res/scripts/main.lua") == "patch-main");
		CHECK(fs.removeFile("/res/scripts/main.lua") == Error::READ_ONLY);
		CHECK(fs.removeFile("/res/textures/hero.png") == Error::OK);
		CHECK(tagOf(fs, "/res/textures/hero.png") == "mod-hero");
		CHECK(fs.removeFile("/res/levels/2.map") == Error::OK);
		CHECK(tagOf(fs, "/res/levels/2.map") == "mod-l2");

		// Saves go to their own mount
		CHECK(fs.writeFile("/save/slot1.dat", "level=3") == Error::OK);
		CHECK(g.dir.read("save/slot1.dat") == "level=3");
	}

	TEST_CASE("Opaque layer hides everything below; aliases resolve across the stack")
	{
		Game g;
		VirtualFileSystem fs;
		Stack s = mountStack(fs, g);
		std::shared_ptr<MemoryBackend> veil;
		MountId id = fs.mountMemory("/res/levels", MountOptions{15, MountFlags::Opaque}, &veil);
		REQUIRE(id);
		veil->addFile("only.map", versioned("veil-only", 100));
		// Layers below priority 15 (base, patch) are hidden under /res/levels; mods (20) and hot-fix (30) stay visible
		CHECK(test::listNames(fs, "/res/levels") == std::vector<std::string>{"2.map", "only.map"});
		CHECK_FALSE(fs.exists("res://levels/1.map"));
		CHECK(tagOf(fs, "res://levels/2.map") == "mod-l2");
		CHECK(tagOf(fs, "/res/scripts/main.lua") == "patch-main");   // outside the opaque mount point
		CHECK(fs.unmount(id) == Error::OK);
		CHECK(tagOf(fs, "res://levels/1.map") == "patch-l1");
		CHECK(fs.addAlias("lvl://", "/res/levels") == Error::OK);
		CHECK(tagOf(fs, "lvl://3.map") == "patch-l3");
		(void)s;
	}

	TEST_CASE("concurrent: readers always see a complete version while layers are swapped and overrides written" * VFS_NEEDS_THREADS)
	{
		Game g;
		// Extra patch versions for remount churn
		std::vector<std::string> versions;
		for (int v = 0; v < 4; ++v)
		{
			versions.push_back(g.dir.sub("v" + std::to_string(v) + ".vfs"));
			REQUIRE(test::buildArchive(versions.back(), {
				{"scripts/main.lua", versioned("v" + std::to_string(v) + "-main", 30000 + v * 1000), fo(v % 2 == 1, true, 12)},
				{"levels/1.map", versioned("v" + std::to_string(v) + "-l1", 150000), fo(false, true)},
			}) == Error::OK);
		}
		std::vector<std::shared_ptr<ArchiveBackend>> backends;
		for (auto& p : versions)
			backends.push_back(ArchiveBackend::create(p));

		runWithWatchdog("scenario concurrent", [&] {
			VirtualFileSystem fs;
			Stack s = mountStack(fs, g);
			std::atomic<bool> stop{false};
			test::FailureLog failures;
			std::atomic<long> reads{0};

			std::vector<std::thread> readers;
			for (int t = 0; t < 4; ++t)
				readers.emplace_back([&, t] {
					const char* paths[] = {"/res/scripts/main.lua", "res://levels/1.map", "/res/config.json", "/res/textures/hero.png"};
					while (!stop)
					{
						for (const char* p : paths)
						{
							std::vector<uint8_t> data;
							const Error e = fs.readFile(p, data);
							if (e == Error::OK && checkVersioned(data).empty())
								failures.add(std::string("torn read of ") + p);
							else if (e != Error::OK && e != Error::NOT_FOUND)
								failures.add(std::string("read error ") + p);
							++reads;
						}
						// Streamed partial reads must also be consistent with one version
						if (auto f = fs.openFileStream("/res/levels/1.map", IFileStream::Mode::READ))
						{
							auto all = test::readStream(*f);
							if (checkVersioned(all).empty())
								failures.add("torn stream of levels/1.map");
						}
						auto names = test::listNames(fs, "/res/levels");
						if (std::find(names.begin(), names.end(), "1.map") == names.end() ||
							std::find(names.begin(), names.end(), "2.map") == names.end())
							failures.add("listing lost base files");
						(void)t;
					}
				});

			std::thread mutator([&] {
				const int rounds = 60 * test::scale();
				for (int i = 0; i < rounds; ++i)
				{
					switch (i % 4)
					{
					case 0:
						if (fs.remount(s.patch, backends[static_cast<size_t>(i / 4) % backends.size()]) != Error::OK)
							failures.add("remount failed");
						break;
					case 1:
					{
						if (fs.unmount(s.patch) != Error::OK)
							failures.add("unmount failed");
						s.patch = fs.mount("/res", backends[static_cast<size_t>(i) % backends.size()], MountOptions{10, 0});
						if (!s.patch)
							failures.add("mount failed");
						break;
					}
					case 2:
						// Hot-fix override written atomically (rename on the memory layer)
						{
							const auto blob = versioned("hot" + std::to_string(i) + "-config", 300 + static_cast<size_t>(i));
							if (fs.writeFileAtomic("/res/config.json", blob.data(), blob.size()) != Error::OK)
								failures.add("hot-fix write failed");
						}
						break;
					default:
						fs.removeFile("/res/config.json");   // reveals the base version again
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				stop = true;
			});
			mutator.join();
			for (auto& r : readers)
				r.join();
			CHECK_MESSAGE(failures.load() == 0, failures.str());
			CHECK(reads > 0);
		});
	}
}
