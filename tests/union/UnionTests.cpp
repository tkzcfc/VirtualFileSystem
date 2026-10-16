// 分层合并视图测试：优先级、合并列表、Opaque、写入目标、copy-up、删除、rename、mkdir/rmdir
#include "TestUtils.h"
#include "vfs/memory/MemoryBackend.h"
#include <algorithm>
#include <cstring>

using namespace vfs;
using test::listNames;
using test::readAll;
using Names = std::vector<std::string>;

namespace
{

// 预填内容的只读内存后端
std::shared_ptr<MemoryBackend> roMemory(std::initializer_list<std::pair<const char*, const char*>> files)
{
	auto b = MemoryBackend::create();
	for (const auto& f : files)
	{
		if (std::string_view(f.first).back() == '/')
			REQUIRE(b->createDir(std::string_view(f.first).substr(0, std::strlen(f.first) - 1)) == Error::OK);
		else
			REQUIRE(b->addFile(f.first, std::string_view(f.second)) == Error::OK);
	}
	b->setReadOnly(true);
	return b;
}

std::shared_ptr<MemoryBackend> rwMemory(std::initializer_list<std::pair<const char*, const char*>> files = {})
{
	auto b = roMemory(files);
	b->setReadOnly(false);
	return b;
}

// 直接查看某个后端里的文件内容（绕过 VFS）
std::string backendContent(const std::shared_ptr<MemoryBackend>& b, std::string_view rel)
{
	auto s = b->open(rel, IFileStream::Mode::READ, nullptr);
	if (!s)
		return "<none>";
	return test::str(test::readStream(*s));
}

MountOptions prio(int p, uint32_t flags = 0)
{
	MountOptions o;
	o.priority = p;
	o.flags = flags;
	return o;
}

} // namespace

TEST_SUITE("union")
{
	TEST_CASE("优先级：priority 大的优先，相同时后挂载的优先")
	{
		VirtualFileSystem fs;
		MountId a = fs.mount("/", roMemory({{"f.txt", "A"}}));
		MountId b = fs.mount("/", roMemory({{"f.txt", "B"}}));
		CHECK(readAll(fs, "/f.txt") == "B");

		Stat st;
		REQUIRE(fs.stat("/f.txt", st) == Error::OK);
		CHECK(st.mount == b);

		MountId c = fs.mount("/", roMemory({{"f.txt", "C"}}), prio(-1));
		CHECK(readAll(fs, "/f.txt") == "B");
		MountId d = fs.mount("/", roMemory({{"f.txt", "D"}}), prio(5));
		CHECK(readAll(fs, "/f.txt") == "D");

		CHECK(fs.unmount(d) == Error::OK);
		CHECK(readAll(fs, "/f.txt") == "B");
		CHECK(fs.unmount(b) == Error::OK);
		CHECK(readAll(fs, "/f.txt") == "A");
		CHECK(fs.unmount(a) == Error::OK);
		CHECK(readAll(fs, "/f.txt") == "C");
		CHECK(fs.unmount(c) == Error::OK);
		CHECK(readAll(fs, "/f.txt") == "<ERR:NOT_FOUND>");
		CHECK(fs.unmount(c) == Error::NOT_FOUND);
	}

	TEST_CASE("嵌套挂载按 priority 排序，与深度无关")
	{
		VirtualFileSystem fs;
		fs.mount("/a", roMemory({{"b/x.txt", "deep-by-root"}}), prio(10));
		fs.mount("/a/b", roMemory({{"x.txt", "nested"}, {"y.txt", "only-nested"}}));
		// /a 优先级更高，/a/b/x.txt 由它提供
		CHECK(readAll(fs, "/a/b/x.txt") == "deep-by-root");
		CHECK(readAll(fs, "/a/b/y.txt") == "only-nested");
		CHECK(listNames(fs, "/a/b") == Names{"x.txt", "y.txt"});
	}

	TEST_CASE("列目录：合并、去重、按字节序排序")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"d/b.txt", "1"}, {"d/a.txt", "1"}, {"d/sub/x", "1"}}));
		fs.mount("/", roMemory({{"d/c.txt", "2"}, {"d/a.txt", "2"}, {"d/Z.txt", "2"}}));
		CHECK(listNames(fs, "/d") == Names{"Z.txt", "a.txt", "b.txt", "c.txt", "sub/"});
		CHECK(readAll(fs, "/d/a.txt") == "2");

		// 回调返回 false 时停止遍历
		int count = 0;
		CHECK(fs.list("/d", [&count](const DirEntry&) { return ++count < 2; }) == Error::OK);
		CHECK(count == 2);

		Error err = Error::OK;
		auto entries = fs.list("/d", &err);
		CHECK(err == Error::OK);
		CHECK(entries.size() == 5);

		CHECK(fs.list("/d/a.txt", [](const DirEntry&) { return true; }) == Error::NOT_DIR);
		CHECK(fs.list("/nope", [](const DirEntry&) { return true; }) == Error::NOT_FOUND);
	}

	TEST_CASE("类型冲突：高层文件遮蔽低层目录，查找子路径返回 NOT_DIR")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"x/y.txt", "low"}, {"f", "low-file"}}));
		fs.mount("/", roMemory({{"x", "high-file"}, {"f/inner.txt", "high-dir"}}));

		CHECK(fs.isFile("/x"));
		CHECK(readAll(fs, "/x/y.txt") == "<ERR:NOT_DIR>");
		Stat st;
		CHECK(fs.stat("/x/y.txt", st) == Error::NOT_DIR);
		CHECK(listNames(fs, "/") == Names{"f/", "x"});

		// 高层目录、低层文件：按目录处理
		CHECK(fs.isDir("/f"));
		CHECK(readAll(fs, "/f/inner.txt") == "high-dir");
	}

	TEST_CASE("Opaque：挂载点以下屏蔽更低的层")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"data/a.txt", "low"}, {"data/b.txt", "low"}, {"other.txt", "low"}}));
		fs.mount("/data", roMemory({{"a.txt", "opaque"}}), prio(0, MountFlags::Opaque));
		CHECK(readAll(fs, "/data/a.txt") == "opaque");
		CHECK(readAll(fs, "/data/b.txt") == "<ERR:NOT_FOUND>");
		CHECK(listNames(fs, "/data") == Names{"a.txt"});
		CHECK(readAll(fs, "/other.txt") == "low");

		// 比 Opaque 层优先级更高的层仍然可见
		fs.mount("/", roMemory({{"data/c.txt", "high"}}), prio(10));
		CHECK(listNames(fs, "/data") == Names{"a.txt", "c.txt"});
	}

	TEST_CASE("虚拟祖先目录：挂载点的祖先显示为只读目录")
	{
		VirtualFileSystem fs;
		fs.mount("/x/y/z", roMemory({{"f.txt", "1"}}));
		CHECK(fs.isDir("/"));
		CHECK(fs.isDir("/x"));
		CHECK(fs.isDir("/x/y"));
		CHECK(fs.isDir("/x/y/z"));
		CHECK(listNames(fs, "/") == Names{"x/"});
		CHECK(listNames(fs, "/x/y") == Names{"z/"});
		CHECK(readAll(fs, "/x/y/z/f.txt") == "1");
		Stat st;
		REQUIRE(fs.stat("/x", st) == Error::OK);
		CHECK_FALSE(st.writable);
		CHECK(fs.createDir("/x/new") == Error::READ_ONLY);
		CHECK(fs.removeDir("/x") == Error::BUSY);
		CHECK(fs.removeDir("/x/y/z") == Error::BUSY);
		CHECK(fs.rename("/x/y/z", "/q") == Error::BUSY);
	}

	TEST_CASE("空 VFS：根目录存在且为空")
	{
		VirtualFileSystem fs;
		CHECK(fs.isDir("/"));
		CHECK(listNames(fs, "/").empty());
		CHECK(fs.writeFile("/a.txt", "x") == Error::READ_ONLY);
		CHECK_FALSE(fs.exists("/a.txt"));
		CHECK(fs.fileSize("/a.txt") == -1);
	}

	TEST_CASE("写入目标：可见项可写则原地写")
	{
		VirtualFileSystem fs;
		auto low = rwMemory({{"f.txt", "low"}});
		auto high = rwMemory();
		fs.mount("/", low);
		fs.mount("/", high);
		REQUIRE(fs.writeFile("/f.txt", "changed") == Error::OK);
		CHECK(backendContent(low, "f.txt") == "changed");
		CHECK(backendContent(high, "f.txt") == "<none>");
	}

	TEST_CASE("写入目标：新文件写到覆盖该路径的最高优先级可写层")
	{
		VirtualFileSystem fs;
		auto w1 = rwMemory();
		auto w2 = rwMemory();
		fs.mount("/", w1, prio(1));
		fs.mount("/", w2, prio(2));
		fs.mount("/", roMemory({{"ro.txt", "x"}}), prio(0));
		REQUIRE(fs.writeFile("/new.txt", "n") == Error::OK);
		CHECK(backendContent(w2, "new.txt") == "n");
		CHECK(backendContent(w1, "new.txt") == "<none>");
	}

	TEST_CASE("写入目标：WriteTarget 标记的挂载优先")
	{
		VirtualFileSystem fs;
		auto marked = rwMemory();
		auto other = rwMemory();
		fs.mount("/", marked, prio(1, MountFlags::WriteTarget));
		fs.mount("/", other, prio(2));
		REQUIRE(fs.writeFile("/new.txt", "n") == Error::OK);
		CHECK(backendContent(marked, "new.txt") == "n");
		CHECK(backendContent(other, "new.txt") == "<none>");
	}

	TEST_CASE("写入目标：没有可写层返回 READ_ONLY；ReadOnly 标志生效")
	{
		VirtualFileSystem fs;
		auto rw = rwMemory({{"f.txt", "x"}});
		fs.mount("/", rw, prio(0, MountFlags::ReadOnly));
		CHECK(fs.writeFile("/f.txt", "y") == Error::READ_ONLY);
		CHECK(fs.writeFile("/g.txt", "y") == Error::READ_ONLY);
		CHECK(fs.removeFile("/f.txt") == Error::READ_ONLY);
		CHECK(fs.createDir("/d") == Error::READ_ONLY);
		CHECK(fs.openFileStream("/f.txt", IFileStream::Mode::APPEND) == nullptr);
		CHECK(backendContent(rw, "f.txt") == "x");
		Stat st;
		REQUIRE(fs.stat("/f.txt", st) == Error::OK);
		CHECK_FALSE(st.writable);
	}

	TEST_CASE("写入目标：可写层被只读层遮蔽时拒绝写入")
	{
		VirtualFileSystem fs;
		auto rw = rwMemory();
		fs.mount("/", rw, prio(0));
		fs.mount("/", roMemory({{"f.txt", "ro"}}), prio(10));
		// 写到低优先级可写层会被只读层挡住，读不到 → READ_ONLY
		CHECK(fs.writeFile("/f.txt", "x") == Error::READ_ONLY);
		CHECK(backendContent(rw, "f.txt") == "<none>");
		// 不冲突的新文件可以写
		CHECK(fs.writeFile("/g.txt", "x") == Error::OK);
	}

	TEST_CASE("写入目标：可写层只覆盖子树时，子树外不可写")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"res/a.txt", "a"}}));
		auto user = rwMemory();
		fs.mount("/user", user);
		CHECK(fs.writeFile("/res/b.txt", "b") == Error::READ_ONLY);
		CHECK(fs.writeFile("/user/save.dat", "s") == Error::OK);
		CHECK(backendContent(user, "save.dat") == "s");
	}

	TEST_CASE("copy-up 目录：父目录只在只读层时，在写入目标中自动 mkdir -p")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"a/b/c.txt", "c"}}));
		auto rw = rwMemory();
		fs.mount("/", rw);
		REQUIRE(fs.writeFile("/a/b/new.txt", "n") == Error::OK);
		CHECK(backendContent(rw, "a/b/new.txt") == "n");
		Stat st;
		REQUIRE(rw->stat("a/b", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		CHECK(listNames(fs, "/a/b") == Names{"c.txt", "new.txt"});

		// 父目录在合并视图中也不存在 → NOT_FOUND（open(O_CREAT) 不建父目录）
		CHECK(fs.writeFile("/missing/x.txt", "x") == Error::NOT_FOUND);
		Error err = Error::OK;
		CHECK(fs.openFileStream("/missing/x.txt", IFileStream::Mode::WRITE, &err) == nullptr);
		CHECK(err == Error::NOT_FOUND);
		// 父路径是文件 → NOT_DIR
		CHECK(fs.writeFile("/a/b/c.txt/x", "x") == Error::NOT_DIR);
	}

	TEST_CASE("copy-up 文件：APPEND/OVERLAPPED 先拷贝下层内容，WRITE 直接新建")
	{
		VirtualFileSystem fs;
		auto ro = roMemory({{"cfg/a.ini", "base"}, {"cfg/b.ini", "base"}, {"cfg/c.ini", "base"}});
		fs.mount("/", ro);
		auto rw = rwMemory();
		fs.mount("/", rw, prio(1));

		{
			auto s = fs.openFileStream("/cfg/a.ini", IFileStream::Mode::APPEND);
			REQUIRE(s);
			CHECK(s->size() == 4);
			CHECK(s->write("+x", 2) == 2);
		}
		CHECK(readAll(fs, "/cfg/a.ini") == "base+x");
		CHECK(backendContent(rw, "cfg/a.ini") == "base+x");
		CHECK(backendContent(ro, "cfg/a.ini") == "base");

		{
			auto s = fs.openFileStream("/cfg/b.ini", IFileStream::Mode::OVERLAPPED);
			REQUIRE(s);
			CHECK(s->write("B", 1) == 1);
		}
		CHECK(readAll(fs, "/cfg/b.ini") == "Base");

		{
			auto s = fs.openFileStream("/cfg/c.ini", IFileStream::Mode::WRITE);
			REQUIRE(s);
			CHECK(s->size() == 0);
			CHECK(s->write("new", 3) == 3);
		}
		CHECK(readAll(fs, "/cfg/c.ini") == "new");
		CHECK(backendContent(ro, "cfg/c.ini") == "base");

		// copy-up 不留下临时文件
		CHECK(listNames(fs, "/cfg") == Names{"a.ini", "b.ini", "c.ini"});
	}

	TEST_CASE("removeFile：只读层返回 READ_ONLY；删除覆盖文件后原版重新露出")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"f.txt", "original"}}));
		auto rw = rwMemory();
		MountId rwId = fs.mount("/", rw);

		CHECK(fs.removeFile("/f.txt") == Error::READ_ONLY);
		REQUIRE(fs.writeFile("/f.txt", "override") == Error::OK);
		Stat st;
		REQUIRE(fs.stat("/f.txt", st) == Error::OK);
		CHECK(st.mount == rwId);
		CHECK(st.writable);
		CHECK(readAll(fs, "/f.txt") == "override");

		CHECK(fs.removeFile("/f.txt") == Error::OK);
		CHECK(readAll(fs, "/f.txt") == "original");
		CHECK(fs.removeFile("/f.txt") == Error::READ_ONLY);
		CHECK(fs.removeFile("/nope") == Error::NOT_FOUND);

		REQUIRE(fs.createDir("/dir") == Error::OK);
		CHECK(fs.removeFile("/dir") == Error::IS_DIR);
	}

	TEST_CASE("rename：同一可写后端内成功；只读源 READ_ONLY；跨后端 CROSS_DEVICE")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"ro.txt", "r"}}));
		auto a = rwMemory({{"x.txt", "x"}, {"d/in.txt", "i"}});
		auto b = rwMemory({{"y.txt", "y"}});
		fs.mount("/a", a);
		fs.mount("/b", b);

		CHECK(fs.rename("/a/x.txt", "/a/x2.txt") == Error::OK);
		CHECK(readAll(fs, "/a/x2.txt") == "x");
		CHECK_FALSE(fs.exists("/a/x.txt"));

		CHECK(fs.rename("/a/d", "/a/d2") == Error::OK);
		CHECK(readAll(fs, "/a/d2/in.txt") == "i");

		CHECK(fs.rename("/ro.txt", "/a/ro.txt") == Error::READ_ONLY);
		CHECK(fs.rename("/a/x2.txt", "/b/x.txt") == Error::CROSS_DEVICE);
		CHECK(fs.rename("/nope", "/a/n") == Error::NOT_FOUND);
		CHECK(fs.rename("/a", "/c") == Error::BUSY);

		// moveFile：跨后端 copy + remove
		CHECK(fs.moveFile("/a/x2.txt", "/b/x.txt") == Error::OK);
		CHECK(readAll(fs, "/b/x.txt") == "x");
		CHECK_FALSE(fs.exists("/a/x2.txt"));
		CHECK(fs.moveFile("/a/d2", "/b/d2") == Error::CROSS_DEVICE);   // 目录不支持跨后端移动

		// copyFile
		CHECK(fs.copyFile("/ro.txt", "/b/copy.txt") == Error::OK);
		CHECK(readAll(fs, "/b/copy.txt") == "r");
	}

	TEST_CASE("createDir / createDirs")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"ro/sub/f.txt", "f"}}));
		auto rw = rwMemory();
		fs.mount("/", rw);

		CHECK(fs.createDir("/ro") == Error::EXISTS);
		CHECK(fs.createDir("/ro/sub/f.txt") == Error::EXISTS);
		CHECK(fs.createDir("/x/y") == Error::NOT_FOUND);
		CHECK(fs.createDir("/ro/sub/f.txt/z") == Error::NOT_DIR);

		// 中间目录只在只读层：自动 copy-up
		CHECK(fs.createDir("/ro/sub/new") == Error::OK);
		Stat st;
		CHECK(rw->stat("ro/sub/new", st) == Error::OK);

		CHECK(fs.createDirs("/x/y/z") == Error::OK);
		CHECK(fs.isDir("/x/y/z"));
		CHECK(fs.createDirs("/x/y/z") == Error::OK);   // 已存在不算错
		CHECK(fs.createDirs("/ro/sub") == Error::OK);
		CHECK(fs.createDirs("/ro/sub/f.txt/q") == Error::NOT_DIR);
		CHECK(fs.createDir("/") == Error::EXISTS);
	}

	TEST_CASE("removeDir：只读层贡献了子孙时返回 READ_ONLY，什么都不删")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"a/b/c.txt", "pack"}}));
		auto local = rwMemory({{"a/mine.txt", "m"}});
		fs.mount("/", local);

		CHECK(fs.removeDir("/a") == Error::READ_ONLY);
		CHECK(fs.removeDir("/a", true) == Error::READ_ONLY);
		CHECK(backendContent(local, "a/mine.txt") == "m");

		// 本地文件可以逐个删除，/a/b/c.txt 始终存在
		CHECK(fs.removeFile("/a/mine.txt") == Error::OK);
		CHECK(readAll(fs, "/a/b/c.txt") == "pack");
		CHECK(fs.removeDir("/a/b") == Error::READ_ONLY);
	}

	TEST_CASE("removeDir：非递归要求为空，递归只删可写层")
	{
		VirtualFileSystem fs;
		auto rw = rwMemory({{"d/x.txt", "x"}, {"d/sub/y.txt", "y"}, {"e/", ""}});
		fs.mount("/", rw);

		CHECK(fs.removeDir("/d") == Error::NOT_EMPTY);
		CHECK(fs.removeDir("/d/x.txt") == Error::NOT_DIR);
		CHECK(fs.removeDir("/nope") == Error::NOT_FOUND);
		CHECK(fs.removeDir("/e") == Error::OK);
		CHECK_FALSE(fs.exists("/e"));

		CHECK(fs.removeDir("/d", true) == Error::OK);
		CHECK_FALSE(fs.exists("/d"));
		CHECK(listNames(fs, "/").empty());
		CHECK(fs.removeDir("/") == Error::BUSY);
	}

	TEST_CASE("removeDir：两个可写层都有该目录时返回 READ_ONLY（只删写入目标）")
	{
		VirtualFileSystem fs;
		auto w1 = rwMemory({{"d/", ""}});
		auto w2 = rwMemory({{"d/", ""}});
		fs.mount("/", w1);
		fs.mount("/", w2);
		CHECK(fs.removeDir("/d") == Error::READ_ONLY);
		Stat st;
		CHECK(w1->stat("d", st) == Error::OK);
		CHECK(w2->stat("d", st) == Error::OK);
	}

	TEST_CASE("惰性卸载：流在卸载后仍然可读；FailIfBusy 严格模式")
	{
		VirtualFileSystem fs;
		MountId id = fs.mount("/dlc", roMemory({{"big.bin", "0123456789"}}));
		auto s = fs.openFileStream("/dlc/big.bin", IFileStream::Mode::READ);
		REQUIRE(s);
		char buf[4];
		CHECK(s->read(buf, 4) == 4);

		CHECK(fs.unmount(id, UnmountFlags::FailIfBusy) == Error::BUSY);
		CHECK(fs.exists("/dlc/big.bin"));

		CHECK(fs.unmount(id) == Error::OK);
		CHECK_FALSE(fs.exists("/dlc/big.bin"));
		CHECK(fs.mounts().empty());
		// 后端由流保活，继续读完
		CHECK(test::str(test::readStream(*s)) == "456789");
		s.reset();

		MountId id2 = fs.mount("/dlc", roMemory({{"f", "1"}}));
		s = fs.openFileStream("/dlc/f", IFileStream::Mode::READ);
		s->close();
		CHECK(fs.unmount(id2, UnmountFlags::FailIfBusy) == Error::OK);
	}

	TEST_CASE("内存后端 unlink 语义：删除打开中的文件，流继续可用")
	{
		VirtualFileSystem fs;
		fs.mount("/", rwMemory({{"f.txt", "data"}}));
		auto s = fs.openFileStream("/f.txt", IFileStream::Mode::READ);
		REQUIRE(s);
		CHECK(fs.removeFile("/f.txt") == Error::OK);
		CHECK_FALSE(fs.exists("/f.txt"));
		CHECK(test::str(test::readStream(*s)) == "data");
		// 同名新文件与旧流无关
		REQUIRE(fs.writeFile("/f.txt", "new") == Error::OK);
		s->seek(0, SEEK_SET);
		CHECK(test::str(test::readStream(*s)) == "data");
	}

	TEST_CASE("remount 替换后端；mounts() 反映当前状态；generation 递增")
	{
		VirtualFileSystem fs;
		uint64_t g0 = fs.generation();
		MountId id = fs.mount("/m", roMemory({{"f", "old"}}), prio(3, MountFlags::ReadOnly));
		CHECK(fs.generation() > g0);
		auto infos = fs.mounts();
		REQUIRE(infos.size() == 1);
		CHECK(infos[0].id == id);
		CHECK(infos[0].mountPoint == "/m");
		CHECK(infos[0].priority == 3);
		CHECK(infos[0].flags == MountFlags::ReadOnly);

		uint64_t g1 = fs.generation();
		CHECK(fs.remount(id, roMemory({{"f", "new"}})) == Error::OK);
		CHECK(fs.generation() > g1);
		CHECK(readAll(fs, "/m/f") == "new");
		CHECK(fs.remount(id + 100, roMemory({})) == Error::NOT_FOUND);
		CHECK(fs.remount(id, nullptr) != Error::OK);
	}

	TEST_CASE("挂载参数校验")
	{
		VirtualFileSystem fs;
		Error err = Error::OK;
		CHECK(fs.mount("/../x", roMemory({}), {}, &err) == 0);
		CHECK(err == Error::INVALID_PATH);
		CHECK(fs.mount("/x", nullptr, {}, &err) == 0);
		CHECK(fs.mountNative("/n", "/definitely/not/existing/dir", {}, {}, &err) == 0);
		CHECK(err == Error::NOT_FOUND);
		CHECK(fs.mountArchive("/n", "/definitely/not/existing.vfs", {}, {}, &err) == 0);
		CHECK(err == Error::NOT_FOUND);
		CHECK(fs.mounts().empty());
	}

	TEST_CASE("别名：前缀替换（res:// → /res）")
	{
		VirtualFileSystem fs;
		fs.mount("/res", roMemory({{"img/a.png", "png"}}));
		CHECK(fs.addAlias("res://", "/res") == Error::OK);
		CHECK(readAll(fs, "res://img/a.png") == "png");
		CHECK(fs.isDir("res://img"));
		CHECK(fs.addAlias("root:", "/") == Error::OK);
		CHECK(readAll(fs, "root:res/img/a.png") == "png");
		CHECK(fs.removeAlias("res://") == Error::OK);
		CHECK(fs.removeAlias("res://") == Error::NOT_FOUND);
		CHECK_FALSE(fs.exists("res://img/a.png"));
	}

	TEST_CASE("非法路径在所有 API 上都返回 INVALID_PATH")
	{
		VirtualFileSystem fs;
		fs.mount("/", rwMemory({{"f", "x"}}));
		Stat st;
		CHECK(fs.stat("/../f", st) == Error::INVALID_PATH);
		CHECK(fs.writeFile("/a/../../f", "x") == Error::INVALID_PATH);
		CHECK(fs.createDir(std::string_view("/a\0b", 4)) == Error::INVALID_PATH);
		CHECK(fs.removeFile("/..") == Error::INVALID_PATH);
		CHECK(fs.rename("/f", "/../g") == Error::INVALID_PATH);
		Error err = Error::OK;
		CHECK(fs.openFileStream("/../f", IFileStream::Mode::READ, &err) == nullptr);
		CHECK(err == Error::INVALID_PATH);
		CHECK_FALSE(fs.exists("/../f"));
		// 规范化后合法
		CHECK(readAll(fs, "\\x\\..\\f") == "x");
	}

	TEST_CASE("readFile / openFileStream 对目录返回 IS_DIR；writeFileAtomic 不留下临时文件")
	{
		VirtualFileSystem fs;
		fs.mount("/", rwMemory({{"d/f", "x"}}));
		CHECK(readAll(fs, "/d") == "<ERR:IS_DIR>");
		Error err = Error::OK;
		CHECK(fs.openFileStream("/d", IFileStream::Mode::READ, &err) == nullptr);
		CHECK(err == Error::IS_DIR);
		CHECK(fs.writeFile("/d", "x") == Error::IS_DIR);

		CHECK(fs.writeFileAtomic("/d/f", "atomic", 6) == Error::OK);
		CHECK(readAll(fs, "/d/f") == "atomic");
		CHECK(fs.writeFileAtomic("/d/g", "", 0) == Error::OK);
		CHECK(fs.fileSize("/d/g") == 0);
		CHECK(listNames(fs, "/d") == Names{"f", "g"});
	}

	TEST_CASE("walk：深度优先、合并视图、可提前停止")
	{
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"a/1", "1"}, {"a/b/2", "2"}}));
		fs.mount("/", roMemory({{"a/b/3", "3"}, {"c", "c"}}));
		std::vector<std::string> seen;
		CHECK(fs.walk("/", [&seen](const std::string& full, const DirEntry&)
		{
			seen.push_back(full);
			return true;
		}) == Error::OK);
		std::sort(seen.begin(), seen.end());
		CHECK(seen == Names{"/a", "/a/1", "/a/b", "/a/b/2", "/a/b/3", "/c"});

		int n = 0;
		fs.walk("/", [&n](const std::string&, const DirEntry&) { return ++n < 3; });
		CHECK(n == 3);
	}

	TEST_CASE("单文件挂载：子路径返回 NOT_DIR，可读写")
	{
		test::TempDir dir;
		dir.write("cfg.json", "{}");
		VirtualFileSystem fs;
		fs.mount("/", roMemory({{"config.json", "packed"}}));
		REQUIRE(fs.mountFile("/config.json", dir.sub("cfg.json")) != 0);
		CHECK(readAll(fs, "/config.json") == "{}");
		CHECK(readAll(fs, "/config.json/x") == "<ERR:NOT_DIR>");
		CHECK(fs.writeFile("/config.json", "{\"a\":1}") == Error::OK);
		CHECK(dir.read("cfg.json") == "{\"a\":1}");
		CHECK(fs.removeFile("/config.json") == Error::BUSY);
		CHECK(fs.realPath("/config.json") == dir.sub("cfg.json"));
	}
}
