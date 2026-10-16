// vfstool info：显示 header 字段与条目统计
#include "Commands.h"
#include <set>

NS_VFS_BEGIN

namespace tool
{

int runInfo(const InfoOptions& options, std::ostream& out, std::ostream& err)
{
	std::shared_ptr<const ArchiveIndex> index;
	int code = openArchive(options.archive, index, err);
	if (code != 0)
		return code;

	const ArchiveHeader& h = index->header;
	size_t files = 0, dirs = 0, encrypted = 0, compressed = 0, shared = 0;
	uint64_t totalSize = 0;
	std::set<uint64_t> offsets;   // 统计去重：存储数据起点相同的非空文件只算一次
	for (const auto& entry : index->entries)
	{
		if (entry.type == EntryType::DIR)
		{
			++dirs;
			continue;
		}
		++files;
		totalSize += entry.size;
		if (entry.cipher != CipherType::NONE)
			++encrypted;
		if (entry.compression != CompressionType::NONE)
			++compressed;
		if (entry.storedSize > 0 && !offsets.insert(entry.offset).second)
			++shared;
	}
	const bool indexEncrypted = (h.flags & ArchiveFlags::IndexEncrypted) != 0;
	const bool keyStored = (h.flags & ArchiveFlags::KeyStored) != 0;

	if (options.json)
	{
		out << "{\n"
			<< "  \"archive\": \"" << options.archive.archive << "\",\n"
			<< "  \"version\": " << h.version << ",\n"
			<< "  \"baseOffset\": " << index->baseOffset << ",\n"
			<< "  \"archiveSize\": " << index->archiveSize << ",\n"
			<< "  \"alignment\": " << h.alignment << ",\n"
			<< "  \"indexEncrypted\": " << (indexEncrypted ? "true" : "false") << ",\n"
			<< "  \"keyStored\": " << (keyStored ? "true" : "false") << ",\n"
			<< "  \"dataOffset\": " << h.dataOffset << ",\n"
			<< "  \"dataSize\": " << h.dataSize << ",\n"
			<< "  \"indexOffset\": " << h.indexOffset << ",\n"
			<< "  \"indexSize\": " << h.indexSize << ",\n"
			<< "  \"entries\": " << h.entryCount << ",\n"
			<< "  \"files\": " << files << ",\n"
			<< "  \"emptyDirs\": " << dirs << ",\n"
			<< "  \"encryptedFiles\": " << encrypted << ",\n"
			<< "  \"compressedFiles\": " << compressed << ",\n"
			<< "  \"dedupShared\": " << shared << ",\n"
			<< "  \"totalSize\": " << totalSize << ",\n"
			<< "  \"indexHash\": \"" << std::hex << h.indexHash << std::dec << "\",\n"
			<< "  \"headerChecksum\": \"" << std::hex << h.headerChecksum << std::dec << "\"\n"
			<< "}\n";
		return 0;
	}

	out << "归档:       " << options.archive.archive << "\n"
		<< "格式版本:   " << h.version << "\n"
		<< "归档大小:   " << index->archiveSize << " 字节（起始偏移 " << index->baseOffset << "）\n"
		<< "对齐:       " << h.alignment << "\n"
		<< "数据区:     偏移 " << h.dataOffset << "，大小 " << h.dataSize << "\n"
		<< "索引区:     偏移 " << h.indexOffset << "，大小 " << h.indexSize << "，"
		<< (indexEncrypted ? (keyStored ? "ChaCha20 加密（密钥存放在文件中）" : "ChaCha20 加密（外部密钥）") : "明文")
		<< "\n"
		<< "条目:       " << h.entryCount << "（文件 " << files << "，空目录 " << dirs << "）\n"
		<< "原始总大小: " << totalSize << " 字节\n"
		<< "XOR 加密:   " << encrypted << " 个文件\n"
		<< "LZ4 压缩:   " << compressed << " 个文件\n"
		<< "去重共享:   " << shared << " 个条目\n"
		<< "校验:       header XXH3-64 = " << std::hex << h.headerChecksum << "，index XXH3-64 = " << h.indexHash
		<< std::dec << "（均已通过）\n";
	return 0;
}

} // namespace tool

NS_VFS_END
