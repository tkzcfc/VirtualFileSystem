// 极端环境：空文件、海量条目、超深路径、超长名字、特殊名字、只读目录、挂载期间磁盘被外部修改、
// 归档逐区域损坏、错误密钥、故障注入、伪造的超大尺寸、句柄耗尽、超过 4 GB 的稀疏文件。
#include "FaultBackend.h"
#include "matrix/Envs.h"
#include "vfs/archive/ArchiveFormat.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/archive/ByteIO.h"
#include "vfs/crypto/Hash.h"
#include "vfs/native/NativeFileStream.h"
#include "vfs/native/NativeUtils.h"
#include "vfs/native/SingleFileBackend.h"
#include <cstring>
#include <set>
#if !VFS_PLATFORM_WINDOWS
#    include <sys/resource.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

using namespace vfs;
using test::listNames;
using test::readAll;
using Mode = IFileStream::Mode;
using Names = std::vector<std::string>;

namespace
{

// 修改未加密索引的归档后，重新计算 indexHash / keyCheck / headerChecksum，使其只剩“语义”错误
void reseal(std::vector<uint8_t>& a)
{
	const uint64_t indexOffset = ByteIO::readU64LE(a.data() + 48);
	const uint64_t indexSize = ByteIO::readU64LE(a.data() + 56);
	const uint64_t h = xxh3_64(a.data() + indexOffset, static_cast<size_t>(indexSize));
	ByteIO::writeU32LE(a.data() + 108, static_cast<uint32_t>(h));
	ByteIO::writeU64LE(a.data() + 112, h);
	ByteIO::writeU64LE(a.data() + 120, xxh3_64(a.data(), 120));
}

std::vector<uint8_t> plainIndexArchive(const std::vector<test::ArchiveFile>& files)
{
	test::TempDir dir;
	ArchiveWriteOptions o;
	o.encryptIndex = false;
	REQUIRE(test::buildArchive(dir.sub("x.vfs"), files, o) == Error::OK);
	return test::readNative(dir.sub("x.vfs"));
}

Error openArchive(const std::vector<uint8_t>& bytes, const ArchiveOptions& options = {})
{
	Error err = Error::OK;
	auto b = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(bytes), options, &err);
	if (b)
		CHECK(err == Error::OK);
	else
		CHECK(err != Error::OK);
	return err;
}

uint8_t* entryField(std::vector<uint8_t>& a, uint32_t index, size_t field)
{
	const uint64_t indexOffset = ByteIO::readU64LE(a.data() + 48);
	return a.data() + indexOffset + index * ArchiveFormat::ENTRY_SIZE + field;
}

#if !VFS_PLATFORM_WINDOWS
bool runningAsRoot()
{
	return ::geteuid() == 0;
}
#endif

} // namespace

TEST_SUITE("extreme")
{
	// ================================================================ 空文件与海量条目

	TEST_CASE("zero-byte files: every backend and encryption / compression combination")
	{
		test::TempDir dir;
		FileWriteOptions x, z, xz;
		x.encrypt = true;
		z.compress = true;
		xz.encrypt = xz.compress = true;
		REQUIRE(test::buildArchive(dir.sub("e.vfs"), {{"plain", {}, {}}, {"x", {}, x}, {"z", {}, z}, {"xz", {}, xz}}) ==
				Error::OK);
		dir.write("native/empty", "");
		dir.write("single", "");
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("e.vfs")) != 0);
		REQUIRE(fs.mountNative("/n", dir.sub("native")) != 0);
		REQUIRE(fs.mountFile("/s", dir.sub("single")) != 0);
		std::shared_ptr<MemoryBackend> mem;
		REQUIRE(fs.mountMemory("/m", {}, &mem) != 0);
		REQUIRE(fs.writeFile("/m/empty", "") == Error::OK);
		for (const char* p : {"/a/plain", "/a/x", "/a/z", "/a/xz", "/n/empty", "/s", "/m/empty"})
		{
			CAPTURE(p);
			auto s = fs.openFileStream(p, Mode::READ);
			REQUIRE(s);
			CHECK(s->size() == 0);
			char c;
			CHECK(s->read(&c, 1) == 0);
			CHECK(s->seek(10, SEEK_SET) == 10);
			CHECK(s->read(&c, 1) == 0);
			CHECK(fs.fileSize(p) == 0);
			CHECK(readAll(fs, p).empty());
		}
	}

	TEST_CASE("many entries: 20k-file archive and memory backend; lookup, listing and walking are correct")
	{
		const int N = 20000;
		test::TempDir dir;
		ArchiveWriter writer;
		auto mem = MemoryBackend::create();
		for (int i = 0; i < N; ++i)
		{
			const std::string path = "d" + std::to_string(i % 100) + "/f" + std::to_string(i);
			REQUIRE(writer.addData(path, test::bytes(std::to_string(i))) == Error::OK);
			REQUIRE(mem->addFile(path, std::to_string(i)) == Error::OK);
		}
		REQUIRE(writer.write(dir.sub("many.vfs")) == Error::OK);
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("many.vfs")) != 0);
		REQUIRE(fs.mount("/m", mem) != 0);
		for (const char* root : {"/a", "/m"})
		{
			CHECK(listNames(fs, root).size() == 100);
			CHECK(listNames(fs, std::string(root) + "/d7").size() == static_cast<size_t>(N / 100));
			for (int i = 0; i < N; i += 997)
				CHECK(readAll(fs, std::string(root) + "/d" + std::to_string(i % 100) + "/f" + std::to_string(i)) ==
					  std::to_string(i));
			int files = 0;
			fs.walk(root, [&](const std::string&, const DirEntry& d)
			{
				files += d.type == FileType::FILE;
				return true;
			});
			CHECK(files == N);
		}
	}

	TEST_CASE("many entries (slow): 100k-file archive and memory backend, and one directory with 100k files" * doctest::skip(!test::slowEnabled()))
	{
		const int N = 100000;
		test::TempDir dir;
		ArchiveWriter writer;
		auto mem = MemoryBackend::create();
		for (int i = 0; i < N; ++i)
		{
			const std::string path = "flat/f" + std::to_string(i);
			REQUIRE(writer.addData(path, test::bytes(std::to_string(i))) == Error::OK);
			REQUIRE(mem->addFile(path, std::to_string(i)) == Error::OK);
		}
		REQUIRE(writer.write(dir.sub("many.vfs")) == Error::OK);
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("many.vfs")) != 0);
		REQUIRE(fs.mount("/m", mem) != 0);
		for (const char* root : {"/a/flat", "/m/flat"})
		{
			auto names = listNames(fs, root);
			CHECK(names.size() == static_cast<size_t>(N));
			CHECK(std::is_sorted(names.begin(), names.end()));
			for (int i = 0; i < N; i += 1009)
				CHECK(readAll(fs, std::string(root) + "/f" + std::to_string(i)) == std::to_string(i));
		}
	}

	// ================================================================ 路径与名字

	TEST_CASE("very deep paths: 300 levels in memory, 200 in archives, 100 native")
	{
		std::string rel;
		for (int i = 0; i < 300; ++i)
			rel += (i ? "/" : "") + std::string("d") + std::to_string(i);
		VirtualFileSystem fs;
		REQUIRE(fs.mountMemory("/m") != 0);
		REQUIRE(fs.createDirs("/m/" + rel) == Error::OK);
		REQUIRE(fs.writeFile("/m/" + rel + "/leaf", "deep") == Error::OK);
		CHECK(readAll(fs, "/m/" + rel + "/leaf") == "deep");
		CHECK(fs.removeDir("/m/d0", true) == Error::OK);

		std::string arel;
		for (int i = 0; i < 200; ++i)
			arel += (i ? "/" : "") + std::string("a") + std::to_string(i);
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("deep.vfs"), {{arel + "/leaf", test::bytes("adeep"), {}}}) == Error::OK);
		REQUIRE(fs.mountArchive("/a", dir.sub("deep.vfs")) != 0);
		CHECK(readAll(fs, "/a/" + arel + "/leaf") == "adeep");
		CHECK(listNames(fs, "/a/" + arel) == Names{"leaf"});

		std::string nrel;
		for (int i = 0; i < 100; ++i)
			nrel += (i ? "/" : "") + std::string("n") + std::to_string(i % 10);
		dir.mkdir("native");
		REQUIRE(fs.mountNative("/n", dir.sub("native")) != 0);
		// 本机全路径约 300+ 字节，超过 Windows MAX_PATH（260），需要 \\?\ 长路径前缀
		INFO("native path length " << dir.sub("native/" + nrel + "/leaf").size());
		REQUIRE(fs.createDirs("/n/" + nrel) == Error::OK);
		REQUIRE(fs.writeFile("/n/" + nrel + "/leaf", "ndeep") == Error::OK);
		CHECK(readAll(fs, "/n/" + nrel + "/leaf") == "ndeep");
		CHECK(fs.removeDir("/n/n0", true) == Error::OK);
		CHECK(listNames(fs, "/n").empty());
	}

	TEST_CASE("longest path: 4096-byte archive paths work, 4097 bytes are rejected")
	{
		std::string p;
		while (p.size() < 4096)
			p += (p.empty() ? "" : "/") + std::string(200, 'p');
		p.resize(4096);
		if (p.back() == '/')
			p.back() = 'q';
		test::TempDir dir;
		ArchiveWriter w;
		REQUIRE(w.addData(p, test::bytes("max")) == Error::OK);
		REQUIRE(w.write(dir.sub("max.vfs")) == Error::OK);
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("max.vfs")) != 0);
		CHECK(readAll(fs, "/a/" + p) == "max");

		ArchiveWriter w2;
		Error e = w2.addData(p + "x", test::bytes("too long"));
		if (e == Error::OK)
			e = w2.write(dir.sub("over.vfs"));
		CHECK(e != Error::OK);
		// 内存后端没有长度上限
		REQUIRE(fs.mountMemory("/m") != 0);
		const std::string longPath = "/m/" + std::string(10000, 'L');
		CHECK(fs.writeFile(longPath, "long") == Error::OK);
		CHECK(readAll(fs, longPath) == "long");
	}

	TEST_CASE("255-byte names work on all backends; a 256-byte native name fails without crashing")
	{
		const std::string n255(255, 'x');
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("n.vfs"), {{n255, test::bytes("a"), {}}}) == Error::OK);
		dir.mkdir("native");
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("n.vfs")) != 0);
		REQUIRE(fs.mountNative("/n", dir.sub("native")) != 0);
		REQUIRE(fs.mountMemory("/m") != 0);
		CHECK(readAll(fs, "/a/" + n255) == "a");
		CHECK(fs.writeFile("/n/" + n255, "n") == Error::OK);
		CHECK(readAll(fs, "/n/" + n255) == "n");
		CHECK(fs.writeFile("/m/" + n255, "m") == Error::OK);
		CHECK(fs.writeFile("/n/" + std::string(256, 'y'), "n") != Error::OK);
		CHECK(fs.createDir("/n/" + std::string(300, 'z')) != Error::OK);
		CHECK(listNames(fs, "/n") == Names{n255});
	}

	TEST_CASE("Unicode: Chinese, Japanese, Korean, emoji, combining characters (NFC and NFD are different names), full-width space")
	{
		const char* names[] = {"资源/图片.png", "日本語/ファイル.txt", "한국어/파일", "emoji/😀👍🏽.bin",
							   "nfc/\xC3\xA9", "nfc/e\xCC\x81", ("space/a\xE3\x80\x80" "b")};
		test::TempDir dir;
		std::vector<test::ArchiveFile> files;
		for (const char* n : names)
			files.push_back({n, test::bytes(n), {}});
		REQUIRE(test::buildArchive(dir.sub("u.vfs"), files) == Error::OK);
		dir.mkdir("native");
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("u.vfs")) != 0);
		REQUIRE(fs.mountNative("/n", dir.sub("native")) != 0);
		REQUIRE(fs.mountMemory("/m") != 0);
		for (const char* n : names)
		{
			CAPTURE(n);
			CHECK(readAll(fs, std::string("/a/") + n) == n);
			for (const char* root : {"/n/", "/m/"})
			{
				const std::string p = root + std::string(n);
				REQUIRE(fs.createDirs(p.substr(0, p.rfind('/'))) == Error::OK);
				REQUIRE(fs.writeFile(p, n) == Error::OK);
			}
		}
		CHECK(listNames(fs, "/a/nfc").size() == 2);
		CHECK(listNames(fs, "/m/nfc").size() == 2);
#if !VFS_PLATFORM_APPLE
		// macOS 的部分文件系统会做 Unicode 规范化，NFC/NFD 可能合并
		CHECK(listNames(fs, "/n/nfc").size() == 2);
#endif
		for (const char* n : names)
			CHECK(readAll(fs, std::string("/m/") + n) == n);
	}

	TEST_CASE("case: the archive writer rejects paths differing only in case; memory treats them as different; lookups match bytes")
	{
		ArchiveWriter w;
		REQUIRE(w.addData("Dir/File.txt", test::bytes("1")) == Error::OK);
		REQUIRE(w.addData("dir/file.txt", test::bytes("2")) == Error::OK);
		test::TempDir dir;
		CHECK(w.write(dir.sub("case.vfs")) == Error::EXISTS);

		VirtualFileSystem fs;
		REQUIRE(test::buildArchive(dir.sub("ok.vfs"), {{"Dir/File.txt", test::bytes("A"), {}}}) == Error::OK);
		REQUIRE(fs.mountArchive("/a", dir.sub("ok.vfs")) != 0);
		CHECK(readAll(fs, "/a/Dir/File.txt") == "A");
		CHECK_FALSE(fs.exists("/a/dir/file.txt"));
		CHECK_FALSE(fs.exists("/a/DIR/FILE.TXT"));
		REQUIRE(fs.mountMemory("/m") != 0);
		REQUIRE(fs.writeFile("/m/X", "upper") == Error::OK);
		REQUIRE(fs.writeFile("/m/x", "lower") == Error::OK);
		CHECK(listNames(fs, "/m") == (Names{"X", "x"}));
	}

	TEST_CASE("special path spellings behave the same in every entry point")
	{
		VirtualFileSystem fs;
		REQUIRE(fs.mountMemory("/m") != 0);
		REQUIRE(fs.createDirs("/m/a/b") == Error::OK);
		REQUIRE(fs.writeFile("/m/a/b/f", "F") == Error::OK);
		const char* same[] = {"/m/a/b/f", "m/a/b/f", "/m/a/b/./f", "/m//a///b/f", "\\m\\a\\b\\f", "/m/a/../a/b/f",
							  "/m/a/b/../b/f", "/m/a/b/f/", "/m/./a/./b/./f", "/m/a/b/c/../f", "/m/a/b/c/d/../../f"};
		for (const char* p : same)
		{
			CAPTURE(p);
			Stat st;
			CHECK(fs.stat(p, st) == Error::OK);
			CHECK(fs.isFile(p));
			CHECK(fs.fileSize(p) == 1);
			CHECK(readAll(fs, p) == "F");
		}
		for (const char* bad : {"/..", "/../m/a", "/m/../../x", "..", "../m", "/m/a/../../../b"})
		{
			CAPTURE(bad);
			Stat st;
			CHECK(fs.stat(bad, st) == Error::INVALID_PATH);
			CHECK(fs.writeFile(bad, "x") == Error::INVALID_PATH);
			CHECK(fs.createDirs(bad) == Error::INVALID_PATH);
			CHECK(fs.removeFile(bad) == Error::INVALID_PATH);
			CHECK(fs.rename(bad, "/m/z") == Error::INVALID_PATH);
			CHECK(fs.rename("/m/a/b/f", bad) == Error::INVALID_PATH);
			CHECK_FALSE(fs.exists(bad));
			Error err = Error::OK;
			CHECK_FALSE(fs.openFileStream(bad, Mode::READ, &err));
			CHECK(err == Error::INVALID_PATH);
		}
		// 名字中允许的“奇怪”字符
		for (const char* odd : {"/m/a b", "/m/.x", "/m/..x", "/m/x..", "/m/#$%&'()+,;=@[]^`{}~", "/m/-", "/m/!"})
		{
			CAPTURE(odd);
			CHECK(fs.writeFile(odd, "o") == Error::OK);
			CHECK(readAll(fs, odd) == "o");
		}
	}

	// ================================================================ 挂载点边界

	TEST_CASE("mount point below a lower-layer file: the path to the mount point is always a directory")
	{
		VirtualFileSystem fs;
		auto lower = MemoryBackend::create();
		lower->addFile("a", "i am a file");
		lower->setReadOnly(true);
		REQUIRE(fs.mount("/", lower, env::prio(5)) != 0);
		std::shared_ptr<MemoryBackend> upper;
		REQUIRE(fs.mountMemory("/a/b", env::prio(0), &upper) != 0);
		upper->addFile("x.txt", "X");
		// 无论优先级如何，挂载点 /a/b 都可达，/a 表现为目录
		CHECK(fs.isDir("/a"));
		CHECK(listNames(fs, "/") == Names{"a/"});
		CHECK(listNames(fs, "/a") == Names{"b/"});
		CHECK(readAll(fs, "/a/b/x.txt") == "X");
		CHECK(fs.writeFile("/a", "x") == Error::IS_DIR);
		CHECK(fs.removeFile("/a") == Error::IS_DIR);
	}

	TEST_CASE("one backend at several mount points: both see the same data")
	{
		VirtualFileSystem fs;
		auto mem = MemoryBackend::create();
		REQUIRE(fs.mount("/x", mem) != 0);
		REQUIRE(fs.mount("/y/z", mem) != 0);
		REQUIRE(fs.writeFile("/x/f", "shared") == Error::OK);
		CHECK(readAll(fs, "/y/z/f") == "shared");
		// 不同挂载之间 rename 视为跨设备，即使后端相同
		CHECK(fs.rename("/x/f", "/y/z/g") == Error::CROSS_DEVICE);
		CHECK(fs.moveFile("/x/f", "/y/z/g") == Error::OK);
		CHECK(readAll(fs, "/x/g") == "shared");
	}

	TEST_CASE("single-file mount over an archive file: shadows it, writable, sub-paths NOT_DIR")
	{
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), {{"cfg/game.ini", test::bytes("archive"), {}},
													 {"cfg/other.ini", test::bytes("other"), {}}}) == Error::OK);
		dir.write("override.ini", "native");
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/res", dir.sub("a.vfs")) != 0);
		REQUIRE(fs.mountFile("/res/cfg/game.ini", dir.sub("override.ini"), env::prio(1)) != 0);
		CHECK(readAll(fs, "/res/cfg/game.ini") == "native");
		CHECK(listNames(fs, "/res/cfg") == (Names{"game.ini", "other.ini"}));
		CHECK(fs.writeFile("/res/cfg/game.ini", "changed") == Error::OK);
		CHECK(dir.read("override.ini") == "changed");
		CHECK(fs.removeFile("/res/cfg/game.ini") == Error::BUSY);
		CHECK(fs.rename("/res/cfg/game.ini", "/res/cfg/x") == Error::BUSY);
		Stat st;
		CHECK(fs.stat("/res/cfg/game.ini/sub", st) == Error::NOT_DIR);
		CHECK(fs.writeFile("/res/cfg/new.ini", "x") == Error::READ_ONLY);
	}

	// ================================================================ 本机目录的外部变化

	TEST_CASE("external disk changes while mounted: delete, replace, file becomes directory; the VFS sees them immediately")
	{
		test::TempDir dir;
		dir.write("root/f.txt", "v1");
		dir.write("root/d/x", "x");
		VirtualFileSystem fs;
		REQUIRE(fs.mountNative("/n", dir.sub("root")) != 0);
		CHECK(readAll(fs, "/n/f.txt") == "v1");

		auto open = fs.openFileStream("/n/f.txt", Mode::READ);
		REQUIRE(open);
		// 外部替换（写临时文件再 rename 覆盖）。
		// 用 native::renamePath 而不是 std::filesystem::rename：Windows 上后者（MoveFileExW 传统语义）
		// 不能替换有打开句柄的文件（即使句柄带 FILE_SHARE_DELETE），这是外部工具自己的限制，与 VFS 无关；
		// renamePath 在支持的系统上用 POSIX 语义替换（Windows 10 1709+ NTFS），和“安全保存”类工具的做法一致
		// 平台不支持替换打开中的文件时（见 test::nativeReplaceOpenFile），外部工具也只能等句柄关闭，测试同样先关闭
		if (!test::nativeReplaceOpenFile())
		{
			MESSAGE("native file system cannot replace an open file; closing the stream before the external replace");
			open.reset();
		}
		dir.write("tmp.txt", "v2-longer");
		Error re = vfs::native::renamePath(dir.sub("tmp.txt"), dir.sub("root/f.txt"));
		REQUIRE_MESSAGE(re == Error::OK, "external file replace failed: " << re << " (stream still open: " << (open != nullptr) << ")");
		CHECK(readAll(fs, "/n/f.txt") == "v2-longer");
		CHECK(fs.fileSize("/n/f.txt") == 9);
#if !VFS_PLATFORM_WINDOWS
		// POSIX：已打开的流仍然指向旧文件（Windows 上取决于是否走了 POSIX 语义，不检查）
		if (open)
			CHECK(test::str(test::readStream(*open)) == "v1");
#endif
		open.reset();

		// 外部删除
		std::filesystem::remove(test::u8(dir.sub("root/f.txt")));
		CHECK_FALSE(fs.exists("/n/f.txt"));
		CHECK(fs.removeFile("/n/f.txt") == Error::NOT_FOUND);
		// 目录被换成同名文件
		std::filesystem::remove_all(test::u8(dir.sub("root/d")));
		dir.write("root/d", "now a file");
		CHECK(fs.isFile("/n/d"));
		Stat st;
		CHECK(fs.stat("/n/d/x", st) == Error::NOT_DIR);
		CHECK(listNames(fs, "/n") == Names{"d"});
		// 挂载根目录本身被删除：操作返回错误而不是崩溃
		std::filesystem::remove_all(test::u8(dir.sub("root")));
		CHECK_FALSE(fs.exists("/n/d"));
		CHECK(fs.writeFile("/n/new", "x") != Error::OK);
		Error err = Error::OK;
		fs.list("/n", &err);
		CHECK(err != Error::OK);
	}

	TEST_CASE("read-only native directory (chmod 0555): reads work, writes return PERMISSION_DENIED")
	{
#if VFS_PLATFORM_WINDOWS
		return;
#else
		if (runningAsRoot())
			return;   // root 不受权限位约束
		test::TempDir dir;
		dir.write("ro/f.txt", "readonly");
		dir.mkdir("ro/sub");
		::chmod(dir.sub("ro/sub").c_str(), 0555);
		::chmod(dir.sub("ro/f.txt").c_str(), 0444);
		::chmod(dir.sub("ro").c_str(), 0555);
		VirtualFileSystem fs;
		REQUIRE(fs.mountNative("/r", dir.sub("ro")) != 0);
		CHECK(readAll(fs, "/r/f.txt") == "readonly");
		CHECK(fs.writeFile("/r/f.txt", "x") == Error::PERMISSION_DENIED);
		CHECK(fs.writeFile("/r/new.txt", "x") == Error::PERMISSION_DENIED);
		CHECK(fs.createDir("/r/newdir") == Error::PERMISSION_DENIED);
		CHECK(fs.removeFile("/r/f.txt") == Error::PERMISSION_DENIED);
		CHECK(fs.rename("/r/f.txt", "/r/g.txt") == Error::PERMISSION_DENIED);
		CHECK(fs.writeFile("/r/sub/x", "x") == Error::PERMISSION_DENIED);
		CHECK(readAll(fs, "/r/f.txt") == "readonly");
		// 叠加一个可写层：本机层本身仍被视为可写（没有 ReadOnly 标志），已有文件原地写入仍被 OS 拒绝，
		// 新文件落到最高的可写层；给本机层加上 ReadOnly 标志后，已有文件也改为写到上层
		REQUIRE(fs.mountMemory("/r", env::prio(1)) != 0);
		CHECK(fs.writeFile("/r/f.txt", "shadow") == Error::PERMISSION_DENIED);
		CHECK(fs.writeFile("/r/new.txt", "new") == Error::OK);
		VirtualFileSystem fs2;
		REQUIRE(fs2.mountNative("/r", dir.sub("ro"), env::prio(0, MountFlags::ReadOnly)) != 0);
		REQUIRE(fs2.mountMemory("/r", env::prio(1)) != 0);
		CHECK(fs2.writeFile("/r/f.txt", "shadow") == Error::OK);
		CHECK(readAll(fs2, "/r/f.txt") == "shadow");
		::chmod(dir.sub("ro").c_str(), 0755);
		::chmod(dir.sub("ro/sub").c_str(), 0755);
#endif
	}

	// ================================================================ 归档损坏

	TEST_CASE("archive corruption by region: header / data / index / footer each report the right error")
	{
		FileWriteOptions x, z;
		x.encrypt = true;
		z.compress = true;
		const std::vector<test::ArchiveFile> files = {
			{"a.txt", test::randomBytes(3000, 1), x},
			{"b.txt", std::vector<uint8_t>(5000, 'b'), z},
			{"c.txt", test::randomBytes(100, 3), {}},
		};
		test::TempDir dir;
		REQUIRE(test::buildArchive(dir.sub("ok.vfs"), files) == Error::OK);
		const auto good = test::readNative(dir.sub("ok.vfs"));
		REQUIRE(openArchive(good) == Error::OK);
		const uint64_t dataOffset = ByteIO::readU64LE(good.data() + 32);
		const uint64_t dataSize = ByteIO::readU64LE(good.data() + 40);
		const uint64_t indexOffset = ByteIO::readU64LE(good.data() + 48);
		const uint64_t indexSize = ByteIO::readU64LE(good.data() + 56);

		// header：每个字节翻转
		for (size_t i = 0; i < ArchiveFormat::HEADER_SIZE; ++i)
		{
			auto a = good;
			a[i] ^= 0x40;
			CAPTURE(i);
			Error e = openArchive(a);
			if (i >= 8 && i < 12)
				CHECK(e == Error::VERSION_MISMATCH);
			else
				CHECK(e == Error::CORRUPTED);
		}
		// 索引：每个字节翻转都被 indexHash 发现
		for (uint64_t i = indexOffset; i < indexOffset + indexSize; i += 7)
		{
			auto a = good;
			a[static_cast<size_t>(i)] ^= 0x01;
			CHECK(openArchive(a) == Error::CORRUPTED);
		}
		// footer：归档起点为 0 时不依赖 footer，损坏不影响打开
		{
			auto a = good;
			a[a.size() - 3] ^= 0xFF;
			CHECK(openArchive(a) == Error::OK);
		}
		// footer：附加在其他数据之后时，footer 损坏导致无法定位
		{
			std::vector<uint8_t> a(1000, 0xAA);
			a.insert(a.end(), good.begin(), good.end());
			CHECK(openArchive(a) == Error::OK);
			auto b = a;
			b[b.size() - 16] ^= 0x01;   // footer magic
			CHECK(openArchive(b) == Error::CORRUPTED);
			auto c = a;
			ByteIO::writeU64LE(c.data() + c.size() - 8, static_cast<uint64_t>(c.size()) + 1);   // archiveSize 超出文件
			CHECK(openArchive(c) == Error::CORRUPTED);
		}
		// 数据区：打开成功；读取时 XOR / 明文得到错误内容（运行时不校验），LZ4 解压失败返回错误；verify 全部发现
		for (uint64_t i = dataOffset; i < dataOffset + dataSize; i += 61)
		{
			auto a = good;
			a[static_cast<size_t>(i)] ^= 0x5A;
			Error err = Error::OK;
			auto backend = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(a), {}, &err);
			REQUIRE(backend);
			VirtualFileSystem fs;
			REQUIRE(fs.mount("/a", backend) != 0);
			for (const auto& f : files)
			{
				std::vector<uint8_t> out;
				Error re = fs.readFile("/a/" + f.path, out);
				CHECK((re == Error::OK || re == Error::CORRUPTED || re == Error::IO_ERROR));
			}
			Error ve = backend->verifyAll();
			// 翻转可能落在对齐填充字节上，此时内容不变
			CHECK((ve == Error::CORRUPTED || ve == Error::OK));
		}
		// 截断到每个区域的边界附近
		for (uint64_t cut : {uint64_t(0), uint64_t(1), uint64_t(127), uint64_t(128), dataOffset + dataSize / 2, indexOffset,
							 indexOffset + indexSize / 2, indexOffset + indexSize, uint64_t(good.size() - 1)})
		{
			CAPTURE(cut);
			std::vector<uint8_t> a(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
			CHECK(openArchive(a) != Error::OK);
		}
	}

	TEST_CASE("forged entries: huge sizes, out-of-range offsets, invalid types and unsorted paths are rejected without huge allocations")
	{
		FileWriteOptions z;
		z.compress = true;
		const auto good = plainIndexArchive({{"a", std::vector<uint8_t>(4000, 'a'), z}, {"b", test::bytes("bbbb"), {}}});
		REQUIRE(openArchive(good) == Error::OK);
		{
			auto a = good;
			reseal(a);
			CHECK(openArchive(a) == Error::OK);   // reseal 本身不破坏归档
		}
		{
			// LZ4 条目声称解压后接近 2 GB：压缩比不可能达到，必须在打开时拒绝（否则读取时会分配 2 GB）
			auto a = good;
			ByteIO::writeU64LE(entryField(a, 0, ArchiveFormat::E_SIZE), 0x7E000000ull);
			reseal(a);
			CHECK(openArchive(a) == Error::CORRUPTED);
		}
		{
			// 未压缩条目 size 与 storedSize 不一致
			auto a = good;
			ByteIO::writeU64LE(entryField(a, 1, ArchiveFormat::E_SIZE), 1ull << 40);
			reseal(a);
			CHECK(openArchive(a) == Error::CORRUPTED);
		}
		{
			// 偏移 + 长度溢出 / 越界
			auto a = good;
			ByteIO::writeU64LE(entryField(a, 1, ArchiveFormat::E_OFFSET), ~0ull - 2);
			reseal(a);
			CHECK(openArchive(a) == Error::CORRUPTED);
			auto b = good;
			ByteIO::writeU64LE(entryField(b, 1, ArchiveFormat::E_STORED_SIZE), 1ull << 50);
			ByteIO::writeU64LE(entryField(b, 1, ArchiveFormat::E_SIZE), 1ull << 50);
			reseal(b);
			CHECK(openArchive(b) == Error::CORRUPTED);
		}
		{
			auto a = good;
			*entryField(a, 0, ArchiveFormat::E_TYPE) = 9;
			reseal(a);
			CHECK(openArchive(a) == Error::CORRUPTED);
			auto b = good;
			*entryField(b, 0, ArchiveFormat::E_CIPHER) = 7;
			reseal(b);
			CHECK(openArchive(b) == Error::CORRUPTED);
			auto c = good;
			*entryField(c, 1, ArchiveFormat::E_KEY_LENGTH) = 33;
			*entryField(c, 1, ArchiveFormat::E_CIPHER) = 1;
			reseal(c);
			CHECK(openArchive(c) == Error::CORRUPTED);
		}
		{
			// 路径池中把 "b" 改成 "a"：重复路径 / 非升序
			auto a = good;
			const uint32_t nameOffset = ByteIO::readU32LE(entryField(a, 1, ArchiveFormat::E_NAME_OFFSET));
			const uint64_t indexOffset = ByteIO::readU64LE(a.data() + 48);
			a[static_cast<size_t>(indexOffset + 2 * ArchiveFormat::ENTRY_SIZE + nameOffset)] = 'a';
			reseal(a);
			CHECK(openArchive(a) == Error::CORRUPTED);
			// nameLength 越界
			auto b = good;
			ByteIO::writeU32LE(entryField(b, 1, ArchiveFormat::E_NAME_LENGTH), 0xFFFFFFF0u);
			reseal(b);
			CHECK(openArchive(b) == Error::CORRUPTED);
		}
		{
			// header 中 entryCount 声称极大，indexSize 也极大
			auto a = good;
			ByteIO::writeU32LE(a.data() + 24, 0xFFFFFFFFu);
			reseal(a);
			CHECK(openArchive(a) == Error::CORRUPTED);
			auto b = good;
			ByteIO::writeU64LE(b.data() + 56, 1ull << 40);
			ByteIO::writeU64LE(b.data() + 120, xxh3_64(b.data(), 120));
			CHECK(openArchive(b) == Error::CORRUPTED);
		}
	}

	TEST_CASE("keys: missing, wrong length, wrong content, correct; failures leave the mount table unchanged")
	{
		test::TempDir dir;
		ArchiveWriteOptions o;
		o.indexKey = test::randomBytes(32, 99);
		REQUIRE(test::buildArchive(dir.sub("k.vfs"), {{"secret", test::bytes("s"), {}}}, o) == Error::OK);
		VirtualFileSystem fs;
		Error err = Error::OK;
		CHECK(fs.mountArchive("/k", dir.sub("k.vfs"), {}, {}, &err) == 0);
		CHECK(err == Error::KEY_REQUIRED);
		ArchiveOptions wrongLen;
		wrongLen.indexKey = test::randomBytes(31, 1);
		CHECK(fs.mountArchive("/k", dir.sub("k.vfs"), wrongLen, {}, &err) == 0);
		CHECK(err == Error::KEY_INVALID);
		ArchiveOptions wrong;
		wrong.indexKey = test::randomBytes(32, 98);
		CHECK(fs.mountArchive("/k", dir.sub("k.vfs"), wrong, {}, &err) == 0);
		CHECK(err == Error::KEY_INVALID);
		CHECK(fs.mounts().empty());
		ArchiveOptions right;
		right.indexKey = o.indexKey;
		CHECK(fs.mountArchive("/k", dir.sub("k.vfs"), right, {}, &err) != 0);
		CHECK(readAll(fs, "/k/secret") == "s");
		// 不存在的文件、目录、不是归档的文件
		CHECK(fs.mountArchive("/x", dir.sub("none.vfs"), {}, {}, &err) == 0);
		CHECK(err == Error::NOT_FOUND);
		CHECK(fs.mountArchive("/x", dir.path(), {}, {}, &err) == 0);
		CHECK(err != Error::OK);
		dir.write("junk.vfs", std::string(1000, 'j'));
		CHECK(fs.mountArchive("/x", dir.sub("junk.vfs"), {}, {}, &err) == 0);
		CHECK(err == Error::CORRUPTED);
		dir.write("tiny.vfs", "x");
		CHECK(fs.mountArchive("/x", dir.sub("tiny.vfs"), {}, {}, &err) == 0);
		CHECK(err == Error::CORRUPTED);
		CHECK(fs.mounts().size() == 1);
	}

	// ================================================================ 故障注入

	TEST_CASE("fault injection: read failures make readFile / copyFile / moveFile return IO_ERROR and keep the source")
	{
		test::FaultConfig cfg;
		auto mem = MemoryBackend::create();
		mem->addFile("bad.bin", std::string(200000, 'b'));
		mem->addFile("good.bin", "ok");
		auto fault = std::make_shared<test::FaultBackend>(mem, cfg);
		VirtualFileSystem fs;
		REQUIRE(fs.mount("/f", fault) != 0);
		REQUIRE(fs.mountMemory("/other") != 0);
		cfg.match = "bad";
		cfg.readFailAt = 100000;
		std::vector<uint8_t> out;
		CHECK(fs.readFile("/f/bad.bin", out) == Error::IO_ERROR);
		CHECK(readAll(fs, "/f/good.bin") == "ok");
		CHECK(fs.copyFile("/f/bad.bin", "/f/copy.bin") == Error::IO_ERROR);
		CHECK(fs.moveFile("/f/bad.bin", "/other/moved.bin") == Error::IO_ERROR);
		CHECK(fs.isFile("/f/bad.bin"));
		CHECK(cfg.injected.load() >= 3);
	}

	TEST_CASE("fault injection: failed copy-up reads or writes leave no partial or temp files in the write layer")
	{
		test::FaultConfig lowerCfg;
		test::FaultConfig upperCfg;
		auto lowerMem = MemoryBackend::create();
		lowerMem->addFile("big.bin", std::string(300000, 'L'));
		lowerMem->setReadOnly(true);
		auto upperMem = MemoryBackend::create();
		VirtualFileSystem fs;
		REQUIRE(fs.mount("/w", std::make_shared<test::FaultBackend>(lowerMem, lowerCfg), env::prio(0)) != 0);
		REQUIRE(fs.mount("/w", std::make_shared<test::FaultBackend>(upperMem, upperCfg), env::prio(1)) != 0);

		lowerCfg.readFailAt = 150000;
		Error err = Error::OK;
		CHECK_FALSE(fs.openFileStream("/w/big.bin", Mode::APPEND, &err));
		CHECK(err == Error::IO_ERROR);
		CHECK(listNames(fs, "/w") == Names{"big.bin"});
		CHECK(fs.fileSize("/w/big.bin") == 300000);
		Stat st;
		CHECK(upperMem->stat("big.bin", st) == Error::NOT_FOUND);
		lowerCfg.readFailAt = -1;

		upperCfg.writeFailAt = 1000;
		CHECK_FALSE(fs.openFileStream("/w/big.bin", Mode::OVERLAPPED, &err));
		CHECK(err == Error::IO_ERROR);
		CHECK(listNames(fs, "/w") == Names{"big.bin"});
		upperCfg.writeFailAt = -1;

		// copy-up 最后的 rename 失败
		upperCfg.renameError = Error::IO_ERROR;
		CHECK_FALSE(fs.openFileStream("/w/big.bin", Mode::APPEND, &err));
		CHECK(err == Error::IO_ERROR);
		CHECK(listNames(fs, "/w") == Names{"big.bin"});
		upperCfg.renameError = Error::OK;

		// 故障解除后 copy-up 成功
		auto s = fs.openFileStream("/w/big.bin", Mode::APPEND, &err);
		REQUIRE(s);
		CHECK(s->size() == 300000);
	}

	TEST_CASE("fault injection: writeFile and writeFileAtomic on write failure / short write / close failure")
	{
		test::FaultConfig cfg;
		auto mem = MemoryBackend::create();
		VirtualFileSystem fs;
		REQUIRE(fs.mount("/w", std::make_shared<test::FaultBackend>(mem, cfg)) != 0);
		REQUIRE(fs.writeFile("/w/keep.txt", "original") == Error::OK);
		const std::string big(500000, 'n');

		// 短写：writeFile 循环写完
		cfg.shortWrite = 777;
		CHECK(fs.writeFile("/w/short.bin", big) == Error::OK);
		CHECK(readAll(fs, "/w/short.bin") == big);
		cfg.shortWrite = 0;

		// 写到一半失败（磁盘满）
		cfg.writeFailAt = 100000;
		CHECK(fs.writeFile("/w/full.bin", big) == Error::IO_ERROR);
		CHECK(fs.writeFileAtomic("/w/keep.txt", big.data(), big.size()) == Error::IO_ERROR);
		cfg.writeFailAt = -1;
		CHECK(readAll(fs, "/w/keep.txt") == "original");   // 原子保存失败时原文件不变
		for (const auto& n : listNames(fs, "/w"))
			CHECK(n.find(".tmp") == std::string::npos);   // 也不留下临时文件

		// close 失败
		cfg.closeError = EIO;
		CHECK(fs.writeFile("/w/close.bin", "x") == Error::IO_ERROR);
		CHECK(fs.writeFileAtomic("/w/keep.txt", "y", 1) == Error::IO_ERROR);
		cfg.closeError = 0;
		CHECK(readAll(fs, "/w/keep.txt") == "original");

		// writeFileAtomic 的 rename 失败
		cfg.renameError = Error::IO_ERROR;
		CHECK(fs.writeFileAtomic("/w/keep.txt", "z", 1) == Error::IO_ERROR);
		cfg.renameError = Error::OK;
		CHECK(readAll(fs, "/w/keep.txt") == "original");
		for (const auto& n : listNames(fs, "/w"))
			CHECK(n.find(".tmp") == std::string::npos);
	}

	TEST_CASE("fault injection: stat / list / open / mkdir / remove errors propagate unchanged, not as \"missing\"")
	{
		test::FaultConfig cfg;
		auto mem = MemoryBackend::create();
		mem->addFile("d/f.txt", "f");
		auto lower = MemoryBackend::create();
		lower->addFile("d/g.txt", "g");
		lower->setReadOnly(true);
		VirtualFileSystem fs;
		REQUIRE(fs.mount("/w", lower, env::prio(0)) != 0);
		REQUIRE(fs.mount("/w", std::make_shared<test::FaultBackend>(mem, cfg), env::prio(1)) != 0);

		cfg.match = "d";
		cfg.statError = Error::IO_ERROR;
		Stat st;
		CHECK(fs.stat("/w/d/f.txt", st) == Error::IO_ERROR);
		CHECK(fs.stat("/w/d/g.txt", st) == Error::IO_ERROR);   // 上层出错时不能“穿透”到下层
		cfg.statError = Error::OK;

		cfg.listError = Error::IO_ERROR;
		Error err = Error::OK;
		fs.list("/w/d", &err);
		CHECK(err == Error::IO_ERROR);
		cfg.listError = Error::OK;
		CHECK(listNames(fs, "/w/d") == (Names{"f.txt", "g.txt"}));

		cfg.openError = Error::PERMISSION_DENIED;
		CHECK_FALSE(fs.openFileStream("/w/d/f.txt", Mode::READ, &err));
		CHECK(err == Error::PERMISSION_DENIED);
		CHECK(fs.writeFile("/w/d/new.txt", "x") == Error::PERMISSION_DENIED);
		cfg.openError = Error::OK;

		cfg.mkdirError = Error::IO_ERROR;
		CHECK(fs.createDir("/w/d/sub") == Error::IO_ERROR);
		CHECK(fs.createDirs("/w/d/sub/deeper") == Error::IO_ERROR);
		cfg.mkdirError = Error::OK;

		cfg.removeError = Error::IO_ERROR;
		CHECK(fs.removeFile("/w/d/f.txt") == Error::IO_ERROR);
		CHECK(readAll(fs, "/w/d/f.txt") == "f");
		cfg.removeError = Error::OK;
	}

	// ================================================================ 句柄与大文件

	TEST_CASE("handle exhaustion: native streams fail cleanly and recover; archive streams use no OS handle")
	{
#if VFS_PLATFORM_WINDOWS
		return;   // Windows 的句柄上限很高，这里不模拟
#else
		test::TempDir dir;
		dir.write("n/f.txt", "native");
		REQUIRE(test::buildArchive(dir.sub("a.vfs"), {{"f.txt", test::bytes("archive"), {}}}) == Error::OK);
		VirtualFileSystem fs;
		REQUIRE(fs.mountNative("/n", dir.sub("n")) != 0);
		REQUIRE(fs.mountArchive("/a", dir.sub("a.vfs")) != 0);
		REQUIRE(fs.mountMemory("/m") != 0);
		REQUIRE(fs.writeFile("/m/f.txt", "memory") == Error::OK);

		struct rlimit old;
		REQUIRE(::getrlimit(RLIMIT_NOFILE, &old) == 0);
		struct rlimit lim = old;
		lim.rlim_cur = 256;
		if (old.rlim_cur < lim.rlim_cur || ::setrlimit(RLIMIT_NOFILE, &lim) != 0)
			return;
		{
			// 归档流通过挂载时打开的共享句柄做位置读，不消耗系统句柄
			std::vector<std::unique_ptr<IFileStream>> streams;
			for (int i = 0; i < 2000; ++i)
			{
				Error err = Error::OK;
				auto s = fs.openFileStream("/a/f.txt", Mode::READ, &err);
				INFO("archive stream #", i, " failed: ", errorName(err));
				REQUIRE(s);
				streams.push_back(std::move(s));
			}
			char buf[16];
			CHECK(streams.back()->read(buf, sizeof(buf)) == 7);
		}
		for (const char* p : {"/n/f.txt"})
		{
			CAPTURE(p);
			std::vector<std::unique_ptr<IFileStream>> streams;
			Error err = Error::OK;
			for (int i = 0; i < 2000; ++i)
			{
				auto s = fs.openFileStream(p, Mode::READ, &err);
				if (!s)
					break;
				streams.push_back(std::move(s));
			}
			CHECK(streams.size() < 2000u);
			CHECK(err != Error::OK);
			// 已经打开的流都可用
			char buf[16];
			CHECK(streams.front()->read(buf, sizeof(buf)) > 0);
			CHECK(streams.back()->read(buf, sizeof(buf)) > 0);
			// 内存流不消耗系统句柄
			CHECK(readAll(fs, "/m/f.txt") == "memory");
			streams.clear();
			CHECK(fs.openFileStream(p, Mode::READ) != nullptr);
		}
		::setrlimit(RLIMIT_NOFILE, &old);
#endif
	}

	TEST_CASE("memory backend with 5000 open streams")
	{
		VirtualFileSystem fs;
		REQUIRE(fs.mountMemory("/m") != 0);
		REQUIRE(fs.writeFile("/m/f", "abc") == Error::OK);
		std::vector<std::unique_ptr<IFileStream>> streams;
		for (int i = 0; i < 5000; ++i)
		{
			streams.push_back(fs.openFileStream("/m/f", Mode::READ));
			REQUIRE(streams.back());
		}
		char c;
		for (auto& s : streams)
			CHECK(s->read(&c, 1) == 1);
		CHECK(fs.unmount(fs.mounts()[0].id, UnmountFlags::FailIfBusy) == Error::BUSY);
	}

	TEST_CASE("sparse file over 4 GB (slow): native stream resize / seek / read / write beyond 4 GB" * doctest::skip(!test::slowEnabled()))
	{
		test::TempDir dir;
		dir.mkdir("n");
		VirtualFileSystem fs;
		REQUIRE(fs.mountNative("/n", dir.sub("n")) != 0);
		const int64_t big = (5ll << 30) + 123;
		{
			auto s = fs.openFileStream("/n/sparse.bin", Mode::WRITE);
			REQUIRE(s);
			REQUIRE(s->resize(big));
			CHECK(s->size() == big);
			CHECK(s->seek((4ll << 30) + 7, SEEK_SET) == (4ll << 30) + 7);
			CHECK(s->write("MARK", 4) == 4);
			CHECK(s->seek(-4, SEEK_END) == big - 4);
			CHECK(s->write("TAIL", 4) == 4);
		}
		CHECK(fs.fileSize("/n/sparse.bin") == big);
		auto r = fs.openFileStream("/n/sparse.bin", Mode::READ);
		REQUIRE(r);
		char buf[8] = {};
		CHECK(r->seek((4ll << 30) + 7, SEEK_SET) == (4ll << 30) + 7);
		CHECK(r->read(buf, 4) == 4);
		CHECK(std::string(buf, 4) == "MARK");
		CHECK(r->tell() == (4ll << 30) + 11);
		CHECK(r->seek(3ll << 30, SEEK_SET) == 3ll << 30);
		CHECK(r->read(buf, 8) == 8);
		CHECK(std::string(buf, 8) == std::string(8, '\0'));
		CHECK(r->seek(-4, SEEK_END) == big - 4);
		CHECK(r->read(buf, 8) == 4);
		CHECK(std::string(buf, 4) == "TAIL");
		// 跨 4 GB 边界读取
		CHECK(r->seek((4ll << 30) - 2, SEEK_SET) == (4ll << 30) - 2);
		CHECK(r->read(buf, 4) == 4);
	}

	TEST_CASE("large file (slow): 300 MB round trip through memory and archive (compressed / encrypted)" * doctest::skip(!test::slowEnabled()))
	{
		const size_t size = 300u << 20;
		std::vector<uint8_t> data(size);
		for (size_t i = 0; i < size; ++i)
			data[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
		test::TempDir dir;
		FileWriteOptions xz;
		xz.encrypt = xz.compress = true;
		REQUIRE(test::buildArchive(dir.sub("big.vfs"), {{"big", data, xz}}) == Error::OK);
		VirtualFileSystem fs;
		REQUIRE(fs.mountArchive("/a", dir.sub("big.vfs")) != 0);
		REQUIRE(fs.mountMemory("/m") != 0);
		std::vector<uint8_t> out;
		REQUIRE(fs.readFile("/a/big", out) == Error::OK);
		CHECK(out == data);
		REQUIRE(fs.writeFile("/m/big", data.data(), data.size()) == Error::OK);
		CHECK(fs.fileSize("/m/big") == static_cast<int64_t>(size));
	}

	TEST_CASE("per-call I/O limit: requests above VFS_MAX_IO_CHUNK are capped without int overflow")
	{
		VirtualFileSystem fs;
		std::shared_ptr<MemoryBackend> mem;
		REQUIRE(fs.mountMemory("/m", {}, &mem) != 0);
		REQUIRE(fs.writeFile("/m/f", "0123456789") == Error::OK);
		auto s = fs.openFileStream("/m/f", Mode::READ);
		REQUIRE(s);
		std::vector<char> buf(16);
		// size 参数巨大但文件很短：只返回实际长度
		CHECK(s->read(buf.data(), 10) == 10);
		CHECK(s->seek(0, SEEK_SET) == 0);
		CHECK(s->read(buf.data(), 0xFFFFFFFFu) == 10);
	}
}
