// 本文件：后端 × 分层组合的测试环境。每个环境都在 /w 下提供一个可写视图，并预置
//   /w/base/a.txt = "A"、/w/base/sub/b.txt = "B"
// 这两个“底层文件”在不同环境中可能位于只读层（归档、只读内存、只读本机目录），由 baseWritable 区分。
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

// 预置底层文件的只读内存后端
inline std::shared_ptr<MemoryBackend> baseMemory(std::string_view prefix = "")
{
	auto b = MemoryBackend::create();
	REQUIRE(b->addFile(std::string(prefix) + "base/a.txt", "A") == Error::OK);
	REQUIRE(b->addFile(std::string(prefix) + "base/sub/b.txt", "B") == Error::OK);
	return b;
}

// 预置底层文件的归档
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
	bool baseWritable = true;   // 底层文件能否删除、改名
	bool nativeTop = false;     // 写入层是本机目录（名字可移植性规则、大小写敏感性随 OS）
	bool memoryTop = false;     // 写入层是内存后端
	bool nativeBase = false;    // 底层（base/ 下的初始文件）在本机目录里

	// 写入层是否区分大小写：本机写入层取决于临时目录所在的文件系统（运行时探测，
	// Linux 通常区分；Windows NTFS、macOS APFS 默认不区分），其他后端总是区分
	bool caseSensitive() const { return !nativeTop || test::nativeCaseSensitive(); }

	// 底层文件是否按字节匹配名字（未开启 NativeOptions::exactCase 时，本机底层随文件系统）
	bool baseCaseSensitive() const { return !nativeBase || test::nativeCaseSensitive() || test::nativeCaseLookupOnly(); }
	// 写入层是“查找区分大小写、创建不区分”的本机目录（iOS 模拟器）：两个大小写测试的模型都不适用
	bool caseLookupOnly() const { return nativeTop && test::nativeCaseLookupOnly(); }
};

// 单个本机目录
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

// 单个内存后端
struct MemoryEnv : EnvBase
{
	MemoryEnv()
	{
		memoryTop = true;
		REQUIRE(fs.mount("/w", baseMemory()) != 0);
	}
};

// 嵌套挂载：只读内存挂在根，本机目录挂在 /w
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

// 归档在下，本机目录在上（典型的“补丁覆盖”）
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

// 归档在下，内存在上
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

// 同一挂载点、相同优先级：后挂载的本机目录在上
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

// 三层：归档（a.txt）< 只读内存（sub/b.txt）< 可写内存
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

// 只读本机目录在下（ReadOnly 标志），内存在上
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

// 归档挂在根（包含 w/base/...），本机目录挂在更深的 /w：层栈跨越不同深度的挂载点
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
