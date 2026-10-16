// 本文件：测试公共工具（临时目录、本机文件读写、归档构建、确定性随机数据）。
#pragma once

#include "doctest.h"
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveWriter.h"
#include <filesystem>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

// 让 doctest 在断言失败时打印错误码名字。
// 注意：doctest 对枚举优先按底层整数打印（不会用 StringMaker 特化），必须提供 operator<<
namespace vfs
{
inline std::ostream& operator<<(std::ostream& os, Error e)
{
	return os << errorName(e);
}
} // namespace vfs

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
 * 运行时探测本机临时目录所在文件系统是否区分大小写（Linux 通常是，Windows/macOS 默认不是）。
 * 结果缓存，只探测一次。
 */
bool nativeCaseSensitive();

/**
 * 运行时探测：本机文件系统能否替换（rename 覆盖）一个正被打开的文件。
 * POSIX 总是可以；Windows 需要 POSIX 语义的 FileRenameInfoEx（Windows 10 1709+ 的 NTFS），
 * FAT/exFAT、部分网络盘、旧系统和 Wine 不支持，此时替换会在其他句柄关闭前返回 PERMISSION_DENIED。
 * 结果缓存，只探测一次。
 */
bool nativeReplaceOpenFile();

/**
 * 当前线程最近一次本机操作的原始错误码，形如 " (os=2)"；没有时为空串。用于失败详情
 */
std::string osErrorSuffix();

/**
 * 线程安全的错误码直方图：并发测试记录每种返回码出现的次数，失败时整体打印，方便跨平台诊断。
 */
class ErrorHistogram
{
public:
	void add(vfs::Error e)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		++m_counts[e];
	}

	size_t count(vfs::Error e) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_counts.find(e);
		return it == m_counts.end() ? 0 : it->second;
	}

	// 形如 "OK=120 NOT_FOUND=3 PERMISSION_DENIED=1"
	std::string str() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::string s;
		for (const auto& [e, n] : m_counts)
		{
			if (!s.empty())
				s.push_back(' ');
			s += vfs::errorName(e);
			s += '=';
			s += std::to_string(n);
		}
		return s.empty() ? "<empty>" : s;
	}

private:
	mutable std::mutex m_mutex;
	std::map<vfs::Error, size_t> m_counts;
};

/**
 * 线程安全的失败记录：计数，并保留前几条带上下文（实际错误码、路径）的描述，断言失败时打印。
 * ++failures 仍可用（不带详情）。
 */
class FailureLog
{
public:
	static constexpr size_t MAX_DETAILS = 8;

	void add(const std::string& what)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		++m_count;
		if (m_details.size() < MAX_DETAILS)
			m_details.push_back(what);
	}

	FailureLog& operator++()
	{
		add("<no details>");
		return *this;
	}

	// err 不是 OK 时记录 "what -> 错误码名"
	void expectOk(vfs::Error err, const std::string& what)
	{
		if (err != vfs::Error::OK)
			add(what + " -> " + vfs::errorName(err) + osErrorSuffix());
	}

	// 内容不一致时记录长度与开头
	void expectEq(const std::string& got, const std::string& want, const std::string& what)
	{
		if (got != want)
			add(what + " -> got " + std::to_string(got.size()) + " bytes \"" + got.substr(0, 40) + "\", expected " +
				std::to_string(want.size()) + " bytes \"" + want.substr(0, 40) + "\"");
	}

	size_t load() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_count;
	}

	std::string str() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::string s = std::to_string(m_count) + " failure(s)";
		for (const auto& d : m_details)
			s += "\n  " + d;
		if (m_count > m_details.size())
			s += "\n  ...";
		return s;
	}

private:
	mutable std::mutex m_mutex;
	size_t m_count = 0;
	std::vector<std::string> m_details;
};

/**
 * 测试规模系数：环境变量 VFS_TEST_SCALE（默认 1）。压力测试的轮数按它放大。
 */
int scale();

vfs::Error buildArchive(const std::string& outPath, const std::vector<ArchiveFile>& files,
						const vfs::ArchiveWriteOptions& options = {}, vfs::ArchiveWriteStats* stats = nullptr);

} // namespace test
