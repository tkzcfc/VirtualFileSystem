// vfstool list / ls：列出条目
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
				<< (entry.compression != CompressionType::NONE ? "true" : "false") << ", \"hash\": \"" << std::hex
				<< std::setw(16) << std::setfill('0') << entry.hash << std::dec << std::setfill(' ') << "\"}";
			first = false;
		}
		else if (options.longFormat)
		{
			// 标志：D=空目录，E=XOR 加密，C=LZ4 压缩
			std::string flags;
			flags += isDir ? 'D' : '-';
			flags += entry.cipher != CipherType::NONE ? 'E' : '-';
			flags += entry.compression != CompressionType::NONE ? 'C' : '-';
			out << flags << " " << std::setw(12) << entry.size << " " << std::setw(12) << entry.storedSize << " "
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
