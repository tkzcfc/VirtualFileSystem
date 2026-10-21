// vfstool list / ls: list entries.
#include "Commands.h"
#include <iomanip>

NS_VFS_BEGIN

namespace tool
{

int runList(const ListOptions& options, std::ostream& out, std::ostream& err)
{
	std::shared_ptr<const ArchiveIndex> index;
	int code = openArchive(options.archive, index, err);
	if (code != 0)
		return code;

	bool first = true;
	if (options.json)
		out << "[\n";
	for (const auto& entry : index->entries)
	{
		if (!options.pattern.empty() && !Glob::match(options.pattern, entry.path))
			continue;
		const bool isDir = entry.type == EntryType::DIR;
		if (options.json)
		{
			out << (first ? "" : ",\n") << "  {\"path\": \"" << entry.path << "\", \"type\": \""
				<< (isDir ? "dir" : "file") << "\", \"size\": " << entry.size << ", \"storedSize\": " << entry.storedSize
				<< ", \"offset\": " << entry.offset << ", \"encrypted\": "
				<< (entry.cipher != CipherType::NONE ? "true" : "false") << ", \"compressed\": "
				<< (entry.compression != CompressionType::NONE ? "true" : "false") << ", \"blockSize\": "
				<< (entry.compression != CompressionType::NONE ? entry.blockSize() : 0) << ", \"hash\": \"" << std::hex
				<< std::setw(16) << std::setfill('0') << entry.hash << std::dec << std::setfill(' ') << "\"}";
			first = false;
		}
		else if (options.longFormat)
		{
			// Flags: D = empty directory, E = XOR encryption, C = LZ4 compression; then the LZ4 block size ("-" if not compressed).
			std::string flags;
			flags += isDir ? 'D' : '-';
			flags += entry.cipher != CipherType::NONE ? 'E' : '-';
			flags += entry.compression != CompressionType::NONE ? 'C' : '-';
			const std::string block = entry.compression != CompressionType::NONE ? formatBlockSize(entry.blockSize()) : "-";
			out << flags << " " << std::setw(12) << entry.size << " " << std::setw(12) << entry.storedSize << " "
				<< std::setw(5) << block << " "
				<< std::hex << std::setw(16) << std::setfill('0') << entry.hash << std::dec << std::setfill(' ') << "  "
				<< entry.path << (isDir ? "/" : "") << "\n";
		}
		else
			out << entry.path << (isDir ? "/" : "") << "\n";
	}
	if (options.json)
		out << (first ? "" : "\n") << "]\n";
	return 0;
}

} // namespace tool

NS_VFS_END
