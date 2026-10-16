// 路径规范化测试（决策 #11、#12）
#include "TestUtils.h"
#include "vfs/Path.h"

using namespace vfs;

namespace
{

std::string norm(std::string_view in)
{
	std::string out;
	Error e = Path::normalize(in, out);
	return e == Error::OK ? out : std::string("<") + errorName(e) + ">";
}

} // namespace

TEST_SUITE("path")
{
	TEST_CASE("normalize：基本形式")
	{
		CHECK(norm("") == "/");
		CHECK(norm("/") == "/");
		CHECK(norm("///") == "/");
		CHECK(norm("/a") == "/a");
		CHECK(norm("/a/") == "/a");
		CHECK(norm("a/b") == "/a/b");      // 没有前导 '/' 按相对根处理
		CHECK(norm("//a//b//") == "/a/b"); // 合并重复的 '/'
		CHECK(norm("/a/./b/.") == "/a/b"); // 去掉 '.'
	}

	TEST_CASE("normalize：反斜杠等价于斜杠")
	{
		CHECK(norm("\\a\\b") == "/a/b");
		CHECK(norm("a\\b/c\\") == "/a/b/c");
	}

	TEST_CASE("normalize：按字面消解 ..，越过根报错")
	{
		CHECK(norm("/a/b/../c") == "/a/c");
		CHECK(norm("/a/../..") == "<INVALID_PATH>");
		CHECK(norm("/..") == "<INVALID_PATH>");
		CHECK(norm("../a") == "<INVALID_PATH>");
		CHECK(norm("/a/b/../../c/./d/..") == "/c");
		CHECK(norm("/a/..") == "/");
		// "..." 是普通名字
		CHECK(norm("/.../x") == "/.../x");
	}

	TEST_CASE("normalize：拒绝 NUL 与控制字符")
	{
		CHECK(norm(std::string_view("/a\0b", 4)) == "<INVALID_PATH>");
		CHECK(norm("/a\nb") == "<INVALID_PATH>");
		CHECK(norm("/a\tb") == "<INVALID_PATH>");
		CHECK(norm("/a\x7f") == "<INVALID_PATH>");
	}

	TEST_CASE("normalize：UTF-8 原样保留、大小写敏感")
	{
		CHECK(norm("/资源/图片/../配置.json") == "/资源/配置.json");
		CHECK(norm("/日本語/한국어/emoji😀.txt") == "/日本語/한국어/emoji😀.txt");
		CHECK(norm("/A/b") != norm("/a/b"));
	}

	TEST_CASE("normalizeRelative 与 isNormalizedRelative")
	{
		std::string out;
		CHECK(Path::normalizeRelative("/a/b/", out) == Error::OK);
		CHECK(out == "a/b");
		CHECK(Path::normalizeRelative("/", out) == Error::OK);
		CHECK(out.empty());

		CHECK(Path::isNormalizedRelative("a/b"));
		CHECK(Path::isNormalizedRelative("中文/文件.txt"));
		CHECK_FALSE(Path::isNormalizedRelative(""));
		CHECK_FALSE(Path::isNormalizedRelative("/a"));
		CHECK_FALSE(Path::isNormalizedRelative("a/"));
		CHECK_FALSE(Path::isNormalizedRelative("a//b"));
		CHECK_FALSE(Path::isNormalizedRelative("a/./b"));
		CHECK_FALSE(Path::isNormalizedRelative("a/../b"));
		CHECK_FALSE(Path::isNormalizedRelative(".."));
		CHECK_FALSE(Path::isNormalizedRelative("a\\b"));
	}

	TEST_CASE("isUnder：按路径段比较")
	{
		std::string_view rel;
		CHECK(Path::isUnder("/a/b", "/", rel));
		CHECK(rel == "a/b");
		CHECK(Path::isUnder("/", "/", rel));
		CHECK(rel.empty());
		CHECK(Path::isUnder("/a/b", "/a", rel));
		CHECK(rel == "b");
		CHECK(Path::isUnder("/a", "/a", rel));
		CHECK(rel.empty());
		CHECK_FALSE(Path::isUnder("/ab", "/a", rel));
		CHECK_FALSE(Path::isUnder("/a", "/a/b", rel));

		CHECK(Path::isStrictAncestor("/", "/a"));
		CHECK(Path::isStrictAncestor("/a", "/a/b/c"));
		CHECK_FALSE(Path::isStrictAncestor("/a", "/a"));
		CHECK_FALSE(Path::isStrictAncestor("/a", "/ab"));
	}

	TEST_CASE("parent / fileName / join")
	{
		CHECK(Path::parent("/a/b") == "/a");
		CHECK(Path::parent("/a") == "/");
		CHECK(Path::parent("/") == "/");
		CHECK(Path::fileName("/a/b") == "b");
		CHECK(Path::fileName("/") == "");
		CHECK(Path::fileName("a/b.txt") == "b.txt");
		CHECK(Path::join("/", "a") == "/a");
		CHECK(Path::join("/x", "a") == "/x/a");
		CHECK(Path::joinRelative("", "a") == "a");
		CHECK(Path::joinRelative("x", "a") == "x/a");
		CHECK(Path::parentRelative("a/b") == "a");
		CHECK(Path::parentRelative("a") == "");
	}

	TEST_CASE("isPortableComponent：Windows 保留名与非法字符")
	{
		CHECK(Path::isPortableComponent("file.txt"));
		CHECK(Path::isPortableComponent("console"));
		CHECK(Path::isPortableComponent("COM10"));
		CHECK(Path::isPortableComponent("中文"));
		CHECK_FALSE(Path::isPortableComponent("CON"));
		CHECK_FALSE(Path::isPortableComponent("con"));
		CHECK_FALSE(Path::isPortableComponent("nul.txt"));
		CHECK_FALSE(Path::isPortableComponent("Aux"));
		CHECK_FALSE(Path::isPortableComponent("com1"));
		CHECK_FALSE(Path::isPortableComponent("LPT9.log"));
		CHECK_FALSE(Path::isPortableComponent("a:b"));
		CHECK_FALSE(Path::isPortableComponent("trailing."));
		CHECK_FALSE(Path::isPortableComponent("trailing "));
		CHECK_FALSE(Path::isPortableComponent(""));
	}
}
