// vfstool keygen：生成 32 字节随机索引密钥（hex）
#include "Commands.h"
#include "vfs/native/NativeFileStream.h"
#include <random>

NS_VFS_BEGIN

namespace tool
{

int runKeygen(const KeygenOptions& options, std::ostream& out, std::ostream& err)
{
	// std::random_device 在主流平台上取自 OS 的随机源（/dev/urandom、BCryptGenRandom 等）
	std::random_device rd;
	uint8_t key[32];
	for (size_t i = 0; i < sizeof(key); i += 4)
	{
		uint32_t v = rd();
		key[i] = static_cast<uint8_t>(v);
		key[i + 1] = static_cast<uint8_t>(v >> 8);
		key[i + 2] = static_cast<uint8_t>(v >> 16);
		key[i + 3] = static_cast<uint8_t>(v >> 24);
	}
	std::string hex = toHex(key, sizeof(key));
	if (options.outputFile.empty())
	{
		out << hex << "\n";
		return 0;
	}
	NativeFileStream fs;
	if (!fs.open(options.outputFile, IFileStream::Mode::WRITE))
	{
		err << "错误: 无法写入 " << options.outputFile << "\n";
		return static_cast<int>(ExitCode::IO);
	}
	hex.push_back('\n');
	if (fs.write(hex.data(), static_cast<unsigned int>(hex.size())) != static_cast<int>(hex.size()))
		return static_cast<int>(ExitCode::IO);
	out << "已生成密钥: " << options.outputFile << "\n";
	return 0;
}

} // namespace tool

NS_VFS_END
