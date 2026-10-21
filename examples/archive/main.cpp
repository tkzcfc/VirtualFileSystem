// 打包与读取 .vfs 归档：自定义索引密钥（不存入文件）、单文件 XOR 加密、LZ4 压缩，
// 然后用同一个密钥挂载读取；演示密钥错误时的报错。
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveWriter.h"
#include <cstdio>
#include <filesystem>
#include <string>

USING_NS_VFS;
namespace fs_ = std::filesystem;

int main()
{
	const fs_::path work = fs_::temp_directory_path() / "vfs_example_archive";
	fs_::remove_all(work);
	fs_::create_directories(work);
	const std::string archivePath = (work / "game.vfs").string();

	// 32 字节索引密钥：实际项目中应来自构建系统/密钥文件，这里用固定值演示
	std::vector<uint8_t> key(32);
	for (size_t i = 0; i < key.size(); ++i)
		key[i] = static_cast<uint8_t>(i * 7 + 1);

	// 1. 打包
	{
		ArchiveWriteOptions wo;
		wo.indexKey = key;     // 索引用这个密钥做 ChaCha20 加密
		wo.storeKey = false;   // 不写进文件：没有密钥就读不出文件列表
		ArchiveWriter writer(wo);

		std::string lua;
		for (int i = 0; i < 200; ++i)
			lua += "print('line " + std::to_string(i) + "')\n";   // 重复内容，压缩效果明显

		FileWriteOptions plain;                 // 明文存储
		FileWriteOptions packed;                // 压缩 + 加密
		packed.compress = true;
		packed.encrypt = true;
		writer.addData("config.json", std::vector<uint8_t>{'{', '}'}, plain);
		writer.addData("scripts/main.lua", std::vector<uint8_t>(lua.begin(), lua.end()), packed);
		writer.addData("scripts/copy.lua", std::vector<uint8_t>(lua.begin(), lua.end()), packed);   // 内容相同，去重

		ArchiveWriteStats stats;
		if (writer.write(archivePath, &stats) != Error::OK)
		{
			std::printf("pack failed: %s\n", writer.lastError().c_str());
			return 1;
		}
		std::printf("packed %u files, %lld bytes on disk\n", stats.fileCount,
					static_cast<long long>(fs_::file_size(archivePath)));
	}

	// 2. 用正确的密钥挂载并读取
	{
		VirtualFileSystem fs;
		ArchiveOptions ao;
		ao.indexKey = key;
		Error err = Error::OK;
		fs.mountArchive("/res", archivePath, ao, {}, &err);
		std::printf("mount with key: %s\n", errorName(err));

		std::string text;
		fs.readFile("/res/scripts/main.lua", text);
		std::printf("main.lua: %zu bytes, first line: %s", text.size(), text.substr(0, text.find('\n') + 1).c_str());

		Stat st;
		fs.stat("/res/scripts/copy.lua", st);
		std::printf("copy.lua size: %lld\n", static_cast<long long>(st.size));
	}

	// 3. 不给密钥：索引无法解密，挂载失败
	{
		VirtualFileSystem fs;
		Error err = Error::OK;
		fs.mountArchive("/res", archivePath, {}, {}, &err);
		std::printf("mount without key: %s\n", errorName(err));
	}

	fs_::remove_all(work);
	return 0;
}
