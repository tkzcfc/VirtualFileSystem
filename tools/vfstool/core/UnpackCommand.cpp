// vfstool unpack：把整个归档解包到目录
#include "Commands.h"

NS_VFS_BEGIN

namespace tool
{

int runUnpack(const UnpackOptions& options, std::ostream& out, std::ostream& err)
{
	if (options.outputDir.empty())
	{
		err << "error: -o <output dir> is required\n";
		return static_cast<int>(ExitCode::USAGE);
	}
	std::shared_ptr<const ArchiveIndex> index;
	int code = openArchive(options.archive, index, err);
	if (code != 0)
		return code;

	bool hasInclude = false;
	for (const auto& r : options.filterRules)
		hasInclude = hasInclude || r.positive;

	// 整个解包过程复用一个来源流，避免每个文件都重新打开归档
	Error e = Error::OK;
	auto stream = index->source->openStream(&e);
	if (!stream)
	{
		err << "error: cannot read archive: " << errorMessage(e) << "\n";
		return exitCodeFor(e);
	}

	size_t files = 0, dirs = 0;
	int result = 0;
	for (const auto& entry : index->entries)
	{
		if (!Glob::evaluate(options.filterRules, entry.path, !hasInclude))
			continue;
		e = writeEntryToDisk(*index, entry, options.outputDir, options.force, !options.noVerify, stream.get());
		if (e != Error::OK)
		{
			if (e == Error::EXISTS)
				err << "error: file exists (use --force to overwrite): " << entry.path << "\n";
			else if (e == Error::CORRUPTED)
				err << "error: content verification failed: " << entry.path << "\n";
			else
				err << "error: write failed: " << entry.path << " (" << errorName(e) << ")\n";
			result = e == Error::CORRUPTED ? static_cast<int>(ExitCode::INTEGRITY) : exitCodeFor(e);
			return result;
		}
		if (options.verbose)
			out << "  " << entry.path << (entry.type == EntryType::DIR ? "/" : "") << "\n";
		if (entry.type == EntryType::DIR)
			++dirs;
		else
			++files;
	}
	out << "unpacked " << files << " file(s) and " << dirs << " empty dir(s) to " << options.outputDir << "\n";
	return result;
}

} // namespace tool

NS_VFS_END
