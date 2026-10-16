// vfstool 核心逻辑测试：glob 规则与各子命令（直接调用 run* 函数，不经过进程）
#include "TestUtils.h"
#include "core/Commands.h"
#include "core/Glob.h"
#include <sstream>

using namespace vfs;
using namespace vfs::tool;

namespace
{

// 构造一个源目录
void makeSource(const test::TempDir& dir)
{
	dir.write("src/main.lua", "print('hello')\n");
	dir.write("src/lib/util.lua", std::string(2000, 'u'));
	dir.write("src/res/bg.bin", test::randomBytes(5000, 1));
	dir.write("src/res/sub/icon.bin", test::randomBytes(300, 2));
	dir.write("src/res/copy.bin", test::randomBytes(5000, 1));   // 与 bg.bin 相同，触发去重
	dir.write("src/文档/说明.txt", "中文内容");
	dir.write("src/.git/HEAD", "ref: refs/heads/main");
	dir.write("src/.DS_Store", "junk");
	dir.write("src/notes.tmp", "tmp");
	dir.mkdir("src/emptydir");
}

ArchiveArgs archiveArgs(const std::string& path)
{
	ArchiveArgs a;
	a.archive = path;
	a.key.useEnv = false;
	return a;
}

PackOptions packOptions(const test::TempDir& dir, const std::string& out)
{
	PackOptions o;
	o.inputDir = dir.sub("src");
	o.outputPath = dir.sub(out);
	o.key.useEnv = false;
	return o;
}

std::string listArchive(const std::string& path)
{
	ListOptions lo;
	lo.archive = archiveArgs(path);
	std::ostringstream out, err;
	REQUIRE(runList(lo, out, err) == 0);
	return out.str();
}

} // namespace

TEST_SUITE("tool")
{
	TEST_CASE("Glob::match: * ? ** [] and escapes")
	{
		CHECK(Glob::match("*.lua", "main.lua"));
		CHECK(Glob::match("*.lua", "a/b/main.lua"));      // 不含 '/' 的模式匹配文件名
		CHECK_FALSE(Glob::match("*.lua", "main.luac"));
		CHECK(Glob::match("a/*.lua", "a/x.lua"));
		CHECK_FALSE(Glob::match("a/*.lua", "a/b/x.lua")); // * 不跨目录
		CHECK(Glob::match("a/**/x.lua", "a/x.lua"));      // **/ 可以匹配零层
		CHECK(Glob::match("a/**/x.lua", "a/b/c/x.lua"));
		CHECK(Glob::match("**/*.bin", "res/sub/icon.bin"));
		CHECK(Glob::match("**/*.bin", "top.bin"));
		CHECK(Glob::match("res/**", "res/a/b"));
		CHECK_FALSE(Glob::match("res/**", "resx/a"));
		CHECK(Glob::match("file?.txt", "file1.txt"));
		CHECK_FALSE(Glob::match("file?.txt", "file12.txt"));
		CHECK(Glob::match("[abc].txt", "b.txt"));
		CHECK_FALSE(Glob::match("[abc].txt", "d.txt"));
		CHECK(Glob::match("[a-c]x", "bx"));
		CHECK(Glob::match("[!a-c]x", "dx"));
		CHECK(Glob::match("\\*.txt", "*.txt"));
		CHECK_FALSE(Glob::match("\\*.txt", "a.txt"));
		CHECK(Glob::match("/top.txt", "top.txt"));        // 前导 '/' 锚定到根
		CHECK_FALSE(Glob::match("/top.txt", "a/top.txt"));
		CHECK(Glob::match("中文/*.txt", "中文/说明.txt"));
	}

	TEST_CASE("Glob::evaluate: the last matching rule wins")
	{
		std::vector<Rule> rules = {{"**/*.bin", true}, {"res/sub/**", false}};
		bool matched = false;
		CHECK(Glob::evaluate(rules, "res/bg.bin", false, &matched));
		CHECK(matched);
		CHECK_FALSE(Glob::evaluate(rules, "res/sub/icon.bin", true));
		CHECK(Glob::evaluate(rules, "x.lua", true, &matched));
		CHECK_FALSE(matched);
		CHECK_FALSE(Glob::evaluate(rules, "x.lua", false));
	}

	TEST_CASE("hex key parsing")
	{
		std::vector<uint8_t> key;
		CHECK(parseHexKey(std::string(64, 'a'), key));
		CHECK(key.size() == 32);
		CHECK(key[0] == 0xaa);
		CHECK(parseHexKey(std::string(64, 'F') + "\n", key));
		CHECK_FALSE(parseHexKey(std::string(63, 'a'), key));
		CHECK_FALSE(parseHexKey(std::string(64, 'g'), key));
		CHECK(toHex(key.data(), 2) == "ffff");
	}

	TEST_CASE("pack → list → cat → verify → unpack round trip; default excludes and empty-directory rules")
	{
		test::TempDir dir;
		makeSource(dir);
		PackOptions po = packOptions(dir, "out.vfs");
		po.encryptRules = {{"**/*.bin", true}};
		po.compressRules = {{"*.lua", true}};
		po.filterRules = {{"*.tmp", false}};
		std::ostringstream out, err;
		REQUIRE(runPack(po, out, err) == 0);

		const std::string listing = listArchive(dir.sub("out.vfs"));
		CHECK(listing.find("main.lua") != std::string::npos);
		CHECK(listing.find("文档/说明.txt") != std::string::npos);
		CHECK(listing.find(".git") == std::string::npos);
		CHECK(listing.find(".DS_Store") == std::string::npos);
		CHECK(listing.find("notes.tmp") == std::string::npos);
		CHECK(listing.find("emptydir") == std::string::npos);

		// cat
		CatOptions co;
		co.archive = archiveArgs(dir.sub("out.vfs"));
		co.path = "res/bg.bin";
		std::ostringstream catOut(std::ios::binary), catErr;
		REQUIRE(runCat(co, catOut, catErr) == 0);
		CHECK(catOut.str() == test::str(test::randomBytes(5000, 1)));
		co.path = "missing";
		std::ostringstream o2, e2;
		CHECK(runCat(co, o2, e2) == static_cast<int>(ExitCode::IO));

		// verify
		VerifyOptions vo;
		vo.archive = archiveArgs(dir.sub("out.vfs"));
		std::ostringstream vout, verr;
		CHECK(runVerify(vo, vout, verr) == 0);

		// unpack
		UnpackOptions uo;
		uo.archive = archiveArgs(dir.sub("out.vfs"));
		uo.outputDir = dir.sub("unpacked");
		std::ostringstream uout, uerr;
		REQUIRE(runUnpack(uo, uout, uerr) == 0);
		CHECK(dir.read("unpacked/main.lua") == dir.read("src/main.lua"));
		CHECK(dir.read("unpacked/lib/util.lua") == dir.read("src/lib/util.lua"));
		CHECK(dir.read("unpacked/res/sub/icon.bin") == dir.read("src/res/sub/icon.bin"));
		CHECK(dir.read("unpacked/文档/说明.txt") == "中文内容");
		CHECK_FALSE(dir.exists("unpacked/.git"));

		// 已存在的文件：不加 --force 时失败
		std::ostringstream uout2, uerr2;
		CHECK(runUnpack(uo, uout2, uerr2) != 0);
		uo.force = true;
		std::ostringstream uout3, uerr3;
		CHECK(runUnpack(uo, uout3, uerr3) == 0);
	}

	TEST_CASE("pack: two runs produce byte-identical output; --empty-dirs, --prefix, --include")
	{
		test::TempDir dir;
		makeSource(dir);
		PackOptions po = packOptions(dir, "1.vfs");
		po.encryptAll = true;
		po.compressAll = true;
		std::ostringstream o, e;
		REQUIRE(runPack(po, o, e) == 0);
		po.outputPath = dir.sub("2.vfs");
		REQUIRE(runPack(po, o, e) == 0);
		CHECK(test::readNative(dir.sub("1.vfs")) == test::readNative(dir.sub("2.vfs")));

		PackOptions p2 = packOptions(dir, "3.vfs");
		p2.emptyDirs = true;
		p2.prefix = "game/data";
		REQUIRE(runPack(p2, o, e) == 0);
		const std::string l3 = listArchive(dir.sub("3.vfs"));
		CHECK(l3.find("game/data/emptydir") != std::string::npos);
		CHECK(l3.find("game/data/main.lua") != std::string::npos);

		PackOptions p3 = packOptions(dir, "4.vfs");
		p3.filterRules = {{"**/*.lua", true}};
		REQUIRE(runPack(p3, o, e) == 0);
		const std::string l4 = listArchive(dir.sub("4.vfs"));
		CHECK(l4.find("main.lua") != std::string::npos);
		CHECK(l4.find("lib/util.lua") != std::string::npos);
		CHECK(l4.find("bg.bin") == std::string::npos);

		// --dry-run 不生成文件
		PackOptions p4 = packOptions(dir, "5.vfs");
		p4.dryRun = true;
		std::ostringstream dout, derr;
		REQUIRE(runPack(p4, dout, derr) == 0);
		CHECK_FALSE(dir.exists("5.vfs"));
		CHECK(dout.str().find("main.lua") != std::string::npos);
	}

	TEST_CASE("pack/info/verify: external keys and error codes")
	{
		test::TempDir dir;
		makeSource(dir);
		const std::string hex(64, '7');
		PackOptions po = packOptions(dir, "k.vfs");
		po.key.keyHex = hex;
		std::ostringstream o, e;
		REQUIRE(runPack(po, o, e) == 0);

		InfoOptions io;
		io.archive = archiveArgs(dir.sub("k.vfs"));
		std::ostringstream iout, ierr;
		CHECK(runInfo(io, iout, ierr) == static_cast<int>(ExitCode::KEY));
		io.archive.key.keyHex = std::string(64, '8');
		std::ostringstream iout2, ierr2;
		CHECK(runInfo(io, iout2, ierr2) == static_cast<int>(ExitCode::KEY));
		io.archive.key.keyHex = hex;
		std::ostringstream iout3, ierr3;
		CHECK(runInfo(io, iout3, ierr3) == 0);
		CHECK(iout3.str().find("1") != std::string::npos);

		// 密钥文件
		dir.write("key.txt", hex + "\n");
		io.archive.key.keyHex.clear();
		io.archive.key.keyFile = dir.sub("key.txt");
		io.json = true;
		std::ostringstream iout4, ierr4;
		CHECK(runInfo(io, iout4, ierr4) == 0);
		CHECK(iout4.str().find('{') != std::string::npos);

		// 损坏
		auto data = test::readNative(dir.sub("k.vfs"));
		data[200] ^= 0xFF;
		test::writeNative(dir.sub("bad.vfs"), data);
		VerifyOptions vo;
		vo.archive = archiveArgs(dir.sub("bad.vfs"));
		vo.archive.key.keyHex = hex;
		std::ostringstream vout, verr;
		int rc = runVerify(vo, vout, verr);
		CHECK((rc == static_cast<int>(ExitCode::INTEGRITY) || rc == static_cast<int>(ExitCode::FORMAT)));

		// 不是归档
		dir.write("not.vfs", "hello");
		InfoOptions bad;
		bad.archive = archiveArgs(dir.sub("not.vfs"));
		std::ostringstream bout, berr;
		CHECK(runInfo(bad, bout, berr) == static_cast<int>(ExitCode::FORMAT));
		bad.archive = archiveArgs(dir.sub("missing.vfs"));
		CHECK(runInfo(bad, bout, berr) == static_cast<int>(ExitCode::IO));
	}

	TEST_CASE("extract: files and directories")
	{
		test::TempDir dir;
		makeSource(dir);
		PackOptions po = packOptions(dir, "x.vfs");
		std::ostringstream o, e;
		REQUIRE(runPack(po, o, e) == 0);
		ExtractOptions xo;
		xo.archive = archiveArgs(dir.sub("x.vfs"));
		xo.paths = {"main.lua", "res/sub"};
		xo.outputDir = dir.sub("ex");
		REQUIRE(runExtract(xo, o, e) == 0);
		CHECK(dir.exists("ex/main.lua"));
		CHECK(dir.exists("ex/res/sub/icon.bin"));
		CHECK_FALSE(dir.exists("ex/res/bg.bin"));
		xo.paths = {"nope"};
		xo.force = true;
		CHECK(runExtract(xo, o, e) != 0);
	}

	TEST_CASE("keygen: produces 64 hex characters")
	{
		KeygenOptions ko;
		std::ostringstream o, e;
		REQUIRE(runKeygen(ko, o, e) == 0);
		std::string s = o.str();
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		std::vector<uint8_t> key;
		CHECK(parseHexKey(s, key));
	}
}
