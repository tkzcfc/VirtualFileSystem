// This file: test environments for backend x layering combinations. Each environment provides a writable view under /w and preloads
//   /w/base/a.txt = "A", /w/base/sub/b.txt = "B"
// Those two "base files" may sit on a read-only layer (archive, read-only memory, or a read-only native directory) depending on the environment; baseWritable distinguishes that.
#pragma once

#include "TestUtils.h"
#include "vfs/archive/ArchiveBackend.h"
#include "vfs/memory/MemoryBackend.h"

namespace env
{

using namespace vfs;

inline MountOptions prio(int p, uint32_t flags = 0)
{
	MountOptions o;
	o.priority = p;
	o.flags = flags;
	return o;
}

// Read-only memory backend preloaded with the base files
inline std::shared_ptr<MemoryBackend> baseMemory(std::string_view prefix = "")
{
	auto b = MemoryBackend::create();
	REQUIRE(b->addFile(std::string(prefix) + "base/a.txt", "A") == Error::OK);
	REQUIRE(b->addFile(std::string(prefix) + "base/sub/b.txt", "B") == Error::OK);
	return b;
}

// Archive preloaded with the base files
inline std::shared_ptr<ArchiveBackend> baseArchive(const test::TempDir& dir, std::string_view prefix = "")
{
	FileWriteOptions enc;
	enc.encrypt = true;
	FileWriteOptions lz;
	lz.compress = true;
	std::vector<test::ArchiveFile> files = {
		{std::string(prefix) + "base/a.txt", test::bytes("A"), enc},
		{std::string(prefix) + "base/sub/b.txt", test::bytes("B"), lz},
	};
	const std::string path = dir.sub("base.vfs");
	REQUIRE(test::buildArchive(path, files) == Error::OK);
	Error err = Error::OK;
	auto a = ArchiveBackend::create(path, {}, &err);
	REQUIRE(err == Error::OK);
	REQUIRE(a);
	return a;
}

struct EnvBase
{
	test::TempDir dir;
	VirtualFileSystem fs;
	bool baseWritable = true;   // whether the base files can be deleted or renamed
	bool nativeTop = false;     // the write layer is a native directory (name portability rules and case sensitivity follow the OS)
	bool memoryTop = false;     // the write layer is a memory backend
	bool nativeBase = false;    // the base (the initial files under base/) lives in a native directory

	// Whether the write layer is case-sensitive: a native write layer depends on the filesystem of the temp directory (probed at runtime;
	// Linux usually is; Windows NTFS and macOS APFS are not by default). Other backends always are
	bool caseSensitive() const { return !nativeTop || test::nativeCaseSensitive(); }

	// Whether base-file names match byte for byte (without NativeOptions::exactCase, a native base follows the filesystem)
	bool baseCaseSensitive() const { return !nativeBase || test::nativeCaseSensitive() || test::nativeCaseLookupOnly(); }
	// The write layer is a native directory that is case-sensitive for lookup and case-insensitive for create (iOS simulator): neither case-test model applies
	bool caseLookupOnly() const { return nativeTop && test::nativeCaseLookupOnly(); }
};

// A single native directory
struct NativeEnv : EnvBase
{
	NativeEnv()
	{
		nativeBase = true;
		nativeTop = true;
		dir.write("root/base/a.txt", "A");
		dir.write("root/base/sub/b.txt", "B");
		REQUIRE(fs.mountNative("/w", dir.sub("root")) != 0);
	}
};

// A single memory backend
struct MemoryEnv : EnvBase
{
	MemoryEnv()
	{
		memoryTop = true;
		REQUIRE(fs.mount("/w", baseMemory()) != 0);
	}
};

// Nested mounts: read-only memory at the root, a native directory at /w
struct NestedNativeEnv : EnvBase
{
	NestedNativeEnv()
	{
		nativeBase = true;
		nativeTop = true;
		auto rootMem = MemoryBackend::create();
		rootMem->addFile("readme.txt", "root");
		rootMem->setReadOnly(true);
		REQUIRE(fs.mount("/", rootMem) != 0);
		dir.write("root/base/a.txt", "A");
		dir.write("root/base/sub/b.txt", "B");
		REQUIRE(fs.mountNative("/w", dir.sub("root")) != 0);
	}
};

// Archive underneath, native directory on top (the typical "patch overlay")
struct ArchiveUnderNativeEnv : EnvBase
{
	ArchiveUnderNativeEnv()
	{
		baseWritable = false;
		nativeTop = true;
		REQUIRE(fs.mount("/w", baseArchive(dir), prio(0)) != 0);
		dir.mkdir("upper");
		REQUIRE(fs.mountNative("/w", dir.sub("upper"), prio(1)) != 0);
	}
};

// Archive underneath, memory on top
struct MemoryOverArchiveEnv : EnvBase
{
	MemoryOverArchiveEnv()
	{
		baseWritable = false;
		memoryTop = true;
		REQUIRE(fs.mount("/w", baseArchive(dir), prio(0)) != 0);
		REQUIRE(fs.mountMemory("/w", prio(1)) != 0);
	}
};

// Same mount point and the same priority: the native directory mounted later is on top
struct TieEnv : EnvBase
{
	TieEnv()
	{
		baseWritable = false;
		nativeTop = true;
		REQUIRE(fs.mount("/w", baseArchive(dir), prio(3)) != 0);
		dir.mkdir("upper");
		REQUIRE(fs.mountNative("/w", dir.sub("upper"), prio(3)) != 0);
	}
};

// Three layers: archive (a.txt) < read-only memory (sub/b.txt) < writable memory
struct ThreeLayerEnv : EnvBase
{
	ThreeLayerEnv()
	{
		baseWritable = false;
		memoryTop = true;
		FileWriteOptions none;
		const std::string path = dir.sub("a.vfs");
		REQUIRE(test::buildArchive(path, {{"base/a.txt", test::bytes("A"), none}}) == Error::OK);
		REQUIRE(fs.mountArchive("/w", path, {}, prio(0)) != 0);
		auto mid = MemoryBackend::create();
		mid->addFile("base/sub/b.txt", "B");
		mid->setReadOnly(true);
		REQUIRE(fs.mount("/w", mid, prio(5)) != 0);
		REQUIRE(fs.mountMemory("/w", prio(10, MountFlags::WriteTarget)) != 0);
	}
};

// Read-only native directory underneath (ReadOnly flag), memory on top
struct MemoryOverReadOnlyNativeEnv : EnvBase
{
	MemoryOverReadOnlyNativeEnv()
	{
		nativeBase = true;
		baseWritable = false;
		memoryTop = true;
		dir.write("root/base/a.txt", "A");
		dir.write("root/base/sub/b.txt", "B");
		REQUIRE(fs.mountNative("/w", dir.sub("root"), prio(0, MountFlags::ReadOnly)) != 0);
		REQUIRE(fs.mountMemory("/w", prio(1)) != 0);
	}
};

// Archive mounted at the root (containing w/base/...), native directory mounted deeper at /w: the layer stack spans mount points of different depths
struct ArchiveRootNativeSubEnv : EnvBase
{
	ArchiveRootNativeSubEnv()
	{
		baseWritable = false;
		nativeTop = true;
		REQUIRE(fs.mount("/", baseArchive(dir, "w/"), prio(0)) != 0);
		dir.mkdir("upper");
		REQUIRE(fs.mountNative("/w", dir.sub("upper"), prio(1)) != 0);
	}
};

} // namespace env

#define VFS_ENVS                                                                                                       \
	env::NativeEnv, env::MemoryEnv, env::NestedNativeEnv, env::ArchiveUnderNativeEnv, env::MemoryOverArchiveEnv,      \
		env::TieEnv, env::ThreeLayerEnv, env::MemoryOverReadOnlyNativeEnv, env::ArchiveRootNativeSubEnv
