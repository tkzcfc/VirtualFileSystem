// 流式 API：openFileStream 打开文件后分块写入、seek、追加、分块读取；
// 接口与 axmol 的 IFileStream 一致，适合大文件或需要随机访问的场景。
#include "vfs/VirtualFileSystem.h"
#include <cstdio>
#include <cstring>

USING_NS_VFS;

int main()
{
	VirtualFileSystem fs;
	fs.mountMemory("/tmp", MountOptions{0, MountFlags::WriteTarget});

	// 1. 分块写入：WRITE 模式会创建/截断文件；父目录必须已存在（与本机文件系统一致），先 mkdir -p
	fs.createDirs("/tmp/logs");
	{
		Error err = Error::OK;
		auto out = fs.openFileStream("/tmp/logs/run.log", IFileStream::Mode::WRITE, &err);
		if (!out)
		{
			std::printf("open for write failed: %s\n", errorName(err));
			return 1;
		}
		for (int i = 0; i < 3; ++i)
		{
			char line[32];
			int n = std::snprintf(line, sizeof(line), "chunk %d\n", i);
			out->write(line, static_cast<unsigned int>(n));
		}
		// 回到开头改写第一个字节
		out->seek(0, SEEK_SET);
		out->write("C", 1);
	}   // 流析构时关闭

	// 2. 追加：APPEND 模式总是写到末尾
	{
		auto out = fs.openFileStream("/tmp/logs/run.log", IFileStream::Mode::APPEND);
		out->write("tail\n", 5);
	}

	// 3. 分块读取，并演示随机访问
	auto in = fs.openFileStream("/tmp/logs/run.log", IFileStream::Mode::READ);
	std::printf("size: %lld\n", static_cast<long long>(in->size()));
	char buf[8];
	std::string all;
	int n = 0;
	while ((n = in->read(buf, sizeof(buf))) > 0)   // 返回 0 表示到达末尾，-1 表示出错
		all.append(buf, static_cast<size_t>(n));
	std::printf("content:\n%s", all.c_str());

	in->seek(-5, SEEK_END);
	n = in->read(buf, 4);
	std::printf("last word: %.*s\n", n, buf);

	// 4. 不存在的文件：返回空指针和错误码
	Error err = Error::OK;
	auto missing = fs.openFileStream("/tmp/nope.txt", IFileStream::Mode::READ, &err);
	std::printf("open missing: %s\n", missing ? "opened?" : errorName(err));
	return 0;
}
