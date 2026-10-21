// Minimal example: build an archive in memory, stack it with a writable memory layer at /res, and show read, override, and directory listing.
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/archive/ArchiveWriter.h"
#include <iostream>

USING_NS_VFS;

int main()
{
	// 1. Build an archive: config.json in plaintext, script.lua compressed and XOR-encrypted
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
			std::cerr << "pack failed: " << writer.lastError() << "\n";
			return 1;
		}
	}

	// 2. Archive as the read-only lower layer, memory as the writable upper layer, same mount point
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

	// 3. Read a file from the archive
	std::string text;
	fs.readFile("/res/scripts/main.lua", text);
	std::cout << "main.lua: " << text;

	// 4. Write a file of the same name: it lands on the writable layer and hides the archive original
	fs.writeFile("/res/config.json", "{\"patched\":true}");
	fs.readFile("/res/config.json", text);
	std::cout << "config.json: " << text << "\n";

	// 5. List the merged view
	for (const auto& e : fs.list("/res"))
		std::cout << (e.type == FileType::DIR ? "[dir]  " : "[file] ") << e.name << "\n";

	// 6. Delete the override: the original from the archive shows through again
	fs.removeFile("/res/config.json");
	fs.readFile("/res/config.json", text);
	std::cout << "config.json after removing the override: " << text << "\n";
	return 0;
}
