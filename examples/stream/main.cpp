// Streaming API: after openFileStream opens a file, write in chunks, seek, append, and read in chunks;
// the interface matches axmol IFileStream and suits large files or random access.
#include "vfs/VirtualFileSystem.h"
#include <cstdio>
#include <cstring>

USING_NS_VFS;

int main()
{
	VirtualFileSystem fs;
	fs.mountMemory("/tmp", MountOptions{0, MountFlags::WriteTarget});

	// 1. Chunked write: WRITE creates or truncates the file; the parent directory must already exist (same as the native filesystem), so mkdir -p first
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
		// Rewind and overwrite the first byte
		out->seek(0, SEEK_SET);
		out->write("C", 1);
	}   // The stream closes on destruction

	// 2. Append: APPEND always writes at the end
	{
		auto out = fs.openFileStream("/tmp/logs/run.log", IFileStream::Mode::APPEND);
		out->write("tail\n", 5);
	}

	// 3. Chunked read, and a demonstration of random access
	auto in = fs.openFileStream("/tmp/logs/run.log", IFileStream::Mode::READ);
	std::printf("size: %lld\n", static_cast<long long>(in->size()));
	char buf[8];
	std::string all;
	int n = 0;
	while ((n = in->read(buf, sizeof(buf))) > 0)   // 0 means end of file, -1 means an error
		all.append(buf, static_cast<size_t>(n));
	std::printf("content:\n%s", all.c_str());

	in->seek(-5, SEEK_END);
	n = in->read(buf, 4);
	std::printf("last word: %.*s\n", n, buf);

	// 4. Missing file: returns a null pointer and an error code
	Error err = Error::OK;
	auto missing = fs.openFileStream("/tmp/nope.txt", IFileStream::Mode::READ, &err);
	std::printf("open missing: %s\n", missing ? "opened?" : errorName(err));
	return 0;
}
