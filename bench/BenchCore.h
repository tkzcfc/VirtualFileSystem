// This file: the shared core of vfs_bench. The command-line tool (main.cpp, iOS host) and the Android bench APK
// (android/BenchApk.cpp) both call vfsbench::run; a host can supply the dataset itself and add read / metadata
// targets of its own (the APK adds AAssetManager-based targets this way).
#pragma once

#include "vfs/VirtualFileSystem.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vfsbench
{

constexpr size_t KiB = 1024;
constexpr size_t MiB = 1024 * 1024;

struct Dataset
{
	std::string root;                  // native data directory
	std::vector<std::string> small;    // relative paths (under small/, one large directory)
	std::vector<std::string> medium;   // under medium/
	std::string large;                 // relative path of the large file
	size_t smallSize = 4 * KiB;
	size_t mediumSize = 256 * KiB;
	size_t largeSize = 64 * MiB;
	std::string archivePlain, archiveXor, archiveLz4;   // native paths of the three archives
	std::vector<std::string> allNativeFiles;            // used by --cold to drop caches
};


/**
 * Random-read handle: opened once, then read by offset many times.
 */
class Handle
{
public:
	virtual ~Handle() = default;

	// Read size bytes from offset; returns the number of bytes read, or -1 on failure
	virtual int64_t readAt(int64_t offset, void* buf, size_t size) = 0;
};

/**
 * Write handle for the write.large.seq / write.overwrite / write.append scenarios.
 */
class WriteHandle
{
public:
	virtual ~WriteHandle() = default;

	// Write at the current position (append mode: at the end); false on failure
	virtual bool write(const void* data, size_t size) = 0;

	// Write at an absolute offset; false on failure
	virtual bool writeAt(int64_t offset, const void* data, size_t size) = 0;

	// Close and report errors from the close itself
	virtual bool close() = 0;
};

// How a write handle is opened
enum class WriteMode
{
	Create,   // create or truncate
	Update,   // existing file, read/write, no truncation
	Append,   // create if missing, every write goes to the end
};

/**
 * A target under test: a baseline (calling the OS / C / C++ file API directly) or some VFS combination.
 * Every method may be called concurrently from several threads. Paths are relative to the data-set root.
 */
class Target
{
public:
	virtual ~Target() = default;

	virtual std::string name() const = 0;

	virtual bool baseline() const { return false; }

	// Whether it takes part in metadata scenarios (stat / miss / list). Of the three read baselines only stdio participates, to avoid duplication
	virtual bool supportsMeta() const { return true; }

	// Open + read the whole file + close; returns the byte count, or -1 on failure
	virtual int64_t readWhole(const std::string& rel, std::vector<uint8_t>& buf) = 0;

	// Read the whole file sequentially in chunks, calling onChunk once per chunk (used to record per-chunk latency)
	virtual int64_t readChunks(const std::string& rel, std::vector<uint8_t>& buf, const std::function<void()>& onChunk) = 0;

	// Open and close immediately
	virtual bool openClose(const std::string& rel) = 0;

	virtual std::unique_ptr<Handle> openHandle(const std::string& rel) = 0;

	virtual bool stat(const std::string& rel, int64_t& size) = 0;

	virtual bool exists(const std::string& rel) = 0;

	// List a directory; returns the entry count, or -1 on failure
	virtual int64_t list(const std::string& rel) = 0;

	// Write-related: implemented only by write targets
	virtual bool resetWriteArea(int /*threads*/) { return false; }

	virtual bool writeWhole(const std::string& /*rel*/, const uint8_t* /*data*/, size_t /*size*/) { return false; }

	virtual bool remove(const std::string& /*rel*/) { return false; }

	// Handle-based write scenarios (write.large.seq / write.overwrite / write.append)
	virtual bool supportsWriter() const { return false; }

	virtual std::unique_ptr<WriteHandle> openWriter(const std::string& /*rel*/, WriteMode /*mode*/) { return nullptr; }
};

/**
 * A target that runs every operation through its own VirtualFileSystem under a path prefix (e.g. "/d").
 * @param mount mounts the backends into the file system; returns false on failure
 * @return nullptr when mount fails
 */
std::unique_ptr<Target> makeVfsTarget(const std::string& name, const std::string& prefix,
									  const std::function<bool(vfs::VirtualFileSystem&)>& mount);

/**
 * Hooks for a host embedding the benchmark. Every member is optional.
 */
struct BenchHost
{
	// Fill ds.root, ds.small, ds.medium, ds.large and the sizes from data the host already has (instead of generating it).
	// workDir is the --dir working directory. An archive the host leaves at <workDir>/plain.vfs, xor.vfs or lz4.vfs is used
	// as is; missing ones are packed from the dataset. Returns false on failure.
	std::function<bool(const std::string& workDir, Dataset& ds)> prepareDataset;

	// Append extra read / metadata targets (run after the built-in ones). Returns false on failure.
	std::function<bool(const Dataset& ds, std::vector<std::unique_ptr<Target>>& out)> addReadTargets;

	// Names of the extra targets, shown by --list
	std::string extraTargetNames;
};

/**
 * Parse the command line (same options as the vfs_bench tool), run the benchmark and print the results to std::cout.
 * @return 0 on success, 1 when a case failed or setup failed, 2 on a usage error
 */
int run(int argc, char** argv, const BenchHost& host = {});

} // namespace vfsbench
