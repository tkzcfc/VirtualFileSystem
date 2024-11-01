#include "NativeFileSystem.h"
#include "NativeFileStream.h"
#include <filesystem>

namespace fs = std::filesystem;

NS_VFS_BEGIN

NativeFileSystem::NativeFileSystem(const std::string& archiveLocation, const std::string& mntpoint)
	: FileSystem(archiveLocation, mntpoint)
{}

NativeFileSystem::~NativeFileSystem()
{}

void NativeFileSystem::enumerate(const std::string& dir, const std::function<bool(const FileInfo&)>& call)
{
	if (!fs::exists(dir) || !fs::is_directory(dir))
		return;

	uint8_t defaultFlgs = FileFlags::Read;
	if (!isReadonly())
		defaultFlgs |= FileFlags::Write;

	FileInfo info;
	for (const auto& entry : fs::directory_iterator(dir))
	{
		auto filePath = entry.path().string().substr(m_archiveLocation.size());

		info.flags = defaultFlgs;
		if (entry.is_directory())
			info.flags |= FileFlags::Dir;
		else
			info.flags |= FileFlags::File;

		info.filePath = m_mntpoint + filePath;
		if (call(info))
			break;
	}
}

std::unique_ptr<FileStream> NativeFileSystem::openFileStream(const std::string& filePath, FileStream::Mode mode)
{
	auto fs = std::make_unique<NativeFileStream>();
	return fs->open(filePath, mode) ? std::move(fs) : nullptr;
}

bool NativeFileSystem::removeFile(const std::string& filePath)
{
	return fs::remove(filePath);
}

bool NativeFileSystem::isFile(const std::string& filePath) const
{
	return fs::exists(filePath) && fs::is_regular_file(filePath);
}

bool NativeFileSystem::isDir(const std::string& dirPath) const
{
	return fs::exists(dirPath) && fs::is_directory(dirPath);
}

bool NativeFileSystem::createDir(const std::string& dirPath)
{
	try {
		if (fs::create_directories(dirPath)) {
			//std::cout << "createDir: " << dirPath << std::endl;
			return true;
		}
		else {
			//std::cout << "Directory already exists: " << dirPath << std::endl;
			return false;
		}
	}
	catch (const fs::filesystem_error& e) 
	{
		std::cerr << "createDir error: " << e.what() << std::endl;
		return false;
	}
}

NS_VFS_END
