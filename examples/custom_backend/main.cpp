// 自定义后端：实现 IBackend，把“程序生成的内容”挂进虚拟文件系统。
// 这里的后端是只读的：每个 /gen/<n>.txt 文件的内容是 n 的平方，目录里固定有 10 个文件。
#include "vfs/VirtualFileSystem.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

USING_NS_VFS;

namespace
{

// 只读的字符串流：自定义后端通常需要配一个自己的 IFileStream
class StringStream : public IFileStream
{
public:
	explicit StringStream(std::string data) : m_data(std::move(data)) {}

	bool open(std::string_view, IFileStream::Mode) override { return false; }   // 由后端构造，不支持重新打开
	int close() override
	{
		m_open = false;
		return 0;
	}
	int64_t seek(int64_t offset, int origin) const override
	{
		int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? m_pos : static_cast<int64_t>(m_data.size());
		if (base + offset < 0)
			return -1;
		m_pos = base + offset;
		return m_pos;
	}
	int read(void* buf, unsigned int size) const override
	{
		if (!m_open)
			return -1;
		int64_t avail = std::max<int64_t>(0, static_cast<int64_t>(m_data.size()) - m_pos);
		size_t n = static_cast<size_t>(std::min<int64_t>(avail, size));
		std::memcpy(buf, m_data.data() + m_pos, n);
		m_pos += static_cast<int64_t>(n);
		return static_cast<int>(n);
	}
	int write(const void*, unsigned int) const override { return -1; }   // 只读
	int64_t tell() const override { return m_pos; }
	int64_t size() const override { return static_cast<int64_t>(m_data.size()); }
	bool isOpen() const override { return m_open; }

private:
	std::string m_data;
	mutable int64_t m_pos = 0;   // IFileStream 的接口是 const，位置需要 mutable
	bool m_open = true;
};

// 生成内容的后端。rel 是相对挂载点的规范路径（不以 '/' 开头，根目录为空串）
class SquaresBackend : public IBackend
{
public:
	uint32_t capabilities() const override { return BackendCaps::Read; }

	Error stat(std::string_view rel, Stat& out) const override
	{
		if (rel.empty())
		{
			out.type = FileType::DIR;
			out.size = 0;
			return Error::OK;
		}
		int n = 0;
		if (!parse(rel, n))
			return Error::NOT_FOUND;
		out.type = FileType::FILE;
		out.size = static_cast<int64_t>(content(n).size());
		return Error::OK;
	}

	std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override
	{
		int n = 0;
		Error e = Error::OK;
		if (mode != IFileStream::Mode::READ)
			e = Error::READ_ONLY;
		else if (rel.empty())
			e = Error::IS_DIR;
		else if (!parse(rel, n))
			e = Error::NOT_FOUND;
		if (err)
			*err = e;
		return e == Error::OK ? std::make_unique<StringStream>(content(n)) : nullptr;
	}

	Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override
	{
		if (!relDir.empty())
			return parseName(relDir) ? Error::NOT_DIR : Error::NOT_FOUND;
		for (int i = 0; i < 10; ++i)
			if (!cb(DirEntry{std::to_string(i) + ".txt", FileType::FILE}))
				break;
		return Error::OK;
	}

	const char* typeName() const override { return "squares"; }

private:
	static bool parseName(std::string_view rel)
	{
		int n = 0;
		return parse(rel, n);
	}
	// "<0..9>.txt" → n
	static bool parse(std::string_view rel, int& n)
	{
		if (rel.size() != 5 || rel.substr(1) != ".txt" || rel[0] < '0' || rel[0] > '9')
			return false;
		n = rel[0] - '0';
		return true;
	}
	static std::string content(int n) { return std::to_string(n * n); }
};

}   // namespace

int main()
{
	VirtualFileSystem fs;
	fs.mount("/gen", std::make_shared<SquaresBackend>());
	// 在它上面再叠一个可写内存层：写入落到内存层，遮蔽生成的内容
	fs.mountMemory("/gen", MountOptions{1, MountFlags::WriteTarget});

	std::string text;
	fs.readFile("/gen/7.txt", text);
	std::printf("/gen/7.txt = %s\n", text.c_str());

	fs.writeFile("/gen/7.txt", "overridden");
	fs.readFile("/gen/7.txt", text);
	std::printf("/gen/7.txt = %s (after write)\n", text.c_str());

	std::printf("/gen/42.txt exists: %s\n", fs.exists("/gen/42.txt") ? "yes" : "no");
	std::printf("/gen has %zu entries\n", fs.list("/gen").size());
	return 0;
}
