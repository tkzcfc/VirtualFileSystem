#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include "vfs/VirtualFileSystem.h"
#include "vfs/native/NativeFileSystem.h"
#include "vfs/memory/MemoryFileSystem.h"

#include "md5.h"
#include <assert.h>
#include <fstream>

USING_NS_VFS;


std::vector<uint8_t> readFileToVector(const std::string& filename) {
	std::ifstream file(filename, std::ios::binary);
	if (!file) {
		std::cerr << "无法打开文件: " << filename << std::endl;
		return {};
	}

	file.seekg(0, std::ios::end);
	std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);

	std::vector<uint8_t> buffer(size);
	buffer.resize(size);

	if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
		std::cerr << "读取文件失败: " << filename << std::endl;
		return {};
	}

	return buffer;
}


void readFile(VirtualFileSystem& virtualFileSystem, const std::string& fileName, bool printContent)
{
	printf("\n\n>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %s <<<<<<<<<<<<<<<<<<<<<<<<<<< read begin\n", fileName.c_str());
	auto stream = virtualFileSystem.openFileStream(fileName, FileStream::Mode::READ);
	if (stream)
	{
		auto dataLen = stream->size();
		std::vector<uint8_t> buf;
		buf.resize(dataLen);

		const size_t MAX_READ_LEN = 1024 * 10;
		size_t index = 0;
		while (index < dataLen)
		{
			auto readLen = std::min(MAX_READ_LEN, dataLen - index);
			auto realReadLen = stream->read((buf.data() + index), MAX_READ_LEN);
			assert(readLen == realReadLen);
			index += readLen;

			printf("stream tell: %llu\n", stream->tell());
		}

		std::string str = std::string((char*)buf.data(), buf.size());
		printf("file size: %llu\n", dataLen);
		printf("file md5: %s\n", md5(str).c_str());
		if(printContent)
			printf("%s\n", str.c_str());
	}
	else
	{
		printf("no file: %s\n", fileName.c_str());
	}
	printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %s <<<<<<<<<<<<<<<<<<<<<<<<<<< read end\n\n", fileName.c_str());
}


void writeFile(VirtualFileSystem& virtualFileSystem, const std::string& fileName, std::vector<uint8_t>& data)
{
	printf("\n\n>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %s <<<<<<<<<<<<<<<<<<<<<<<<<<< write begin\n", fileName.c_str());
	auto stream = virtualFileSystem.openFileStream(fileName, FileStream::Mode::WRITE);
	if (stream)
	{
		const size_t MAX_WRITE_LEN = 1024 * 10;
		size_t index = 0;
		while (index < data.size())
		{
			auto writeLen = std::min(MAX_WRITE_LEN, data.size() - index);
			auto realWriteLen = stream->write(reinterpret_cast<uint8_t*>(&data[index]), writeLen);
			assert(writeLen == realWriteLen);
			index += writeLen;

			printf("stream size: %llu, stream tell: %llu\n", stream->size(), stream->tell());
		}
	}
	else
	{
		printf("no file: %s\n", fileName.c_str());
	}
	printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %s <<<<<<<<<<<<<<<<<<<<<<<<<<< write end\n\n", fileName.c_str());
}

//void simplifyPath_Test()
//{
//	std::cout << simplifyPath("aaa/bbb/ccc/../e/../../test.txt") << std::endl; // 输出: /aaa/test.txt
//	std::cout << simplifyPath("/aaa/bbb/ccc/../e/../////../test.txt") << std::endl; // 输出: /aaa/test.txt
//	std::cout << simplifyPath("/aaa/bbb/ccc/d/e/../../test.txt") << std::endl; // 输出: /aaa/bbb/ccc/test.txt
//	std::cout << simplifyPath("/a/b/ccccc/d/eeee/../") << std::endl;    // 输出: /a/b/ccccc/d
//	std::cout << simplifyPath("/a/b/cccc/ddddd/e/./") << std::endl;     // 输出: /a/b/cccc/ddddd/e/
//	std::cout << simplifyPath("/./") << std::endl;               // 输出: /
//	std::cout << simplifyPath("/././././././test.txt") << std::endl;               // 输出: /test.txt
//	std::cout << simplifyPath("/../") << std::endl;             // 输出: (空字符串)
//	std::cout << simplifyPath("/a/b/c/d/e/../../../../../../") << std::endl; // 输出: (空字符串)
//}


void enumerateFiles(VirtualFileSystem& virtualFileSystem, const std::string& dir)
{
	printf("\n\nenumerate: %s\n", dir.c_str());
	virtualFileSystem.enumerate(dir, [](const FileInfo& fileInfo) -> bool {
		if (fileInfo.flags & FileFlags::Dir)
			std::cout << "dir : ";
		
		if (fileInfo.flags & FileFlags::File)
			std::cout << "file: ";

		std::cout << fileInfo.filePath << "     ";

		if (fileInfo.flags & FileFlags::Read)
			std::cout << "r";
		if (fileInfo.flags & FileFlags::Write)
			std::cout << "w";

		std::cout << "\n";
		return false;
	});
}

template <class T>
void readWriteTest(bool isMemoryFileSystem)
{
	auto data = readFileToVector("./test-data/template.zip");

	VirtualFileSystem virtualFileSystem;
	virtualFileSystem.mount(new T("./test-data/dlc1", "/root"));

	virtualFileSystem.createDir("/root/dir1_sub/cdef//../def/./aaa"); // /root/dir1_sub/def/aaa/

	virtualFileSystem.removeFile("/root/dir1_sub/write_1.zip");
	writeFile(virtualFileSystem, "/root/dir1_sub/write_1.zip", data);
	readFile(virtualFileSystem, "/root/dir1_sub/write_1.zip", false);
	assert(virtualFileSystem.removeFile("/root/dir1_sub/write_1.zip") == true);

	if (isMemoryFileSystem)
	{
		auto stream1 = virtualFileSystem.openFileStream("/root/dir1_sub/write_1.zip", FileStream::Mode::WRITE);
		auto stream2 = virtualFileSystem.openFileStream("/root/dir1_sub/write_1.zip", FileStream::Mode::WRITE);
		assert(stream1 != nullptr);
		assert(stream2 == nullptr);
		stream1->close();
		auto stream3 = virtualFileSystem.openFileStream("/root/dir1_sub/write_1.zip", FileStream::Mode::WRITE);
		assert(stream3 != nullptr);
	}

	enumerateFiles(virtualFileSystem, "/root/dir1_sub");
}

void mixTest()
{
	auto data = readFileToVector("./test-data/template.zip");

	VirtualFileSystem virtualFileSystem;
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc1", "/root"));
	virtualFileSystem.mount(new MemoryFileSystem("", "/root"));
	virtualFileSystem.mount(new MemoryFileSystem("", "/"));
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc2", "/root"));
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc2", "/root/vfs/vvv"));

	std::vector<uint8_t> bin = { 'H', 'E', 'L', 'L', 'O', '^', 'v', '^'};

	virtualFileSystem.createDir("/mem");
	writeFile(virtualFileSystem, "/aaaaaaaaaaaaaaaa.txt", bin);
	writeFile(virtualFileSystem, "/bbbbbbbbbbbbbb.txt", bin);
	writeFile(virtualFileSystem, "/ccccccccccccccc.txt", bin);
	writeFile(virtualFileSystem, "/xzcxz/ddddddddd.txt", bin);

	enumerateFiles(virtualFileSystem, "/");
	enumerateFiles(virtualFileSystem, "/root/vfs");
	writeFile(virtualFileSystem, "/root/vfs/ddddddddd.txt", bin);
	enumerateFiles(virtualFileSystem, "/root/vfs/vvv");


	virtualFileSystem.createDir("/root/mem/");
	writeFile(virtualFileSystem, "/root/mem/aaaaaaaaaaaaaaaa.txt", bin);
	writeFile(virtualFileSystem, "/root/mem_data.txt", bin);

	enumerateFiles(virtualFileSystem, "/root/");

	readFile(virtualFileSystem, "/check.txt", true);
	readFile(virtualFileSystem, "/root/../check.txt", true);
	readFile(virtualFileSystem, "/root/file.txt", true);
	readFile(virtualFileSystem, "/root/mem/aaaaaaaaaaaaaaaa.txt", true);
	readFile(virtualFileSystem, "/root/mem_data.txt", true);
	readFile(virtualFileSystem, "/root/check_png.py", false);
}

int main()
{
	readWriteTest<NativeFileSystem>(false);
	//readWriteTest<MemoryFileSystem>(true);
	//mixTest();
	return 0;
}