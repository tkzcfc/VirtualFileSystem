// vfstool keygen: generate a 32-byte random index key (hex)
#include "Commands.h"
#include "vfs/native/NativeFileStream.h"
#include "vfs/native/NativeUtils.h"
#include <random>

NS_VFS_BEGIN

namespace tool
{

int runKeygen(const KeygenOptions& options, std::ostream& out, std::ostream& err)
{
	// On mainstream platforms std::random_device comes from the OS random source (/dev/urandom, BCryptGenRandom, and so on)
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
		err << "error: cannot write " << options.outputFile << "\n";
		return static_cast<int>(ExitCode::IO);
	}
	hex.push_back('\n');
	if (fs.write(hex.data(), static_cast<unsigned int>(hex.size())) != static_cast<int>(hex.size()) || fs.close() != 0)
	{
		// Do not leave a partial key file
		fs.close();
		native::removeFilePath(options.outputFile);
		err << "error: cannot write " << options.outputFile << "\n";
		return static_cast<int>(ExitCode::IO);
	}
	out << "key written: " << options.outputFile << "\n";
	return 0;
}

} // namespace tool

NS_VFS_END
