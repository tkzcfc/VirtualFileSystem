// 本文件：测试公共工具（临时目录、本机文件读写、归档构建、确定性随机数据）。
#pragma once

#include "doctest.h"
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveWriter.h"
#include <filesystem>
#include <string>
#include <vector>

// 让 doctest 在断言失败时打印错误码名字
namespace doctest
{
template <>
struct StringMaker<vfs::Error>
{
	static String convert(vfs::Error e) { return vfs::errorName(e); }
};
} // namespace doctest

namespace test
{

/**
 * 临时目录，析构时递归删除。
 */
class TempDir
{
public:
	TempDir();

	~TempDir();

	TempDir(const TempDir&) = delete;

	TempDir& operator=(const TempDir&) = delete;

	// 目录本身的 UTF-8 路径（'/' 分隔）
	const std::string& path() const { return m_path; }

	// 子路径
	std::string sub(std::string_view rel) const;

	// 写文件（自动创建父目录）
	void write(std::string_view rel, std::string_view content) const;

	void write(std::string_view rel, const std::vector<uint8_t>& content) const;

	// 创建目录
	void mkdir(std::string_view rel) const;

	bool exists(std::string_view rel) const;

	std::string read(std::string_view rel) const;

private:
	std::string m_path;
};

// UTF-8 字符串转 std::filesystem::path
std::filesystem::path u8(std::string_view s);

// 读写整个本机文件
std::vector<uint8_t> readNative(const std::string& path);

void writeNative(const std::string& path, const std::vector<uint8_t>& data);

// 确定性伪随机数据（xorshift），保证每次测试数据相同
std::vector<uint8_t> randomBytes(size_t size, uint64_t seed);

// 字符串转字节
std::vector<uint8_t> bytes(std::string_view s);

std::string str(const std::vector<uint8_t>& v);

// 通过 VFS 读出整个文件为字符串；失败返回 "<ERR:NAME>"
std::string readAll(const vfs::VirtualFileSystem& fs, std::string_view path);

// 通过流读出全部剩余内容
std::vector<uint8_t> readStream(vfs::IFileStream& stream);

// 列目录，返回 "name" 或 "name/"（目录）组成的列表
std::vector<std::string> listNames(const vfs::VirtualFileSystem& fs, std::string_view path);

/**
 * 构建一个归档文件：files 中每一项为 (路径, 内容)，options 指定每文件的加密/压缩。
 */
struct ArchiveFile
{
	std::string path;
	std::vector<uint8_t> content;
	vfs::FileWriteOptions options;
};

/**
 * 慢测试开关：环境变量 VFS_TEST_SLOW=1 时返回 true。
 * 慢测试统一写成 TEST_CASE("..." * doctest::skip(!test::slowEnabled()))，默认运行时跳过。
 */
bool slowEnabled();

/**
 * 测试规模系数：环境变量 VFS_TEST_SCALE（默认 1）。压力测试的轮数按它放大。
 */
int scale();

vfs::Error buildArchive(const std::string& outPath, const std::vector<ArchiveFile>& files,
						const vfs::ArchiveWriteOptions& options = {}, vfs::ArchiveWriteStats* stats = nullptr);

} // namespace test
