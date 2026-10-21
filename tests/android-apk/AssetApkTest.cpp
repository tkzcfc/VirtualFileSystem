// AndroidAssetBackend checks against a real AAssetManager, run inside a NativeActivity.
// Results go to logcat (tag "VFSAPK"); the last line is "SUMMARY: PASS n/n" or "SUMMARY: FAIL k/n".
// The same summary is written to <internalDataPath>/result.txt. Asset contents come from gen_assets.py.
#include "vfs/VirtualFileSystem.h"
#include "vfs/android/AndroidAssetBackend.h"
#include "vfs/archive/ArchiveBackend.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

USING_NS_VFS;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VFSAPK", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VFSAPK", __VA_ARGS__)

static std::atomic<int> g_checks{0}, g_failed{0};

#define CHECK(cond)                                                      \
	do {                                                                 \
		++g_checks;                                                      \
		if (!(cond)) {                                                   \
			++g_failed;                                                  \
			LOGE("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);          \
		}                                                                \
	} while (0)

static constexpr int64_t BIN_SIZE = 300000;
static uint8_t binByte(int64_t i) { return uint8_t((i * 31 + (i >> 8)) & 0xFF); }

static std::string expectedText()
{
	std::string s;
	char line[64];
	for (int i = 0; i < 6000; ++i) {
		std::snprintf(line, sizeof line, "line %06d lorem ipsum dolor sit amet\n", i);
		s += line;
	}
	return s;
}

static std::set<std::string> names(const VirtualFileSystem& fs, const char* path)
{
	std::set<std::string> out;
	for (const auto& e : fs.list(path))
		out.insert(e.name + (e.type == FileType::DIR ? "/" : ""));
	return out;
}

// Positional reads: seek to scattered offsets and compare against the expected bytes.
static bool positionalReads(IFileStream& s, const std::string* text, int rounds, uint32_t seed)
{
	const int64_t size = s.size();
	uint8_t buf[777];
	for (int r = 0; r < rounds; ++r) {
		seed = seed * 1664525u + 1013904223u;
		int64_t off = int64_t(seed % uint32_t(size));
		if (s.seek(off, SEEK_SET) != off)
			return false;
		int n = s.read(buf, sizeof buf);
		int64_t want = std::min<int64_t>(sizeof buf, size - off);
		if (n != want)
			return false;
		for (int i = 0; i < n; ++i) {
			uint8_t e = text ? uint8_t((*text)[size_t(off + i)]) : binByte(off + i);
			if (buf[i] != e)
				return false;
		}
	}
	return true;
}

static void runTests(android_app* app)
{
	AndroidAssetBackend::setAssetManager(app->activity->assetManager);
	const std::string text = expectedText();

	// Sanity: big.bin must be stored uncompressed (fd path), big.txt compressed (AAsset_read path).
	{
		AAsset* a = AAssetManager_open(app->activity->assetManager, "data/big.bin", AASSET_MODE_RANDOM);
		off64_t st = 0, len = 0;
		int fd = a ? AAsset_openFileDescriptor64(a, &st, &len) : -1;
		CHECK(fd >= 0);
		if (fd >= 0) close(fd);
		if (a) AAsset_close(a);
		a = AAssetManager_open(app->activity->assetManager, "data/big.txt", AASSET_MODE_RANDOM);
		fd = a ? AAsset_openFileDescriptor64(a, &st, &len) : -1;
		CHECK(fd < 0);
		if (fd >= 0) close(fd);
		if (a) AAsset_close(a);
	}

	for (int mode = 0; mode < 2; ++mode) {
		const bool manifest = mode == 1;
		LOGI("== %s mode ==", manifest ? "manifest" : "probe");
		AssetOptions opt;
		if (manifest)
			opt.manifest = "assets.lst";
		Error err = Error::OK;
		auto assets = AndroidAssetBackend::create(opt, nullptr, &err);
		CHECK(assets && err == Error::OK);
		if (!assets)
			continue;
		CHECK(assets->hasManifest() == manifest);
		VirtualFileSystem fs;
		fs.mount("/res", assets);

		// read / stat / size
		std::string s;
		CHECK(fs.readFile("/res/hello.txt", s) == Error::OK && s == "Hello, assets!\n");
		CHECK(fs.fileSize("/res/data/big.bin") == BIN_SIZE);
		CHECK(fs.fileSize("/res/data/big.txt") == int64_t(text.size()));
		Stat st;
		CHECK(fs.stat("/res/hello.txt", st) == Error::OK && st.type == FileType::FILE && st.size == 15 && st.mtime == 0);
		CHECK(fs.stat("/res/data", st) == Error::OK && st.type == FileType::DIR);
		CHECK(fs.stat("/res/nested/a/b", st) == Error::OK && st.type == FileType::DIR);
		CHECK(fs.stat("/res/missing.txt", st) == Error::NOT_FOUND);
		CHECK(fs.stat("/res/hello.txt/x", st) != Error::OK);
		CHECK(!fs.openFileStream("/res/hello.txt", IFileStream::Mode::WRITE));
		CHECK(fs.writeFile("/res/new.txt", "x") != Error::OK);
		CHECK(fs.readFile("/res/nested/a/b/c.txt", s) == Error::OK && s == "deep file\n");

		// Unicode names
		CHECK(fs.readFile("/res/unicode/\u4e2d\u6587.txt", s) == Error::OK && s == "\u4f60\u597d\n");
		CHECK(fs.readFile("/res/unicode/caf\u00e9.txt", s) == Error::OK && s == "caf\u00e9\n");

		// Whole-file content
		std::vector<uint8_t> bin;
		CHECK(fs.readFile("/res/data/big.bin", bin) == Error::OK && int64_t(bin.size()) == BIN_SIZE);
		bool same = true;
		for (int64_t i = 0; i < int64_t(bin.size()); ++i)
			if (bin[size_t(i)] != binByte(i)) { same = false; break; }
		CHECK(same);
		CHECK(fs.readFile("/res/data/big.txt", s) == Error::OK && s == text);

		// Listing
		auto data = names(fs, "/res/data");
		CHECK(data.count("big.bin") && data.count("big.txt") && data.size() == 2);
		auto uni = names(fs, "/res/unicode");
		CHECK(uni.count("\u4e2d\u6587.txt") && uni.count("caf\u00e9.txt"));
		auto root = names(fs, "/res");
		CHECK(root.count("hello.txt"));
		if (manifest) {
			// Full tree: subdirectories are listed, and directories that contain only subdirectories exist.
			CHECK(root.count("data/") && root.count("nested/") && root.count("unicode/"));
			CHECK(names(fs, "/res/nested") == (std::set<std::string>{"a/", "only-subdirs/"}));
			CHECK(fs.stat("/res/nested/only-subdirs", st) == Error::OK && st.type == FileType::DIR);
			int walked = 0;
			fs.walk("/res", [&](const std::string&, const DirEntry& e) { walked += e.type == FileType::FILE; return true; });
			CHECK(walked == 8);   // the 8 files listed in assets.lst
		} else {
			// Probe mode (documented limits): subdirectories are not listed,
			// and a directory holding only subdirectories reports NOT_FOUND.
			CHECK(!root.count("data/"));
			CHECK(fs.stat("/res/nested/only-subdirs", st) == Error::NOT_FOUND);
			CHECK(fs.stat("/res/nested/only-subdirs/x", st) == Error::OK && st.type == FileType::DIR);
		}

		// Stream API: seek / tell / size / EOF, and positional reads on both storage kinds.
		{
			auto f = fs.openFileStream("/res/data/big.bin", IFileStream::Mode::READ);
			CHECK(f && f->size() == BIN_SIZE);
			if (f) {
				CHECK(f->seek(-10, SEEK_END) == BIN_SIZE - 10);
				uint8_t b[32];
				CHECK(f->read(b, sizeof b) == 10 && b[0] == binByte(BIN_SIZE - 10));
				CHECK(f->read(b, sizeof b) == 0);
				CHECK(f->seek(1000, SEEK_SET) == 1000 && f->tell() == 1000);
				CHECK(f->seek(5, SEEK_CUR) == 1005);
				CHECK(positionalReads(*f, nullptr, 2000, 7));
			}
			auto t = fs.openFileStream("/res/data/big.txt", IFileStream::Mode::READ);
			CHECK(t && t->size() == int64_t(text.size()));
			if (t)
				CHECK(positionalReads(*t, &text, 300, 11));
		}

		// Concurrent reads: 8 threads, each with its own streams, on both storage kinds.
		{
			std::atomic<int> bad{0};
			std::vector<std::thread> threads;
			for (int ti = 0; ti < 8; ++ti)
				threads.emplace_back([&, ti] {
					for (int k = 0; k < 5; ++k) {
						auto f = fs.openFileStream("/res/data/big.bin", IFileStream::Mode::READ);
						auto t = fs.openFileStream("/res/data/big.txt", IFileStream::Mode::READ);
						if (!f || !t || !positionalReads(*f, nullptr, 300, ti * 100 + k) || !positionalReads(*t, &text, 30, ti * 7 + k))
							++bad;
						std::string h;
						if (fs.readFile("/res/hello.txt", h) != Error::OK || h != "Hello, assets!\n")
							++bad;
					}
				});
			for (auto& th : threads)
				th.join();
			CHECK(bad == 0);
		}

		// Sub-root option: mount only assets/nested/.
		{
			AssetOptions sub;
			sub.root = "nested";
			auto nested = AndroidAssetBackend::create(sub);
			CHECK(nested != nullptr);
			if (nested) {
				VirtualFileSystem fs2;
				fs2.mount("/n", nested);
				CHECK(fs2.readFile("/n/a/b/c.txt", s) == Error::OK && s == "deep file\n");
				CHECK(!fs2.exists("/n/hello.txt"));
			}
		}

		// .vfs archive stored inside the APK, mounted through archiveSource().
		{
			Error e = Error::OK;
			auto src = assets->archiveSource("game.vfs", &e);
			CHECK(src && e == Error::OK);
			auto arc = src ? ArchiveBackend::create(src, {}, &e) : nullptr;
			CHECK(arc && e == Error::OK);
			if (arc) {
				VirtualFileSystem fs3;
				fs3.mount("/game", arc);
				CHECK(fs3.readFile("/game/config.json", s) == Error::OK && s == "{\"level\": 3}\n");
				std::string lua;
				for (int i = 0; i < 500; ++i) lua += "print('hi')\n";
				CHECK(fs3.readFile("/game/scripts/main.lua", s) == Error::OK && s == lua);
				std::vector<uint8_t> tex;
				CHECK(fs3.readFile("/game/textures/t.bin", tex) == Error::OK && tex.size() == 65536);
				bool ok = tex.size() == 65536;
				for (size_t i = 0; ok && i < tex.size(); ++i) ok = tex[i] == binByte(int64_t(i));
				CHECK(ok);
				CHECK(names(fs3, "/game") == (std::set<std::string>{"config.json", "scripts/", "textures/"}));
				auto f = fs3.openFileStream("/game/textures/t.bin", IFileStream::Mode::READ);
				CHECK(f && positionalReads(*f, nullptr, 500, 3));
				std::atomic<int> bad{0};
				std::vector<std::thread> threads;
				for (int ti = 0; ti < 8; ++ti)
					threads.emplace_back([&, ti] {
						for (int k = 0; k < 20; ++k) {
							auto g = fs3.openFileStream("/game/textures/t.bin", IFileStream::Mode::READ);
							std::string c;
							if (!g || !positionalReads(*g, nullptr, 50, ti * 31 + k) || fs3.readFile("/game/config.json", c) != Error::OK)
								++bad;
						}
					});
				for (auto& th : threads)
					th.join();
				CHECK(bad == 0);
			}
		}

		// Layering: assets (bottom) + memory override + writable native save directory.
		{
			const std::string saveDir = std::string(app->activity->internalDataPath) + "/vfs_apk_save";
			VirtualFileSystem fs4;
			fs4.mount("/res", assets, MountOptions{0, MountFlags::ReadOnly});
			std::shared_ptr<MemoryBackend> mem;
			fs4.mountMemory("/res", MountOptions{10, 0}, &mem);
			mem->addFile("hello.txt", {'o', 'v', 'e', 'r'});
			NativeOptions no;
			no.createRoot = true;
			fs4.mountNative("/save", saveDir, MountOptions{0, MountFlags::WriteTarget}, no);
			CHECK(fs4.readFile("/res/hello.txt", s) == Error::OK && s == "over");
			CHECK(fs4.readFile("/res/nested/a/b/c.txt", s) == Error::OK && s == "deep file\n");
			auto merged = names(fs4, "/res/data");
			CHECK(merged.count("big.bin") && merged.count("big.txt"));
			// Writing to /res goes to the memory layer; the asset layer is untouched.
			CHECK(fs4.writeFile("/res/data/new.txt", "mem") == Error::OK);
			CHECK(names(fs4, "/res/data").count("new.txt"));
			// Copy an asset to the native save directory and read it back.
			fs4.removeFile("/save/c.txt");
			CHECK(fs4.copyFile("/res/data/big.txt", "/save/big.txt") == Error::OK);
			CHECK(fs4.readFile("/save/big.txt", s) == Error::OK && s == text);
			CHECK(fs4.removeFile("/save/big.txt") == Error::OK);
		}
	}
}

void android_main(android_app* app)
{
	// Android may reuse the process for a new launch: start every run from clean counters
	g_checks = 0;
	g_failed = 0;
	LOGI("START AndroidAssetBackend APK tests");
	runTests(app);
	// The AAssetManager belongs to this activity; do not leave it registered for a later run
	AndroidAssetBackend::setAssetManager(nullptr);
	const int n = g_checks, f = g_failed;
	char summary[96];
	std::snprintf(summary, sizeof summary, "SUMMARY: %s %d/%d", f ? "FAIL" : "PASS", f ? f : n, n);
	if (f) LOGE("%s", summary); else LOGI("%s", summary);
	if (FILE* out = std::fopen((std::string(app->activity->internalDataPath) + "/result.txt").c_str(), "w")) {
		std::fprintf(out, "%s\n", summary);
		std::fclose(out);
	}
	ANativeActivity_finish(app->activity);
	// Drain events until the activity is destroyed.
	while (!app->destroyRequested) {
		int events;
		android_poll_source* source;
		if (ALooper_pollOnce(-1, nullptr, &events, (void**)&source) >= 0 && source)
			source->process(app, source);
	}
}
