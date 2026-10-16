#include "TestUtils.h"
#include "vfs/native/NativeUtils.h"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <random>

namespace stdfs = std::filesystem;

namespace test
{

stdfs::path u8(std::string_view s)
{
	std::u8string u(reinterpret_cast<const char8_t*>(s.data()), s.size());
	return stdfs::path(u);
}

static std::string fromPath(const stdfs::path& p)
{
	std::u8string u = p.generic_u8string();
	return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

TempDir::TempDir()
{
	static std::atomic<uint64_t> s_counter{0};
	std::random_device rd;
	std::string name = "vfs_test_" + std::to_string(rd()) + "_" + std::to_string(s_counter.fetch_add(1));
	stdfs::path p = stdfs::temp_directory_path() / name;
	stdfs::create_directories(p);
	m_path = fromPath(p);
}

// 递归删除，走 native:: 的操作，Windows 上超过 MAX_PATH 的深层目录也能删掉
static void removeTree(const std::string& path)
{
	std::vector<std::pair<std::string, bool>> children;
	vfs::native::listDir(path, [&](const std::string& name, vfs::FileType type) {
		children.emplace_back(path + "/" + name, type == vfs::FileType::DIR);
		return true;
	});
	for (const auto& [child, isDir] : children)
	{
		// listDir 会跟随符号链接，链接本身只删除链接，绝不递归进去
		std::error_code ec;
		if (stdfs::is_symlink(vfs::native::toFsPath(child), ec))
			stdfs::remove(vfs::native::toFsPath(child), ec);
		else if (isDir)
			removeTree(child);
		else
			vfs::native::removeFilePath(child);
	}
	vfs::native::removeDirPath(path);
}

TempDir::~TempDir()
{
	std::error_code ec;
	stdfs::remove_all(u8(m_path), ec);
	// 兜底：Windows 上标准库（如 MinGW 的 directory_iterator）处理不了超过 MAX_PATH 的深层目录
	vfs::Stat st;
	if (vfs::native::statPath(m_path, st) == vfs::Error::OK)
		removeTree(m_path);
}

std::string TempDir::sub(std::string_view rel) const
{
	if (rel.empty())
		return m_path;
	return m_path + "/" + std::string(rel);
}

void TempDir::write(std::string_view rel, std::string_view content) const
{
	write(rel, std::vector<uint8_t>(content.begin(), content.end()));
}

void TempDir::write(std::string_view rel, const std::vector<uint8_t>& content) const
{
	stdfs::path p = u8(sub(rel));
	stdfs::create_directories(p.parent_path());
	writeNative(sub(rel), content);
}

void TempDir::mkdir(std::string_view rel) const
{
	stdfs::create_directories(u8(sub(rel)));
}

bool TempDir::exists(std::string_view rel) const
{
	std::error_code ec;
	return stdfs::exists(u8(sub(rel)), ec);
}

std::string TempDir::read(std::string_view rel) const
{
	return str(readNative(sub(rel)));
}

std::vector<uint8_t> readNative(const std::string& path)
{
	std::ifstream in(u8(path), std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeNative(const std::string& path, const std::vector<uint8_t>& data)
{
	std::ofstream out(u8(path), std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<uint8_t> randomBytes(size_t size, uint64_t seed)
{
	std::vector<uint8_t> out(size);
	uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
	for (size_t i = 0; i < size; ++i)
	{
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		out[i] = static_cast<uint8_t>(x >> 24);
	}
	return out;
}

std::vector<uint8_t> bytes(std::string_view s)
{
	return std::vector<uint8_t>(s.begin(), s.end());
}

std::string str(const std::vector<uint8_t>& v)
{
	return std::string(v.begin(), v.end());
}

std::string readAll(const vfs::VirtualFileSystem& fs, std::string_view path)
{
	std::string out;
	vfs::Error e = fs.readFile(path, out);
	if (e != vfs::Error::OK)
		return std::string("<ERR:") + vfs::errorName(e) + ">";
	return out;
}

std::vector<uint8_t> readStream(vfs::IFileStream& stream)
{
	std::vector<uint8_t> out;
	uint8_t buf[4096];
	while (true)
	{
		int n = stream.read(buf, sizeof(buf));
		if (n <= 0)
			break;
		out.insert(out.end(), buf, buf + n);
	}
	return out;
}

std::vector<std::string> listNames(const vfs::VirtualFileSystem& fs, std::string_view path)
{
	std::vector<std::string> names;
	fs.list(path, [&names](const vfs::DirEntry& e)
	{
		names.push_back(e.name + (e.type == vfs::FileType::DIR ? "/" : ""));
		return true;
	});
	return names;
}

vfs::Error buildArchive(const std::string& outPath, const std::vector<ArchiveFile>& files,
						const vfs::ArchiveWriteOptions& options, vfs::ArchiveWriteStats* stats)
{
	vfs::ArchiveWriter writer(options);
	for (const auto& f : files)
	{
		vfs::Error e = f.path.back() == '/' ? writer.addDir(f.path) : writer.addData(f.path, f.content, f.options);
		if (e != vfs::Error::OK)
			return e;
	}
	return writer.write(outPath, stats);
}

bool nativeCaseSensitive()
{
	static const bool s_value = [] {
		TempDir dir;
		dir.write("CaseProbe.txt", "x");
		return !dir.exists("caseprobe.txt");
	}();
	return s_value;
}

bool nativeReplaceOpenFile()
{
	static const bool s_value = [] {
		TempDir dir;
		dir.write("target.txt", "old");
		dir.write("source.txt", "new");
		vfs::VirtualFileSystem fs;
		fs.mountNative("/", dir.path());
		auto held = fs.openFileStream("/target.txt", vfs::IFileStream::Mode::READ);
		return held && vfs::native::renamePath(dir.sub("source.txt"), dir.sub("target.txt")) == vfs::Error::OK;
	}();
	return s_value;
}

std::string osErrorSuffix()
{
	const int e = vfs::native::lastOsError();
	return e ? " (os=" + std::to_string(e) + ")" : std::string();
}

bool slowEnabled()
{
	const char* v = std::getenv("VFS_TEST_SLOW");
	return v && v[0] && std::string_view(v) != "0";
}

int scale()
{
	const char* v = std::getenv("VFS_TEST_SCALE");
	int n = v ? std::atoi(v) : 1;
	return n > 0 ? n : 1;
}

} // namespace test
