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


template <class T>
void readWriteTest()
{
	auto data = readFileToVector("./test-data/template.zip");

	VirtualFileSystem virtualFileSystem;
	virtualFileSystem.mount(new T("./test-data/dlc1", "/root"));

	virtualFileSystem.createDir("/root/dir1_sub/cdef//../def/./aaa"); // /root/dir1_sub/def/aaa/

	virtualFileSystem.removeFile("/root/dir1_sub/write_1.zip");
	writeFile(virtualFileSystem, "/root/dir1_sub/write_1.zip", data);
	readFile(virtualFileSystem, "/root/dir1_sub/write_1.zip", false);
	assert(virtualFileSystem.removeFile("/root/dir1_sub/write_1.zip") == true);

	virtualFileSystem.enumerate("/root/dir1_sub", [](const FileInfo& fileInfo) -> bool {
		std::cout << fileInfo.filePath << "     " << fileInfo.isDir << "\n";
		return false;
	});

}

void mixTest()
{
	auto data = readFileToVector("./test-data/template.zip");

	VirtualFileSystem virtualFileSystem;
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc1", "/root"));
	virtualFileSystem.mount(new MemoryFileSystem("", "/root"));
	virtualFileSystem.mount(new MemoryFileSystem("", "/"));
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc2", "/root"));

	std::vector<uint8_t> bin = { 0xee, 0x21, 0xe2 };

	virtualFileSystem.createDir("/mem");
	writeFile(virtualFileSystem, "/aaaaaaaaaaaaaaaa.txt", bin);
	writeFile(virtualFileSystem, "/bbbbbbbbbbbbbb.txt", bin);
	writeFile(virtualFileSystem, "/ccccccccccccccc.txt", bin);
	writeFile(virtualFileSystem, "/xzcxz/ddddddddd.txt", bin);
	virtualFileSystem.enumerate("/", [](const FileInfo& fileInfo) -> bool {
		std::cout << fileInfo.filePath << "     " << fileInfo.isDir << "\n";
	return false;
		});
	//readFile(virtualFileSystem, "/check.txt", true);
	//readFile(virtualFileSystem, "/root/../check.txt", true);
	//readFile(virtualFileSystem, "/root/file.txt", true);
	//readFile(virtualFileSystem, "/root/check_png.py", false);
}

int main()
{
	//readWriteTest<NativeFileSystem>();
	//readWriteTest<MemoryFileSystem>();
	mixTest();
	return 0;
}