// 密钥解析与各命令共用的工具函数
#include "Commands.h"
#include "vfs/native/NativeFileStream.h"
#include "vfs/native/NativeUtils.h"
#include "vfs/crypto/Hash.h"
#include <cstdlib>
#include <filesystem>

NS_VFS_BEGIN

namespace tool
{

namespace
{

int hexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

std::string_view trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
		s.remove_suffix(1);
	return s;
}

} // namespace

bool parseHexKey(std::string_view text, std::vector<uint8_t>& key)
{
	text = trim(text);
	if (text.size() != 64)
		return false;
	key.resize(32);
	for (size_t i = 0; i < 32; ++i)
	{
		int hi = hexValue(text[i * 2]);
		int lo = hexValue(text[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return false;
		key[i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return true;
}

std::string toHex(const uint8_t* data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	s.reserve(size * 2);
	for (size_t i = 0; i < size; ++i)
	{
		s.push_back(digits[data[i] >> 4]);
		s.push_back(digits[data[i] & 0xF]);
	}
	return s;
}

Error resolveKey(const KeySource& source, std::vector<uint8_t>& key, std::ostream& err)
{
	key.clear();
	// 优先级：--key-file > --key-hex > VFS_KEY
	if (!source.keyFile.empty())
	{
		NativeFileStream fs;
		if (!fs.open(source.keyFile, IFileStream::Mode::READ))
		{
			err << "error: cannot read key file " << source.keyFile << "\n";
			return native::errorFromErrno(errno);
		}
		std::string content(256, '\0');
		int n = fs.read(content.data(), static_cast<unsigned int>(content.size()));
		content.resize(n > 0 ? static_cast<size_t>(n) : 0);
		// 32 字节原始数据，或 64 个 hex 字符
		if (content.size() == 32)
		{
			key.assign(content.begin(), content.end());
			return Error::OK;
		}
		if (parseHexKey(content, key))
			return Error::OK;
		err << "error: key file must contain 32 raw bytes or 64 hex characters\n";
		return Error::KEY_INVALID;
	}
	if (!source.keyHex.empty())
	{
		err << "warning: --key-hex is visible in shell history and process lists; prefer --key-file or the VFS_KEY environment variable\n";
		if (parseHexKey(source.keyHex, key))
			return Error::OK;
		err << "error: --key-hex must be 64 hex characters\n";
		return Error::KEY_INVALID;
	}
	if (source.useEnv)
	{
		const char* env = std::getenv("VFS_KEY");
		if (env && *env)
		{
			if (parseHexKey(env, key))
				return Error::OK;
			err << "error: environment variable VFS_KEY must be 64 hex characters\n";
			return Error::KEY_INVALID;
		}
	}
	return Error::OK;
}

int exitCodeFor(Error e)
{
	switch (e)
	{
	case Error::OK:
		return static_cast<int>(ExitCode::OK);
	case Error::CORRUPTED:
	case Error::VERSION_MISMATCH:
		return static_cast<int>(ExitCode::FORMAT);
	case Error::KEY_REQUIRED:
	case Error::KEY_INVALID:
		return static_cast<int>(ExitCode::KEY);
	case Error::INVALID_PATH:
	case Error::UNSUPPORTED:
		return static_cast<int>(ExitCode::USAGE);
	default:
		return static_cast<int>(ExitCode::IO);
	}
}

int openArchive(const ArchiveArgs& args, std::shared_ptr<const ArchiveIndex>& index, std::ostream& err)
{
	ArchiveOptions options;
	options.baseOffset = args.baseOffset;
	Error e = resolveKey(args.key, options.indexKey, err);
	if (e != Error::OK)
		return exitCodeFor(e);
	index = ArchiveReader::open(args.archive, options, &e);
	if (!index)
	{
		err << "error: cannot open archive " << args.archive << ": " << errorMessage(e) << " (" << errorName(e) << ")\n";
		return exitCodeFor(e);
	}
	return static_cast<int>(ExitCode::OK);
}

Error writeEntryToDisk(const ArchiveIndex& index, const ArchiveEntry& entry, const std::string& outDir, bool force,
					   bool verify, IFileStream* stream)
{
	std::error_code ec;
	std::filesystem::path target = native::toFsPath(outDir) / native::toFsPath(entry.path);
	if (entry.type == EntryType::DIR)
	{
		std::filesystem::create_directories(target, ec);
		return native::errorFromCode(ec);
	}
	std::filesystem::create_directories(target.parent_path(), ec);
	if (ec)
		return native::errorFromCode(ec);
	if (!force && std::filesystem::exists(target, ec))
		return Error::EXISTS;

	std::vector<uint8_t> data;
	Error e = ArchiveReader::readEntry(index, entry, data, stream);
	if (e != Error::OK)
		return e;
	if (verify && xxh3_128(data.data(), data.size()).low != entry.hash)
		return Error::CORRUPTED;

	NativeFileStream fs;
	if (!fs.open(native::fromFsPath(target), IFileStream::Mode::WRITE))
		return native::errorFromErrno(errno);
	size_t done = 0;
	while (done < data.size())
	{
		int n = fs.write(data.data() + done,
						 static_cast<unsigned int>(std::min<size_t>(data.size() - done, VFS_MAX_IO_CHUNK)));
		if (n <= 0)
			return Error::IO_ERROR;
		done += static_cast<size_t>(n);
	}
	return fs.close() == 0 ? Error::OK : Error::IO_ERROR;
}

} // namespace tool

NS_VFS_END
