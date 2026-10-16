// 内存后端测试
#include "TestUtils.h"
#include "vfs/memory/MemoryBackend.h"

using namespace vfs;
using Names = std::vector<std::string>;

TEST_SUITE("memory")
{
	TEST_CASE("addFile creates parent directories; overwriting does not affect open streams")
	{
		auto b = MemoryBackend::create();
		REQUIRE(b->addFile("a/b/c.txt", "v1") == Error::OK);
		Stat st;
		REQUIRE(b->stat("a/b", st) == Error::OK);
		CHECK(st.type == FileType::DIR);
		auto s = b->open("a/b/c.txt", IFileStream::Mode::READ, nullptr);
		REQUIRE(s);
		REQUIRE(b->addFile("a/b/c.txt", "version2") == Error::OK);
		CHECK(test::str(test::readStream(*s)) == "v1");
		REQUIRE(b->stat("a/b/c.txt", st) == Error::OK);
		CHECK(st.size == 8);
		CHECK(b->addFile("a/b/c.txt/x", "x") == Error::NOT_DIR);
		CHECK(b->addFile("a/b", "x") == Error::IS_DIR);
	}

	TEST_CASE("two write streams share the same data")
	{
		auto b = MemoryBackend::create();
		auto w = b->open("f", IFileStream::Mode::WRITE, nullptr);
		auto r = b->open("f", IFileStream::Mode::READ, nullptr);
		REQUIRE(w);
		REQUIRE(r);
		CHECK(w->write("abc", 3) == 3);
		CHECK(r->size() == 3);
		CHECK(test::str(test::readStream(*r)) == "abc");
	}

	TEST_CASE("rename rules (POSIX)")
	{
		auto b = MemoryBackend::create();
		b->addFile("d1/f", "f");
		b->addFile("d2/g", "g");
		b->addFile("x", "x");
		b->createDir("empty");
		CHECK(b->rename("x", "d1/f") == Error::OK);    // 文件覆盖文件
		CHECK(b->rename("d1", "d2") == Error::NOT_EMPTY);   // 目录覆盖非空目录
		CHECK(b->rename("d1", "empty") == Error::OK);  // 目录覆盖空目录
		CHECK(b->rename("empty", "empty/sub") == Error::INVALID_PATH);   // 移到自身之下
		CHECK(b->rename("d2/g", "empty") == Error::IS_DIR);
		CHECK(b->rename("empty", "d2/g") == Error::NOT_DIR);
		CHECK(b->rename("none", "y") == Error::NOT_FOUND);
		CHECK(b->rename("d2/g", "nodir/g") == Error::NOT_FOUND);
		Stat st;
		REQUIRE(b->stat("empty/f", st) == Error::OK);
		CHECK(st.size == 1);
	}

	TEST_CASE("readOnly: addFile can still prefill, but stream and directory operations are rejected")
	{
		MemoryOptions o;
		o.readOnly = true;
		auto b = MemoryBackend::create(o);
		CHECK(b->addFile("f", "x") == Error::OK);
		CHECK((b->capabilities() & BackendCaps::Write) == 0);
		Error err = Error::OK;
		CHECK(b->open("f", IFileStream::Mode::APPEND, &err) == nullptr);
		CHECK(err == Error::READ_ONLY);
		CHECK(b->open("f", IFileStream::Mode::READ, nullptr) != nullptr);
		CHECK(b->createDir("d") == Error::READ_ONLY);
		CHECK(b->removeFile("f") == Error::READ_ONLY);
	}

	TEST_CASE("list is sorted by bytes; the root cannot be removed")
	{
		auto b = MemoryBackend::create();
		b->addFile("b", "");
		b->addFile("A", "");
		b->addFile("a/x", "");
		Names names;
		b->list("", [&names](const DirEntry& e)
		{
			names.push_back(e.name);
			return true;
		});
		CHECK(names == Names{"A", "a", "b"});
		CHECK(b->removeDir("") == Error::BUSY);
		CHECK(b->removeDir("a") == Error::NOT_EMPTY);
		CHECK(b->list("b", [](const DirEntry&) { return true; }) == Error::NOT_DIR);
	}
}
