// This file: the Android bench APK (a NativeActivity with no Java code). It runs the shared vfs_bench core with AAssetManager-based targets:
//   base:aasset      raw AAssetManager_open / AAsset_read / AAsset_seek64 (AASSET_MODE_RANDOM, as AndroidAssetBackend opens assets)
//   vfs:asset        AndroidAssetBackend mounted on assets/bench/data in probe mode (no manifest)
//   vfs:asset+lst    the same with the assets.lst manifest (directory tree and stat answered from the manifest)
//   vfs:archive-apk  assets/bench/plain.vfs read in place from the APK through AssetBackend::archiveSource (stored uncompressed)
// next to the built-in targets, which run on copies extracted to internal storage (base:stdio, vfs:archive on the extracted plain.vfs, ...).
//
// Arguments: the default command line below, or the whitespace-separated contents of <internalDataPath>/args.txt when that file exists.
// Output: <internalDataPath>/bench.txt (console table), bench.csv, bench.md; progress and the final "BENCH DONE rc=<n>" go to logcat (tag VFSBENCH).
#include "../BenchCore.h"

#include "vfs/VirtualFileSystem.h"
#include "vfs/android/AndroidAssetBackend.h"
#include "vfs/archive/ArchiveBackend.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

USING_NS_VFS;
using namespace vfsbench;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VFSBENCH", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VFSBENCH", __VA_ARGS__)

namespace
{

const char* const DATA_DIR = "bench/data";          // dataset inside assets/
const char* const ARCHIVE = "bench/plain.vfs";      // plain archive of the same dataset, stored uncompressed
const char* const MANIFEST = "assets.lst";          // file list relative to DATA_DIR

AAssetManager* g_mgr = nullptr;

struct AssetCloser
{
	void operator()(AAsset* a) const { AAsset_close(a); }
};
using AssetPtr = std::unique_ptr<AAsset, AssetCloser>;

AssetPtr openAsset(const std::string& path) { return AssetPtr(AAssetManager_open(g_mgr, path.c_str(), AASSET_MODE_RANDOM)); }

bool readAsset(const std::string& path, std::string& out)
{
	AssetPtr a = openAsset(path);
	if (!a)
		return false;
	out.resize(static_cast<size_t>(AAsset_getLength64(a.get())));
	return AAsset_read(a.get(), out.data(), out.size()) == static_cast<int>(out.size());
}

// Copy one asset to a native file in 1 MiB chunks
bool extractAsset(const std::string& asset, const std::string& dest)
{
	AssetPtr a = openAsset(asset);
	if (!a)
		return false;
	FILE* f = std::fopen(dest.c_str(), "wb");
	if (!f)
		return false;
	std::vector<char> buf(MiB);
	bool ok = true;
	for (;;)
	{
		int n = AAsset_read(a.get(), buf.data(), buf.size());
		if (n < 0)
			ok = false;
		if (n <= 0)
			break;
		ok &= std::fwrite(buf.data(), 1, static_cast<size_t>(n), f) == static_cast<size_t>(n);
	}
	return std::fclose(f) == 0 && ok;
}

// The dataset comes from the APK: list it from the manifest and extract it (and plain.vfs) to the working directory
bool prepareDataset(const std::string& workDir, Dataset& ds)
{
	std::string lst;
	if (!readAsset(std::string(DATA_DIR) + "/" + MANIFEST, lst))
	{
		LOGE("missing %s/%s", DATA_DIR, MANIFEST);
		return false;
	}
	ds.root = workDir + "/data";
	std::error_code ec;
	std::filesystem::create_directories(ds.root + "/small", ec);
	std::filesystem::create_directories(ds.root + "/medium", ec);
	std::istringstream in(lst);
	for (std::string rel; std::getline(in, rel);)
	{
		if (rel.empty())
			continue;
		if (rel.rfind("small/", 0) == 0)
			ds.small.push_back(rel);
		else if (rel.rfind("medium/", 0) == 0)
			ds.medium.push_back(rel);
		else
			ds.large = rel;
		if (!extractAsset(std::string(DATA_DIR) + "/" + rel, ds.root + "/" + rel))
		{
			LOGE("cannot extract %s", rel.c_str());
			return false;
		}
	}
	if (ds.small.empty() || ds.medium.empty() || ds.large.empty())
		return false;
	auto sizeOf = [&](const std::string& rel) { return static_cast<size_t>(std::filesystem::file_size(ds.root + "/" + rel, ec)); };
	ds.smallSize = sizeOf(ds.small[0]);
	ds.mediumSize = sizeOf(ds.medium[0]);
	ds.largeSize = sizeOf(ds.large);
	// The extracted copy of the APK's archive serves as vfs:archive; xor / lz4 are packed by the core
	if (!extractAsset(ARCHIVE, workDir + "/plain.vfs"))
	{
		LOGE("cannot extract %s", ARCHIVE);
		return false;
	}
	LOGI("dataset extracted: %zu small, %zu medium, 1 large", ds.small.size(), ds.medium.size());
	return true;
}

// ---------------------------------------------------------------- base:aasset

class AAssetHandle : public Handle
{
public:
	explicit AAssetHandle(AssetPtr a) : m_asset(std::move(a)) {}

	int64_t readAt(int64_t offset, void* buf, size_t size) override
	{
		if (AAsset_seek64(m_asset.get(), offset, SEEK_SET) != offset)
			return -1;
		return AAsset_read(m_asset.get(), buf, size);
	}

private:
	AssetPtr m_asset;
};

class AAssetTarget : public Target
{
public:
	std::string name() const override { return "base:aasset"; }

	bool baseline() const override { return true; }

	int64_t readWhole(const std::string& rel, std::vector<uint8_t>& buf) override { return readChunks(rel, buf, {}); }

	int64_t readChunks(const std::string& rel, std::vector<uint8_t>& buf, const std::function<void()>& onChunk) override
	{
		AssetPtr a = openAsset(full(rel));
		if (!a)
			return -1;
		int64_t total = 0;
		for (;;)
		{
			int n = AAsset_read(a.get(), buf.data(), buf.size());
			if (n < 0)
				return -1;
			if (n == 0)
				break;
			total += n;
			if (onChunk)
				onChunk();
		}
		return total;
	}

	bool openClose(const std::string& rel) override { return openAsset(full(rel)) != nullptr; }

	std::unique_ptr<Handle> openHandle(const std::string& rel) override
	{
		AssetPtr a = openAsset(full(rel));
		return a ? std::make_unique<AAssetHandle>(std::move(a)) : nullptr;
	}

	// AAssetManager has no stat: open the asset and ask for its length
	bool stat(const std::string& rel, int64_t& size) override
	{
		AssetPtr a = openAsset(full(rel));
		if (!a)
			return false;
		size = AAsset_getLength64(a.get());
		return true;
	}

	bool exists(const std::string& rel) override { return openAsset(full(rel)) != nullptr; }

	// AAssetDir lists file names only (no subdirectories)
	int64_t list(const std::string& rel) override
	{
		AAssetDir* d = AAssetManager_openDir(g_mgr, full(rel).c_str());
		if (!d)
			return -1;
		int64_t n = 0;
		while (AAssetDir_getNextFileName(d))
			++n;
		AAssetDir_close(d);
		return n;
	}

private:
	static std::string full(const std::string& rel) { return std::string(DATA_DIR) + "/" + rel; }
};

bool addAssetTargets(const Dataset&, std::vector<std::unique_ptr<Target>>& out)
{
	out.push_back(std::make_unique<AAssetTarget>());

	AssetOptions probe;
	probe.root = DATA_DIR;
	AssetOptions withList = probe;
	withList.manifest = MANIFEST;
	for (auto& [name, options] : {std::make_pair("vfs:asset", probe), std::make_pair("vfs:asset+lst", withList)})
	{
		auto t = makeVfsTarget(name, "/d", [&options](VirtualFileSystem& fs)
		{
			auto backend = AndroidAssetBackend::create(options, g_mgr);
			return backend && fs.mount("/d", backend) != 0;
		});
		if (!t)
			return false;
		out.push_back(std::move(t));
	}

	auto t = makeVfsTarget("vfs:archive-apk", "/d", [](VirtualFileSystem& fs)
	{
		auto assets = AndroidAssetBackend::create({}, g_mgr);
		Error e = Error::OK;
		auto src = assets ? assets->archiveSource(ARCHIVE, &e) : nullptr;
		auto arc = src ? ArchiveBackend::create(src, {}, &e) : nullptr;
		if (!arc)
			LOGE("archive-apk: %s", errorName(e));
		return arc && fs.mount("/d", arc) != 0;
	});
	if (!t)
		return false;
	out.push_back(std::move(t));
	return true;
}

std::vector<std::string> commandLine(const std::string& dataDir)
{
	std::vector<std::string> args = {"vfs_bench", "--dir", dataDir + "/work", "--threads", "1,2,4,8", "--filter", "read.,meta.",
		"--targets", "base:stdio,base:aasset,vfs:asset,vfs:asset+lst,vfs:archive,vfs:archive-apk",
		"--csv", dataDir + "/bench.csv", "--md", dataDir + "/bench.md"};
	std::ifstream f(dataDir + "/args.txt");
	if (f)
	{
		args = {"vfs_bench"};
		for (std::string a; f >> a;)
			args.push_back(a);
	}
	return args;
}

} // namespace

void android_main(android_app* app)
{
	g_mgr = app->activity->assetManager;
	const std::string dataDir = app->activity->internalDataPath;
	// The run takes minutes: keep the screen on so the activity is not paused while it runs
	ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);

	// The benchmark runs on its own thread; this thread keeps serving activity events so the system never sees it hang
	std::atomic<bool> done{false};
	std::thread worker([&]
	{
		std::vector<std::string> args = commandLine(dataDir);
		std::vector<char*> argv;
		std::string joined;
		for (auto& a : args)
		{
			argv.push_back(a.data());
			joined += a + " ";
		}
		LOGI("BENCH START %s", joined.c_str());
		std::ofstream out(dataDir + "/bench.txt", std::ios::trunc);
		std::streambuf* old = std::cout.rdbuf(out.rdbuf());
		BenchHost host;
		host.prepareDataset = prepareDataset;
		host.addReadTargets = addAssetTargets;
		host.extraTargetNames = "base:aasset vfs:asset vfs:asset+lst vfs:archive-apk";
		const int rc = run(static_cast<int>(argv.size()), argv.data(), host);
		std::cout.flush();
		std::cout.rdbuf(old);
		out.close();
		LOGI("BENCH DONE rc=%d", rc);
		done = true;
	});

	bool finishing = false;
	while (!app->destroyRequested)
	{
		if (done && !finishing)
		{
			finishing = true;
			ANativeActivity_finish(app->activity);
		}
		int events;
		android_poll_source* source;
		if (ALooper_pollOnce(done ? -1 : 200, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
			source->process(app, source);
	}
	if (!done)
		LOGE("activity destroyed before the benchmark finished; waiting for it");
	worker.join();
	g_mgr = nullptr;
}
