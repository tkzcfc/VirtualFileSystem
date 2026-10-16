// 最小示例：内存中打一个归档，与可写的内存层叠加挂载到 /res，演示读取、覆盖与列目录。
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/archive/ArchiveWriter.h"
#include <iostream>

USING_NS_VFS;

int main()
{
	// 1. 构建一个归档：config.json 明文，script.lua 压缩 + XOR 加密
	auto memory = MemoryBackend::create();
	{
		auto out = memory->open("game.vfs", IFileStream::Mode::WRITE, nullptr);
		ArchiveWriter writer;
		writer.addData("config.json", std::vector<uint8_t>{'{', '}'});
		std::string script = "print('hello from archive')\n";
		FileWriteOptions fo;
		fo.encrypt = true;
		fo.compress = true;
		writer.addData("scripts/main.lua", std::vector<uint8_t>(script.begin(), script.end()), fo);
		if (writer.write(*out) != Error::OK)
		{
			std::cerr << "打包失败: " << writer.lastError() << "\n";
			return 1;
		}
	}

	// 2. 归档作为只读底层，内存层作为可写上层，挂在同一个挂载点
	VirtualFileSystem fs;
	std::vector<uint8_t> bytes;
	{
		auto in = memory->open("game.vfs", IFileStream::Mode::READ, nullptr);
		bytes.resize(static_cast<size_t>(in->size()));
		in->read(bytes.data(), static_cast<unsigned int>(bytes.size()));
	}
	auto archive = ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(std::move(bytes)));
	fs.mount("/res", archive);
	fs.mountMemory("/res", MountOptions{1, MountFlags::WriteTarget});

	// 3. 读取归档里的文件
	std::string text;
	fs.readFile("/res/scripts/main.lua", text);
	std::cout << "main.lua: " << text;

	// 4. 写入同名文件：落到可写层，遮蔽归档中的原版
	fs.writeFile("/res/config.json", "{\"patched\":true}");
	fs.readFile("/res/config.json", text);
	std::cout << "config.json: " << text << "\n";

	// 5. 列出合并视图
	for (const auto& e : fs.list("/res"))
		std::cout << (e.type == FileType::DIR ? "[目录] " : "[文件] ") << e.name << "\n";

	// 6. 删除覆盖文件：归档里的原版重新露出
	fs.removeFile("/res/config.json");
	fs.readFile("/res/config.json", text);
	std::cout << "删除覆盖后 config.json: " << text << "\n";
	return 0;
}
