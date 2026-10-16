// 后端 × 分层组合矩阵：同一套文件 / 目录 / 改名 / 流操作在 9 种挂载组合上逐一验证。
#include "Envs.h"
#include <algorithm>
#include <cstring>
#include <set>

using namespace vfs;
using test::listNames;
using test::readAll;
using Mode = IFileStream::Mode;
using Names = std::vector<std::string>;

namespace
{

std::string blob(size_t size, uint64_t seed)
{
	return test::str(test::randomBytes(size, seed));
}

} // namespace

TEST_SUITE("matrix")
{
	// ================================================================ 文件读写

	TEST_CASE_TEMPLATE("files: writes and reads of various sizes", E, VFS_ENVS)
	{
		E e;
		const size_t sizes[] = {0, 1, 7, 4095, 4096, 4097, 65539, 1u << 20};
		for (size_t size : sizes)
		{
			CAPTURE(size);
			const std::string path = "/w/sizes/f" + std::to_string(size) + ".bin";
			REQUIRE(e.fs.createDirs("/w/sizes") == Error::OK);
			const std::string data = blob(size, size + 1);
			REQUIRE(e.fs.writeFile(path, data) == Error::OK);
			CHECK(e.fs.fileSize(path) == static_cast<int64_t>(size));
			CHECK(readAll(e.fs, path) == data);
			auto s = e.fs.openFileStream(path, Mode::READ);
			REQUIRE(s);
			CHECK(s->size() == static_cast<int64_t>(size));
		}
	}

	TEST_CASE_TEMPLATE("files: WRITE truncates, APPEND appends, OVERLAPPED rewrites in place", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.writeFile("/w/m.txt", "0123456789") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/m.txt", "abc") == Error::OK);
		CHECK(readAll(e.fs, "/w/m.txt") == "abc");

		{
			auto s = e.fs.openFileStream("/w/m.txt", Mode::APPEND);
			REQUIRE(s);
			s->seek(0, SEEK_SET);
			CHECK(s->write("de", 2) == 2);   // APPEND 总是写到末尾
			CHECK(s->write("f", 1) == 1);
		}
		CHECK(readAll(e.fs, "/w/m.txt") == "abcdef");

		{
			auto s = e.fs.openFileStream("/w/m.txt", Mode::OVERLAPPED);
			REQUIRE(s);
			CHECK(s->seek(2, SEEK_SET) == 2);
			CHECK(s->write("XY", 2) == 2);
			CHECK(s->seek(0, SEEK_SET) == 0);
			char buf[16] = {};
			CHECK(s->read(buf, sizeof(buf)) == 6);
			CHECK(std::string(buf, 6) == "abXYef");
		}
		CHECK(readAll(e.fs, "/w/m.txt") == "abXYef");

		// APPEND / OVERLAPPED 创建不存在的文件
		{
			auto s = e.fs.openFileStream("/w/new_append.txt", Mode::APPEND);
			REQUIRE(s);
			CHECK(s->size() == 0);
		}
		{
			auto s = e.fs.openFileStream("/w/new_ov.txt", Mode::OVERLAPPED);
			REQUIRE(s);
		}
		CHECK(e.fs.isFile("/w/new_append.txt"));
		CHECK(e.fs.isFile("/w/new_ov.txt"));
	}

	TEST_CASE_TEMPLATE("files: removeFile cases", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.writeFile("/w/del.txt", "x") == Error::OK);
		CHECK(e.fs.removeFile("/w/del.txt") == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/del.txt"));
		CHECK(e.fs.removeFile("/w/del.txt") == Error::NOT_FOUND);
		CHECK(e.fs.removeFile("/w/nope/del.txt") == Error::NOT_FOUND);
		REQUIRE(e.fs.createDir("/w/d") == Error::OK);
		CHECK(e.fs.removeFile("/w/d") == Error::IS_DIR);
		REQUIRE(e.fs.writeFile("/w/file", "f") == Error::OK);
		CHECK(e.fs.removeFile("/w/file/child") == Error::NOT_DIR);
		CHECK(e.fs.isDir("/w/d"));
	}

	TEST_CASE_TEMPLATE("files: the parent must exist; a file as parent returns NOT_DIR", E, VFS_ENVS)
	{
		E e;
		CHECK(e.fs.writeFile("/w/missing/x.txt", "x") == Error::NOT_FOUND);
		REQUIRE(e.fs.writeFile("/w/plain", "p") == Error::OK);
		CHECK(e.fs.writeFile("/w/plain/x.txt", "x") == Error::NOT_DIR);
		CHECK(e.fs.writeFile("/w/base", "x") == Error::IS_DIR);
		Error err = Error::OK;
		CHECK_FALSE(e.fs.openFileStream("/w/base", Mode::READ, &err));
		CHECK(err == Error::IS_DIR);
		CHECK_FALSE(e.fs.openFileStream("/w/none.txt", Mode::READ, &err));
		CHECK(err == Error::NOT_FOUND);
	}

	// ================================================================ 目录

	TEST_CASE_TEMPLATE("directories: createDir / createDirs / list / removeDir lifecycle", E, VFS_ENVS)
	{
		E e;
		CHECK(e.fs.createDir("/w/d1") == Error::OK);
		CHECK(e.fs.createDir("/w/d1") == Error::EXISTS);
		CHECK(e.fs.createDir("/w/x/y") == Error::NOT_FOUND);
		CHECK(e.fs.createDirs("/w/x/y/z") == Error::OK);
		CHECK(e.fs.createDirs("/w/x/y/z") == Error::OK);
		CHECK(e.fs.isDir("/w/x/y"));
		REQUIRE(e.fs.writeFile("/w/x/y/z/f.txt", "f") == Error::OK);
		CHECK(e.fs.createDir("/w/x/y/z/f.txt") == Error::EXISTS);
		CHECK(e.fs.createDirs("/w/x/y/z/f.txt/q") == Error::NOT_DIR);
		CHECK(e.fs.createDir("/w/x/y/z/f.txt/q") == Error::NOT_DIR);

		CHECK(listNames(e.fs, "/w/x/y/z") == Names{"f.txt"});
		Error err = Error::OK;
		CHECK(e.fs.list("/w/x/y/z/f.txt", &err).empty());
		CHECK(err == Error::NOT_DIR);
		e.fs.list("/w/none", &err);
		CHECK(err == Error::NOT_FOUND);

		CHECK(e.fs.removeDir("/w/x") == Error::NOT_EMPTY);
		CHECK(e.fs.removeDir("/w/x/y/z/f.txt") == Error::NOT_DIR);
		CHECK(e.fs.removeDir("/w/none") == Error::NOT_FOUND);
		CHECK(e.fs.removeDir("/w/x", true) == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/x"));
		CHECK(e.fs.removeDir("/w/d1") == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/d1"));
	}

	TEST_CASE_TEMPLATE("directories: list is merged, deduplicated and sorted by bytes", E, VFS_ENVS)
	{
		E e;
		for (const char* n : {"zeta", "Alpha", "beta", "_x", "10", "2"})
			REQUIRE(e.fs.writeFile(std::string("/w/base/") + n, n) == Error::OK);
		REQUIRE(e.fs.createDir("/w/base/dir") == Error::OK);
		Names names = listNames(e.fs, "/w/base");
		Names expected = {"10", "2", "Alpha", "_x", "a.txt", "beta", "dir/", "sub/", "zeta"};
		CHECK(names == expected);
		CHECK(std::is_sorted(names.begin(), names.end()));
		// 覆盖底层文件不产生重复项
		REQUIRE(e.fs.writeFile("/w/base/a.txt", "A2") == Error::OK);
		CHECK(listNames(e.fs, "/w/base") == expected);
	}

	TEST_CASE_TEMPLATE("directories: deep tree (64 levels) create, write, walk, recursive remove", E, VFS_ENVS)
	{
		E e;
		std::string path = "/w/deep";
		for (int i = 0; i < 64; ++i)
			path += "/d" + std::to_string(i);
		REQUIRE(e.fs.createDirs(path) == Error::OK);
		REQUIRE(e.fs.writeFile(path + "/leaf.txt", "leaf") == Error::OK);
		CHECK(readAll(e.fs, path + "/leaf.txt") == "leaf");
		int dirs = 0;
		int files = 0;
		REQUIRE(e.fs.walk("/w/deep", [&](const std::string&, const DirEntry& d)
		{
			(d.type == FileType::DIR ? dirs : files)++;
			return true;
		}) == Error::OK);
		CHECK(dirs == 64);
		CHECK(files == 1);
		CHECK(e.fs.removeDir("/w/deep", true) == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/deep"));
	}

	TEST_CASE_TEMPLATE("directories: 1500 files in one directory, complete sorted listing, empty after removing each", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDir("/w/many") == Error::OK);
		std::set<std::string> expected;
		for (int i = 0; i < 1500; ++i)
		{
			std::string name = "f" + std::to_string(i * 7919 % 100000) + ".txt";
			expected.insert(name);
			REQUIRE(e.fs.writeFile("/w/many/" + name, name) == Error::OK);
		}
		Names names = listNames(e.fs, "/w/many");
		CHECK(names == Names(expected.begin(), expected.end()));
		for (const auto& n : expected)
			REQUIRE(e.fs.removeFile("/w/many/" + n) == Error::OK);
		CHECK(listNames(e.fs, "/w/many").empty());
		CHECK(e.fs.removeDir("/w/many") == Error::OK);
	}

	// ================================================================ rename / move

	TEST_CASE_TEMPLATE("rename files: same dir, across dirs, overwrite, type conflict, missing, same path", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDirs("/w/r/a") == Error::OK);
		REQUIRE(e.fs.createDirs("/w/r/b") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/r/a/f.txt", "F") == Error::OK);

		CHECK(e.fs.rename("/w/r/a/f.txt", "/w/r/a/g.txt") == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/r/a/f.txt"));
		CHECK(readAll(e.fs, "/w/r/a/g.txt") == "F");

		CHECK(e.fs.rename("/w/r/a/g.txt", "/w/r/b/g.txt") == Error::OK);
		CHECK(readAll(e.fs, "/w/r/b/g.txt") == "F");

		// 覆盖已有文件
		REQUIRE(e.fs.writeFile("/w/r/b/old.txt", "OLD") == Error::OK);
		CHECK(e.fs.rename("/w/r/b/g.txt", "/w/r/b/old.txt") == Error::OK);
		CHECK(readAll(e.fs, "/w/r/b/old.txt") == "F");
		CHECK_FALSE(e.fs.exists("/w/r/b/g.txt"));

		// 文件改名到已有目录
		CHECK(e.fs.rename("/w/r/b/old.txt", "/w/r/a") == Error::IS_DIR);
		// 源不存在
		CHECK(e.fs.rename("/w/r/none", "/w/r/x") == Error::NOT_FOUND);
		// 目标父目录不存在 / 是文件
		CHECK(e.fs.rename("/w/r/b/old.txt", "/w/r/zz/old.txt") == Error::NOT_FOUND);
		CHECK(e.fs.rename("/w/r/b/old.txt", "/w/r/b/old.txt/x") == Error::NOT_DIR);
		// 相同路径直接成功
		CHECK(e.fs.rename("/w/r/b/old.txt", "/w/r/b/old.txt") == Error::OK);
		CHECK(e.fs.rename("/w/r/b/old.txt", "/w/r/b/./old.txt") == Error::OK);
		CHECK(readAll(e.fs, "/w/r/b/old.txt") == "F");
	}

	TEST_CASE_TEMPLATE("rename directories: move with content, into own subtree, over empty dir / non-empty dir / file", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDirs("/w/t/src/inner") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/t/src/inner/x.txt", "X") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/t/src/y.txt", "Y") == Error::OK);

		CHECK(e.fs.rename("/w/t/src", "/w/t/src/inner/self") == Error::INVALID_PATH);
		CHECK(e.fs.rename("/w/t/src", "/w/t/moved") == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/t/src"));
		CHECK(readAll(e.fs, "/w/t/moved/inner/x.txt") == "X");
		CHECK(readAll(e.fs, "/w/t/moved/y.txt") == "Y");

		// 目录改名到已有文件
		REQUIRE(e.fs.writeFile("/w/t/file", "f") == Error::OK);
		CHECK(e.fs.rename("/w/t/moved", "/w/t/file") == Error::NOT_DIR);

		// 覆盖非空目录：失败，两边都不变
		REQUIRE(e.fs.createDirs("/w/t/full") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/t/full/keep.txt", "K") == Error::OK);
		CHECK(e.fs.rename("/w/t/moved", "/w/t/full") != Error::OK);
		CHECK(readAll(e.fs, "/w/t/full/keep.txt") == "K");
		CHECK(readAll(e.fs, "/w/t/moved/y.txt") == "Y");

		// 覆盖空目录：POSIX 语义成功
		REQUIRE(e.fs.createDir("/w/t/empty") == Error::OK);
		CHECK(e.fs.rename("/w/t/moved", "/w/t/empty") == Error::OK);
		CHECK(readAll(e.fs, "/w/t/empty/inner/x.txt") == "X");
		CHECK_FALSE(e.fs.exists("/w/t/moved"));
	}

	TEST_CASE_TEMPLATE("moveFile / copyFile / writeFileAtomic", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.writeFile("/w/c.txt", "copy me") == Error::OK);
		CHECK(e.fs.copyFile("/w/c.txt", "/w/c2.txt") == Error::OK);
		CHECK(readAll(e.fs, "/w/c2.txt") == "copy me");
		CHECK(readAll(e.fs, "/w/c.txt") == "copy me");
		CHECK(e.fs.copyFile("/w/none", "/w/c3.txt") == Error::NOT_FOUND);
		CHECK_FALSE(e.fs.exists("/w/c3.txt"));

		// 复制到自身：内容不能丢
		CHECK(e.fs.copyFile("/w/c.txt", "/w/c.txt") == Error::OK);
		CHECK(readAll(e.fs, "/w/c.txt") == "copy me");

		CHECK(e.fs.moveFile("/w/c2.txt", "/w/m.txt") == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/c2.txt"));
		CHECK(readAll(e.fs, "/w/m.txt") == "copy me");

		const std::string big = blob(300000, 9);
		CHECK(e.fs.writeFileAtomic("/w/atomic.bin", big.data(), big.size()) == Error::OK);
		CHECK(readAll(e.fs, "/w/atomic.bin") == big);
		CHECK(e.fs.writeFileAtomic("/w/atomic.bin", "small", 5) == Error::OK);
		CHECK(readAll(e.fs, "/w/atomic.bin") == "small");
		// 不留下任何临时文件
		for (const auto& n : listNames(e.fs, "/w"))
			CHECK(n.find("tmp") == std::string::npos);
	}

	// ================================================================ 底层文件

	TEST_CASE_TEMPLATE("lower files: read, override, restore after removing the override, remove, rename", E, VFS_ENVS)
	{
		E e;
		CHECK(readAll(e.fs, "/w/base/a.txt") == "A");
		CHECK(readAll(e.fs, "/w/base/sub/b.txt") == "B");

		Stat st;
		REQUIRE(e.fs.stat("/w/base/a.txt", st) == Error::OK);
		CHECK(st.writable == e.baseWritable);

		// 覆盖：总是可以（只读层上通过遮蔽实现）
		REQUIRE(e.fs.writeFile("/w/base/a.txt", "A2") == Error::OK);
		CHECK(readAll(e.fs, "/w/base/a.txt") == "A2");
		REQUIRE(e.fs.stat("/w/base/a.txt", st) == Error::OK);
		CHECK(st.writable);
		CHECK(e.fs.removeFile("/w/base/a.txt") == Error::OK);
		if (e.baseWritable)
			CHECK_FALSE(e.fs.exists("/w/base/a.txt"));
		else
			CHECK(readAll(e.fs, "/w/base/a.txt") == "A");   // 原版重新露出

		// 直接删除 / 改名底层文件
		if (e.baseWritable)
		{
			CHECK(e.fs.rename("/w/base/sub/b.txt", "/w/base/b_renamed.txt") == Error::OK);
			CHECK(readAll(e.fs, "/w/base/b_renamed.txt") == "B");
			CHECK(e.fs.removeFile("/w/base/b_renamed.txt") == Error::OK);
			CHECK(listNames(e.fs, "/w/base") == Names{"sub/"});
		}
		else
		{
			CHECK(e.fs.removeFile("/w/base/sub/b.txt") == Error::READ_ONLY);
			CHECK(e.fs.rename("/w/base/a.txt", "/w/base/a_renamed.txt") == Error::READ_ONLY);
			CHECK(readAll(e.fs, "/w/base/sub/b.txt") == "B");
			CHECK(readAll(e.fs, "/w/base/a.txt") == "A");
			CHECK_FALSE(e.fs.exists("/w/base/a_renamed.txt"));
		}
	}

	TEST_CASE_TEMPLATE("lower files: APPEND / OVERLAPPED keep the content (copy-up from read-only layers)", E, VFS_ENVS)
	{
		E e;
		{
			auto s = e.fs.openFileStream("/w/base/a.txt", Mode::APPEND);
			REQUIRE(s);
			CHECK(s->size() == 1);
			CHECK(s->write("pp", 2) == 2);
		}
		CHECK(readAll(e.fs, "/w/base/a.txt") == "App");
		{
			auto s = e.fs.openFileStream("/w/base/sub/b.txt", Mode::OVERLAPPED);
			REQUIRE(s);
			CHECK(s->seek(1, SEEK_SET) == 1);
			CHECK(s->write("Z", 1) == 1);
		}
		CHECK(readAll(e.fs, "/w/base/sub/b.txt") == "BZ");
		// copy-up 不留下临时文件
		CHECK(listNames(e.fs, "/w/base/sub") == Names{"b.txt"});
		CHECK(listNames(e.fs, "/w/base") == (Names{"a.txt", "sub/"}));
	}

	TEST_CASE_TEMPLATE("lower directories: creating children inside, removeDir and rename rules", E, VFS_ENVS)
	{
		E e;
		CHECK(e.fs.createDir("/w/base/sub/new") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/base/sub/new/n.txt", "N") == Error::OK);
		CHECK(listNames(e.fs, "/w/base/sub") == (Names{"b.txt", "new/"}));
		CHECK(e.fs.removeDir("/w/base/sub/new", true) == Error::OK);

		Error rd = e.fs.removeDir("/w/base", true);
		if (e.baseWritable)
		{
			CHECK(rd == Error::OK);
			CHECK_FALSE(e.fs.exists("/w/base"));
		}
		else
		{
			CHECK(rd == Error::READ_ONLY);
			CHECK(readAll(e.fs, "/w/base/a.txt") == "A");
			CHECK(readAll(e.fs, "/w/base/sub/b.txt") == "B");
			CHECK(e.fs.rename("/w/base/sub", "/w/sub2") == Error::READ_ONLY);
		}
	}

	// ================================================================ 名字与路径

	TEST_CASE_TEMPLATE("names: Chinese, Japanese, emoji, spaces, leading dot, multiple dots, 255-byte names", E, VFS_ENVS)
	{
		E e;
		const std::string names[] = {
			"中文文件.txt", "日本語のファイル", "emoji_😀🎮.dat", "has space.txt", " leading", ".hidden",
			"a.b.c.d", "...x", std::string(255, 'n'), std::string(85, '\xE4') /* 占位，下面替换 */,
		};
		for (std::string n : names)
		{
			if (n[0] == '\xE4')
			{
				// 85 个三字节汉字 = 255 字节
				n.clear();
				for (int i = 0; i < 85; ++i)
					n += "字";
			}
			CAPTURE(n);
			const std::string path = "/w/names/" + n;
			REQUIRE(e.fs.createDirs("/w/names") == Error::OK);
			REQUIRE(e.fs.writeFile(path, n) == Error::OK);
			CHECK(readAll(e.fs, path) == n);
			Names listed = listNames(e.fs, "/w/names");
			CHECK(std::find(listed.begin(), listed.end(), n) != listed.end());
			// 长名字改成短名字，短名字加后缀（避免超过 255 字节的单段上限）
			const std::string renamed = n.size() > 200 ? std::string("/w/names/short") : path + "_r";
			CHECK(e.fs.rename(path, renamed) == Error::OK);
			CHECK(readAll(e.fs, renamed) == n);
			CHECK(e.fs.removeFile(renamed) == Error::OK);
		}
		CHECK(listNames(e.fs, "/w/names").empty());
	}

	TEST_CASE_TEMPLATE("names: trailing dot or space, colon, Windows reserved names (rejected by native, allowed by memory)", E, VFS_ENVS)
	{
		E e;
		for (const char* n : {"trail.", "trail ", "a:b", "CON", "nul.txt", "com1"})
		{
			CAPTURE(n);
			Error err = e.fs.writeFile(std::string("/w/") + n, "x");
			if (e.nativeTop)
				CHECK(err == Error::INVALID_PATH);
			else
			{
				CHECK(err == Error::OK);
				CHECK(readAll(e.fs, std::string("/w/") + n) == "x");
			}
		}
	}

	TEST_CASE_TEMPLATE("names: names differing only in case are different files (case-sensitive write layer)", E, VFS_ENVS)
	{
		E e;
		if (!e.caseSensitive())
			return;
		REQUIRE(e.fs.writeFile("/w/Case.txt", "upper") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/case.txt", "lower") == Error::OK);
		CHECK(readAll(e.fs, "/w/Case.txt") == "upper");
		CHECK(readAll(e.fs, "/w/case.txt") == "lower");
		CHECK_FALSE(e.fs.exists("/w/CASE.TXT"));
		CHECK(e.fs.removeFile("/w/case.txt") == Error::OK);
		CHECK(readAll(e.fs, "/w/Case.txt") == "upper");
		// 底层文件：内存、归档按字节匹配；本机底层在大小写不敏感的文件系统上（未开 exactCase）
		// 会匹配到只差大小写的名字，这是 docs/mount-rules.md 中记录的平台差异
		INFO("nativeBase=" << e.nativeBase << " nativeCaseSensitive=" << test::nativeCaseSensitive());
		CHECK(e.fs.exists("/w/BASE/a.txt") == !e.baseCaseSensitive());
		CHECK(e.fs.exists("/w/base/A.txt") == !e.baseCaseSensitive());
	}

	TEST_CASE_TEMPLATE("names: on a case-insensitive write layer, names differing only in case are the same file", E, VFS_ENVS)
	{
		E e;
		if (e.caseSensitive())
			return;
		// Windows / macOS 默认文件系统：这是平台能力，VFS 不做模拟（需要一致行为请开 NativeOptions::exactCase）
		REQUIRE(e.fs.writeFile("/w/Case.txt", "upper") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/case.txt", "lower") == Error::OK);
		CHECK(readAll(e.fs, "/w/Case.txt") == "lower");
		CHECK(e.fs.exists("/w/CASE.TXT"));
	}

	TEST_CASE_TEMPLATE("path variants: . .. repeated slashes, backslashes, trailing slash, no leading slash all reach the same file", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.writeFile("/w/p/q.txt", "q") == Error::NOT_FOUND);
		REQUIRE(e.fs.createDir("/w/p") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/p/q.txt", "q") == Error::OK);
		for (const char* v : {"/w/p/q.txt", "w/p/q.txt", "//w//p///q.txt", "\\w\\p\\q.txt", "/w/./p/./q.txt",
							  "/w/p/../p/q.txt", "/w/base/../p/q.txt", "/w/p/q.txt/", "/./w/p/q.txt"})
		{
			CAPTURE(v);
			CHECK(readAll(e.fs, v) == "q");
		}
		CHECK(e.fs.stat("/w/../../p", *std::make_unique<Stat>()) == Error::INVALID_PATH);
		CHECK(e.fs.writeFile("/../w/x", "x") == Error::INVALID_PATH);
		CHECK(e.fs.writeFile("/w/a\x01" "b", "x") == Error::INVALID_PATH);
		CHECK(e.fs.writeFile(std::string("/w/a\0b", 6), "x") == Error::INVALID_PATH);
		CHECK(e.fs.removeFile("/w/p/q.txt/") == Error::OK);
	}

	// ================================================================ 流

	TEST_CASE_TEMPLATE("streams: seek past EOF, zero-byte read/write, use after close, zero-filled holes", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.writeFile("/w/s.bin", "abc") == Error::OK);
		auto s = e.fs.openFileStream("/w/s.bin", Mode::OVERLAPPED);
		REQUIRE(s);
		char buf[8];
		CHECK(s->read(buf, 0) == 0);
		CHECK(s->write(buf, 0) == 0);
		CHECK(s->tell() == 0);
		CHECK(s->seek(100, SEEK_SET) == 100);
		CHECK(s->read(buf, sizeof(buf)) == 0);
		CHECK(s->seek(-1, SEEK_SET) == -1);
		CHECK(s->tell() == 100);
		CHECK(s->seek(6, SEEK_SET) == 6);
		CHECK(s->write("Z", 1) == 1);
		CHECK(s->size() == 7);
		CHECK(s->close() == 0);
		CHECK_FALSE(s->isOpen());
		errno = 0;
		CHECK(s->write("x", 1) == -1);
		CHECK(errno == EBADF);
		CHECK(s->read(buf, 1) == -1);
		CHECK(s->seek(0, SEEK_SET) == -1);
		CHECK(s->tell() == -1);
		CHECK(s->size() == -1);
		CHECK(readAll(e.fs, "/w/s.bin") == std::string("abc\0\0\0Z", 7));

		// 底层只读文件的读流：EOF 之后的 seek 与 0 字节读
		auto r = e.fs.openFileStream("/w/base/sub/b.txt", Mode::READ);
		REQUIRE(r);
		CHECK(r->seek(0, SEEK_END) == 1);
		CHECK(r->read(buf, 0) == 0);
		CHECK(r->seek(1000000, SEEK_CUR) == 1000001);
		CHECK(r->read(buf, 4) == 0);
		CHECK(r->write("x", 1) == -1);
	}

	TEST_CASE_TEMPLATE("streams: an open reader can finish after the file is removed", E, VFS_ENVS)
	{
		E e;
		const std::string data = blob(100000, 77);
		REQUIRE(e.fs.writeFile("/w/open.bin", data) == Error::OK);
		auto s = e.fs.openFileStream("/w/open.bin", Mode::READ);
		REQUIRE(s);
		REQUIRE(e.fs.removeFile("/w/open.bin") == Error::OK);
		CHECK_FALSE(e.fs.exists("/w/open.bin"));
		CHECK(test::str(test::readStream(*s)) == data);
		// 同名新文件与旧流无关
		REQUIRE(e.fs.writeFile("/w/open.bin", "new") == Error::OK);
		CHECK(readAll(e.fs, "/w/open.bin") == "new");
	}

	TEST_CASE_TEMPLATE("streams: 300 open streams (same and different files) with independent positions", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDir("/w/h") == Error::OK);
		for (int i = 0; i < 30; ++i)
			REQUIRE(e.fs.writeFile("/w/h/" + std::to_string(i), blob(64, i)) == Error::OK);
		std::vector<std::unique_ptr<IFileStream>> streams;
		for (int i = 0; i < 300; ++i)
		{
			auto s = e.fs.openFileStream("/w/h/" + std::to_string(i % 30), Mode::READ);
			REQUIRE(s);
			s->seek(i % 64, SEEK_SET);
			streams.push_back(std::move(s));
		}
		for (int i = 0; i < 300; ++i)
		{
			const std::string expected = blob(64, i % 30);
			char c = 0;
			REQUIRE(streams[i]->read(&c, 1) == 1);
			CHECK(c == expected[i % 64]);
		}
	}

	// ================================================================ 元数据

	TEST_CASE_TEMPLATE("stat and generation: type, size and writable flag follow operations", E, VFS_ENVS)
	{
		E e;
		const uint64_t g0 = e.fs.generation();
		Stat st;
		CHECK(e.fs.stat("/w/none", st) == Error::NOT_FOUND);
		REQUIRE(e.fs.writeFile("/w/st.txt", "12345") == Error::OK);
		REQUIRE(e.fs.stat("/w/st.txt", st) == Error::OK);
		CHECK(st.type == FileType::FILE);
		CHECK(st.size == 5);
		CHECK(st.writable);
		CHECK(st.mount != 0);
		CHECK(e.fs.generation() > g0);

		const uint64_t g1 = e.fs.generation();
		readAll(e.fs, "/w/st.txt");
		e.fs.list("/w");
		CHECK(e.fs.generation() == g1);   // 只读操作不改变
		REQUIRE(e.fs.createDir("/w/gd") == Error::OK);
		CHECK(e.fs.generation() > g1);

		REQUIRE(e.fs.stat("/w", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		CHECK(e.fs.fileSize("/w") == -1);
		CHECK(e.fs.fileSize("/w/none") == -1);
		REQUIRE(e.fs.stat("/", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
	}

	TEST_CASE_TEMPLATE("realPath: native write layers return the real path, other backends an empty string", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.writeFile("/w/real.txt", "r") == Error::OK);
		const std::string real = e.fs.realPath("/w/real.txt");
		if (e.nativeTop)
		{
			REQUIRE_FALSE(real.empty());
			CHECK(test::str(test::readNative(real)) == "r");
		}
		else
			CHECK(real.empty());
		CHECK(e.fs.realPath("/w/none").empty());
	}

	TEST_CASE_TEMPLATE("walk matches level-by-level list", E, VFS_ENVS)
	{
		E e;
		REQUIRE(e.fs.createDirs("/w/tree/a/b") == Error::OK);
		REQUIRE(e.fs.createDirs("/w/tree/c") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/tree/a/b/1", "1") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/tree/a/2", "2") == Error::OK);
		REQUIRE(e.fs.writeFile("/w/tree/3", "3") == Error::OK);
		std::vector<std::string> walked;
		e.fs.walk("/w", [&](const std::string& full, const DirEntry& d)
		{
			walked.push_back(full + (d.type == FileType::DIR ? "/" : ""));
			return true;
		});
		std::vector<std::string> listed;
		std::function<void(const std::string&)> rec = [&](const std::string& dir)
		{
			for (const auto& n : listNames(e.fs, dir))
			{
				listed.push_back(dir + "/" + n);
				if (n.back() == '/')
					rec(dir + "/" + n.substr(0, n.size() - 1));
			}
		};
		rec("/w");
		CHECK(walked == listed);
		CHECK(std::find(walked.begin(), walked.end(), "/w/base/sub/b.txt") != walked.end());
		CHECK(std::find(walked.begin(), walked.end(), "/w/tree/a/b/1") != walked.end());
	}
}
