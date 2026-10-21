// 本文件：vfs_bench 性能基准。对比 std::fstream、C FILE*、POSIX pread / Win32 ReadFile 与各种 VFS 后端和分层组合，
// 覆盖小 / 中 / 大文件读取、随机读、元数据（open / stat / 不存在 / 列目录）与写入（并行写、创建删除抖动）。
// 不依赖任何第三方库；用法见 docs/benchmark.md 或 vfs_bench --help。
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveWriter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if VFS_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#if VFS_PLATFORM_APPLE
#include <sys/sysctl.h>
#endif
#endif

USING_NS_VFS;
namespace stdfs = std::filesystem;

namespace
{

using Clock = std::chrono::steady_clock;

constexpr size_t KiB = 1024;
constexpr size_t MiB = 1024 * 1024;

// ================================================================ 命令行选项

struct Options
{
	double scale = 1.0;                     // 数据规模倍数（文件数量与大文件大小）
	std::vector<int> threads;               // 线程数列表
	int iterations = 3;                     // 每个场景重复次数，取耗时中位数的那一次
	std::vector<std::string> filters;       // 场景过滤（子串匹配 "场景/目标"，任意一个命中即运行）
	std::string csvPath;                    // 可选 CSV 输出
	std::string mdPath;                     // 可选 Markdown 输出
	std::string workDir;                    // 工作目录（默认系统临时目录下）
	bool keep = false;                      // 结束后保留工作目录
	bool cold = false;                      // 每次迭代前尽量清除页缓存（仅 Linux 有效）
	bool list = false;                      // 只列出场景
};

void printHelp()
{
	std::cout <<
		"Usage: vfs_bench [options]\n"
		"  --scale <x>        dataset size multiplier, default 1 (10000 small files x 4KiB, 200 medium files x 256KiB, one 64MiB large file)\n"
		"  --threads <list>   comma-separated thread counts, N = hardware threads, default 1,2,4,8,N\n"
		"  --iterations <n>   repetitions per scenario (median is reported), default 3\n"
		"  --filter <list>    only run cases whose \"scenario/target\" contains any of the substrings (e.g. read.small or archive-lz4)\n"
		"  --csv <file>       also write CSV\n"
		"  --md <file>        also write Markdown tables\n"
		"  --dir <path>       working directory for the generated dataset, default: system temp dir\n"
		"  --keep             keep the working directory afterwards\n"
		"  --cold             drop the page cache of data files before each iteration (posix_fadvise, Linux only; default: warm cache)\n"
		"  --list             list all scenarios and targets, then exit\n"
		"  -h, --help         show this help\n";
}

int hardwareThreads()
{
	unsigned n = std::thread::hardware_concurrency();
	return n == 0 ? 4 : static_cast<int>(n);
}

bool parseThreads(const std::string& text, std::vector<int>& out)
{
	out.clear();
	std::stringstream ss(text);
	std::string item;
	while (std::getline(ss, item, ','))
	{
		int n = 0;
		if (item == "N" || item == "n")
			n = hardwareThreads();
		else
			n = std::atoi(item.c_str());
		if (n <= 0 || n > 1024)
			return false;
		if (std::find(out.begin(), out.end(), n) == out.end())
			out.push_back(n);
	}
	return !out.empty();
}

std::vector<std::string> splitList(const std::string& text)
{
	std::vector<std::string> out;
	std::stringstream ss(text);
	std::string item;
	while (std::getline(ss, item, ','))
	{
		if (!item.empty())
			out.push_back(item);
	}
	return out;
}

bool parseArgs(int argc, char** argv, Options& opt)
{
	parseThreads("1,2,4,8,N", opt.threads);
	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		auto next = [&](std::string& v) -> bool
		{
			if (i + 1 >= argc)
			{
				std::cerr << "option " << a << " requires an argument\n";
				return false;
			}
			v = argv[++i];
			return true;
		};
		std::string v;
		if (a == "-h" || a == "--help")
		{
			printHelp();
			std::exit(0);
		}
		else if (a == "--scale")
		{
			if (!next(v))
				return false;
			opt.scale = std::atof(v.c_str());
			if (opt.scale <= 0)
			{
				std::cerr << "--scale must be greater than 0\n";
				return false;
			}
		}
		else if (a == "--threads")
		{
			if (!next(v) || !parseThreads(v, opt.threads))
			{
				std::cerr << "invalid --threads value\n";
				return false;
			}
		}
		else if (a == "--iterations")
		{
			if (!next(v))
				return false;
			opt.iterations = std::max(1, std::atoi(v.c_str()));
		}
		else if (a == "--filter")
		{
			if (!next(v))
				return false;
			for (auto& f : splitList(v))
				opt.filters.push_back(f);
		}
		else if (a == "--csv")
		{
			if (!next(opt.csvPath))
				return false;
		}
		else if (a == "--md")
		{
			if (!next(opt.mdPath))
				return false;
		}
		else if (a == "--dir")
		{
			if (!next(opt.workDir))
				return false;
		}
		else if (a == "--keep")
			opt.keep = true;
		else if (a == "--cold")
			opt.cold = true;
		else if (a == "--list")
			opt.list = true;
		else
		{
			std::cerr << "unknown option: " << a << "\n";
			printHelp();
			return false;
		}
	}
	return true;
}

// ================================================================ 数据集

struct Dataset
{
	std::string root;                  // 本机数据目录
	std::vector<std::string> small;    // 相对路径（small/ 下，单个大目录）
	std::vector<std::string> medium;   // medium/ 下
	std::string large;                 // 大文件相对路径
	size_t smallSize = 4 * KiB;
	size_t mediumSize = 256 * KiB;
	size_t largeSize = 64 * MiB;
	std::string archivePlain, archiveXor, archiveLz4;   // 三个归档的本机路径
	std::vector<std::string> allNativeFiles;            // 用于 --cold 清缓存
};

// 生成可压缩的伪文本（LZ4 大约能压到 1/2 ~ 1/3），内容由种子确定
std::vector<uint8_t> makeContent(size_t size, uint64_t seed)
{
	static const char* const WORDS[] = {"vfs ", "archive ", "mount ", "layer ", "stream ", "read ", "write ", "native ",
										"memory ", "copy-up ", "index ", "entry ", "\n", "0123 ", "data ", "path "};
	std::vector<uint8_t> out;
	out.reserve(size);
	uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
	while (out.size() < size)
	{
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		// 大约 1/8 的概率插入一个随机字节，避免压缩比过高
		if ((x & 7) == 0)
			out.push_back(static_cast<uint8_t>(x >> 24));
		const char* w = WORDS[(x >> 8) & 15];
		for (const char* p = w; *p && out.size() < size; ++p)
			out.push_back(static_cast<uint8_t>(*p));
	}
	return out;
}

bool writeNative(const std::string& path, const std::vector<uint8_t>& data)
{
	std::ofstream f(stdfs::path(path), std::ios::binary | std::ios::trunc);
	f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	return static_cast<bool>(f);
}

std::string fileName(const char* prefix, size_t i)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%s%06zu.bin", prefix, i);
	return buf;
}

bool buildArchive(const Dataset& ds, const std::string& out, bool encrypt, bool compress)
{
	ArchiveWriter writer;
	FileWriteOptions fo;
	fo.encrypt = encrypt;
	fo.compress = compress;
	auto add = [&](const std::string& rel) { return writer.addFile(rel, ds.root + "/" + rel, fo) == Error::OK; };
	for (auto& r : ds.small)
		if (!add(r))
			return false;
	for (auto& r : ds.medium)
		if (!add(r))
			return false;
	if (!add(ds.large))
		return false;
	Error e = writer.write(out);
	if (e != Error::OK)
		std::cerr << "packing failed: " << out << ": " << errorName(e) << " " << writer.lastError() << "\n";
	return e == Error::OK;
}

bool buildDataset(const Options& opt, Dataset& ds)
{
	const size_t smallCount = std::max<size_t>(100, static_cast<size_t>(10000 * opt.scale));
	const size_t mediumCount = std::max<size_t>(10, static_cast<size_t>(200 * opt.scale));
	ds.largeSize = std::max<size_t>(4 * MiB, static_cast<size_t>(64.0 * opt.scale) * MiB);

	ds.root = opt.workDir + "/data";
	std::error_code ec;
	stdfs::create_directories(stdfs::path(ds.root) / "small", ec);
	stdfs::create_directories(stdfs::path(ds.root) / "medium", ec);
	if (ec)
	{
		std::cerr << "cannot create working directory " << ds.root << ": " << ec.message() << "\n";
		return false;
	}
	for (size_t i = 0; i < smallCount; ++i)
		ds.small.push_back("small/" + fileName("f", i));
	for (size_t i = 0; i < mediumCount; ++i)
		ds.medium.push_back("medium/" + fileName("m", i));
	ds.large = "large.bin";

	// 小文件内容全部取自同一个块的不同片段，生成快；归档去重关闭与否不影响（片段各不相同）
	const std::vector<uint8_t> pool = makeContent(ds.smallSize * 64 + 4096, 1);
	for (size_t i = 0; i < smallCount; ++i)
	{
		const size_t off = (i * 4099) % (pool.size() - ds.smallSize);
		std::vector<uint8_t> data(pool.begin() + static_cast<std::ptrdiff_t>(off),
								  pool.begin() + static_cast<std::ptrdiff_t>(off + ds.smallSize));
		data[0] = static_cast<uint8_t>(i);
		data[1] = static_cast<uint8_t>(i >> 8);
		data[2] = static_cast<uint8_t>(i >> 16);
		if (!writeNative(ds.root + "/" + ds.small[i], data))
			return false;
	}
	for (size_t i = 0; i < mediumCount; ++i)
	{
		if (!writeNative(ds.root + "/" + ds.medium[i], makeContent(ds.mediumSize, 100 + i)))
			return false;
	}
	if (!writeNative(ds.root + "/" + ds.large, makeContent(ds.largeSize, 7)))
		return false;

	ds.archivePlain = opt.workDir + "/plain.vfs";
	ds.archiveXor = opt.workDir + "/xor.vfs";
	ds.archiveLz4 = opt.workDir + "/lz4.vfs";
	if (!buildArchive(ds, ds.archivePlain, false, false) || !buildArchive(ds, ds.archiveXor, true, false) ||
		!buildArchive(ds, ds.archiveLz4, false, true))
		return false;

	for (auto& r : ds.small)
		ds.allNativeFiles.push_back(ds.root + "/" + r);
	for (auto& r : ds.medium)
		ds.allNativeFiles.push_back(ds.root + "/" + r);
	ds.allNativeFiles.push_back(ds.root + "/" + ds.large);
	ds.allNativeFiles.push_back(ds.archivePlain);
	ds.allNativeFiles.push_back(ds.archiveXor);
	ds.allNativeFiles.push_back(ds.archiveLz4);
	return true;
}

// 尽量把数据文件从页缓存中逐出（不需要 root）。目录项 / inode 缓存无法这样清除
void dropCaches(const Dataset& ds)
{
#if (VFS_PLATFORM_LINUX || VFS_PLATFORM_ANDROID)
	for (auto& p : ds.allNativeFiles)
	{
		int fd = ::open(p.c_str(), O_RDONLY);
		if (fd < 0)
			continue;
		::fdatasync(fd);
		::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		::close(fd);
	}
#else
	(void)ds;
#endif
}

// ================================================================ 被测目标

/**
 * 随机读句柄：打开一次，多次按偏移读取。
 */
class Handle
{
public:
	virtual ~Handle() = default;

	// 从 offset 读 size 字节，返回读到的字节数，失败返回 -1
	virtual int64_t readAt(int64_t offset, void* buf, size_t size) = 0;
};

/**
 * 被测目标：基线（直接调用 OS / C / C++ 文件 API）或某种 VFS 组合。
 * 所有方法都可以被多个线程并发调用。路径都是相对数据集根目录的相对路径。
 */
class Target
{
public:
	virtual ~Target() = default;

	virtual std::string name() const = 0;

	virtual bool baseline() const { return false; }

	// 是否参与元数据场景（stat / miss / list）。三个读基线只让 stdio 参与，避免重复
	virtual bool supportsMeta() const { return true; }

	// 打开 + 读完整个文件 + 关闭；返回字节数，失败返回 -1
	virtual int64_t readWhole(const std::string& rel, std::vector<uint8_t>& buf) = 0;

	// 分块顺序读完整个文件，每块调用一次 onChunk（用于记录每块的延迟）
	virtual int64_t readChunks(const std::string& rel, std::vector<uint8_t>& buf, const std::function<void()>& onChunk) = 0;

	// 打开后立即关闭
	virtual bool openClose(const std::string& rel) = 0;

	virtual std::unique_ptr<Handle> openHandle(const std::string& rel) = 0;

	virtual bool stat(const std::string& rel, int64_t& size) = 0;

	virtual bool exists(const std::string& rel) = 0;

	// 列目录，返回条目数，失败返回 -1
	virtual int64_t list(const std::string& rel) = 0;

	// 写入相关：只有写目标实现
	virtual bool resetWriteArea(int /*threads*/) { return false; }

	virtual bool writeWhole(const std::string& /*rel*/, const uint8_t* /*data*/, size_t /*size*/) { return false; }

	virtual bool remove(const std::string& /*rel*/) { return false; }
};

// ---------------------------------------------------------------- 基线：C FILE*

#if VFS_PLATFORM_WINDOWS
int seek64(FILE* f, int64_t off) { return ::_fseeki64(f, off, SEEK_SET); }
#else
int seek64(FILE* f, int64_t off) { return ::fseeko(f, static_cast<off_t>(off), SEEK_SET); }
#endif

class StdioHandle : public Handle
{
public:
	explicit StdioHandle(FILE* f) : m_file(f) {}

	~StdioHandle() override { std::fclose(m_file); }

	int64_t readAt(int64_t offset, void* buf, size_t size) override
	{
		if (seek64(m_file, offset) != 0)
			return -1;
		return static_cast<int64_t>(std::fread(buf, 1, size, m_file));
	}

private:
	FILE* m_file;
};

// 基线目标的公共部分：根目录、写入区、std::filesystem 实现的元数据操作
class NativeBaseline : public Target
{
public:
	explicit NativeBaseline(std::string root) : m_root(std::move(root)) {}

	bool baseline() const override { return true; }

	bool stat(const std::string& rel, int64_t& size) override
	{
		std::error_code ec;
		auto st = stdfs::status(full(rel), ec);
		if (ec || !stdfs::is_regular_file(st))
			return false;
		size = static_cast<int64_t>(stdfs::file_size(full(rel), ec));
		return !ec;
	}

	bool exists(const std::string& rel) override
	{
		std::error_code ec;
		return stdfs::exists(full(rel), ec);
	}

	int64_t list(const std::string& rel) override
	{
		std::error_code ec;
		int64_t n = 0;
		for (stdfs::directory_iterator it(full(rel), ec), end; !ec && it != end; it.increment(ec))
			++n;
		return ec ? -1 : n;
	}

	bool resetWriteArea(int threads) override
	{
		std::error_code ec;
		stdfs::remove_all(full("w"), ec);
		for (int t = 0; t < threads; ++t)
		{
			stdfs::create_directories(full("w/t" + std::to_string(t)), ec);
			if (ec)
				return false;
		}
		return true;
	}

	bool remove(const std::string& rel) override
	{
		return std::remove(full(rel).c_str()) == 0;
	}

protected:
	std::string full(const std::string& rel) const { return m_root + "/" + rel; }

	std::string m_root;
};

class StdioTarget : public NativeBaseline
{
public:
	using NativeBaseline::NativeBaseline;

	std::string name() const override { return "base:stdio"; }

	int64_t readWhole(const std::string& rel, std::vector<uint8_t>& buf) override
	{
		return readChunks(rel, buf, {});
	}

	int64_t readChunks(const std::string& rel, std::vector<uint8_t>& buf, const std::function<void()>& onChunk) override
	{
		FILE* f = std::fopen(full(rel).c_str(), "rb");
		if (!f)
			return -1;
		int64_t total = 0;
		for (;;)
		{
			size_t n = std::fread(buf.data(), 1, buf.size(), f);
			if (n == 0)
				break;
			total += static_cast<int64_t>(n);
			if (onChunk)
				onChunk();
		}
		std::fclose(f);
		return total;
	}

	bool openClose(const std::string& rel) override
	{
		FILE* f = std::fopen(full(rel).c_str(), "rb");
		if (!f)
			return false;
		std::fclose(f);
		return true;
	}

	std::unique_ptr<Handle> openHandle(const std::string& rel) override
	{
		FILE* f = std::fopen(full(rel).c_str(), "rb");
		return f ? std::make_unique<StdioHandle>(f) : nullptr;
	}

	bool writeWhole(const std::string& rel, const uint8_t* data, size_t size) override
	{
		FILE* f = std::fopen(full(rel).c_str(), "wb");
		if (!f)
			return false;
		bool ok = std::fwrite(data, 1, size, f) == size;
		return std::fclose(f) == 0 && ok;
	}
};

// ---------------------------------------------------------------- 基线：std::fstream

class FstreamHandle : public Handle
{
public:
	explicit FstreamHandle(const std::string& path) : m_file(stdfs::path(path), std::ios::binary) {}

	bool ok() const { return m_file.is_open(); }

	int64_t readAt(int64_t offset, void* buf, size_t size) override
	{
		m_file.clear();
		m_file.seekg(offset);
		m_file.read(static_cast<char*>(buf), static_cast<std::streamsize>(size));
		return static_cast<int64_t>(m_file.gcount());
	}

private:
	std::ifstream m_file;
};

class FstreamTarget : public NativeBaseline
{
public:
	using NativeBaseline::NativeBaseline;

	std::string name() const override { return "base:fstream"; }

	bool supportsMeta() const override { return false; }

	int64_t readWhole(const std::string& rel, std::vector<uint8_t>& buf) override
	{
		return readChunks(rel, buf, {});
	}

	int64_t readChunks(const std::string& rel, std::vector<uint8_t>& buf, const std::function<void()>& onChunk) override
	{
		std::ifstream f(stdfs::path(full(rel)), std::ios::binary);
		if (!f.is_open())
			return -1;
		int64_t total = 0;
		for (;;)
		{
			f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
			std::streamsize n = f.gcount();
			if (n <= 0)
				break;
			total += n;
			if (onChunk)
				onChunk();
		}
		return total;
	}

	bool openClose(const std::string& rel) override
	{
		std::ifstream f(stdfs::path(full(rel)), std::ios::binary);
		return f.is_open();
	}

	std::unique_ptr<Handle> openHandle(const std::string& rel) override
	{
		auto h = std::make_unique<FstreamHandle>(full(rel));
		if (!h->ok())
			return nullptr;
		return h;
	}

	bool writeWhole(const std::string& rel, const uint8_t* data, size_t size) override
	{
		std::ofstream f(stdfs::path(full(rel)), std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
		f.close();
		return !f.fail();
	}
};

// ---------------------------------------------------------------- 基线：POSIX pread / Win32 ReadFile

#if VFS_PLATFORM_WINDOWS

std::wstring widen(const std::string& s)
{
	int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

HANDLE openOs(const std::string& path, bool write)
{
	return ::CreateFileW(widen(path).c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						 nullptr, write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

class OsHandle : public Handle
{
public:
	explicit OsHandle(HANDLE h) : m_handle(h) {}

	~OsHandle() override { ::CloseHandle(m_handle); }

	int64_t readAt(int64_t offset, void* buf, size_t size) override
	{
		OVERLAPPED ov{};
		ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
		ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
		DWORD got = 0;
		if (!::ReadFile(m_handle, buf, static_cast<DWORD>(size), &got, &ov))
			return ::GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
		return got;
	}

private:
	HANDLE m_handle;
};

#else

class OsHandle : public Handle
{
public:
	explicit OsHandle(int fd) : m_fd(fd) {}

	~OsHandle() override { ::close(m_fd); }

	int64_t readAt(int64_t offset, void* buf, size_t size) override
	{
		return static_cast<int64_t>(::pread(m_fd, buf, size, static_cast<off_t>(offset)));
	}

private:
	int m_fd;
};

#endif

class OsTarget : public NativeBaseline
{
public:
	using NativeBaseline::NativeBaseline;

#if VFS_PLATFORM_WINDOWS
	std::string name() const override { return "base:win32"; }
#else
	std::string name() const override { return "base:posix"; }
#endif

	bool supportsMeta() const override { return false; }

	int64_t readWhole(const std::string& rel, std::vector<uint8_t>& buf) override
	{
		return readChunks(rel, buf, {});
	}

	int64_t readChunks(const std::string& rel, std::vector<uint8_t>& buf, const std::function<void()>& onChunk) override
	{
#if VFS_PLATFORM_WINDOWS
		HANDLE h = openOs(full(rel), false);
		if (h == INVALID_HANDLE_VALUE)
			return -1;
		int64_t total = 0;
		for (;;)
		{
			DWORD got = 0;
			if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0)
				break;
			total += got;
			if (onChunk)
				onChunk();
		}
		::CloseHandle(h);
		return total;
#else
		int fd = ::open(full(rel).c_str(), O_RDONLY);
		if (fd < 0)
			return -1;
		int64_t total = 0;
		for (;;)
		{
			ssize_t n = ::read(fd, buf.data(), buf.size());
			if (n <= 0)
				break;
			total += n;
			if (onChunk)
				onChunk();
		}
		::close(fd);
		return total;
#endif
	}

	bool openClose(const std::string& rel) override
	{
#if VFS_PLATFORM_WINDOWS
		HANDLE h = openOs(full(rel), false);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		::CloseHandle(h);
		return true;
#else
		int fd = ::open(full(rel).c_str(), O_RDONLY);
		if (fd < 0)
			return false;
		::close(fd);
		return true;
#endif
	}

	std::unique_ptr<Handle> openHandle(const std::string& rel) override
	{
#if VFS_PLATFORM_WINDOWS
		HANDLE h = openOs(full(rel), false);
		return h == INVALID_HANDLE_VALUE ? nullptr : std::make_unique<OsHandle>(h);
#else
		int fd = ::open(full(rel).c_str(), O_RDONLY);
		return fd < 0 ? nullptr : std::make_unique<OsHandle>(fd);
#endif
	}

	bool writeWhole(const std::string& rel, const uint8_t* data, size_t size) override
	{
#if VFS_PLATFORM_WINDOWS
		HANDLE h = openOs(full(rel), true);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD put = 0;
		bool ok = ::WriteFile(h, data, static_cast<DWORD>(size), &put, nullptr) && put == size;
		return ::CloseHandle(h) && ok;
#else
		int fd = ::open(full(rel).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			return false;
		bool ok = ::write(fd, data, size) == static_cast<ssize_t>(size);
		return ::close(fd) == 0 && ok;
#endif
	}
};

// ---------------------------------------------------------------- VFS 目标

class VfsHandle : public Handle
{
public:
	explicit VfsHandle(std::unique_ptr<IFileStream> s) : m_stream(std::move(s)) {}

	int64_t readAt(int64_t offset, void* buf, size_t size) override
	{
		if (m_stream->seek(offset, SEEK_SET) != offset)
			return -1;
		return m_stream->read(buf, static_cast<unsigned int>(size));
	}

private:
	std::unique_ptr<IFileStream> m_stream;
};

class VfsTarget : public Target
{
public:
	VfsTarget(std::string name, std::string prefix) : m_name(std::move(name)), m_prefix(std::move(prefix)) {}

	VirtualFileSystem& fs() { return m_fs; }

	std::string name() const override { return m_name; }

	int64_t readWhole(const std::string& rel, std::vector<uint8_t>& buf) override
	{
		return readChunks(rel, buf, {});
	}

	int64_t readChunks(const std::string& rel, std::vector<uint8_t>& buf, const std::function<void()>& onChunk) override
	{
		auto s = m_fs.openFileStream(full(rel), IFileStream::Mode::READ);
		if (!s)
			return -1;
		int64_t total = 0;
		for (;;)
		{
			int n = s->read(buf.data(), static_cast<unsigned int>(buf.size()));
			if (n <= 0)
				break;
			total += n;
			if (onChunk)
				onChunk();
		}
		return total;
	}

	bool openClose(const std::string& rel) override
	{
		return m_fs.openFileStream(full(rel), IFileStream::Mode::READ) != nullptr;
	}

	std::unique_ptr<Handle> openHandle(const std::string& rel) override
	{
		auto s = m_fs.openFileStream(full(rel), IFileStream::Mode::READ);
		return s ? std::make_unique<VfsHandle>(std::move(s)) : nullptr;
	}

	bool stat(const std::string& rel, int64_t& size) override
	{
		Stat st;
		if (m_fs.stat(full(rel), st) != Error::OK || st.type != FileType::FILE)
			return false;
		size = st.size;
		return true;
	}

	bool exists(const std::string& rel) override { return m_fs.exists(full(rel)); }

	int64_t list(const std::string& rel) override
	{
		int64_t n = 0;
		Error e = m_fs.list(full(rel), [&n](const DirEntry&) { ++n; return true; });
		return e == Error::OK ? n : -1;
	}

	bool resetWriteArea(int threads) override
	{
		Error e = m_fs.removeDir(full("w"), true);
		if (e != Error::OK && e != Error::NOT_FOUND)
			return false;
		for (int t = 0; t < threads; ++t)
		{
			if (m_fs.createDirs(full("w/t" + std::to_string(t))) != Error::OK)
				return false;
		}
		return true;
	}

	bool writeWhole(const std::string& rel, const uint8_t* data, size_t size) override
	{
		auto s = m_fs.openFileStream(full(rel), IFileStream::Mode::WRITE);
		if (!s)
			return false;
		bool ok = s->write(data, static_cast<unsigned int>(size)) == static_cast<int>(size);
		return s->close() == 0 && ok;
	}

	bool remove(const std::string& rel) override { return m_fs.removeFile(full(rel)) == Error::OK; }

private:
	std::string full(const std::string& rel) const { return m_prefix + "/" + rel; }

	std::string m_name;
	std::string m_prefix;
	VirtualFileSystem m_fs;
};

std::shared_ptr<MemoryBackend> memoryCopyOf(const Dataset& ds)
{
	auto mem = MemoryBackend::create();
	auto load = [&](const std::string& rel)
	{
		std::ifstream f(stdfs::path(ds.root + "/" + rel), std::ios::binary);
		std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		mem->addFile(rel, std::move(data));
	};
	for (auto& r : ds.small)
		load(r);
	for (auto& r : ds.medium)
		load(r);
	load(ds.large);
	return mem;
}

bool checkMount(MountId id, const std::string& what)
{
	if (id == 0)
		std::cerr << "mount failed: " << what << "\n";
	return id != 0;
}

// 读 / 元数据场景的全部目标
std::vector<std::unique_ptr<Target>> makeReadTargets(const Options& opt, const Dataset& ds)
{
	std::vector<std::unique_ptr<Target>> out;
	out.push_back(std::make_unique<StdioTarget>(ds.root));
	out.push_back(std::make_unique<FstreamTarget>(ds.root));
	out.push_back(std::make_unique<OsTarget>(ds.root));

	const std::string overlayDir = opt.workDir + "/overlay";   // 空的本机覆盖目录
	std::error_code ec;
	stdfs::create_directories(overlayDir, ec);
	bool ok = true;

	{
		auto t = std::make_unique<VfsTarget>("vfs:native", "/d");
		ok &= checkMount(t->fs().mountNative("/d", ds.root), "native");
		out.push_back(std::move(t));
	}
	{
		auto t = std::make_unique<VfsTarget>("vfs:memory", "/d");
		ok &= checkMount(t->fs().mount("/d", memoryCopyOf(ds)), "memory");
		out.push_back(std::move(t));
	}
	const std::pair<const char*, std::string> archives[] = {
		{"vfs:archive", ds.archivePlain}, {"vfs:archive-xor", ds.archiveXor}, {"vfs:archive-lz4", ds.archiveLz4}};
	for (auto& [name, path] : archives)
	{
		auto t = std::make_unique<VfsTarget>(name, "/d");
		ok &= checkMount(t->fs().mountArchive("/d", path), name);
		out.push_back(std::move(t));
	}
	{
		// 归档在下、空的本机覆盖目录在上：开发期“本机文件覆盖归档”的典型用法
		auto t = std::make_unique<VfsTarget>("vfs:archive+native", "/d");
		ok &= checkMount(t->fs().mountArchive("/d", ds.archivePlain, {}, MountOptions{0, 0}), "archive+native/archive");
		ok &= checkMount(t->fs().mountNative("/d", overlayDir, MountOptions{1, 0}), "archive+native/native");
		out.push_back(std::move(t));
	}
	{
		auto t = std::make_unique<VfsTarget>("vfs:memory+archive", "/d");
		ok &= checkMount(t->fs().mountArchive("/d", ds.archivePlain, {}, MountOptions{0, 0}), "memory+archive/archive");
		ok &= checkMount(t->fs().mountMemory("/d", MountOptions{1, 0}), "memory+archive/memory");
		out.push_back(std::move(t));
	}
	{
		// 三层：归档 + 本机覆盖 + 内存（可写）
		auto t = std::make_unique<VfsTarget>("vfs:3layer", "/d");
		ok &= checkMount(t->fs().mountArchive("/d", ds.archivePlain, {}, MountOptions{0, 0}), "3layer/archive");
		ok &= checkMount(t->fs().mountNative("/d", overlayDir, MountOptions{1, 0}), "3layer/native");
		ok &= checkMount(t->fs().mountMemory("/d", MountOptions{2, MountFlags::WriteTarget}), "3layer/memory");
		out.push_back(std::move(t));
	}
	{
		// 深层嵌套：数据（本机）挂在第 8 层，每一级祖先上都有一个空的内存挂载，另有 32 个无关挂载撑大挂载表
		std::string prefix;
		auto t = std::make_unique<VfsTarget>("vfs:nested8", "/n1/n2/n3/n4/n5/n6/n7/n8");
		for (int i = 1; i <= 8; ++i)
		{
			prefix += "/n" + std::to_string(i);
			ok &= checkMount(t->fs().mountMemory(prefix, MountOptions{i, 0}), "nested8/memory");
		}
		ok &= checkMount(t->fs().mountNative(prefix, ds.root), "nested8/native");
		for (int i = 0; i < 32; ++i)
			ok &= checkMount(t->fs().mountMemory("/other/m" + std::to_string(i)), "nested8/other");
		out.push_back(std::move(t));
	}
	if (!ok)
		out.clear();
	return out;
}

// 写场景的目标
std::vector<std::unique_ptr<Target>> makeWriteTargets(const Options& opt, const Dataset& ds)
{
	std::vector<std::unique_ptr<Target>> out;
	const std::string root = opt.workDir + "/write";
	std::error_code ec;
	for (const char* sub : {"stdio", "fstream", "os", "vfs-native"})
		stdfs::create_directories(stdfs::path(root) / sub, ec);
	out.push_back(std::make_unique<StdioTarget>(root + "/stdio"));
	out.push_back(std::make_unique<FstreamTarget>(root + "/fstream"));
	out.push_back(std::make_unique<OsTarget>(root + "/os"));
	bool ok = true;
	{
		auto t = std::make_unique<VfsTarget>("vfs:native", "/d");
		ok &= checkMount(t->fs().mountNative("/d", root + "/vfs-native"), "write/native");
		out.push_back(std::move(t));
	}
	{
		auto t = std::make_unique<VfsTarget>("vfs:memory", "/d");
		ok &= checkMount(t->fs().mountMemory("/d"), "write/memory");
		out.push_back(std::move(t));
	}
	{
		auto t = std::make_unique<VfsTarget>("vfs:memory+archive", "/d");
		ok &= checkMount(t->fs().mountArchive("/d", ds.archivePlain, {}, MountOptions{0, 0}), "write/archive");
		ok &= checkMount(t->fs().mountMemory("/d", MountOptions{1, 0}), "write/memory");
		out.push_back(std::move(t));
	}
	if (!ok)
		out.clear();
	return out;
}

// ================================================================ 运行与统计

struct ThreadStats
{
	std::vector<uint32_t> latNs;   // 每次操作的延迟（纳秒，超过 4 秒截断）
	uint64_t ops = 0;
	uint64_t bytes = 0;
	bool failed = false;
};

struct Result
{
	std::string scenario;
	std::string target;
	bool baseline = false;
	int threads = 0;
	uint64_t ops = 0;
	uint64_t bytes = 0;
	double seconds = 0;
	double p50us = 0;
	double p99us = 0;
	double ratio = 0;   // 相对 base:stdio 的吞吐比
	bool failed = false;
};

inline uint32_t elapsedNs(Clock::time_point t0)
{
	auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
	return static_cast<uint32_t>(std::min<int64_t>(ns, 0xFFFFFFFFll));
}

using Work = std::function<void(int tid, int threads, ThreadStats& st)>;

// 多线程运行一次：所有线程同时起跑，计时覆盖从起跑到最后一个线程结束
Result runOnce(int threads, const Work& work)
{
	std::vector<ThreadStats> stats(static_cast<size_t>(threads));
	std::atomic<int> ready{0};
	std::atomic<bool> go{false};
	std::vector<std::thread> pool;
	for (int t = 0; t < threads; ++t)
	{
		pool.emplace_back([&, t]()
		{
			ready.fetch_add(1);
			while (!go.load(std::memory_order_acquire))
				std::this_thread::yield();
			work(t, threads, stats[static_cast<size_t>(t)]);
		});
	}
	while (ready.load() < threads)
		std::this_thread::yield();
	auto t0 = Clock::now();
	go.store(true, std::memory_order_release);
	for (auto& th : pool)
		th.join();
	Result r;
	r.threads = threads;
	r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
	std::vector<uint32_t> all;
	for (auto& s : stats)
	{
		r.ops += s.ops;
		r.bytes += s.bytes;
		r.failed |= s.failed;
		all.insert(all.end(), s.latNs.begin(), s.latNs.end());
	}
	if (!all.empty())
	{
		auto pct = [&all](double p)
		{
			size_t k = std::min(all.size() - 1, static_cast<size_t>(p * static_cast<double>(all.size())));
			std::nth_element(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(k), all.end());
			return all[k] / 1000.0;
		};
		r.p50us = pct(0.50);
		r.p99us = pct(0.99);
	}
	return r;
}

struct Scenario
{
	std::string name;
	bool write = false;    // 使用写目标
	bool meta = false;     // 元数据场景（只有支持的目标参与）
	bool bytes = true;     // 是否报告 MB/s
	std::string desc;
	// 每次迭代前的准备（例如清空写入区），返回 false 表示失败
	std::function<bool(Target&, int threads)> prepare;
	std::function<Work(Target&)> make;
};

uint64_t xorshift(uint64_t& x)
{
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return x;
}

std::vector<Scenario> makeScenarios(const Options& opt, const Dataset& ds)
{
	std::vector<Scenario> sc;
	const Dataset* d = &ds;
	const size_t randReads = std::max<size_t>(256, static_cast<size_t>(4096 * opt.scale));
	const size_t writeFiles = std::max<size_t>(20, static_cast<size_t>(200 * opt.scale));
	const size_t churnOps = std::max<size_t>(100, static_cast<size_t>(1000 * opt.scale));

	// 读完整个文件：每个线程读完整个文件集合，起点错开
	auto readSet = [](const std::vector<std::string>* files, size_t bufSize)
	{
		return [files, bufSize](Target& tg) -> Work
		{
			return [&tg, files, bufSize](int tid, int threads, ThreadStats& st)
			{
				std::vector<uint8_t> buf(bufSize);
				st.latNs.reserve(files->size());
				const size_t n = files->size();
				const size_t start = n * static_cast<size_t>(tid) / static_cast<size_t>(threads);
				for (size_t i = 0; i < n; ++i)
				{
					auto t0 = Clock::now();
					int64_t got = tg.readWhole((*files)[(start + i) % n], buf);
					st.latNs.push_back(elapsedNs(t0));
					if (got < 0)
					{
						st.failed = true;
						return;
					}
					st.bytes += static_cast<uint64_t>(got);
					++st.ops;
				}
			};
		};
	};

	sc.push_back({"read.small", false, false, true, "open + read 4KiB + close; each thread reads every small file", {}, readSet(&ds.small, 64 * KiB)});
	sc.push_back({"read.medium", false, false, true, "open + read 256KiB in 64KiB chunks + close", {}, readSet(&ds.medium, 64 * KiB)});

	sc.push_back({"read.large.seq", false, false, true, "sequential read of the large file in 1MiB chunks, once per thread; latency per chunk", {},
		[d](Target& tg) -> Work
		{
			return [&tg, d](int, int, ThreadStats& st)
			{
				std::vector<uint8_t> buf(1 * MiB);
				st.latNs.reserve(d->largeSize / MiB + 2);
				auto t0 = Clock::now();
				int64_t got = tg.readChunks(d->large, buf, [&]()
				{
					st.latNs.push_back(elapsedNs(t0));
					++st.ops;
					t0 = Clock::now();
				});
				if (got != static_cast<int64_t>(d->largeSize))
					st.failed = true;
				else
					st.bytes += static_cast<uint64_t>(got);
			};
		}});

	sc.push_back({"read.large.rand", false, false, true, "open the large file once, 4KiB reads at random offsets; latency per read (open counted in total time)", {},
		[d, randReads](Target& tg) -> Work
		{
			return [&tg, d, randReads](int tid, int, ThreadStats& st)
			{
				std::vector<uint8_t> buf(4 * KiB);
				st.latNs.reserve(randReads);
				auto h = tg.openHandle(d->large);
				if (!h)
				{
					st.failed = true;
					return;
				}
				uint64_t x = 0x1234567ull + static_cast<uint64_t>(tid) * 7919;
				const uint64_t range = d->largeSize - buf.size();
				for (size_t i = 0; i < randReads; ++i)
				{
					const int64_t off = static_cast<int64_t>(xorshift(x) % range);
					auto t0 = Clock::now();
					int64_t got = h->readAt(off, buf.data(), buf.size());
					st.latNs.push_back(elapsedNs(t0));
					if (got != static_cast<int64_t>(buf.size()))
					{
						st.failed = true;
						return;
					}
					st.bytes += static_cast<uint64_t>(got);
					++st.ops;
				}
			};
		}});

	auto perSmall = [d](std::function<bool(Target&, const std::string&)> op)
	{
		return [d, op](Target& tg) -> Work
		{
			return [&tg, d, op](int tid, int threads, ThreadStats& st)
			{
				const size_t n = d->small.size();
				st.latNs.reserve(n);
				const size_t start = n * static_cast<size_t>(tid) / static_cast<size_t>(threads);
				for (size_t i = 0; i < n; ++i)
				{
					auto t0 = Clock::now();
					bool ok = op(tg, d->small[(start + i) % n]);
					st.latNs.push_back(elapsedNs(t0));
					if (!ok)
					{
						st.failed = true;
						return;
					}
					++st.ops;
				}
			};
		};
	};

	sc.push_back({"meta.open", false, false, false, "open and close small files (no read)", {},
		perSmall([](Target& tg, const std::string& rel) { return tg.openClose(rel); })});
	sc.push_back({"meta.stat", false, true, false, "stat existing small files", {},
		perSmall([](Target& tg, const std::string& rel)
		{
			int64_t size = -1;
			return tg.stat(rel, size) && size > 0;
		})});
	sc.push_back({"meta.miss", false, true, false, "exists() on names missing from a large directory (layered setups must check every layer)", {},
		perSmall([](Target& tg, const std::string& rel) { return !tg.exists(rel + ".missing"); })});

	sc.push_back({"meta.list", false, true, false, "list the large directory holding all small files, 10 times per thread; latency per listing", {},
		[d](Target& tg) -> Work
		{
			return [&tg, d](int, int, ThreadStats& st)
			{
				for (int i = 0; i < 10; ++i)
				{
					auto t0 = Clock::now();
					int64_t n = tg.list("small");
					st.latNs.push_back(elapsedNs(t0));
					if (n != static_cast<int64_t>(d->small.size()))
					{
						st.failed = true;
						return;
					}
					++st.ops;
				}
			};
		}});

	auto resetArea = [](Target& tg, int threads) { return tg.resetWriteArea(threads); };
	auto payload = std::make_shared<std::vector<uint8_t>>(makeContent(64 * KiB, 99));

	sc.push_back({"write.create", true, false, true, "each thread creates and writes 64KiB files in its own directory", resetArea,
		[payload, writeFiles](Target& tg) -> Work
		{
			return [&tg, payload, writeFiles](int tid, int, ThreadStats& st)
			{
				st.latNs.reserve(writeFiles);
				const std::string dir = "w/t" + std::to_string(tid) + "/";
				for (size_t i = 0; i < writeFiles; ++i)
				{
					auto t0 = Clock::now();
					bool ok = tg.writeWhole(dir + fileName("c", i), payload->data(), payload->size());
					st.latNs.push_back(elapsedNs(t0));
					if (!ok)
					{
						st.failed = true;
						return;
					}
					st.bytes += payload->size();
					++st.ops;
				}
			};
		}});

	sc.push_back({"write.churn", true, false, false, "each thread loops: create + write 4KiB file, then delete it (one op = create + delete)", resetArea,
		[payload, churnOps](Target& tg) -> Work
		{
			return [&tg, payload, churnOps](int tid, int, ThreadStats& st)
			{
				st.latNs.reserve(churnOps);
				const std::string dir = "w/t" + std::to_string(tid) + "/";
				for (size_t i = 0; i < churnOps; ++i)
				{
					const std::string path = dir + fileName("x", i % 16);
					auto t0 = Clock::now();
					bool ok = tg.writeWhole(path, payload->data(), 4 * KiB) && tg.remove(path);
					st.latNs.push_back(elapsedNs(t0));
					if (!ok)
					{
						st.failed = true;
						return;
					}
					++st.ops;
				}
			};
		}});
	return sc;
}

bool matchFilter(const Options& opt, const std::string& scenario, const std::string& target)
{
	if (opt.filters.empty())
		return true;
	const std::string key = scenario + "/" + target;
	for (auto& f : opt.filters)
	{
		if (key.find(f) != std::string::npos)
			return true;
	}
	return false;
}

// ================================================================ 输出

std::string fmt(double v, int prec)
{
	std::ostringstream os;
	os << std::fixed << std::setprecision(prec) << v;
	return os.str();
}

std::string fmtCount(double v)
{
	if (v >= 1e6)
		return fmt(v / 1e6, 2) + "M";
	if (v >= 1e3)
		return fmt(v / 1e3, 1) + "k";
	return fmt(v, 0);
}

struct Row
{
	std::string scenario, target, threads, opsps, mbps, p50, p99, ratio;
};

Row toRow(const Result& r, bool bytes)
{
	Row row;
	row.scenario = r.scenario;
	row.target = r.target;
	row.threads = std::to_string(r.threads);
	if (r.failed)
	{
		row.opsps = "FAIL";
		return row;
	}
	const double ops = static_cast<double>(r.ops) / r.seconds;
	row.opsps = fmtCount(ops);
	row.mbps = bytes ? fmt(static_cast<double>(r.bytes) / MiB / r.seconds, 0) : "-";
	row.p50 = fmt(r.p50us, r.p50us < 10 ? 2 : 1);
	row.p99 = fmt(r.p99us, r.p99us < 10 ? 2 : 1);
	row.ratio = r.ratio > 0 ? fmt(r.ratio, 2) : "-";
	return row;
}

void printTableHeader()
{
	std::cout << std::left << std::setw(16) << "scenario" << std::setw(22) << "target" << std::right << std::setw(4) << "thr"
			  << std::setw(10) << "ops/s" << std::setw(9) << "MiB/s" << std::setw(10) << "p50(us)" << std::setw(10) << "p99(us)"
			  << std::setw(9) << "vs stdio" << "\n";
}

void printRow(const Row& r)
{
	std::cout << std::left << std::setw(16) << r.scenario << std::setw(22) << r.target << std::right << std::setw(4) << r.threads
			  << std::setw(10) << r.opsps << std::setw(9) << r.mbps << std::setw(10) << r.p50 << std::setw(10) << r.p99
			  << std::setw(9) << r.ratio << "\n";
}

std::string compilerInfo()
{
#if defined(__clang__)
	return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
	return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
	return "MSVC " + std::to_string(_MSC_VER);
#else
	return "unknown";
#endif
}

#if VFS_PLATFORM_WINDOWS
// 读注册表字符串值（HKLM），失败返回空串
std::string regString(const wchar_t* subKey, const wchar_t* value)
{
	wchar_t buf[256];
	DWORD size = sizeof(buf);
	if (::RegGetValueW(HKEY_LOCAL_MACHINE, subKey, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
		return {};
	int len = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
	std::string out(len > 1 ? static_cast<size_t>(len - 1) : 0, '\0');
	if (len > 1)
		::WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr, nullptr);
	return out;
}
#endif

[[maybe_unused]] std::string trim(std::string s)   // 部分平台（如 WebAssembly）的 cpuInfo 用不到
{
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n'))
		s.pop_back();
	size_t p = s.find_first_not_of(" \t");
	return p == std::string::npos ? std::string() : s.substr(p);
}

std::string osInfo()
{
#if VFS_PLATFORM_WINDOWS
	// 产品名（注册表里 Windows 11 仍写作 "Windows 10"，以内部版本号为准）+ 版本号
	const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	std::string name = regString(key, L"ProductName");
	std::string build = regString(key, L"CurrentBuildNumber");
	std::string display = regString(key, L"DisplayVersion");
	std::string s = name.empty() ? "Windows" : name;
	if (!display.empty())
		s += " " + display;
	if (!build.empty())
		s += " (build " + build + ")";
	return s;
#else
	struct utsname u;
	if (::uname(&u) == 0)
		return std::string(u.sysname) + " " + u.release + " " + u.machine;
	return "POSIX";
#endif
}

std::string cpuInfo()
{
#if (VFS_PLATFORM_LINUX || VFS_PLATFORM_ANDROID)
	std::ifstream f("/proc/cpuinfo");
	std::string line;
	while (std::getline(f, line))
	{
		// x86 是 "model name"；部分 ARM 内核只有 "Hardware" / "CPU part"
		if (line.rfind("model name", 0) == 0 || line.rfind("Hardware", 0) == 0)
		{
			auto p = line.find(':');
			if (p != std::string::npos)
				return trim(line.substr(p + 1));
		}
	}
#elif VFS_PLATFORM_APPLE
	char buf[256];
	size_t size = sizeof(buf);
	if (::sysctlbyname("machdep.cpu.brand_string", buf, &size, nullptr, 0) == 0 && size > 0)
		return trim(std::string(buf, strnlen(buf, size)));
#elif VFS_PLATFORM_WINDOWS
	std::string s = regString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
	if (!s.empty())
		return trim(s);
#endif
	return "unknown";
}

std::string nowString()
{
	std::time_t t = std::time(nullptr);
	char buf[64];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	return buf;
}

} // namespace

int main(int argc, char** argv)
{
	Options opt;
	if (!parseArgs(argc, argv, opt))
		return 2;

	bool ownWorkDir = false;
	if (opt.workDir.empty())
	{
		std::error_code ec;
		opt.workDir = (stdfs::temp_directory_path(ec) / ("vfs_bench_" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count() % 1000000))).string();
		ownWorkDir = true;
	}
	std::replace(opt.workDir.begin(), opt.workDir.end(), '\\', '/');

	Dataset ds;
	std::vector<Scenario> scenarios;
	if (opt.list)
	{
		scenarios = makeScenarios(opt, ds);
		std::cout << "Scenarios:\n";
		for (auto& s : scenarios)
			std::cout << "  " << std::left << std::setw(18) << s.name << s.desc << "\n";
		std::cout << "Read / metadata targets: base:stdio base:fstream base:posix|base:win32 vfs:native vfs:memory vfs:archive "
					 "vfs:archive-xor vfs:archive-lz4 vfs:archive+native vfs:memory+archive vfs:3layer vfs:nested8\n"
					 "Write targets: base:stdio base:fstream base:posix|base:win32 vfs:native vfs:memory vfs:memory+archive\n";
		return 0;
	}

	std::cout << "# vfs_bench\n"
			  << "Date: " << nowString() << "\n"
			  << "OS: " << osInfo() << "\n"
			  << "CPU: " << cpuInfo() << ", hardware threads: " << hardwareThreads() << "\n"
			  << "Compiler: " << compilerInfo() <<
#if defined(NDEBUG)
				 " (Release)"
#else
				 " (Debug: results are not meaningful)"
#endif
			  << "\n"
			  << "Cache: " << (opt.cold ? "cold (posix_fadvise DONTNEED before each iteration, Linux only)" : "warm (one warm-up run first)") << "\n"
			  << "Scale: " << opt.scale << ", iterations: " << opt.iterations << " (median), working dir: " << opt.workDir << "\n\n";

	auto t0 = Clock::now();
	if (!buildDataset(opt, ds))
		return 1;
	scenarios = makeScenarios(opt, ds);
	std::cout << "Dataset: small files " << ds.small.size() << " x " << ds.smallSize / KiB << "KiB, medium files " << ds.medium.size() << " x "
			  << ds.mediumSize / KiB << "KiB, large file " << ds.largeSize / MiB << "MiB; archives plain "
			  << stdfs::file_size(ds.archivePlain) / MiB << "MiB / xor " << stdfs::file_size(ds.archiveXor) / MiB << "MiB / lz4 "
			  << stdfs::file_size(ds.archiveLz4) / MiB << "MiB; generated in "
			  << fmt(std::chrono::duration<double>(Clock::now() - t0).count(), 1) << "s\n\n";

	auto readTargets = makeReadTargets(opt, ds);
	auto writeTargets = makeWriteTargets(opt, ds);
	if (readTargets.empty() || writeTargets.empty())
		return 1;

	std::vector<Result> results;
	std::vector<bool> resultBytes;
	printTableHeader();
	for (auto& s : scenarios)
	{
		auto& targets = s.write ? writeTargets : readTargets;
		for (int threads : opt.threads)
		{
			for (auto& tg : targets)
			{
				if (!matchFilter(opt, s.name, tg->name()))
					continue;
				if (s.meta && !tg->supportsMeta())
					continue;
				Work work = s.make(*tg);
				auto runPrepared = [&]() -> Result
				{
					if (s.prepare && !s.prepare(*tg, threads))
					{
						Result bad;
						bad.failed = true;
						bad.threads = threads;
						bad.seconds = 1;
						return bad;
					}
					if (opt.cold && !s.write)
						dropCaches(ds);
					return runOnce(threads, work);
				};
				if (!opt.cold)
					runPrepared();   // 预热
				std::vector<Result> runs;
				for (int i = 0; i < opt.iterations; ++i)
					runs.push_back(runPrepared());
				std::sort(runs.begin(), runs.end(), [](const Result& a, const Result& b) { return a.seconds < b.seconds; });
				Result r = runs[runs.size() / 2];
				for (auto& x : runs)
					r.failed |= x.failed;
				r.scenario = s.name;
				r.target = tg->name();
				r.baseline = tg->baseline();
				// 相对 base:stdio 的吞吐比（同一场景、同一线程数）
				for (auto& b : results)
				{
					if (b.scenario == r.scenario && b.threads == r.threads && b.target == "base:stdio" && !b.failed && !r.failed)
						r.ratio = (static_cast<double>(r.ops) / r.seconds) / (static_cast<double>(b.ops) / b.seconds);
				}
				if (r.target == "base:stdio" && !r.failed)
					r.ratio = 1.0;
				printRow(toRow(r, s.bytes));
				std::cout.flush();
				results.push_back(r);
				resultBytes.push_back(s.bytes);
			}
		}
	}

	if (!opt.csvPath.empty())
	{
		std::ofstream f(stdfs::path(opt.csvPath));
		f << "scenario,target,threads,ops,bytes,seconds,ops_per_sec,mib_per_sec,p50_us,p99_us,ratio_vs_stdio,failed\n";
		for (auto& r : results)
		{
			f << r.scenario << "," << r.target << "," << r.threads << "," << r.ops << "," << r.bytes << "," << fmt(r.seconds, 6) << ","
			  << fmt(static_cast<double>(r.ops) / r.seconds, 1) << "," << fmt(static_cast<double>(r.bytes) / MiB / r.seconds, 2) << ","
			  << fmt(r.p50us, 3) << "," << fmt(r.p99us, 3) << "," << fmt(r.ratio, 4) << "," << (r.failed ? 1 : 0) << "\n";
		}
		std::cout << "\nCSV written to " << opt.csvPath << "\n";
	}
	if (!opt.mdPath.empty())
	{
		std::ofstream f(stdfs::path(opt.mdPath));
		std::string last;
		for (size_t i = 0; i < results.size(); ++i)
		{
			Row row = toRow(results[i], resultBytes[i]);
			if (row.scenario != last)
			{
				last = row.scenario;
				f << "\n### " << row.scenario << "\n\n| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |\n|---|---:|---:|---:|---:|---:|---:|\n";
			}
			f << "| " << row.target << " | " << row.threads << " | " << row.opsps << " | " << row.mbps << " | " << row.p50 << " | "
			  << row.p99 << " | " << row.ratio << " |\n";
		}
		std::cout << "Markdown written to " << opt.mdPath << "\n";
	}

	bool anyFailed = false;
	for (auto& r : results)
		anyFailed |= r.failed;

	readTargets.clear();
	writeTargets.clear();
	if (!opt.keep)
	{
		std::error_code ec;
		if (ownWorkDir)
			stdfs::remove_all(opt.workDir, ec);
		else
		{
			stdfs::remove_all(stdfs::path(opt.workDir) / "data", ec);
			stdfs::remove_all(stdfs::path(opt.workDir) / "write", ec);
			stdfs::remove_all(stdfs::path(opt.workDir) / "overlay", ec);
			for (auto* p : {&ds.archivePlain, &ds.archiveXor, &ds.archiveLz4})
				stdfs::remove(*p, ec);
		}
	}
	return anyFailed ? 1 : 0;
}
