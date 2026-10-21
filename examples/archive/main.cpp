// Pack and read a .vfs archive: a custom index key (not stored in the file), per-file XOR encryption, LZ4 compression,
// then mount and read with the same key; show the error when the key is wrong.
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

	// 32-byte index key: a real project should take this from the build system or a key file; a fixed value is used here as a demo.
	std::vector<uint8_t> key(32);
	for (size_t i = 0; i < key.size(); ++i)
		key[i] = static_cast<uint8_t>(i * 7 + 1);

	// 1. Pack.
	{
		ArchiveWriteOptions wo;
		wo.indexKey = key;     // ChaCha20-encrypt the index with this key.
		wo.storeKey = false;   // Do not write it into the file: without the key the file list cannot be read.
		ArchiveWriter writer(wo);

		std::string lua;
		for (int i = 0; i < 200; ++i)
			lua += "print('line " + std::to_string(i) + "')\n";   // Repeated content, so compression is obvious.

		FileWriteOptions plain;                 // Stored in the clear.
		FileWriteOptions packed;                // Compressed + encrypted.
		packed.compress = true;
		packed.encrypt = true;
		writer.addData("config.json", std::vector<uint8_t>{'{', '}'}, plain);
		writer.addData("scripts/main.lua", std::vector<uint8_t>(lua.begin(), lua.end()), packed);
		writer.addData("scripts/copy.lua", std::vector<uint8_t>(lua.begin(), lua.end()), packed);   // Identical content, so it is deduped.

		ArchiveWriteStats stats;
		if (writer.write(archivePath, &stats) != Error::OK)
		{
			std::printf("pack failed: %s\n", writer.lastError().c_str());
			return 1;
		}
		std::printf("packed %u files, %lld bytes on disk\n", stats.fileCount,
					static_cast<long long>(fs_::file_size(archivePath)));
	}

	// 2. Mount and read with the correct key.
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

	// 3. No key: the index cannot be decrypted and the mount fails.
	{
		VirtualFileSystem fs;
		Error err = Error::OK;
		fs.mountArchive("/res", archivePath, {}, {}, &err);
		std::printf("mount without key: %s\n", errorName(err));
	}

	fs_::remove_all(work);
	return 0;
}
