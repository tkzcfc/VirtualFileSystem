// 本机目录后端测试：UTF-8 路径、可移植名字检查、只读、symlink 沙箱、realPath、删除打开中的文件
#include "TestUtils.h"
#include "vfs/native/NativeBackend.h"
#include <filesystem>

using namespace vfs;
using Names = std::vector<std::string>;

TEST_SUITE("native")
{
	TEST_CASE("UTF-8 paths: Chinese, Japanese and emoji names can be written, read and listed")
	{
		test::TempDir dir;
		dir.write("资源/图片/背景.png", "png-data");
		VirtualFileSystem fs;
		REQUIRE(fs.mountNative("/", dir.path()) != 0);
		CHECK(test::readAll(fs, "/资源/图片/背景.png") == "png-data");
		CHECK(test::listNames(fs, "/资源") == Names{"图片/"});

		REQUIRE(fs.createDirs("/セーブ/😀") == Error::OK);
		REQUIRE(fs.writeFile("/セーブ/😀/데이터.sav", "save") == Error::OK);
		CHECK(dir.read("セーブ/😀/데이터.sav") == "save");
		CHECK(fs.rename("/セーブ/😀/데이터.sav", "/セーブ/重命名.sav") == Error::OK);
		CHECK(dir.read("セーブ/重命名.sav") == "save");
	}

	TEST_CASE("stat: type, size, mtime, writable")
	{
		test::TempDir dir;
		dir.write("f.bin", std::string(1234, 'x'));
		VirtualFileSystem fs;
		MountId id = fs.mountNative("/n", dir.path());
		Stat st;
		REQUIRE(fs.stat("/n/f.bin", st) == Error::OK);
		CHECK(st.type == FileType::FILE);
		CHECK(st.size == 1234);
		CHECK(st.mtime > 0);
		CHECK(st.mount == id);
		CHECK(st.writable);
		REQUIRE(fs.stat("/n", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		CHECK(fs.stat("/n/f.bin/x", st) == Error::NOT_DIR);
		CHECK(fs.stat("/n/none", st) == Error::NOT_FOUND);
	}

	TEST_CASE("non-portable names are rejected (Windows reserved names, colon)")
	{
		test::TempDir dir;
		VirtualFileSystem fs;
		fs.mountNative("/", dir.path());
		CHECK(fs.writeFile("/CON", "x") == Error::INVALID_PATH);
		CHECK(fs.writeFile("/a:b", "x") == Error::INVALID_PATH);
		CHECK(fs.createDir("/nul.txt") == Error::INVALID_PATH);
		CHECK(fs.writeFile("/trailing.", "x") == Error::INVALID_PATH);
		CHECK(fs.writeFile("/console", "x") == Error::OK);
	}

	TEST_CASE("readOnly option: every write operation returns READ_ONLY")
	{
		test::TempDir dir;
		dir.write("f", "x");
		auto b = NativeBackend::create(dir.path(), NativeOptions{true});
		REQUIRE(b);
		CHECK((b->capabilities() & BackendCaps::Write) == 0);
		CHECK(b->open("f", IFileStream::Mode::WRITE, nullptr) == nullptr);
		CHECK(b->removeFile("f") == Error::READ_ONLY);
		CHECK(b->createDir("d") == Error::READ_ONLY);
		CHECK(b->rename("f", "g") == Error::READ_ONLY);
		CHECK(dir.read("f") == "x");
	}

	TEST_CASE("create: root missing / root is a file / createRoot")
	{
		test::TempDir dir;
		Error err = Error::OK;
		CHECK(NativeBackend::create(dir.sub("missing"), {}, &err) == nullptr);
		CHECK(err == Error::NOT_FOUND);
		dir.write("file", "x");
		CHECK(NativeBackend::create(dir.sub("file"), {}, &err) == nullptr);
		CHECK(err == Error::NOT_DIR);
		NativeOptions o;
		o.createRoot = true;
		CHECK(NativeBackend::create(dir.sub("made/deep"), o, &err) != nullptr);
		CHECK(dir.exists("made/deep"));
	}

	TEST_CASE("realPath returns the native path; virtual directories return an empty string")
	{
		test::TempDir dir;
		dir.write("a/b.txt", "x");
		VirtualFileSystem fs;
		fs.mountNative("/x/y", dir.path());
		CHECK(fs.realPath("/x/y/a/b.txt") == dir.sub("a/b.txt"));
		CHECK(fs.realPath("/x").empty());
		CHECK(fs.realPath("/x/y/none").empty());
	}

	TEST_CASE("removing an open file follows OS behaviour (NativeFileStream uses FILE_SHARE_DELETE)")
	{
		test::TempDir dir;
		dir.write("f", "data");
		VirtualFileSystem fs;
		fs.mountNative("/", dir.path());
		auto s = fs.openFileStream("/f", IFileStream::Mode::READ);
		REQUIRE(s);
		CHECK(fs.removeFile("/f") == Error::OK);
		CHECK(test::str(test::readStream(*s)) == "data");
	}

	TEST_CASE("directory operations: createDir / removeDir / rename over an existing file")
	{
		test::TempDir dir;
		VirtualFileSystem fs;
		fs.mountNative("/", dir.path());
		CHECK(fs.createDir("/d") == Error::OK);
		CHECK(fs.createDir("/d") == Error::EXISTS);
		CHECK(fs.writeFile("/d/a", "a") == Error::OK);
		CHECK(fs.writeFile("/d/b", "b") == Error::OK);
		CHECK(fs.removeDir("/d") == Error::NOT_EMPTY);
		// rename 到已有文件：POSIX 语义，覆盖
		CHECK(fs.rename("/d/a", "/d/b") == Error::OK);
		CHECK(test::readAll(fs, "/d/b") == "a");
		CHECK(fs.removeDir("/d", true) == Error::OK);
		CHECK_FALSE(dir.exists("d"));
	}


	TEST_CASE("exactCase: names differing only in case are treated as missing, consistently on all platforms")
	{
		test::TempDir dir;
		dir.write("Data/Hero.png", "hero");
		VirtualFileSystem fs;
		NativeOptions o;
		o.exactCase = true;
		REQUIRE(fs.mountNative("/", dir.path(), {}, o) != 0);
		INFO("nativeCaseSensitive=" << test::nativeCaseSensitive());

		// 精确名字正常工作
		CHECK(test::readAll(fs, "/Data/Hero.png") == "hero");
		CHECK(test::listNames(fs, "/Data") == Names{"Hero.png"});
		// 最后一级或父目录只差大小写：都不存在
		CHECK_FALSE(fs.exists("/Data/hero.png"));
		CHECK_FALSE(fs.exists("/data/Hero.png"));
		CHECK(test::readAll(fs, "/data/Hero.png") == "<ERR:NOT_FOUND>");
		CHECK(fs.list("/data", [](const DirEntry&) { return true; }) == Error::NOT_FOUND);
		CHECK(fs.removeFile("/Data/HERO.png") == Error::NOT_FOUND);
		CHECK(fs.rename("/Data/HERO.png", "/Data/x.png") == Error::NOT_FOUND);
		CHECK(fs.createDir("/data/sub") == Error::NOT_FOUND);
		CHECK(fs.removeDir("/DATA") == Error::NOT_FOUND);

		// 写入只差大小写的名字：
		// 区分大小写的文件系统上是另一个文件；不区分的文件系统上会落到已有文件，拒绝为 EXISTS 而不是悄悄覆盖
		Error w = fs.writeFile("/Data/hero.png", "lower");
		Error d = fs.createDir("/data");
		if (test::nativeCaseSensitive())
		{
			CHECK(w == Error::OK);
			CHECK(d == Error::OK);
			CHECK(test::readAll(fs, "/Data/hero.png") == "lower");
		}
		else
		{
			CHECK(w == Error::EXISTS);
			CHECK(d == Error::EXISTS);
			CHECK(dir.read("Data/Hero.png") == "hero");
			// 只改大小写的改名是允许的
			CHECK(fs.rename("/Data/Hero.png", "/Data/HERO.png") == Error::OK);
			CHECK(test::listNames(fs, "/Data") == Names{"HERO.png"});
			CHECK(test::readAll(fs, "/Data/HERO.png") == "hero");
			CHECK_FALSE(fs.exists("/Data/Hero.png"));
		}
	}

	TEST_CASE("exactCase off (default): native lookups follow the file system's case rules")
	{
		test::TempDir dir;
		dir.write("Data/Hero.png", "hero");
		VirtualFileSystem fs;
		REQUIRE(fs.mountNative("/", dir.path()) != 0);
		INFO("nativeCaseSensitive=" << test::nativeCaseSensitive());
		CHECK(fs.exists("/data/hero.png") == !test::nativeCaseSensitive());
		// strictCase 只打警告，不改变结果
		NativeOptions o;
		o.strictCase = true;
		VirtualFileSystem warn;
		REQUIRE(warn.mountNative("/", dir.path(), {}, o) != 0);
		CHECK(warn.exists("/data/hero.png") == !test::nativeCaseSensitive());
	}

#if !VFS_PLATFORM_WINDOWS
	TEST_CASE("followSymlinks=false: symlinks escaping the root are rejected")
	{
		test::TempDir outside;
		outside.write("secret.txt", "secret");
		test::TempDir dir;
		dir.write("inside.txt", "ok");
		std::filesystem::create_symlink(test::u8(outside.path()), test::u8(dir.sub("escape")));
		std::filesystem::create_symlink(test::u8(dir.sub("inside.txt")), test::u8(dir.sub("alias.txt")));

		VirtualFileSystem open;
		open.mountNative("/", dir.path());
		CHECK(test::readAll(open, "/escape/secret.txt") == "secret");

		VirtualFileSystem sandbox;
		NativeOptions o;
		o.followSymlinks = false;
		sandbox.mountNative("/", dir.path(), {}, o);
		CHECK(test::readAll(sandbox, "/escape/secret.txt") == "<ERR:PERMISSION_DENIED>");
		CHECK(sandbox.writeFile("/escape/new.txt", "x") == Error::PERMISSION_DENIED);
		CHECK_FALSE(outside.exists("new.txt"));
		// 链接目标仍在根内：允许
		CHECK(test::readAll(sandbox, "/alias.txt") == "ok");
	}
#endif
}
