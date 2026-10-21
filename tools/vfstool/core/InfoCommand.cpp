// vfstool info: print header fields and entry statistics.
#include "Commands.h"
#include <map>
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
	std::map<uint64_t, size_t> blockSizes;   // LZ4 block size -> file count
	std::set<uint64_t> offsets;   // Dedup accounting: non-empty files that share a stored-data start count once.
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
		{
			++compressed;
			++blockSizes[entry.blockSize()];
		}
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
			<< "  \"blockSizes\": {" << [&] {
				   std::string j;
				   for (const auto& kv : blockSizes)
					   j += (j.empty() ? "\"" : ", \"") + std::to_string(kv.first) + "\": " + std::to_string(kv.second);
				   return j;
			   }() << "},\n"
			<< "  \"dedupShared\": " << shared << ",\n"
			<< "  \"totalSize\": " << totalSize << ",\n"
			<< "  \"indexHash\": \"" << std::hex << h.indexHash << std::dec << "\",\n"
			<< "  \"headerChecksum\": \"" << std::hex << h.headerChecksum << std::dec << "\"\n"
			<< "}\n";
		return 0;
	}

	out << "Archive:        " << options.archive.archive << "\n"
		<< "Format version: " << h.version << "\n"
		<< "Archive size:   " << index->archiveSize << " bytes (base offset " << index->baseOffset << ")\n"
		<< "Alignment:      " << h.alignment << "\n"
		<< "Data:           offset " << h.dataOffset << ", size " << h.dataSize << "\n"
		<< "Index:          offset " << h.indexOffset << ", size " << h.indexSize << ", "
		<< (indexEncrypted ? (keyStored ? "ChaCha20-encrypted (key stored in archive)" : "ChaCha20-encrypted (external key)") : "plaintext")
		<< "\n"
		<< "Entries:        " << h.entryCount << " (files " << files << ", empty dirs " << dirs << ")\n"
		<< "Total size:     " << totalSize << " bytes\n"
		<< "XOR-encrypted:  " << encrypted << " file(s)\n"
		<< "LZ4-compressed: " << compressed << " file(s)" << [&] {
			   std::string t;
			   for (const auto& kv : blockSizes)
				   t += (t.empty() ? ", block size " : ", ") + formatBlockSize(kv.first) + " x" + std::to_string(kv.second);
			   return t;
		   }() << "\n"
		<< "Deduplicated:   " << shared << " entr(ies)\n"
		<< "Checksums:      header XXH3-64 = " << std::hex << h.headerChecksum << ", index XXH3-64 = " << h.indexHash
		<< std::dec << " (all verified)\n";
	return 0;
}

} // namespace tool

NS_VFS_END
